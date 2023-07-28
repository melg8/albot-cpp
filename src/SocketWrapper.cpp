#include "albot/SocketWrapper.hpp"
#include "albot/MovementMath.hpp"
#include <regex>
#include <algorithm>

#include <spdlog/sinks/stdout_color_sinks.h>
#include <spdlog/async.h>

template <typename T, typename K>
inline T getOrElse(const nlohmann::json& n, K key, T defaultValue) {
    if (!n.is_object()) {
        return defaultValue;
    }
    if (n.find(key) == n.end()) return defaultValue;
    if (n[key].is_null()) return defaultValue;
    return n[key].template get<T>();
}

SocketWrapper::SocketWrapper(std::string characterId, std::string fullUrl, Bot& player)
    : webSocket(), player(player), characterId(characterId), hasReceivedFirstEntities(false) {
    // In order to faciliate for websocket connection, a special URL needs to be used.
    // By adding this, the connection can be established as a websocket connection.
    // A real socket.io client likely uses this or something similar internally
    this->mLogger = spdlog::stdout_color_mt(player.info.character->name + ":SocketWrapper");
    fullUrl += "/socket.io/?EIO=4&transport=websocket";
    if (fullUrl.find("wss://") == std::string::npos) {
        this->webSocket.setUrl("wss://" + fullUrl);
    } else {
        this->webSocket.setUrl(fullUrl);
    }
    this->webSocket.disableAutomaticReconnection(); // turn off
    this->pingInterval = 4000;
    lastPing = std::chrono::high_resolution_clock::now();

    initializeSystem();
}
SocketWrapper::~SocketWrapper() {
    webSocket.close();
}

void SocketWrapper::sanitizeInput(nlohmann::json& entity) {
    if (entity.find("rip") != entity.end()) {
        if (entity["rip"].is_number()) {
            entity["rip"] = (int(entity["rip"]) == 1);
        }
    }
}

