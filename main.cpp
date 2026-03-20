#include <iostream>
#include <signal.h>
#include <atomic>
#include <set>

#include "INI.h"
#include "NetBridge.h"
#include "RESTInterface.hpp"

//Version
#define MAJOR_VERSION 1
#define MINOR_VERSION 0

std::map<std::string, std::shared_ptr<NetBridge>> gSectionBridges;  // section → bridge
std::map<std::pair<std::string, uint16_t>, std::shared_ptr<NetBridge>> gBridgesByEndpoint; // (ip:port) → bridge
std::map<std::string, NetBridge::Config> gSectionConfigs;  // section → config (for removal)
std::string gConfigFilePath;
std::atomic<bool> gReloadConfig(false);

// Helper function to extract display name from section name (remove "config_" or "flow_" prefix)
std::string getDisplayName(const std::string& sectionName) {
    size_t pos = sectionName.find('_');
    if (pos != std::string::npos) {
        return sectionName.substr(pos + 1);
    }
    return sectionName;
}

// Helper function to parse comma-separated FIFO paths
std::vector<std::string> parseFifoPaths(const std::string& fifoPathString) {
    std::vector<std::string> paths;
    if (fifoPathString.empty()) {
        return paths;
    }
    
    size_t start = 0;
    size_t end = fifoPathString.find(',');
    
    while (end != std::string::npos) {
        std::string path = fifoPathString.substr(start, end - start);
        // Trim whitespace
        path.erase(0, path.find_first_not_of(" \t\n\r"));
        path.erase(path.find_last_not_of(" \t\n\r") + 1);
        if (!path.empty()) {
            paths.push_back(path);
        }
        start = end + 1;
        end = fifoPathString.find(',', start);
    }
    
    // Add the last path
    std::string path = fifoPathString.substr(start);
    path.erase(0, path.find_first_not_of(" \t\n\r"));
    path.erase(path.find_last_not_of(" \t\n\r") + 1);
    if (!path.empty()) {
        paths.push_back(path);
    }
    
    return paths;
}

// Helper function to parse and add a config section
bool addConfigSection(INI &rConfigs, const std::string &sectionName) {
    try {
        // Step 1: Check if config (by sectionName) already exists
        if (gSectionBridges.find(sectionName) != gSectionBridges.end()) {
            return true;  // Section already exists, skip it
        }
        
        // Step 2: Parse the config section
        NetBridge::Config lConfig;
        lConfig.mListenPort = std::stoi(rConfigs[sectionName]["listen_port"]);
        lConfig.mListenIp = rConfigs[sectionName]["listen_ip"];
        
        std::string outPortString = rConfigs[sectionName]["out_port"];
        lConfig.mOutPort = !outPortString.empty() ? std::stoi(outPortString) : 0;
        
        lConfig.mOutIp = rConfigs[sectionName]["out_ip"];
        lConfig.mPsk = rConfigs[sectionName]["key"];
        
        std::string reorderString = rConfigs[sectionName]["reorder_distance"];
        lConfig.mReorder = !reorderString.empty() ? std::stoi(reorderString) : 4;
        
        std::string latencyString = rConfigs[sectionName]["latency"];
        lConfig.mLatency = !latencyString.empty() ? std::stoi(latencyString) : 1000;
        
        std::string singleSenderString = rConfigs[sectionName]["single_sender"];
        lConfig.mSingleSender = !singleSenderString.empty() && singleSenderString == "true" ? true : false;
        
        lConfig.mStreamId = rConfigs[sectionName]["stream_id"];
        std::cout << getDisplayName(sectionName) << " stream_id: '" << lConfig.mStreamId << "'" << std::endl;
        
        // Parse output_type (default: UDP)
        std::string outputTypeString = rConfigs[sectionName]["output_type"];
        if (outputTypeString == "fifo") {
            lConfig.mOutputType = NetBridge::OutputType::FIFO;
        } else if (outputTypeString == "both") {
            lConfig.mOutputType = NetBridge::OutputType::BOTH;
        } else {
            lConfig.mOutputType = NetBridge::OutputType::UDP;  // Default to UDP
        }
        
        // Parse comma-separated fifo_paths if output_type includes FIFO
        std::string fifoPathString = rConfigs[sectionName]["fifo_paths"];
        lConfig.mFifoPaths = parseFifoPaths(fifoPathString);
        
        std::string tagString = rConfigs[sectionName]["tag"];
        if (!tagString.empty()) {
            lConfig.mMode = NetBridge::Mode::MPSRTTS;
            lConfig.mTag = std::stoi(tagString);
        } else {
            lConfig.mMode = NetBridge::Mode::MPEGTS;
        }
        
        // Step 3: Find bridge by listen_ip:listen_port
        auto endpointKey = std::make_pair(lConfig.mListenIp, lConfig.mListenPort);
        auto bridgeIt = gBridgesByEndpoint.find(endpointKey);
        std::shared_ptr<NetBridge> targetBridge = nullptr;
        
        if (bridgeIt != gBridgesByEndpoint.end()) {
            targetBridge = bridgeIt->second;
        }
        
        // Step 4: If bridge does not exist, create it. Otherwise, reuse it.
        if (targetBridge == nullptr) {
            // Create a new bridge and start the SRT server
            auto newBridge = std::make_shared<NetBridge>();
            if (!newBridge->startBridge(lConfig)) {
                std::cout << "Failed starting bridge using config: " << getDisplayName(sectionName) << std::endl;
                return false;
            }
            std::cout << "Started new bridge on " << lConfig.mListenIp << ":" << lConfig.mListenPort << std::endl;
            targetBridge = newBridge;
            gBridgesByEndpoint[endpointKey] = targetBridge;
        } else {
            // Bridge exists, add this config as an interface
            targetBridge->addInterface(lConfig);
        }
        
        // Step 5: Store the config mapping for this sectionName
        gSectionBridges[sectionName] = targetBridge;
        gSectionConfigs[sectionName] = lConfig;  // Save config for later removal
        return true;
    } catch (const std::exception &e) {
        std::cout << "Error parsing config section " << getDisplayName(sectionName) << ": " << e.what() << std::endl;
        return false;
    }
}

