#include "NetSession.h"

#include <algorithm>

#include <ixwebsocket/IXNetSystem.h>
#include <ixwebsocket/IXWebSocket.h>
#include <ixwebsocket/IXWebSocketMessage.h>

#include <nlohmann/json.hpp>
#include <spdlog/spdlog.h>

using json = nlohmann::json;

namespace Net {

NetSession& NetSession::Instance() {
    static NetSession instance;
    return instance;
}

void NetSession::SetError(const std::string& err) {
    std::lock_guard<std::mutex> lock(mErrorMutex);
    mLastError = err;
    SPDLOG_ERROR("[NetSession] {}", err);
}

std::string NetSession::GetLastError() {
    std::lock_guard<std::mutex> lock(mErrorMutex);
    return mLastError;
}

std::string NetSession::GetRoomCode() {
    std::lock_guard<std::mutex> lock(mRoomCodeMutex);
    return mRoomCode;
}

// ---------------------------------------------------------------------------
// Host
// ---------------------------------------------------------------------------
// Hosting no longer means binding a local listening socket. The host is just
// another outbound WebSocket client to the relay, same as a guest, which is
// what lets this work from behind NAT/firewalls with zero configuration. The
// relay is what hands out the room code and multiplexes every guest's traffic
// onto this one socket (see NetProtocol.h's "Relay routing" section).

bool NetSession::StartHost(const std::string& relayUrl, std::string& errorOut) {
    if (mRole != Role::None) {
        errorOut = "A session is already active. Disconnect first.";
        return false;
    }

    ix::initNetSystem();

    mRole = Role::Host;
    mPlayerCount = 1;
    mStatus = ConnectionStatus::Connecting;
    {
        std::lock_guard<std::mutex> lock(mClientsMutex);
        mClients = {};
    }

    mSocket = std::make_unique<ix::WebSocket>();
    mSocket->setUrl(relayUrl);
    mSocket->disableAutomaticReconnection();

    mSocket->setOnMessageCallback([this](const ix::WebSocketMessagePtr& msg) {
        if (msg->type == ix::WebSocketMessageType::Open) {
            json req{ { "type", RelayCtl::CreateRoom } };
            mSocket->send(req.dump());
        } else if (msg->type == ix::WebSocketMessageType::Message) {
            if (msg->binary) {
                HandleHostBinary(msg->str);
            } else {
                HandleRelayText(msg->str);
            }
        } else if (msg->type == ix::WebSocketMessageType::Error) {
            SetError("Relay connection error: " + msg->errorInfo.reason);
            mStatus = ConnectionStatus::Failed;
        } else if (msg->type == ix::WebSocketMessageType::Close) {
            mStatus = ConnectionStatus::Disconnected;
        }
    });

    mSocket->start();
    SPDLOG_INFO("[NetSession] Connecting to relay {} to host a room...", relayUrl);
    return true;
}

void NetSession::StopHost() {
    if (mSocket) {
        mSocket->stop();
        mSocket.reset();
    }
    std::lock_guard<std::mutex> lock(mClientsMutex);
    mClients = {};
    mPlayerCount = 1;
    mRole = Role::None;
    mStatus = ConnectionStatus::Idle;
    std::lock_guard<std::mutex> roomLock(mRoomCodeMutex);
    mRoomCode.clear();
}

// Shared by both host and client sockets: JSON control frames from the relay
// (room codes, join/leave notifications, errors) never contain game data.
void NetSession::HandleRelayText(const std::string& text) {
    json msg;
    try {
        msg = json::parse(text);
    } catch (const json::parse_error&) {
        return;
    }
    const std::string type = msg.value("type", "");

    if (type == RelayCtl::RoomCreated && mRole == Role::Host) {
        {
            std::lock_guard<std::mutex> lock(mRoomCodeMutex);
            mRoomCode = msg.value("code", "");
        }
        mStatus = ConnectionStatus::Connected;
        SPDLOG_INFO("[NetSession] Hosting room {}", GetRoomCode());
    } else if (type == RelayCtl::PeerJoined && mRole == Role::Host) {
        const int slot = msg.value("slot", -1);
        if (slot < 1 || slot >= MAX_NET_PLAYERS) {
            return;
        }
        std::lock_guard<std::mutex> lock(mClientsMutex);
        mClients[slot].slot = slot;
        mClients[slot].connected = true;
        mClients[slot].lastInput = InputState{};
        mPlayerCount = static_cast<int>(
            1 + std::count_if(mClients.begin(), mClients.end(), [](const RemoteClient& c) { return c.connected; }));
        SPDLOG_INFO("[NetSession] Guest connected, relay slot {}", slot);
    } else if (type == RelayCtl::PeerLeft && mRole == Role::Host) {
        const int slot = msg.value("slot", -1);
        if (slot < 1 || slot >= MAX_NET_PLAYERS) {
            return;
        }
        {
            std::lock_guard<std::mutex> lock(mClientsMutex);
            mClients[slot] = RemoteClient{};
            mPlayerCount = static_cast<int>(1 + std::count_if(mClients.begin(), mClients.end(),
                                                               [](const RemoteClient& c) { return c.connected; }));
        }
        PlayerLeftMsg left;
        left.slot = static_cast<uint8_t>(slot);
        if (mSocket) {
            mSocket->sendBinary(ToRelayUnicast(RELAY_BROADCAST, ToPayload(left)));
        }
    } else if (type == RelayCtl::Joined && mRole == Role::Client) {
        // Confirms the relay side of the handshake; we still need the host's own
        // Welcome/Reject (sent as a normal game message) before we're truly in.
        SPDLOG_INFO("[NetSession] Relay accepted join, waiting on host...");
        HelloMsg hello;
        std::snprintf(hello.displayName, sizeof(hello.displayName), "Guest");
        hello.characterId = mLocalCharacterId;
        if (mSocket) {
            mSocket->sendBinary(ToPayload(hello));
        }
    } else if (type == RelayCtl::Error) {
        SetError("Relay: " + msg.value("message", std::string("unknown error")));
        mStatus = ConnectionStatus::Failed;
    }
}

void NetSession::HandleHostBinary(const std::string& data) {
    const uint8_t senderSlot = PeekSenderSlot(data);
    const std::string payload = StripSenderSlot(data);
    if (senderSlot < 1 || senderSlot >= MAX_NET_PLAYERS) {
        return;
    }

    switch (PeekType(payload)) {
        case MsgType::Hello: {
            HelloMsg hello;
            if (!FromPayload(payload, hello)) {
                return;
            }
            std::lock_guard<std::mutex> lock(mClientsMutex);
            if (!mClients[senderSlot].connected) {
                // PeerJoined should have arrived first, but don't assume ordering.
                mClients[senderSlot].connected = true;
            }
            mClients[senderSlot].slot = senderSlot;
            mClients[senderSlot].characterId = hello.characterId;

            WelcomeMsg welcome;
            welcome.assignedSlot = static_cast<uint8_t>(senderSlot);
            welcome.totalPlayers = static_cast<uint8_t>(mPlayerCount.load());
            if (mSocket) {
                mSocket->sendBinary(ToRelayUnicast(senderSlot, ToPayload(welcome)));
            }
            SPDLOG_INFO("[NetSession] Guest in slot {} said hello", senderSlot);
            break;
        }
        case MsgType::Input: {
            InputMsg inputMsg;
            if (!FromPayload(payload, inputMsg)) {
                return;
            }
            std::lock_guard<std::mutex> lock(mClientsMutex);
            if (mClients[senderSlot].connected) {
                mClients[senderSlot].lastInput = inputMsg.input;
            }
            break;
        }
        case MsgType::SelectCharacter: {
            SelectCharacterMsg select;
            if (!FromPayload(payload, select)) {
                return;
            }
            std::lock_guard<std::mutex> lock(mClientsMutex);
            if (mClients[senderSlot].connected) {
                mClients[senderSlot].characterId = select.characterId;
                mClients[senderSlot].ready = select.ready;
            }
            break;
        }
        default:
            break;
    }
}

InputState NetSession::GetRemoteInput(int slot) {
    std::lock_guard<std::mutex> lock(mClientsMutex);
    for (auto& client : mClients) {
        if (client.connected && client.slot == slot) {
            return client.lastInput;
        }
    }
    return InputState{};
}

void NetSession::SetLocalSnapshotSlot(int slot, const KartState& state) {
    if (slot < 0 || slot >= MAX_NET_PLAYERS) {
        return;
    }
    mPendingSnapshot.players[slot] = state;
}

void NetSession::BroadcastSnapshot(uint32_t frame) {
    if (mRole != Role::Host || !mSocket) {
        return;
    }
    mPendingSnapshot.frame = frame;
    mPendingSnapshot.playerCount = static_cast<uint8_t>(mPlayerCount.load());
    mSocket->sendBinary(ToRelayUnicast(RELAY_BROADCAST, ToPayload(mPendingSnapshot)));
}

void NetSession::BroadcastStartRace(const std::string& trackResourceName, uint8_t modeSelection, uint8_t ccSelection,
                                     const uint8_t characterForSlot[MAX_NET_PLAYERS]) {
    if (mRole != Role::Host || !mSocket) {
        return;
    }
    StartRaceMsg msg;
    std::snprintf(msg.trackResourceName, sizeof(msg.trackResourceName), "%s", trackResourceName.c_str());
    msg.modeSelection = modeSelection;
    msg.ccSelection = ccSelection;
    for (int i = 0; i < MAX_NET_PLAYERS; ++i) {
        msg.characterForSlot[i] = characterForSlot[i];
    }
    mSocket->sendBinary(ToRelayUnicast(RELAY_BROADCAST, ToPayload(msg)));
    SPDLOG_INFO("[NetSession] Broadcasting StartRace: track={} mode={} cc={}", msg.trackResourceName, modeSelection,
                ccSelection);
}

bool NetSession::GetGuestCharacter(int slot, uint8_t& outCharacterId) {
    if (slot < 0 || slot >= MAX_NET_PLAYERS) {
        return false;
    }
    std::lock_guard<std::mutex> lock(mClientsMutex);
    if (!mClients[slot].connected) {
        return false;
    }
    outCharacterId = mClients[slot].characterId;
    return true;
}

bool NetSession::GetGuestReady(int slot, bool& outReady) {
    if (slot < 0 || slot >= MAX_NET_PLAYERS) {
        return false;
    }
    std::lock_guard<std::mutex> lock(mClientsMutex);
    if (!mClients[slot].connected) {
        return false;
    }
    outReady = mClients[slot].ready;
    return true;
}

// ---------------------------------------------------------------------------
// Client
// ---------------------------------------------------------------------------
// A guest also only makes one outbound connection, straight to the relay. The
// room code is how the relay knows which host's traffic to pair this socket with.

bool NetSession::Connect(const std::string& relayUrl, const std::string& roomCode, std::string& errorOut) {
    if (mRole != Role::None) {
        errorOut = "A session is already active. Disconnect first.";
        return false;
    }

    ix::initNetSystem();

    mRole = Role::Client;
    mStatus = ConnectionStatus::Connecting;
    mLocalSlot = -1;
    mHasSnapshot = false;

    mSocket = std::make_unique<ix::WebSocket>();
    mSocket->setUrl(relayUrl);
    mSocket->disableAutomaticReconnection();

    mSocket->setOnMessageCallback([this, roomCode](const ix::WebSocketMessagePtr& msg) {
        if (msg->type == ix::WebSocketMessageType::Open) {
            json req{ { "type", RelayCtl::JoinRoom }, { "code", roomCode } };
            mSocket->send(req.dump());
        } else if (msg->type == ix::WebSocketMessageType::Message) {
            if (msg->binary) {
                HandleClientBinary(msg->str);
            } else {
                HandleRelayText(msg->str);
            }
        } else if (msg->type == ix::WebSocketMessageType::Error) {
            SetError("Relay connection error: " + msg->errorInfo.reason);
            mStatus = ConnectionStatus::Failed;
        } else if (msg->type == ix::WebSocketMessageType::Close) {
            mStatus = ConnectionStatus::Disconnected;
        }
    });

    mSocket->start();
    SPDLOG_INFO("[NetSession] Connecting to relay {} to join room {}...", relayUrl, roomCode);
    return true;
}

void NetSession::Disconnect() {
    if (mSocket) {
        mSocket->stop();
        mSocket.reset();
    }
    mRole = Role::None;
    mStatus = ConnectionStatus::Idle;
    mLocalSlot = -1;
    mHasSnapshot = false;
}

void NetSession::HandleClientBinary(const std::string& data) {
    switch (PeekType(data)) {
        case MsgType::Welcome: {
            WelcomeMsg welcome;
            if (!FromPayload(data, welcome)) {
                return;
            }
            mLocalSlot = welcome.assignedSlot;
            mPlayerCount = welcome.totalPlayers;
            mStatus = ConnectionStatus::Connected;
            SPDLOG_INFO("[NetSession] Connected, assigned slot {}", mLocalSlot);
            break;
        }
        case MsgType::Reject: {
            RejectMsg reject;
            FromPayload(data, reject);
            SetError(std::string("Host rejected connection: ") + reject.reason);
            mStatus = ConnectionStatus::Failed;
            break;
        }
        case MsgType::Snapshot: {
            SnapshotMsg snapshot;
            if (!FromPayload(data, snapshot)) {
                return;
            }
            std::lock_guard<std::mutex> lock(mSnapshotMutex);
            mLatestSnapshot = snapshot;
            mHasSnapshot = true;
            break;
        }
        case MsgType::PlayerLeft: {
            PlayerLeftMsg left;
            if (!FromPayload(data, left)) {
                return;
            }
            std::lock_guard<std::mutex> lock(mLeftSlotsMutex);
            mLeftSlots.push_back(left.slot);
            break;
        }
        case MsgType::StartRace: {
            StartRaceMsg msg;
            if (!FromPayload(data, msg)) {
                return;
            }
            std::lock_guard<std::mutex> lock(mRaceStartMutex);
            mPendingRaceStart = msg;
            mHasPendingRaceStart = true;
            break;
        }
        default:
            break;
    }
}

bool NetSession::PopPendingRaceStart(StartRaceMsg& out) {
    std::lock_guard<std::mutex> lock(mRaceStartMutex);
    if (!mHasPendingRaceStart) {
        return false;
    }
    out = mPendingRaceStart;
    mHasPendingRaceStart = false;
    return true;
}

void NetSession::SendLocalInput(const InputState& input) {
    if (mRole != Role::Client || !mSocket) {
        return;
    }
    InputMsg msg;
    msg.input = input;
    // Unprefixed - the relay stamps our sender slot on for the host automatically.
    mSocket->sendBinary(ToPayload(msg));
}

void NetSession::SendCharacterSelect(uint8_t characterId, bool ready) {
    if (mRole != Role::Client || !mSocket) {
        return;
    }
    mLocalCharacterId = characterId;
    SelectCharacterMsg msg;
    msg.characterId = characterId;
    msg.ready = ready;
    mSocket->sendBinary(ToPayload(msg));
}

bool NetSession::GetLatestSnapshot(SnapshotMsg& out) {
    std::lock_guard<std::mutex> lock(mSnapshotMutex);
    if (!mHasSnapshot) {
        return false;
    }
    out = mLatestSnapshot;
    return true;
}

bool NetSession::PopPlayerLeftSlot(int& slotOut) {
    std::lock_guard<std::mutex> lock(mLeftSlotsMutex);
    if (mLeftSlots.empty()) {
        return false;
    }
    slotOut = mLeftSlots.back();
    mLeftSlots.pop_back();
    return true;
}

void NetSession::Tick() {
    // IXWebSocket dispatches on its own background thread(s), so there's no polling
    // required here today. This hook exists so the main loop has one stable place to
    // call into once we wire real per-frame input capture / snapshot application.
}

} // namespace Net