void SocketWrapper::handle_entities(const nlohmann::json& event) {
    const std::string MAP = event["map"].get<std::string>();
    const std::string IN = event["in"].get<std::string>();
    if (event.find("players") != event.end()) {
        nlohmann::json players = event["players"];
        for (auto& player : players) {

            sanitizeInput(player);
            player["in"] = IN;
            player["map"] = MAP;
            player["type"] = "character";
            player["base"] = { {"h", 8}, {"v", 7}, {"vn", 2} };
            auto id = player["id"].get<std::string>();

            if (id == this->player.name) {
                this->player.updateCharacter(player);
            }
            // Avoid data loss by updating the JSON rather than overwriting
            if (this->updatedEntities.find(id) == updatedEntities.end())
                this->updatedEntities[id] = player;
            else {
                this->updatedEntities[id].update(player);
            }
        }
    }

    if (event.find("monsters") != event.end()) {
        nlohmann::json monsters = event["monsters"];

        for (auto& monster : monsters) {
            auto id = monster["id"].get<std::string>();

            sanitizeInput(monster);
            monster["in"] = IN;
            monster["map"] = MAP;
            monster["mtype"] = monster["type"].get<std::string>();
            monster["type"] = "monster";

            if (monster.find("max_hp") == monster.end()) {
                monster["max_hp"] = this->player.info.G->getData()["monsters"][std::string(monster["mtype"])]["hp"];
            }

            if (monster.find("hp") == monster.end()) {
                monster["hp"] = monster["max_hp"];
            }

            if (this->updatedEntities.find(id) == updatedEntities.end())
                this->updatedEntities[id] = monster;
            else {
                this->updatedEntities[id].update(monster);
            }
        }
    }
}
void SocketWrapper::initializeSystem() {
    this->webSocket.setOnMessageCallback(
        [this](const ix::WebSocketMessagePtr& message) { this->messageReceiver(message); });

    // Loading

    this->registerEventCallback("welcome", [this](const nlohmann::json&) {
        this->emit("loaded", { {"success", 1}, {"width",1920},{"height",1080},{"scale",2} });
        this->login(this->player.info);
    });

    this->registerEventCallback("start", [this](const nlohmann::json& event) {
        nlohmann::json entities = event["entities"];
        // The start event contains necessary data to populate the character
        nlohmann::json mut = event;

        mut.erase("entities");
        // Used for, among other things, canMove. This contains the character bounding box
        // h = horizontal, v = vertical, vn = vertical negative
        mut["base"] = { {"h", 8}, {"v", 7}, {"vn", 2} };
        mLogger->info("Started in map {} ", event["map"].get<std::string>());
        std::lock_guard<std::mutex> guard(entityGuard);
        getUpdateEntities().clear();
        handle_entities(entities);
        this->player.updateCharacter(mut);
        this->player.onConnect();
    });
    // Loading + gameplay
    this->registerEventCallback("entities", [this](const nlohmann::json& event) {
        std::lock_guard<std::mutex> guard(entityGuard);
        std::string type = event["type"].get<std::string>();
        if (type == "all") {
            getUpdateEntities().clear();
        }
        handle_entities(event);
    });

    // Gameplay

    // Methods recording the disappearance of entities.
    // Death is also registered locally using this socket event
    this->registerEventCallback("death", [this](const nlohmann::json& event) {
        nlohmann::json copy = event;
        copy["death"] = true;
        onDisappear(copy);
    });

    this->registerEventCallback("disappear", [this](const nlohmann::json& event) {
        onDisappear(event);
    });
    this->registerEventCallback("notthere", [this](const nlohmann::json& event) {
        onDisappear(event);
    });

    // Chests are recorded with the drop event
    this->registerEventCallback("drop", [this](const nlohmann::json& event) {
        std::lock_guard<std::mutex> guard(chestGuard);
        chests.emplace(event["id"].get<std::string>(), event);
    });
    this->registerEventCallback("chest_opened", [this](const nlohmann::json& event) {
        std::lock_guard<std::mutex> guard(chestGuard);
        chests.erase(event["id"].get<std::string>());
    });
    // This contains updates to the player entity. Unlike other entities (AFAIK), these aren't
    // sent using the entities event.
    this->registerEventCallback("player", [this](const nlohmann::json& event) {
        std::lock_guard<std::mutex> guard(entityGuard);
        nlohmann::json copy = event;
        nlohmann::json& playerJson = player.getUpdateCharacter();
        if (copy.contains("moving") && copy["moving"]) {
            if (copy.contains("speed") && playerJson.contains("speed") && double(copy["speed"]) != double(playerJson["speed"])) {
                copy["from_x"] = event["x"];
                copy["from_y"] = event["y"];
                std::pair<double, double> vxy = MovementMath::calculateVelocity(copy);
                copy["vx"] = vxy.first;
                copy["vy"] = vxy.second;
            }
        }
        player.updateCharacter(copy);
    });
    this->registerEventCallback("new_map", [this](const nlohmann::json& event) {
        std::lock_guard<std::mutex> guard(entityGuard);
        getUpdateEntities().clear();
        handle_entities(event["entities"]);
        player.updateCharacter({ {"map", event["name"].get<std::string>()},
                {"x", event["x"].get<int>()},
                {"y", event["y"].get<int>()},
                {"m", event["m"].get<int>()},
                {"moving", false} });
        // this->deleteEntities();
    });
    this->registerEventCallback("cm", [this](const nlohmann::json& event) {
        player.onCm(event["name"].get<std::string>(), event["message"]);
    });
    this->registerEventCallback("pm", [this](const nlohmann::json& event) {
        std::string owner = event["owner"].get<std::string>();
        player.onPm(owner, event["message"].get<std::string>());
    });
    this->registerEventCallback("chat_log", [this](const nlohmann::json& event) {
        player.onChat(event["owner"].get<std::string>(), event["message"].get<std::string>());
    });
    this->registerEventCallback("invite", [this](const nlohmann::json& event) {
        player.onPartyInvite(event["name"].get<std::string>());
    });
    this->registerEventCallback("request", [this](const nlohmann::json& event) {
        player.onPartyRequest(event["name"].get<std::string>());
    });
    /**
     * Position correction.
     */
    this->registerEventCallback("correction", [this](const nlohmann::json& event) {
        std::lock_guard<std::mutex> guard(entityGuard);
        this->mLogger->warn("Location corrected: Client: ({}, {}), Server: ({}, {})", player.getX(), player.getY(), double(event["x"]), double(event["y"]));
        player.updateCharacter(event);
    });
    this->registerEventCallback("party_update", [this](const nlohmann::json& event) {
        player.setParty(event["party"]);
    });

    this->registerEventCallback("game_error", [this](const nlohmann::json& event) {
        this->mLogger->error(event.dump());
        if (event.is_string()) {
            auto evt = event.get<std::string>();
            std::regex rgx(this->waitRegex);
            std::smatch matches;
            if (std::regex_search(evt, matches, rgx)) {
                int secs = std::stoi(matches[1]) + 1;
                this->mLogger->info("Reconnecting in {} seconds", secs);
                std::thread([this](int time) {
                    std::this_thread::sleep_for(std::chrono::seconds(time));
                    this->login(this->player.info);
                }, secs).detach();
            }
        }
    });

    this->registerEventCallback("disconnect", [this](const nlohmann::json& event) {
        this->mLogger->error("Disconnected: {}", event.dump());
    });
    this->registerEventCallback("disconnect_reason", [this](const nlohmann::json& event) {
        this->mLogger->error("Disconnection reason received: {}", event.dump());
    });
}