// Helper function to parse and add a flow section
bool addFlowSection(INI &rConfigs, const std::string &sectionName) {
    try {
        std::string lBindKey = rConfigs[sectionName]["bind_to"];
        if (gSectionBridges.find(lBindKey) == gSectionBridges.end() || lBindKey.empty()) {
            return true;  // Bridge doesn't exist yet, skip this flow
        }
        
        NetBridge::Config lConfig;
        
        std::string outPortString = rConfigs[sectionName]["out_port"];
        lConfig.mOutPort = !outPortString.empty() ? std::stoi(outPortString) : 0;
        
        lConfig.mOutIp = rConfigs[sectionName]["out_ip"];
        lConfig.mStreamId = rConfigs[sectionName]["stream_id"];
        std::cout << getDisplayName(sectionName) << " stream_id: '" << lConfig.mStreamId << "'" << std::endl;
        
        // Parse output_type (default: UDP)
        std::string outputTypeString = rConfigs[sectionName]["output_type"];
        if (outputTypeString == "fifo") {
            lConfig.mOutputType = NetBridge::OutputType::FIFO;
        } else if (outputTypeString == "both") {
            lConfig.mOutputType = NetBridge::OutputType::BOTH;
        } else {
            lConfig.mOutputType = NetBridge::OutputType::UDP;  // Default to UDP
        }
        
        // Parse comma-separated fifo_paths if output_type includes FIFO
        std::string fifoPathString = rConfigs[sectionName]["fifo_paths"];
        lConfig.mFifoPaths = parseFifoPaths(fifoPathString);
        
        std::string tagString = rConfigs[sectionName]["tag"];
        if (!tagString.empty()) {
            lConfig.mMode = NetBridge::Mode::MPSRTTS;
            lConfig.mTag = std::stoi(tagString);
        } else {
            std::cout << "Tag missing: " << getDisplayName(sectionName) << std::endl;
            return false;
        }
        gSectionBridges[lBindKey]->addInterface(lConfig);
        gSectionConfigs[sectionName] = lConfig;  // Save config for later removal
        return true;
    } catch (const std::exception &e) {
        std::cout << "Error parsing flow section " << getDisplayName(sectionName) << ": " << e.what() << std::endl;
        return false;
    }
}

bool startSystem(INI &rConfigs) {
    bool success = true;
    for (auto &rSection: rConfigs.sections) {
        std::string sectionName = rSection.first;
        if (sectionName.find("config") != std::string::npos) {
            if (!addConfigSection(rConfigs, sectionName)) {
                success = false;
            }
        } else if (sectionName.find("flow") != std::string::npos) {
            if (!addFlowSection(rConfigs, sectionName)) {
                success = false;
            }
        }
    }
    return success;
}


