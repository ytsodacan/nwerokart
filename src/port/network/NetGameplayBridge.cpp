#include "NetGameplayBridge.h"

#include <algorithm>
#include <cstdio>
#include <cstdint>

#include "NetProtocol.h"
#include "NetSession.h"

#include "defines.h"
#include "main.h"
#include "menus.h"

using Net::NetSession;

// Declared in engine/TrackBrowser.h (a C++ header with heavier engine deps
// than this translation layer wants to pull in) - forward-declared here
// instead, since these are plain extern "C" functions.
extern "C" {
const char* TrackBrowser_GetTrackResourceName(void);
void TrackBrowser_SetTrack(const char* name);
size_t TrackBrowser_GetTrackIndex(void);
void NetGameplay_CopyTextToClipboard(const char* text);
// Declared in code_800029B0.h - same reasoning as TrackBrowser above, avoid
// pulling in that header's heavier deps just for this one variable.
extern s16 gCurrentCourseId;
// Rank (1..NUM_PLAYERS) -> player index reverse lookup, normally kept in
// sync by update_race_position_data() (race_logic.c) every tick for every
// player using THIS machine's own waypoint/lap-progress tracking. That's
// wrong for a network-driven kart (only position/yaw ever get synced, not
// path-index/lap-completion-percent), so on a client we re-derive it from
// the host's authoritative KartState::place instead - see
// NetGameplay_PerFrameSimSync(). Declared extern (no bound) rather than
// pulled from race_logic.h to avoid that header's heavier deps.
extern s16 gPlayerPositionLUT[];
}

// Set true for exactly one race entry whenever the client applies a StartRace
// message from the host; consumed (reset false) by
// NetGameplay_ConsumeRaceAuthorization() once that RACING transition actually
// takes effect. While false, NetGameplay_ShouldBlockRaceTransition() blocks a
// guest's own local menus from starting an unrelated race.
static bool sClientRaceAuthorizedByHost = false;
static constexpr const char* kDefaultRelayUrl = "wss://neurorelay.sillyprootsoda.com";