void SocketWrapper::login(const CharacterGameInfo& info) {
    const std::string& auth = info.auth;
    const std::string& userId = info.userId;

    emit("auth", { {"user", userId},
                  {"character", characterId},
                  {"auth", auth},
                  {"width",1920},{"height",1080},{"scale",2},
                  {"no_html", true},
                  {"no_graphics", true} /*, {"passphrase", ""}*/ });
}

void SocketWrapper::emit(const std::string& event, const nlohmann::json& json) {
    if (this->webSocket.getReadyState() == ix::ReadyState::Open) {
        this->webSocket.send(std::format("42[\"{}\",{}]", event, json.dump()));
    } else {
        this->mLogger->error("{} attempting to call emit on a socket that hasn't opened yet.", this->characterId);
    }
}

void SocketWrapper::emitRawJsonString(std::string event, std::string json) {
    if (this->webSocket.getReadyState() == ix::ReadyState::Open) {
        this->webSocket.send(std::format("42[\"{}\",{}]", event, json));
    } else {
        this->mLogger->error("{} attempting to call emit on a socket that hasn't opened yet.", this->characterId);
    }
}


void SocketWrapper::messageReceiver(const ix::WebSocketMessagePtr& message) {
    // this->mLogger->info("Received: '{}'", message->str);
    if (pingInterval != 0 && message->type != ix::WebSocketMessageType::Close) {
        auto now = std::chrono::high_resolution_clock::now();
        auto diff = std::chrono::duration_cast<std::chrono::milliseconds>(now - lastPing).count();

        // The socket expects a ping every few milliseconds (currently, it's every 4000 milliseconds)
        // This is sent through the websocket on the frame type 0
        if (diff > pingInterval) {
            lastPing = now;
            sendPing();
        }
    }

    // All the Socket.IO events also come through as messages
    if (message->type == ix::WebSocketMessageType::Message) {
        // Uncomment for data samples
        // this->mLogger->info("> {}", message->str);

        const std::string& messageStr = message->str;
        if (messageStr.length() == 0) {
            this->mLogger->info("Received empty websocket message.");
            return;
        }

        if (messageStr.length() == 1) {
            int type = std::stoi(messageStr);
            if (type == 0)
                this->mLogger->warn("Received empty connect message! Assuming 4000 milliseconds for the ping interval");
            else if (type == 3) {
                // pong
            } else if (type == 2) {
                // ping
                this->webSocket.send("3");
            }

            return;
        }

        std::string args;
        int likelyType = messageStr[0] - '0';      // frame type
        int alternativeType = messageStr[1] - '0'; // message type

        if (likelyType < 0 || likelyType > 9) {
            this->mLogger->error("Failed to parse message: \"{}\". Failed to find frame type", messageStr);
            return;
        }
        if (messageStr.length() == 2 && alternativeType >= 0 && alternativeType <= 9) {
            this->mLogger->debug("Skipping code-only input: \"{}\"", messageStr);
            return;
        }

        // To get the message itself, we need to parse the numbers.
        // We know the string is two or more characters at this point.
        // If alternativeType < 0 || alternativeType > 9, that means the character,
        // when '0' is subtracted, isn't a number. For that to be the case, it would
        // have to be in that range.
        // Anyway, first condition hits if we have one digit, the second if there's two
        if (alternativeType < 0 || alternativeType > 9)
            args = messageStr.substr(1, messageStr.length() - 1);
        else
            args = messageStr.substr(2, messageStr.length() - 2);
        switch (likelyType) {
            case 0: {
                    dispatchEvent("connect", {});
                    this->webSocket.send("40");
                    auto data = nlohmann::json::parse(args);
                    pingInterval = data["pingInterval"].get<int>();
                    this->mLogger->info("Received connection data. Pinging required every {} ms", pingInterval);
                    this->mLogger->info("Ping interval changed from {} to {}", this->webSocket.getPingInterval(), pingInterval);
                    this->webSocket.setPingInterval(pingInterval);
                } break;
            case 1:
                this->mLogger->info("Disconnected: {}", message->str);
                dispatchEvent("disconnect", {});
                break;
            case 2:
                dispatchEvent("ping", {});
                break;
            case 3:
                dispatchEvent("pong", {});
                break;
            case 4: {
                    if (rawCallbacks.size() > 0) {
                        for (auto& callback : rawCallbacks) {
                            callback(message);
                        }
                    }
                    int type = -1;
                    if (alternativeType >= 0 && alternativeType <= 9) type = alternativeType;
                    nlohmann::json json = nlohmann::json::parse(args);

                    if (type == -1) {
                        // Dispatch message event.
                        // Note about the socket.io standard: sending a plain text message using socket.send
                        // should send an event called message with the message as the data. The fallback
                        // exists mainly because I have no idea what I'm doing. This might never be used.
                        dispatchEvent("message", json);
                    }
                    // type = 0 for the connect packet
                    // type = 1 for the disconnect packet
                    if(type == 0) {
                        mLogger->info("Socket connected with SID {}", json["sid"].get<std::string>());
                    } else if(type == 1) { 
                        mLogger->info("Disconnected");
                        this->player.onDisconnect("Unknown closure");
                    } else if (type == 2) { // type = 2 for events
                        if (json.type() == nlohmann::json::value_t::array && json.size() >= 1) {
                            auto& event = json[0];
                            if (event.type() == nlohmann::json::value_t::string) {
                                std::string eventName = event.get<std::string>();
                                if (json.size() == 1) {
                                    // dispatch eventName, {}
                                    dispatchEvent(eventName, {});
                                } else {
                                    auto data = json[1];
                                    if (eventName == "error")
                                        this->mLogger->info("Error received as a message! Dumping JSON:\n{}", json.dump(4));
                                    dispatchEvent(eventName, data);
                                }
                            }
                        }
                    }
                    // type = 3 for ack (AFAIK, not used)
                    // type = 4 for errors (no idea how to handle, gotta revisit that when I have data)
                    else if (type == 4) {
                        this->mLogger->error("Error received from server:\n{}", json.dump(4));
                    }
                    // type = 5 for binary event (AFAIK, not used)
                    // type = 6 for binary ack (AFAIK, not used)
                    else if (type == 3 || type == 5 || type == 6) {
                        this->mLogger->warn("Received an event of type {}. Please report this issue here: "
                                      "https://github.com/LunarWatcher/AdventureLandCpp/issues/1 - Full websocket message: {}",
                                      type, messageStr);
                    } else {
                        this->mLogger->warn("Unknown event received: {}", messageStr);
                    }
                } break;
            case 5:
                dispatchEvent("upgrade", {});
                this->mLogger->info("Upgrade received: {}", messageStr);
                // upgrade
                break;
            case 6:
                dispatchEvent("noop", {});
                this->mLogger->info("noop received: {}", messageStr);
                // noop
                break;
        }

    } else if (message->type == ix::WebSocketMessageType::Error) {
        this->mLogger->error(message->errorInfo.reason);
    } else if (message->type == ix::WebSocketMessageType::Open) {
        this->mLogger->info("Connected");
    } else if (message->type == ix::WebSocketMessageType::Close) {
        this->mLogger->info("Socket disconnected: {}", message->closeInfo.reason);
        this->player.onDisconnect(message->closeInfo.reason);
    }
}

