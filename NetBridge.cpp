//
// Created by Anders Cedronius on 2020-07-07.
//

#include "NetBridge.h"
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include <cstring>
#include <iostream>
#include <errno.h>

//Return a connection object. (Return nullptr if you don't want to connect to that client)
std::shared_ptr<SRTNet::NetworkConnection> NetBridge::validateConnection(struct sockaddr &rSin, SRTSOCKET lNewSocket, std::shared_ptr<SRTNet::NetworkConnection> &rCtx) {

    char addrIPv6[INET6_ADDRSTRLEN];

    if (rSin.sa_family == AF_INET) {
        struct sockaddr_in* inConnectionV4 = (struct sockaddr_in*) &rSin;
        auto *ip = (unsigned char *) &inConnectionV4->sin_addr.s_addr;
        std::cout << "Connecting IPv4: " << unsigned(ip[0]) << "." << unsigned(ip[1]) << "." << unsigned(ip[2]) << "."
                  << unsigned(ip[3]) << std::endl;

        //Do we want to accept this connection?
        //return nullptr;


    } else if (rSin.sa_family == AF_INET6) {
        struct sockaddr_in6* inConnectionV6 = (struct sockaddr_in6*) &rSin;
        inet_ntop(AF_INET6, &inConnectionV6->sin6_addr, addrIPv6, INET6_ADDRSTRLEN);
        printf("Connecting IPv6: %s\n", addrIPv6);

        //Do we want to accept this connection?
        //return nullptr;

    } else {
        //Not IPv4 and not IPv6. That's weird. don't connect.
        return nullptr;
    }

    auto a1 = std::make_shared<SRTNet::NetworkConnection>();
    
    // Extract stream ID from the SRT socket
    char streamIdBuffer[512];
    int streamIdLen = sizeof(streamIdBuffer);
    if (srt_getsockopt(lNewSocket, 0, SRTO_STREAMID, streamIdBuffer, &streamIdLen) == SRT_SUCCESS) {
        std::string streamId(streamIdBuffer, streamIdLen);
        std::cout << "Client stream ID: '" << streamId << "'" << std::endl;
        
        // Validate that this stream_id is registered in our configuration
        bool streamIdRegistered = false;
        bool hasCatchAll = false;
        
        for (const auto &rConnection: mConnections) {
            // Check if there's a matching stream_id
            if (!rConnection.mStreamId.empty() && rConnection.mStreamId == streamId) {
                streamIdRegistered = true;
                break;
            }
            // Check if there's a catch-all interface (empty stream_id requirement)
            if (rConnection.mStreamId.empty()) {
                hasCatchAll = true;
            }
        }
        
        // Reject connection if stream_id is not registered and no catch-all exists
        if (!streamIdRegistered && !hasCatchAll) {
            std::cout << "Rejecting connection: stream_id '" << streamId << "' is not registered" << std::endl;
            return nullptr;
        }
        
        // Store stream ID in connection context
        auto ctx = std::make_shared<ConnectionContext>();
        ctx->streamId = streamId;
        a1->mObject = ctx;
    } else {
        std::cout << "Could not retrieve stream ID from socket" << std::endl;
        auto ctx = std::make_shared<ConnectionContext>();
        ctx->streamId = "";
        a1->mObject = ctx;
    }
    
    return a1;
}

//Handle client disconnect
void NetBridge::handleClientDisconnect(std::shared_ptr<SRTNet::NetworkConnection>& ctx, SRTSOCKET socket) {
    std::cout << "Client disconnected from socket " << socket << std::endl;
}