namespace {

// Translate a Net::InputState (our wire format) into this port's local
// struct Controller, matching what update_controller() would have produced
// from a physical pad this same frame - buttonPressed/Depressed and
// stickPressed/Depressed are edge-detected the same way, against whatever
// was already sitting in *out from last frame.
void ApplyInputStateToController(const Net::InputState& in, struct Controller* out) {
    struct Controller previous = *out;

    out->rawStickX = in.stickX;
    out->rawStickY = in.stickY;
    out->rightRawStickX = 0;
    out->rightRawStickY = 0;

    u16 button = 0;
    if (in.buttons & Net::NET_BTN_A) button |= A_BUTTON;
    if (in.buttons & Net::NET_BTN_B) button |= B_BUTTON;
    if (in.buttons & Net::NET_BTN_Z) button |= Z_TRIG;
    if (in.buttons & Net::NET_BTN_START) button |= START_BUTTON;
    if (in.buttons & Net::NET_BTN_L) button |= L_TRIG;
    if (in.buttons & Net::NET_BTN_R) button |= R_TRIG;
    if (in.buttons & Net::NET_BTN_CUP) button |= U_CBUTTONS;
    if (in.buttons & Net::NET_BTN_CDOWN) button |= D_CBUTTONS;
    if (in.buttons & Net::NET_BTN_CLEFT) button |= L_CBUTTONS;
    if (in.buttons & Net::NET_BTN_CRIGHT) button |= R_CBUTTONS;
    if (in.buttons & Net::NET_BTN_DUP) button |= U_JPAD;
    if (in.buttons & Net::NET_BTN_DDOWN) button |= D_JPAD;
    if (in.buttons & Net::NET_BTN_DLEFT) button |= L_JPAD;
    if (in.buttons & Net::NET_BTN_DRIGHT) button |= R_JPAD;

    out->buttonPressed = button & (button ^ previous.button);
    out->buttonDepressed = previous.button & (button ^ previous.button);
    out->button = button;

    u16 stick = 0;
    if (out->rawStickX < -50) stick |= L_JPAD;
    if (out->rawStickX > 50) stick |= R_JPAD;
    if (out->rawStickY < -50) stick |= D_JPAD;
    if (out->rawStickY > 50) stick |= U_JPAD;
    out->stickPressed = stick & (stick ^ previous.stickDirection);
    out->stickDepressed = previous.stickDirection & (stick ^ previous.stickDirection);
    out->stickDirection = stick;
}

// Translate this port's local struct Controller into our wire format, for
// the client's own outgoing input each frame.
Net::InputState ControllerToInputState(const struct Controller* in, uint32_t frame) {
    Net::InputState out;
    out.frame = frame;
    out.stickX = static_cast<int8_t>(std::clamp<int>(in->rawStickX, -128, 127));
    out.stickY = static_cast<int8_t>(std::clamp<int>(in->rawStickY, -128, 127));

    uint16_t buttons = 0;
    if (in->button & A_BUTTON) buttons |= Net::NET_BTN_A;
    if (in->button & B_BUTTON) buttons |= Net::NET_BTN_B;
    if (in->button & Z_TRIG) buttons |= Net::NET_BTN_Z;
    if (in->button & START_BUTTON) buttons |= Net::NET_BTN_START;
    if (in->button & L_TRIG) buttons |= Net::NET_BTN_L;
    if (in->button & R_TRIG) buttons |= Net::NET_BTN_R;
    if (in->button & U_CBUTTONS) buttons |= Net::NET_BTN_CUP;
    if (in->button & D_CBUTTONS) buttons |= Net::NET_BTN_CDOWN;
    if (in->button & L_CBUTTONS) buttons |= Net::NET_BTN_CLEFT;
    if (in->button & R_CBUTTONS) buttons |= Net::NET_BTN_CRIGHT;
    if (in->button & U_JPAD) buttons |= Net::NET_BTN_DUP;
    if (in->button & D_JPAD) buttons |= Net::NET_BTN_DDOWN;
    if (in->button & L_JPAD) buttons |= Net::NET_BTN_DLEFT;
    if (in->button & R_JPAD) buttons |= Net::NET_BTN_DRIGHT;
    out.buttons = buttons;
    return out;
}

// Kept intentionally lossy to match KartState's wire footprint (see
// NetProtocol.h) - animGroupSelector alone (screen 0's copy) is enough to
// pick the right pose bucket for a remote kart; per-screen frame-level
// interpolation is a v1.1 nicety, same as snapshot interpolation.
Net::KartState BuildKartStateForPlayer(const Player* player) {
    Net::KartState state;
    state.active = (player->type & PLAYER_EXISTS) != 0;
    state.posX = player->pos[0];
    state.posY = player->pos[1];
    state.posZ = player->pos[2];
    state.yaw = static_cast<int16_t>(player->rotation[1]);
    state.itemId = player->currentItemCopy;
    state.lap = static_cast<uint8_t>(std::clamp<int>(player->lapCount, 0, 255));
    state.place = static_cast<uint8_t>(std::clamp<int>(player->currentRank, 0, 255));
    state.animState = static_cast<uint8_t>(player->animGroupSelector[0] & 0xFF);
    return state;
}

void ApplyKartStateToPlayer(const Net::KartState& state, Player* player) {
    if (!state.active) {
        // Host says this slot isn't in the race (disconnected/never joined) -
        // make sure the existing "is this player in the race" checks skip it.
        player->type &= ~PLAYER_EXISTS;
        return;
    }
    player->oldPos[0] = player->pos[0];
    player->oldPos[1] = player->pos[1];
    player->oldPos[2] = player->pos[2];
    player->pos[0] = state.posX;
    player->pos[1] = state.posY;
    player->pos[2] = state.posZ;
    player->rotation[1] = state.yaw;
    player->currentItemCopy = state.itemId;
    player->lapCount = state.lap;
    player->currentRank = state.place;
    player->animGroupSelector[0] = state.animState;
    player->animGroupSelector[1] = state.animState;
    player->animGroupSelector[2] = state.animState;
    player->animGroupSelector[3] = state.animState;
}

Player* PlayerForSlot(int slot) {
    if (slot < 0 || slot >= NUM_PLAYERS) {
        return nullptr;
    }
    return &gPlayers[slot];
}

// Shared by NetGameplay_HostStartRace() (explicit button) and
// NetGameplay_HostBroadcastRaceStartIfHosting() (automatic, any race-entry
// path) - gathers the host's own + every connected guest's character, plus
// the current track/mode/CC, and broadcasts it as a StartRaceMsg. No-op if
// this session isn't the host.
void BroadcastCurrentRaceSetupIfHosting() {
    NetSession& session = NetSession::Instance();
    if (session.GetRole() != Net::Role::Host) {
        return;
    }

    uint8_t characterForSlot[Net::MAX_NET_PLAYERS] = {};
    characterForSlot[0] = static_cast<uint8_t>(gCharacterSelections[0]);
    for (int slot = 1; slot < Net::MAX_NET_PLAYERS; ++slot) {
        uint8_t guestCharacter = 0;
        if (session.GetGuestCharacter(slot, guestCharacter)) {
            characterForSlot[slot] = guestCharacter;
        }
    }

    const char* trackResourceName = TrackBrowser_GetTrackResourceName();
    session.BroadcastStartRace(trackResourceName != nullptr ? trackResourceName : "",
                                static_cast<uint8_t>(gModeSelection), static_cast<uint8_t>(gCCSelection),
                                characterForSlot);
}

struct Controller* ControllerForSlot(int slot) {
    // gControllers is sized NUM_PLAYERS, but this port's per-frame physical
    // read (read_controllers()) and per-player input consumption
    // (handle_a_press_for_all_players_during_race()) only ever address
    // slots 0-3 (the local 4-player splitscreen ceiling) - slots 4-7 are
    // never read as distinct physical/network input in this codebase, only
    // OR'd together into gControllerFive as a "any controller" convenience.
    // MAX_NET_PLAYERS is 8 for future growth, but v1 (and this local
    // splitscreen ceiling) caps meaningful substitution at slot 3.
    if (slot < 0 || slot >= NUM_PLAYERS || slot > 3) {
        return nullptr;
    }
    return &gControllers[slot];
}

} // namespace

