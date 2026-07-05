#ifndef NET_SESSION_H
#define NET_SESSION_H

#include <array>
#include <atomic>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "NetProtocol.h"

namespace ix {
class WebSocket;
struct WebSocketMessage;
using WebSocketMessagePtr = std::unique_ptr<WebSocketMessage>;
} // namespace ix

namespace Net {

enum class Role { None, Host, Client };
// Hosting is itself a two-phase connection to the relay: Connecting (socket opening),
// then Connected as soon as the relay hands back a room code - guests joining after
// that point is a gameplay-layer concern (GetConnectedPlayerCount()), not a status one.
enum class ConnectionStatus { Idle, Connecting, Connected, Failed, Disconnected };

// Host-authoritative online play session, relayed through a always-on WebSocket
// relay server (see docs/relay-protocol.md) rather than direct host<->guest
// connections. Both host and every guest only ever make ONE outbound connection
// each, to the same relay - no port forwarding, no NAT traversal, no tunnels.
//
// Host: runs the real simulation locally (same as today's local multiplayer),
//       receives InputState from each connected guest and substitutes it for
//       that player's slot instead of reading a physical controller, then
//       broadcasts a SnapshotMsg of every kart's state once per tick.
// Client: sends its own InputState upstream every tick, and renders using the
//       most recent SnapshotMsg it received from the host instead of running
//       its own authoritative physics for other players.
//
// This class only handles the transport + session bookkeeping. Wiring
// GetRemoteInput()/SetLocalSnapshotSlot()/GetLatestSnapshot() into the actual
// per-frame controller read & kart tick is a separate, later step.
class NetSession {
  public:
    static NetSession& Instance();

    // ---- Host API ----
    // Connects out to the relay and requests a new room. Non-blocking; poll
    // GetStatus()/GetRoomCode()/GetLastError() after calling this.
    bool StartHost(const std::string& relayUrl, std::string& errorOut);
    void StopHost();

    // ---- Client API ----
    // Set before calling Connect() - sent as part of Hello so the host knows which
    // character this guest wants to play as. Safe to leave at default (0).
    void SetLocalCharacter(uint8_t characterId) {
        mLocalCharacterId = characterId;
    }
    // Connects out to the relay and asks to join an existing room by its code.
    bool Connect(const std::string& relayUrl, const std::string& roomCode, std::string& errorOut);
    void Disconnect();

    // Call once per game frame from the main loop.
    void Tick();

    Role GetRole() const {
        return mRole;
    }
    ConnectionStatus GetStatus() const {
        return mStatus.load();
    }
    int GetLocalSlot() const {
        return mLocalSlot;
    }
    int GetConnectedPlayerCount() const {
        return mPlayerCount.load();
    }
    // Valid once StartHost() reaches ConnectionStatus::Connected. Show this to the
    // host so they can share it - it's what guests type into "Join Game".
    std::string GetRoomCode();
    std::string GetLastError();

    // ---- Host-side gameplay hooks (to be called from the sim once wired up) ----
    // Returns the latest received input for a remote-controlled slot (slot 0 is always
    // the host's own local controller and is never looked up here).
    InputState GetRemoteInput(int slot);
    // Host fills in this frame's authoritative kart state for a slot, then calls
    // BroadcastSnapshot() once per tick after all slots have been set.
    void SetLocalSnapshotSlot(int slot, const KartState& state);
    void BroadcastSnapshot(uint32_t frame);
    // Call once, when the host actually starts the race (e.g. "Start Race" button).
    // Broadcasts track/mode/characters to every connected guest so they can jump
    // straight into the same race instead of navigating their own local menus.
    void BroadcastStartRace(const std::string& trackResourceName, uint8_t modeSelection, uint8_t ccSelection,
                             const uint8_t characterForSlot[MAX_NET_PLAYERS]);
    // Character a connected guest asked to play as (from their Hello). Returns
    // false if that slot isn't connected.
    bool GetGuestCharacter(int slot, uint8_t& outCharacterId);
    // Ready state a connected guest last reported via SelectCharacter. Returns
    // false (and leaves outReady untouched) if that slot isn't connected.
    bool GetGuestReady(int slot, bool& outReady);

    // ---- Client-side gameplay hooks ----
    // Called once per tick with this client's local controller reading.
    void SendLocalInput(const InputState& input);
    // Sendable any time after connecting - updates this guest's character pick
    // and/or ready state on the host's side. Also updates SetLocalCharacter()'s
    // stored value so a later Hello (e.g. after a reconnect) stays consistent.
    void SendCharacterSelect(uint8_t characterId, bool ready);
    // Returns true and fills `out` if a snapshot has arrived since the last call.
    bool GetLatestSnapshot(SnapshotMsg& out);
    // Returns true exactly once per received StartRace message (fills `out` and
    // clears the pending flag). Poll this once a frame from the main loop.
    bool PopPendingRaceStart(StartRaceMsg& out);
    // Pops one pending PlayerLeft notification (see HandleClientBinary's
    // MsgType::PlayerLeft case). Returns false and leaves slotOut untouched
    // once the queue is empty. Call in a loop to drain multiple in one frame.
    bool PopPlayerLeftSlot(int& slotOut);

  private:
    NetSession() = default;
    NetSession(const NetSession&) = delete;

    void HandleRelayText(const std::string& text);
    void HandleHostBinary(const std::string& data);
    void HandleClientBinary(const std::string& data);
    void SetError(const std::string& err);

    Role mRole = Role::None;
    std::atomic<ConnectionStatus> mStatus{ ConnectionStatus::Idle };
    std::mutex mErrorMutex;
    std::string mLastError;

    // Single outbound connection to the relay, used for BOTH host and client roles -
    // there is no listening socket anywhere in this process anymore.
    std::unique_ptr<ix::WebSocket> mSocket;

    // ---- Host state ----
    struct RemoteClient {
        int slot = -1; // the relay's assigned slot for this guest; also our player slot
        bool connected = false;
        InputState lastInput;
        uint8_t characterId = 0; // from this guest's Hello, or a later SelectCharacter
        bool ready = false;      // from this guest's last SelectCharacter
    };
    std::mutex mClientsMutex;
    std::array<RemoteClient, MAX_NET_PLAYERS> mClients;
    std::atomic<int> mPlayerCount{ 1 }; // host is always slot 0
    SnapshotMsg mPendingSnapshot;
    std::mutex mRoomCodeMutex;
    std::string mRoomCode;

    // ---- Client state ----
    uint8_t mLocalCharacterId = 0;
    int mLocalSlot = -1;
    std::mutex mSnapshotMutex;
    SnapshotMsg mLatestSnapshot;
    bool mHasSnapshot = false;
    std::mutex mLeftSlotsMutex;
    std::vector<int> mLeftSlots;
    std::mutex mRaceStartMutex;
    StartRaceMsg mPendingRaceStart;
    bool mHasPendingRaceStart = false;
};

} // namespace Net

#endif // NET_SESSION_H