// Signal handler for SIGHUP to reload configuration
void signalHandler(int signal) {
    if (signal == SIGHUP) {
        std::cout << "Received SIGHUP, will reload configuration at next check" << std::endl;
        gReloadConfig = true;
    }
}

// Reload configuration from file and add new interfaces/bridges
bool reloadConfigFile() {
    std::cout << "Reloading configuration from: " << gConfigFilePath << std::endl;
    
    INI ini(gConfigFilePath, true, INI::PARSE_COMMENTS_ALL | INI::PARSE_COMMENTS_SLASH | INI::PARSE_COMMENTS_HASH);
    
    if (ini.sections.size() < 2) {
        std::cout << "Failed parsing configuration file during reload." << std::endl;
        return false;
    }
    
    bool reloadSuccess = true;
    int newConfigsAdded = 0;
    int newInterfacesAdded = 0;
    int sectionsRemoved = 0;
    
    // Track which sections are in the new config
    std::set<std::string> newSections;
    for (auto &rSection: ini.sections) {
        std::string sectionName = rSection.first;
        if (sectionName.find("config") != std::string::npos || sectionName.find("flow") != std::string::npos) {
            newSections.insert(sectionName);
        }
    }
    
    // Remove sections that are no longer in the config
    std::vector<std::string> sectionsToRemove;
    for (auto &rSection: gSectionBridges) {
        if (newSections.find(rSection.first) == newSections.end()) {
            sectionsToRemove.push_back(rSection.first);
        }
    }
    
    for (const auto &sectionName: sectionsToRemove) {
        auto bridgePtr = gSectionBridges[sectionName];
        
        // Extract stream_id and tag from saved config (not from the new ini which doesn't have the removed section)
        std::string removedStreamId;
        uint8_t removedTag = 0;
        if (gSectionConfigs.find(sectionName) != gSectionConfigs.end()) {
            removedStreamId = gSectionConfigs[sectionName].mStreamId;
            removedTag = gSectionConfigs[sectionName].mTag;
            gSectionConfigs.erase(sectionName);  // Clean up the saved config
        }
        
        gSectionBridges.erase(sectionName);
        
        // Check if this bridge is still referenced by other sections
        bool bridgeStillInUse = false;
        for (auto &rSection: gSectionBridges) {
            if (rSection.second == bridgePtr) {
                bridgeStillInUse = true;
                break;
            }
        }
        
        if (!bridgeStillInUse) {
            // Find and remove this bridge from gBridgesByEndpoint
            for (auto it = gBridgesByEndpoint.begin(); it != gBridgesByEndpoint.end(); ++it) {
                if (it->second == bridgePtr) {
                    std::cout << "Stopping bridge on " << it->first.first << ":" << it->first.second << std::endl;
                    bridgePtr->stopBridge();
                    gBridgesByEndpoint.erase(it);
                    break;
                }
            }
        } else {
            // Bridge is still in use by other sections, just remove this interface
            if (bridgePtr->removeInterface(removedStreamId, removedTag)) {
                if (!removedStreamId.empty()) {
                    std::cout << "Removed interface with stream_id: " << removedStreamId << std::endl;
                } else {
                    std::cout << "Removed interface with tag: " << static_cast<int>(removedTag) << std::endl;
                }
            }
        }
        
        std::cout << "Removed section: " << getDisplayName(sectionName) << std::endl;
        sectionsRemoved++;
    }
    
    // Add new sections from config
    for (auto &rSection: ini.sections) {
        std::string sectionName = rSection.first;
        if (sectionName.find("config") != std::string::npos) {
            // Skip if this config section already exists
            bool alreadyExists = false;
            for (auto &rBridge: gSectionBridges) {
                if (rBridge.first == sectionName) {
                    alreadyExists = true;
                    break;
                }
            }
            
            if (!alreadyExists) {
                if (addConfigSection(ini, sectionName)) {
                    newConfigsAdded++;
                } else {
                    reloadSuccess = false;
                }
            }
        } else if (sectionName.find("flow") != std::string::npos) {
            if (addFlowSection(ini, sectionName)) {
                newInterfacesAdded++;
            } else {
                reloadSuccess = false;
            }
        }
    }
    
    if (reloadSuccess) {
        std::cout << "Configuration reload complete. Added " << newConfigsAdded << " new bridges, " 
                  << newInterfacesAdded << " new interfaces, and removed " << sectionsRemoved << " sections." << std::endl;
    }
    
    return reloadSuccess;
}