extern "C" void NetGameplay_PerFrameInput(void) {
    NetSession& session = NetSession::Instance();
    session.Tick();

    switch (session.GetRole()) {
        case Net::Role::Host: {
            // Slot 0 is always the host's own physical controller
            // (gControllers[0], already populated by read_controllers() this
            // frame) - only substitute connected guest slots.
            for (int slot = 1; slot < Net::MAX_NET_PLAYERS; ++slot) {
                struct Controller* controller = ControllerForSlot(slot);
                if (controller == nullptr) {
                    break; // beyond this port's local splitscreen controller slots
                }
                Net::InputState input = session.GetRemoteInput(slot);
                ApplyInputStateToController(input, controller);
            }
            break;
        }
        case Net::Role::Client: {
            // read_controllers() always lands this machine's own physical pad in
            // gControllers[0] (physical port 0), and the core sim always drives
            // gPlayers[i] from gControllers[i]. gPlayers[0] ("Player One") is the
            // HOST's kart from a guest's point of view - left alone, our own
            // presses would locally jog the host's kart (fighting the snapshot
            // overwrite in NetGameplay_PerFrameSimSync every frame) while our
            // actual kart (gControllers[localSlot]) never receives any input at
            // all, since PerFrameSimSync deliberately skips network-syncing our
            // own slot. Route the real input to where it belongs on both ends.
            struct Controller* physical = ControllerForSlot(0);
            if (physical != nullptr) {
                // Always send the true physical reading upstream, unmodified.
                session.SendLocalInput(ControllerToInputState(physical, static_cast<uint32_t>(gGlobalTimer)));

                // The copy-to-localSlot-then-neutralize-slot0 dance below only
                // makes sense once we're actually racing and gControllers[0]
                // represents someone else's kart. Before that - sitting in the
                // online lobby, character select, any menu - gControllers[0] IS
                // this guest's own real input, and the menu code
                // (player_select_menu_act, main_menu_act, etc.) reads it
                // directly every frame. Zeroing it unconditionally here was the
                // actual cause of "guest can't change their character": the
                // moment Connect() succeeds and localSlot becomes > 0, every
                // button press was wiped before update_menus() ever saw it -
                // including in the lobby, long before any race started.
                const int localSlot = session.GetLocalSlot();
                if (gGamestate == RACING && localSlot > 0) {
                    struct Controller* ours = ControllerForSlot(localSlot);
                    if (ours != nullptr) {
                        // Drive OUR kart locally with our own input.
                        *ours = *physical;
                    }
                    // Neutralize slot 0 so our presses don't also locally jog
                    // whatever kart lives there (usually the host) - it's fully
                    // overwritten by the incoming snapshot at end of frame
                    // regardless, this just avoids the visible fight before that.
                    struct Controller cleared{};
                    *physical = cleared;
                }
            }
            break;
        }
        default:
            break;
    }
}

