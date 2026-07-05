#ifndef NET_PROTOCOL_H
#define NET_PROTOCOL_H

#include <cstdint>
#include <cstring>

// Binary wire protocol shared between host and client over the WebSocket relay.
// Each ix::WebSocket message == exactly one of these structs (no extra framing needed,
// since IXWebSocket already delivers one complete message per callback).
//
// v1 scope: 2 players (host = slot 0, one guest = slot 1).
// Left room to grow to MAX_NET_PLAYERS (8) + a spectator later without changing the format.

namespace Net {

constexpr uint32_t PROTOCOL_VERSION = 1;
constexpr int MAX_NET_PLAYERS = 8;

enum class MsgType : uint8_t {
    Hello = 1,      // client -> host: join request
    Welcome = 2,    // host -> client: accepted, here's your player slot
    Reject = 3,     // host -> client: join refused (full / version mismatch)
    Input = 4,      // client -> host: this client's controller state for a frame
    Snapshot = 5,   // host -> client: authoritative world state for a frame
    PlayerLeft = 6, // host -> client(s): a player disconnected
    Ping = 7,
    Pong = 8,
};

enum ButtonBits : uint16_t {
    BTN_A = 1 << 0,
    BTN_B = 1 << 1,
    BTN_Z = 1 << 2,
    BTN_START = 1 << 3,
    BTN_L = 1 << 4,
    BTN_R = 1 << 5,
    BTN_CUP = 1 << 6,
    BTN_CDOWN = 1 << 7,
    BTN_CLEFT = 1 << 8,
    BTN_CRIGHT = 1 << 9,
    BTN_DUP = 1 << 10,
    BTN_DDOWN = 1 << 11,
    BTN_DLEFT = 1 << 12,
    BTN_DRIGHT = 1 << 13,
};

#pragma pack(push, 1)

struct MsgHeader {
    MsgType type;
};

struct HelloMsg {
    MsgHeader header{ MsgType::Hello };
    uint32_t protocolVersion = PROTOCOL_VERSION;
    char displayName[32] = {};
};

struct WelcomeMsg {
    MsgHeader header{ MsgType::Welcome };
    uint8_t assignedSlot = 0; // which player index (0-7) this client controls
    uint8_t totalPlayers = 0; // how many player slots are currently active
};

struct RejectMsg {
    MsgHeader header{ MsgType::Reject };
    char reason[64] = {};
};

// Matches the button layout of an N64 controller as read by the port's input code.
struct InputState {
    uint32_t frame = 0;
    uint16_t buttons = 0; // bitmask, see ButtonBits
    int8_t stickX = 0;
    int8_t stickY = 0;
};

struct InputMsg {
    MsgHeader header{ MsgType::Input };
    InputState input;
};

// Per-kart transform/state sent every simulation tick from host to clients.
// Kept intentionally small/lossy (int16 angle, no velocity) - fine to expand
// once we're wiring this into real gameplay and know exactly what needs syncing.
struct KartState {
    float posX = 0.0f, posY = 0.0f, posZ = 0.0f;
    int16_t yaw = 0;
    int16_t itemId = -1;
    uint8_t lap = 0;
    uint8_t place = 0;
    uint8_t animState = 0;
    bool active = false;
};

struct SnapshotMsg {
    MsgHeader header{ MsgType::Snapshot };
    uint32_t frame = 0;
    uint8_t playerCount = 0;
    KartState players[MAX_NET_PLAYERS];
};

struct PlayerLeftMsg {
    MsgHeader header{ MsgType::PlayerLeft };
    uint8_t slot = 0;
};

#pragma pack(pop)

// Small helpers for turning a struct into/from the std::string payload IXWebSocket wants.
template <typename T>
inline std::string ToPayload(const T& msg) {
    return std::string(reinterpret_cast<const char*>(&msg), sizeof(T));
}

template <typename T>
inline bool FromPayload(const std::string& data, T& out) {
    if (data.size() != sizeof(T)) {
        return false;
    }
    std::memcpy(&out, data.data(), sizeof(T));
    return true;
}

inline MsgType PeekType(const std::string& data) {
    if (data.empty()) {
        return static_cast<MsgType>(0);
    }
    return static_cast<MsgType>(data[0]);
}

// ---------------------------------------------------------------------------
// Relay routing (neurorelay.sillyprootsoda.com)
// ---------------------------------------------------------------------------
// The relay is a dumb pass-through WebSocket router keyed by room code - it never
// looks at game message contents, only at these routing bytes/JSON control frames.
//
// Binary game messages (everything above this comment) get ONE extra leading byte
// added/stripped by the relay itself, never by the game code on both ends symmetrically:
//   guest -> relay -> host : relay PREPENDS the sender's slot (1..MAX_NET_PLAYERS-1)
//                            before forwarding to the host. Guests send raw, unprefixed.
//   host -> relay -> guest : host PREPENDS a target-slot byte (0 = broadcast to every
//                            guest in the room, N = unicast to just that guest's slot).
//                            The relay strips this byte before delivering to guest(s),
//                            so guests always receive a clean, unprefixed payload.
constexpr uint8_t RELAY_BROADCAST = 0;

inline std::string ToRelayUnicast(uint8_t targetSlot, const std::string& raw) {
    std::string out;
    out.reserve(raw.size() + 1);
    out.push_back(static_cast<char>(targetSlot));
    out += raw;
    return out;
}

// Host-side receive: split the relay's sender-slot prefix off a guest's message.
inline uint8_t PeekSenderSlot(const std::string& data) {
    return data.empty() ? 0 : static_cast<uint8_t>(data[0]);
}

inline std::string StripSenderSlot(const std::string& data) {
    return data.size() > 1 ? data.substr(1) : std::string();
}

// JSON control message type strings exchanged with the relay (text frames, not binary).
namespace RelayCtl {
constexpr const char* CreateRoom = "create_room";
constexpr const char* JoinRoom = "join_room";
constexpr const char* RoomCreated = "room_created";
constexpr const char* Joined = "joined";
constexpr const char* PeerJoined = "peer_joined";
constexpr const char* PeerLeft = "peer_left";
constexpr const char* Error = "error";
} // namespace RelayCtl

} // namespace Net

#endif // NET_PROTOCOL_H