void printUsage() {
    std::cout << "Usage:" << std::endl << std::endl;
    std::cout << "(Executable) configuration.ini" << std::endl;
}

json getStats(std::string cmdString) {
    json j;
    if (cmdString == "dumpall") {
        uint64_t lConnectionCounter = 1;
        for (auto &rBridge: gBridgesByEndpoint) {
            NetBridge::Stats lStats = rBridge.second->getStats();
            std::ostringstream lHandle;
            lHandle << "connection" << unsigned(lConnectionCounter);
            j[lHandle.str().c_str()]["pkt_cnt"] = lStats.mPacketCounter;
            j[lHandle.str().c_str()]["clnt_cnt"] = lStats.mConnections;
            j[lHandle.str().c_str()]["net_ip"] = rBridge.first.first;
            j[lHandle.str().c_str()]["net_port"] = rBridge.first.second;
            lConnectionCounter++;
        }
    }
    return j;
}

int main(int argc, char *argv[]) {
    std::cout << "SRT -> UDP Bridge V." << unsigned(MAJOR_VERSION) << "." <<  unsigned(MINOR_VERSION) << std::endl;

    if (argc != 2) {
        printUsage();
        return EXIT_FAILURE;
    }

    std::string lCommand = argv[1];
    if (!lCommand.compare("--help") || !lCommand.compare("-help")) {
        printUsage();
        return EXIT_SUCCESS;
    }

    // Store config file path globally for reload
    gConfigFilePath = lCommand;

    std::cout << "Parsing configuration in file: " << lCommand << std::endl;
    INI ini(lCommand, true, INI::PARSE_COMMENTS_ALL | INI::PARSE_COMMENTS_SLASH | INI::PARSE_COMMENTS_HASH);

    if (ini.sections.size() < 2) {
        std::cout << "Failed parsing configuration." << lCommand << std::endl;
        return EXIT_FAILURE;
    }

    if (!startSystem(ini)) {
        std::cout << "Failed parsing configuration." << lCommand << std::endl;
        return EXIT_FAILURE;
    }

    std::string lRestIP = ini["restif"]["rest_ip"];
    int lRestPort = std::stoi(ini["restif"]["rest_port"]);
    std::string lRestSecret = ini["restif"]["rest_secret"];

    RESTInterface lRESTInterface;
    lRESTInterface.getStatsCallback=std::bind(&getStats, std::placeholders::_1);
    if (!lRESTInterface.startServer(lRestIP.c_str(), lRestPort, "/restapi/version1", lRestSecret)) {
        std::cout << "REST interface did not start." << std::endl;
        return EXIT_FAILURE;
    }

    // Register SIGHUP signal handler for config reload
    signal(SIGHUP, signalHandler);
    std::cout << "SIGHUP handler registered. Use 'systemctl reload srt-to-udp-server' to reload config." << std::endl;

    bool running = true;
    uint64_t loopCounter = 0;
    
    while (running) {
        // Check if config reload was requested (every 10s)
        if (loopCounter % 10 == 0) {
            if (gReloadConfig) {
                gReloadConfig = false;
                if (!reloadConfigFile()) {
                    std::cout << "Warning: Configuration reload had errors, but continuing with current config." << std::endl;
                }
            }
        }
        
        std::this_thread::sleep_for(std::chrono::seconds(1));
        loopCounter++;
        
        // Print stats every 60s
        if (loopCounter % 60 == 0) {
            uint64_t lConnectionCounter = 1;
            std::cout << std::endl;
            for (auto &rBridge: gBridgesByEndpoint) {
                NetBridge::Stats lStats=rBridge.second->getStats();
                std::cout << "Connection " << unsigned(lConnectionCounter);
                std::cout << " Port: " << unsigned(rBridge.first.second);
                std::cout << " Clients: " << unsigned(lStats.mConnections);
                std::cout << " packetCounter: " << unsigned(lStats.mPacketCounter) << std::endl;
                lConnectionCounter ++;
            }
        }
    }

    return 0;
}