extern "C" void NetGameplay_PerFrameSimSync(uint32_t frameNumber) {
    NetSession& session = NetSession::Instance();

    switch (session.GetRole()) {
        case Net::Role::Host: {
            for (int slot = 0; slot < NUM_PLAYERS && slot < Net::MAX_NET_PLAYERS; ++slot) {
                Player* player = PlayerForSlot(slot);
                if (player == nullptr) {
                    continue;
                }
                session.SetLocalSnapshotSlot(slot, BuildKartStateForPlayer(player));
            }
            session.BroadcastSnapshot(frameNumber);
            break;
        }
        case Net::Role::Client: {
            Net::SnapshotMsg snapshot;
            if (session.GetLatestSnapshot(snapshot)) {
                const int localSlot = session.GetLocalSlot();
                for (int slot = 0; slot < snapshot.playerCount && slot < Net::MAX_NET_PLAYERS; ++slot) {
                    if (slot == localSlot) {
                        continue; // client always simulates + renders its own kart locally
                    }
                    Player* player = PlayerForSlot(slot);
                    if (player != nullptr) {
                        ApplyKartStateToPlayer(snapshot.players[slot], player);
                        // Keep the reverse rank->slot LUT consistent with the
                        // rank we just applied - update_race_position_data()
                        // (race_logic.c) already wrote a stale/wrong entry for
                        // this slot earlier this same frame using unreliable
                        // local waypoint data; the host's snapshot is authoritative.
                        const int rank = static_cast<int>(snapshot.players[slot].place);
                        if (rank >= 0 && rank < NUM_PLAYERS) {
                            gPlayerPositionLUT[rank] = slot;
                        }
                    }
                }
            }

            int leftSlot = -1;
            while (session.PopPlayerLeftSlot(leftSlot)) {
                Player* player = PlayerForSlot(leftSlot);
                if (player != nullptr) {
                    player->type &= ~PLAYER_EXISTS;
                }
            }
            break;
        }
        default:
            break;
    }
}

extern "C" void NetGameplay_ConfigureScreenModeForSession(void) {
    NetSession& session = NetSession::Instance();
    switch (session.GetRole()) {
        case Net::Role::Host:
        case Net::Role::Client: {
            // Both host and guest always render a single fullscreen camera on
            // their own kart - not local splitscreen. gActiveScreenMode is what
            // actually drives camera/viewport setup in spawn_multiplayer_cameras()
            // and load_kart_textures() (both switch on it directly), so forcing
            // it here is sufficient for that. Also force gScreenModeSelection in
            // case anything re-derives gActiveScreenMode from it later - belt and
            // suspenders, since we couldn't fully trace every assignment site.
            gActiveScreenMode = SCREEN_MODE_1P;
            gScreenModeSelection = SCREEN_MODE_1P;

            // gPlayerCountSelection1 is a SEPARATE concern from screen mode: race
            // results, VS-mode win tracking, and lap-finish audio/rank logic in
            // race_logic.c (func_8028EF28 and friends) key off it as "how many
            // real racers are in this race", NOT camera count - e.g. GRAND_PRIX's
            // race-end check is gated on gPlayerCountSelection1 matching the
            // actual player total. Forcing it to 1 (as a prior version of this
            // function did) while gPlayers[1] genuinely exists and races is
            // exactly what caused the host to crash when a network guest's kart
            // triggered lap-finish logic sized/gated for 1 player. Use the real
            // connected total instead - this does NOT reintroduce splitscreen,
            // since gActiveScreenMode (forced above) is what actually controls
            // camera/viewport count in the two functions that matter.
            const int totalPlayers = session.GetConnectedPlayerCount();
            gPlayerCountSelection1 = (totalPlayers >= 1 && totalPlayers <= 4) ? totalPlayers : 1;
            break;
        }
        default:
            break;
    }
}