//Data callback in MPEGTS mode.
bool NetBridge::handleDataMPEGTS(std::unique_ptr <std::vector<uint8_t>> &rContent, SRT_MSGCTRL &rMsgCtrl, std::shared_ptr<SRTNet::NetworkConnection> lCtx, SRTSOCKET lClientHandle) {
    mPacketCounter++;
    
    // Extract stream ID from connection context
    std::string streamId = "";
    if (lCtx) {
        try {
            auto ctx = std::any_cast<std::shared_ptr<ConnectionContext>>(lCtx->mObject);
            if (ctx) {
                streamId = ctx->streamId;
            }
        } catch (const std::bad_any_cast&) {
            // mObject doesn't contain ConnectionContext, that's okay
        }
    }
    
    // Route packet to the correct destination based on stream ID
    for (auto &rConnection: mConnections) {
        // If connection requires a specific stream ID, match it exactly
        if (!rConnection.mStreamId.empty()) {
            if (rConnection.mStreamId == streamId) {
                sendData(rConnection, rContent->data(), rContent->size());
                return true;
            }
        } else {
            // Connection has no stream ID requirement, accept any packet with no stream ID
            if (streamId.empty()) {
                sendData(rConnection, rContent->data(), rContent->size());
                return true;
            }
        }
    }
    
    // No matching connection found: discard packet to avoid corrupting valid data
    return true;
}

//Data callback in MPSRTTS mode.
bool NetBridge::handleDataMPSRTTS(std::unique_ptr <std::vector<uint8_t>> &rContent, SRT_MSGCTRL &msgCtrl, std::shared_ptr<SRTNet::NetworkConnection> lCtx, SRTSOCKET lClientHandle) {
    mPacketCounter++;

    //Did we get the expected size?
    double lPackets = (double) rContent->size() / 189.0;
    if (lPackets != (int) lPackets) {
        std::cout << "Payload not X * 189 " << std::endl;
        return true;  //Drop connection?
    }

    //Place the TS packets in respective tags queue
    for (int x = 0; x < (int)lPackets ; x++) {
        uint8_t tag = rContent->data()[x*189];
        std::vector<uint8_t> lPacket(rContent->data()+(x*189)+1, rContent->data()+(x*189)+189);
        mTSPackets[tag].push_back(lPacket);
    }

    //Check what queue we should empty if any
    std::vector<uint8_t> lSendData(188*7);
    for (auto &rPackets: mTSPackets) {
        if (rPackets.second.size() >= 7) {
            //We should send the data now
            for (int x = 0; x < 7 ; x++) {
               memmove(lSendData.data()+(188*x),rPackets.second.data()[0].data(),188);
                rPackets.second.erase(rPackets.second.begin());
            }
            //Send to what destination
            uint8_t tag = rPackets.first;
            for (auto &rConnection: mConnections) {
                if (rConnection.tag == tag) {
                    sendData(rConnection, lSendData.data(), lSendData.size());
                }
            }
        }
    }
    return true;
}

bool NetBridge::startBridge(Config &rConfig) {
    //Set the mode, save the config and zero counters
    mCurrentMode = rConfig.mMode;
    mCurrentConfig = rConfig;
    mPacketCounter = 0;

    //Start the SRT server
    mSRTServer.clientConnected = std::bind(&NetBridge::validateConnection, this, 
        std::placeholders::_1, std::placeholders::_2, std::placeholders::_3);
    mSRTServer.clientDisconnected = std::bind(&NetBridge::handleClientDisconnect, this,
        std::placeholders::_1, std::placeholders::_2);
    if (rConfig.mMode == Mode::MPEGTS) {
        mSRTServer.receivedData = std::bind(&NetBridge::handleDataMPEGTS, this, std::placeholders::_1,
                                            std::placeholders::_2, std::placeholders::_3, std::placeholders::_4);
    } else {
        mSRTServer.receivedData = std::bind(&NetBridge::handleDataMPSRTTS, this, std::placeholders::_1,
                                            std::placeholders::_2, std::placeholders::_3, std::placeholders::_4);
    }
    if (!mSRTServer.startServer(rConfig.mListenIp, rConfig.mListenPort, rConfig.mReorder, rConfig.mLatency, 100, MTU, 5000, rConfig.mPsk, rConfig.mSingleSender)) {
        std::cout << "SRT Server failed to start." << std::endl;
        return false;
    }

    //Add the output connection
    Connection lConnection;
    lConnection.mOutputType = rConfig.mOutputType;
    
    // Configure UDP if needed
    if (rConfig.mOutputType == OutputType::UDP || rConfig.mOutputType == OutputType::BOTH) {
        lConnection.mNetOut = std::make_shared<kissnet::udp_socket>(kissnet::endpoint(rConfig.mOutIp, rConfig.mOutPort));
    }
    
    // Configure FIFOs if needed
    if (rConfig.mOutputType == OutputType::FIFO || rConfig.mOutputType == OutputType::BOTH) {
        if (!rConfig.mFifoPaths.empty()) {
            if (!createAndOpenFifos(rConfig.mFifoPaths, lConnection.mFifoFds)) {
                std::cout << "Failed to create FIFO(s)" << std::endl;
                return false;
            }
            lConnection.mFifoPaths = rConfig.mFifoPaths;
        }
    }
    
    if (rConfig.mMode == Mode::MPSRTTS) {
        lConnection.tag = rConfig.mTag;
    }
    lConnection.mStreamId = rConfig.mStreamId;  // Store stream ID for MPEGTS stream-based routing
    mConnections.push_back(lConnection);
    return true;
}

