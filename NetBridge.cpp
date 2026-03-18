//
// Created by Anders Cedronius on 2020-07-07.
//

#include "NetBridge.h"

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
   // a1->object = std::make_shared<MyClass>();
    return a1;
}

//Handle client disconnect
void NetBridge::handleClientDisconnect(std::shared_ptr<SRTNet::NetworkConnection>& ctx, SRTSOCKET socket) {
    std::cout << "Client disconnected from socket " << socket << std::endl;
}

//Data callback in MPEGTS mode.
bool NetBridge::handleDataMPEGTS(std::unique_ptr <std::vector<uint8_t>> &rContent, SRT_MSGCTRL &rMsgCtrl, std::shared_ptr<SRTNet::NetworkConnection> lCtx, SRTSOCKET lClientHandle) {
    mPacketCounter++;
    
    // Extract stream ID from message control if available
    std::string streamId = (rMsgCtrl.grpdata != nullptr && rMsgCtrl.grpdata_size > 0) ? 
        std::string((const char*)rMsgCtrl.grpdata, rMsgCtrl.grpdata_size) : "";
    std::cout << "handleDataMPEGTS: detected streamId='" << streamId << "' (size=" << rMsgCtrl.grpdata_size << ")" << std::endl;
    
    // Log configured connections for comparison
    for (size_t i = 0; i < mConnections.size(); i++) {
        std::cout << "  Connection " << i << ": configured streamId='" << mConnections[i].mStreamId << "'" << std::endl;
    }
    
    // Route packet to the correct UDP destination based on stream ID
    for (auto &rConnection: mConnections) {
        // Match by stream ID if specified, otherwise use first connection (backward compatible)
        if (rConnection.mStreamId.empty() || rConnection.mStreamId == streamId) {
            rConnection.mNetOut->send((const std::byte *)rContent->data(), rContent->size());
            return true;
        }
    }
    
    // Fallback: send to first connection if no stream ID match
    if (!mConnections.empty()) {
        mConnections[0].mNetOut->send((const std::byte *)rContent->data(), rContent->size());
    }
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
                    rConnection.mNetOut->send((const std::byte *)lSendData.data(), lSendData.size());
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
    lConnection.mNetOut = std::make_shared<kissnet::udp_socket>(kissnet::endpoint(rConfig.mOutIp, rConfig.mOutPort));
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
    lConnection.mNetOut = std::make_shared<kissnet::udp_socket>(kissnet::endpoint(rConfig.mOutIp, rConfig.mOutPort));
    lConnection.tag = rConfig.mTag;
    lConnection.mStreamId = rConfig.mStreamId;  // Store stream ID for routing
    mConnections.push_back(lConnection);
    return true;
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