extern "C" int NetGameplay_GetLocalCameraPlayerIndex(void) {
    NetSession& session = NetSession::Instance();
    if (session.GetRole() == Net::Role::Client) {
        const int slot = session.GetLocalSlot();
        if (slot >= 0 && slot < NUM_PLAYERS) {
            return slot;
        }
    }
    return 0; // PLAYER_ONE
}

extern "C" int NetGameplay_GetSecondaryCameraPlayerIndex(void) {
    NetSession& session = NetSession::Instance();
    if (session.GetRole() != Net::Role::Client) {
        return 1; // PLAYER_TWO - unchanged single-player/local-multiplayer behavior.
    }
    const int localSlot = session.GetLocalSlot();
    const int totalPlayers = session.GetConnectedPlayerCount();
    for (int slot = 0; slot < totalPlayers && slot < NUM_PLAYERS; ++slot) {
        if (slot != localSlot) {
            return slot;
        }
    }
    return 1; // Fallback if we're somehow the only slot - keeps old behavior.
}

extern "C" void NetGameplay_HostStartRace(void) {
    NetSession& session = NetSession::Instance();
    if (session.GetRole() != Net::Role::Host) {
        return;
    }

    BroadcastCurrentRaceSetupIfHosting();

    // Skip the host's own local menu navigation too - one button starts the
    // race for host and every guest at once. Takes effect next
    // thread5_iteration, same as any other gGamestateNext change.
    gGamestateNext = RACING;
}

extern "C" void NetGameplay_HostBroadcastRaceStartIfHosting(void) {
    BroadcastCurrentRaceSetupIfHosting();
}

extern "C" void NetGameplay_ClientPollStartRace(void) {
    NetSession& session = NetSession::Instance();
    if (session.GetRole() != Net::Role::Client) {
        return;
    }

    Net::StartRaceMsg msg;
    if (!session.PopPendingRaceStart(msg)) {
        return;
    }

    TrackBrowser_SetTrack(msg.trackResourceName);
    // TrackBrowser_SetTrack() alone only updates the TrackBrowser's own internal
    // index/current-track pointer - it does NOT touch gCurrentCourseId, which is
    // the actual variable race setup/results/menu code reads elsewhere (see how
    // the debug menu's own track browsing always follows up with exactly this
    // same call). Without this, a guest kept whatever track they'd last
    // locally browsed to, which is why host and guest ended up racing two
    // different maps.
    gCurrentCourseId = (s16) TrackBrowser_GetTrackIndex();
    gModeSelection = msg.modeSelection;
    gCCSelection = msg.ccSelection;
    for (int i = 0; i < NUM_PLAYERS && i < Net::MAX_NET_PLAYERS; ++i) {
        gCharacterSelections[i] = static_cast<s8>(msg.characterForSlot[i]);
    }
    sClientRaceAuthorizedByHost = true;
    gGamestateNext = RACING;
}

extern "C" int NetGameplay_GetGuestCharacter(int slot) {
    NetSession& session = NetSession::Instance();
    if (session.GetRole() != Net::Role::Host) {
        return -1;
    }
    uint8_t characterId = 0;
    if (!session.GetGuestCharacter(slot, characterId)) {
        return -1;
    }
    return static_cast<int>(characterId);
}

