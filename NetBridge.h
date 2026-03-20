//
// Created by Anders Cedronius on 2020-07-07.
//

#ifndef SRT_TO_UDP_SERVER_NETBRIDGE_H
#define SRT_TO_UDP_SERVER_NETBRIDGE_H

#include "SRTNet.h"
#include "kissnet.hpp"
#include <fstream>
#define MTU 1456 //SRT-max

// Helper struct to store connection-specific data
struct ConnectionContext {
    std::string streamId;
};

class NetBridge {
public:
    enum Mode: uint8_t {
        Unknown,
        MPEGTS, //SPTS or MPTS
        MPSRTTS //Multiplexed MPEG-TS (SPTS / MPTS) to one SRT stream
    };

    enum OutputType: uint8_t {
        UDP,
        FIFO,
        BOTH
    };

    struct Connection {
        std::shared_ptr<kissnet::udp_socket> mNetOut = nullptr;
        std::vector<int> mFifoFds;  // File descriptors for multiple FIFOs
        std::vector<std::string> mFifoPaths;  // Paths to FIFO files
        OutputType mOutputType = OutputType::UDP;
        uint8_t tag = 0;
        std::string mStreamId = "";  // SRT stream ID for routing
        
        // Constructor
        Connection() : mNetOut(nullptr), mFifoFds(), mFifoPaths(), mOutputType(OutputType::UDP), tag(0), mStreamId("") {}
    };

    struct Stats {
        uint64_t mPacketCounter = 0;
        uint64_t mConnections = 0;
    };

    struct Config {
        std::string mListenIp = "";
        uint16_t mListenPort = 0;
        int mReorder = 0;
        std::string mPsk = "";
        std::string mOutIp = "";
        uint16_t mOutPort = 0;
        Mode mMode = Mode::Unknown;
        uint8_t mTag = 0;
        int mLatency = 1000;
        bool mSingleSender = false;
        std::string mStreamId = "";  // SRT stream ID (for stream-based routing)
        OutputType mOutputType = OutputType::UDP;  // UDP, FIFO, or BOTH
        std::vector<std::string> mFifoPaths = {};  // List of FIFO file paths for output
    };

    bool startBridge(Config &rConfig);
    void stopBridge();
    bool addInterface(Config &rConfig);
    bool removeInterface(const std::string &streamId, uint8_t tag);
    bool hasActiveInterfaces() const;
    std::shared_ptr<SRTNet::NetworkConnection> validateConnection(struct sockaddr &sin, SRTSOCKET newSocket, std::shared_ptr<SRTNet::NetworkConnection> &ctx);
    void handleClientDisconnect(std::shared_ptr<SRTNet::NetworkConnection>& ctx, SRTSOCKET socket);
    bool handleDataMPEGTS(std::unique_ptr <std::vector<uint8_t>> &content, SRT_MSGCTRL &msgCtrl, std::shared_ptr<SRTNet::NetworkConnection> ctx, SRTSOCKET clientHandle);
    bool handleDataMPSRTTS(std::unique_ptr <std::vector<uint8_t>> &content, SRT_MSGCTRL &msgCtrl, std::shared_ptr<SRTNet::NetworkConnection> ctx, SRTSOCKET clientHandle);
    Stats getStats();

    Config mCurrentConfig;
    std::atomic<uint64_t> mPacketCounter;

private:
    SRTNet mSRTServer;
    std::vector<Connection> mConnections;
    Mode mCurrentMode;
    std::map<uint8_t, std::vector<std::vector<uint8_t>>> mTSPackets;
    
    // Helper methods for FIFO handling
    bool createAndOpenFifos(const std::vector<std::string> &fifoPaths, std::vector<int> &fifoFds);
    bool writeFifo(int fifoFd, const uint8_t *data, size_t size);
    void closeFifos(std::vector<int> &fifoFds);
    void sendData(const Connection &conn, const uint8_t *data, size_t size);

};

#endif //SRT_TO_UDP_SERVER_NETBRIDGE_H