void NetBridge::stopBridge() {
    mSRTServer.stop();
}

bool NetBridge::addInterface(Config &rConfig) {
    if (mCurrentMode != Mode::MPSRTTS && mCurrentMode != Mode::MPEGTS) {
        return false;
    }
    //Add the output connection
    Connection lConnection;
    lConnection.mOutputType = rConfig.mOutputType;
    
    // Configure UDP if needed
    if (rConfig.mOutputType == OutputType::UDP || rConfig.mOutputType == OutputType::BOTH) {
        lConnection.mNetOut = std::make_shared<kissnet::udp_socket>(kissnet::endpoint(rConfig.mOutIp, rConfig.mOutPort));
    }
    
    // Configure FIFOs if needed
    if (rConfig.mOutputType == OutputType::FIFO || rConfig.mOutputType == OutputType::BOTH) {
        if (!rConfig.mFifoPaths.empty()) {
            if (!createAndOpenFifos(rConfig.mFifoPaths, lConnection.mFifoFds)) {
                std::cout << "Failed to create FIFO(s)" << std::endl;
                return false;
            }
            lConnection.mFifoPaths = rConfig.mFifoPaths;
        }
    }
    
    lConnection.tag = rConfig.mTag;
    lConnection.mStreamId = rConfig.mStreamId;  // Store stream ID for routing
    mConnections.push_back(lConnection);
    return true;
}

bool NetBridge::removeInterface(const std::string &streamId, uint8_t tag) {
    auto it = mConnections.begin();
    while (it != mConnections.end()) {
        // Match by stream ID if provided (non-empty), otherwise match by tag
        if (!streamId.empty()) {
            if (it->mStreamId == streamId) {
                it = mConnections.erase(it);
                return true;
            } else {
                ++it;
            }
        } else {
            if (it->tag == tag) {
                it = mConnections.erase(it);
                return true;
            } else {
                ++it;
            }
        }
    }
    return false;  // Connection not found
}

bool NetBridge::hasActiveInterfaces() const {
    return !mConnections.empty();
}

NetBridge::Stats NetBridge::getStats() {
    NetBridge::Stats lStats;
    lStats.mPacketCounter = mPacketCounter;
    mSRTServer.getActiveClients([&](std::map<SRTSOCKET, std::shared_ptr<SRTNet::NetworkConnection>> &clientList)
                                    {
                                        lStats.mConnections = clientList.size();
                                    }
    );
    return lStats;
}