void SocketWrapper::receiveLocalCm(std::string from, const nlohmann::json& message) {
    player.onCm(from, message);
}

void SocketWrapper::dispatchEvent(std::string eventName, const nlohmann::json& event) {
    auto it = eventCallbacks.find(eventName);
    if (it != eventCallbacks.end()) {
        for (auto& callback : it->second) {
            callback(event);
        }
    }
}

void SocketWrapper::deleteEntities() {
    // NO-OP. Unneeded.
}

void SocketWrapper::registerRawMessageCallback(std::function<void(const ix::WebSocketMessagePtr&)> callback) {
    rawCallbacks.push_back(callback);
}

void SocketWrapper::registerEventCallback(const std::string& event, std::function<void(const nlohmann::json&)> callback) {
    eventCallbacks[event].push_back(callback);
}

void SocketWrapper::onDisappear(const nlohmann::json& event) {
    std::lock_guard<std::mutex> mtx(this->entityGuard);
    updatedEntities[event["id"].get<std::string>()].update({{"dead", true}});
}

void SocketWrapper::connect() {
    // Begin the connection process
    this->webSocket.start();
}

void SocketWrapper::close() {
    this->webSocket.stop();
}

void SocketWrapper::sendPing() {
}

std::map<std::string, nlohmann::json>& SocketWrapper::getEntities() {
    return entities;
}

std::map<std::string, nlohmann::json>& SocketWrapper::getUpdateEntities() {
    return updatedEntities;
}

nlohmann::json& SocketWrapper::getCharacter() {
    return character;
}

nlohmann::json& SocketWrapper::getUpdateCharacter() {
    return updatedCharacter;
}

std::map<std::string, nlohmann::json>& SocketWrapper::getChests() {
    return chests;
}
