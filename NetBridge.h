//
// Created by Anders Cedronius on 2020-07-07.
//

#ifndef SRT_TO_UDP_SERVER_NETBRIDGE_H
#define SRT_TO_UDP_SERVER_NETBRIDGE_H

#include "SRTNet.h"
#include "kissnet.hpp"
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

    struct Connection {
        std::shared_ptr<kissnet::udp_socket> mNetOut = nullptr;
        uint8_t tag = 0;
        std::string mStreamId = "";  // SRT stream ID for routing
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
    };

    bool startBridge(Config &rConfig);
    void stopBridge();
    bool addInterface(Config &rConfig);
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

};

#endif //SRT_TO_UDP_SERVER_NETBRIDGE_H