// Create multiple FIFOs (named pipes) and open them for writing
bool NetBridge::createAndOpenFifos(const std::vector<std::string> &fifoPaths, std::vector<int> &fifoFds) {
    fifoFds.clear();
    
    for (const auto &fifoPath : fifoPaths) {
        if (fifoPath.empty()) {
            continue;
        }
        
        // Check if FIFO already exists
        struct stat buffer;
        if (stat(fifoPath.c_str(), &buffer) == 0) {
            // File exists, try to open it
            int fd = open(fifoPath.c_str(), O_WRONLY | O_NONBLOCK);
            if (fd == -1) {
                std::cout << "Failed to open existing FIFO: " << fifoPath << " - " << strerror(errno) << std::endl;
                closeFifos(fifoFds);
                return false;
            }
            std::cout << "Opened existing FIFO: " << fifoPath << std::endl;
            fifoFds.push_back(fd);
            continue;
        }
        
        // Create new FIFO
        if (mkfifo(fifoPath.c_str(), 0666) == -1) {
            std::cout << "Failed to create FIFO: " << fifoPath << " - " << strerror(errno) << std::endl;
            closeFifos(fifoFds);
            return false;
        }
        
        // Open the FIFO for writing (non-blocking)
        int fd = open(fifoPath.c_str(), O_WRONLY | O_NONBLOCK);
        if (fd == -1) {
            std::cout << "Failed to open FIFO for writing: " << fifoPath << " - " << strerror(errno) << std::endl;
            unlink(fifoPath.c_str());
            closeFifos(fifoFds);
            return false;
        }
        
        std::cout << "Created and opened FIFO: " << fifoPath << std::endl;
        fifoFds.push_back(fd);
    }
    
    return true;
}

// Write data to FIFO, reopening if reader disconnected
bool NetBridge::writeFifo(int &fifoFd, const std::string &fifoPath, const uint8_t *data, size_t size) {
    if (fifoFd == -1 || data == nullptr || size == 0) {
        return false;
    }
    
    ssize_t written = write(fifoFd, data, size);
    if (written == -1) {
        if (errno == EPIPE) {
            // Reader disconnected - close old FD and reopen for new readers
            close(fifoFd);
            fifoFd = open(fifoPath.c_str(), O_WRONLY | O_NONBLOCK);
            if (fifoFd == -1) {
                // Reopen failed - no reader yet, which is fine for non-blocking FIFO
                return true;  // Silent fail
            }
            // Try writing again with fresh FD
            written = write(fifoFd, data, size);
            if (written == -1) {
                // Still failed, handle as EAGAIN/EWOULDBLOCK (no reader yet)
                if (errno != EAGAIN && errno != EWOULDBLOCK && errno != EPIPE) {
                    return false;  // Real error
                }
                return true;  // No reader yet, will retry next packet
            }
            return written == (ssize_t)size;
        }
        // Silent handling for other expected FIFO errors:
        // - EAGAIN/EWOULDBLOCK: no reader yet (non-blocking write)
        if (errno != EAGAIN && errno != EWOULDBLOCK) {
            std::cout << "Error writing to FIFO: " << strerror(errno) << std::endl;
            return false;
        }
        return true;  // Silent fail for these expected errors
    }
    
    return written == (ssize_t)size;
}

// Close all FIFO file descriptors
void NetBridge::closeFifos(std::vector<int> &fifoFds) {
    for (auto &fd : fifoFds) {
        if (fd != -1) {
            close(fd);
            fd = -1;
        }
    }
    fifoFds.clear();
}

// Send data to connection (UDP and/or FIFO)
void NetBridge::sendData(const Connection &conn, const uint8_t *data, size_t size) {
    if (conn.mOutputType == OutputType::UDP || conn.mOutputType == OutputType::BOTH) {
        if (conn.mNetOut) {
            conn.mNetOut->send((const std::byte *)data, size);
        }
    }
    
    if (conn.mOutputType == OutputType::FIFO || conn.mOutputType == OutputType::BOTH) {
        // Write to all FIFOs using indices to access both FD and path
        for (size_t i = 0; i < conn.mFifoFds.size() && i < conn.mFifoPaths.size(); ++i) {
            if (conn.mFifoFds[i] != -1) {
                writeFifo(conn.mFifoFds[i], conn.mFifoPaths[i], data, size);
            }
        }
    }
}

