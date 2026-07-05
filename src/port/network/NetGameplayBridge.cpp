#include "NetGameplayBridge.h"

#include <algorithm>
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
}

// Set true for exactly one race entry whenever the client applies a StartRace
// message from the host; consumed (reset false) by
// NetGameplay_ConsumeRaceAuthorization() once that RACING transition actually
// takes effect. While false, NetGameplay_ShouldBlockRaceTransition() blocks a
// guest's own local menus from starting an unrelated race.
static bool sClientRaceAuthorizedByHost = false;

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

                const int localSlot = session.GetLocalSlot();
                if (localSlot > 0) {
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
        case Net::Role::Host: {
            // Always derive the split from network truth once a session is
            // active - the local Game Select screen's 1P/2P/3P/4P pick is
            // meaningless in online mode (there's no lobby yet forcing people
            // through a dedicated online path), so it must never be trusted
            // over the real connected player count. Previously this only
            // recalculated when already in SCREEN_MODE_1P, which meant a host
            // who locally picked 2P/3P/4P before hosting kept that stale local
            // split (with a local CPU/ghost in the other slot) instead of ever
            // reflecting the actual guest.
            const int totalPlayers = std::max(session.GetConnectedPlayerCount(), 1);
            gPlayerCountSelection1 = std::min(totalPlayers, 4);
            if (gPlayerCountSelection1 <= 1) {
                gActiveScreenMode = SCREEN_MODE_1P;
            } else if (gPlayerCountSelection1 == 2) {
                gActiveScreenMode = SCREEN_MODE_2P_SPLITSCREEN_HORIZONTAL;
            } else {
                gActiveScreenMode = SCREEN_MODE_3P_4P_SPLITSCREEN;
            }
            break;
        }
        case Net::Role::Client:
            // Always a single fullscreen camera on this client's own kart,
            // no matter how many players are actually in the race.
            gActiveScreenMode = SCREEN_MODE_1P;
            gPlayerCountSelection1 = 1;
            break;
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

extern "C" void NetGameplay_HostStartRace(void) {
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

    // Skip the host's own local menu navigation too - one button starts the
    // race for host and every guest at once. Takes effect next
    // thread5_iteration, same as any other gGamestateNext change.
    gGamestateNext = RACING;
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