// Character source that works for either role, unlike the host-only
// NetGameplay_GetGuestCharacter() above: on the host it's still that guest's
// Hello-reported character; on a client, every slot's character was already
// copied into gCharacterSelections[] wholesale by
// NetGameplay_ClientPollStartRace() (see StartRaceMsg), so it's just a lookup
// there, guarded by the real connected-player count computed by
// NetGameplay_ConfigureScreenModeForSession().
extern "C" int NetGameplay_GetNetworkCharacterForSlot(int slot) {
    NetSession& session = NetSession::Instance();
    switch (session.GetRole()) {
        case Net::Role::Host:
            return NetGameplay_GetGuestCharacter(slot);
        case Net::Role::Client:
            if (slot < 0 || slot >= gPlayerCountSelection1 || slot >= NUM_PLAYERS) {
                return -1;
            }
            return static_cast<int>(gCharacterSelections[slot]);
        default:
            return -1;
    }
}

extern "C" int NetGameplay_ShouldBlockRaceTransition(void) {
    NetSession& session = NetSession::Instance();
    if (session.GetRole() != Net::Role::Client) {
        return 0;
    }
    return sClientRaceAuthorizedByHost ? 0 : 1;
}

extern "C" void NetGameplay_ConsumeRaceAuthorization(void) {
    sClientRaceAuthorizedByHost = false;
}

extern "C" void NetGameplay_AuthorizeNativeRaceEntry(void) {
    NetSession& session = NetSession::Instance();
    if (session.GetRole() != Net::Role::Client) {
        return;
    }
    sClientRaceAuthorizedByHost = true;
}

extern "C" void NetGameplay_SendCharacterSelect(int characterId) {
    NetSession& session = NetSession::Instance();
    if (session.GetRole() != Net::Role::Client) {
        return;
    }
    session.SendCharacterSelect(static_cast<uint8_t>(characterId), true);
}

extern "C" int NetGameplay_GetRole(void) {
    NetSession& session = NetSession::Instance();
    switch (session.GetRole()) {
        case Net::Role::Host:
            return 1;
        case Net::Role::Client:
            return 2;
        default:
            return 0;
    }
}

extern "C" int NetGameplay_GetConnectionStatus(void) {
    return static_cast<int>(NetSession::Instance().GetStatus());
}

extern "C" void NetGameplay_StartHostDefaultRelay(void) {
    NetSession& session = NetSession::Instance();
    if (session.GetRole() != Net::Role::None) {
        return;
    }
    std::string error;
    session.StartHost(kDefaultRelayUrl, error);
}

extern "C" void NetGameplay_JoinRoomCode(const char* roomCode) {
    if (roomCode == nullptr || roomCode[0] == '\0') {
        return;
    }

    NetSession& session = NetSession::Instance();
    if (session.GetRole() == Net::Role::Client) {
        session.Disconnect();
    } else if (session.GetRole() == Net::Role::Host) {
        session.StopHost();
    }

    std::string error;
    session.Connect(kDefaultRelayUrl, roomCode, error);
}

extern "C" void NetGameplay_ClearSession(void) {
    NetSession& session = NetSession::Instance();
    if (session.GetRole() == Net::Role::Host) {
        session.StopHost();
    } else if (session.GetRole() == Net::Role::Client) {
        session.Disconnect();
    }
}

extern "C" void NetGameplay_CopyRoomCodeToClipboard(void) {
    std::string roomCode = NetSession::Instance().GetRoomCode();
    if (!roomCode.empty()) {
        NetGameplay_CopyTextToClipboard(roomCode.c_str());
    }
}

extern "C" void NetGameplay_GetRoomCode(char* out, int outSize) {
    if (out == nullptr || outSize <= 0) {
        return;
    }
    std::string roomCode = NetSession::Instance().GetRoomCode();
    std::snprintf(out, static_cast<size_t>(outSize), "%s", roomCode.c_str());
}

extern "C" void NetGameplay_GetLastError(char* out, int outSize) {
    if (out == nullptr || outSize <= 0) {
        return;
    }
    std::string error = NetSession::Instance().GetLastError();
    std::snprintf(out, static_cast<size_t>(outSize), "%s", error.c_str());
}
