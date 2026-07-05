#ifndef NET_GAMEPLAY_BRIDGE_H
#define NET_GAMEPLAY_BRIDGE_H

#include <stdint.h>

// Bridges Net::NetSession (transport-only, knows nothing about game structs)
// to the actual per-frame gameplay state: local controller substitution,
// per-tick snapshot broadcast/apply, and forcing the right screen/camera
// setup for host vs. guest.
//
// Kept as a separate translation layer rather than adding any of this
// directly into NetSession.{h,cpp} - NetSession only ever talks in
// Net::InputState/KartState, never in struct Controller/Player. This file is
// the only place that knows about both.

#ifdef __cplusplus
extern "C" {
#endif

// Call once per game frame, right after read_controllers() has populated
// gControllers[0..3] and before anything consumes them for this frame
// (i.e. right after read_controllers() in thread5_iteration(), before
// game_state_handler()):
//   - Host: overwrites gControllers[slot] for every connected guest slot
//     with their latest network input (translated from Net::InputState).
//     gControllers[0] (the host's own physical controller) is left alone.
//   - Client: sends this client's own gControllers[0] reading upstream.
//   - Either role: pumps Net::NetSession::Instance().Tick().
// No-op if no session is active (NetSession::GetRole() == Role::None).
void NetGameplay_PerFrameInput(void);

// Call once per rendered frame, after all of that frame's gameplay
// simulation sub-ticks have run (end of race_logic_loop(), after the
// process_game_tick() loop and before/after garbage collection - order
// doesn't matter relative to that):
//   - Host: builds a KartState for every player slot and broadcasts it.
//   - Client: applies the latest received snapshot onto every player slot
//     that isn't this client's own local slot, overwriting position/
//     rotation/lap/item/anim directly rather than letting local physics
//     determine it. Also drains any pending PlayerLeft notifications,
//     despawning that slot's kart (clears PLAYER_EXISTS).
// No-op if no session is active.
void NetGameplay_PerFrameSimSync(uint32_t frameNumber);

// Call once, from the top of spawn_players_and_cameras(), before it reads
// gActiveScreenMode / gPlayerCountSelection1, to force the right
// split/fullscreen configuration for the current session role:
//   - Host with N total connected players (self included): forces enough
//     local splitscreen slots to cover all of them, up to the existing
//     4-way local splitscreen ceiling. v1 scope is host + 1 guest (2 total);
//     >2 total is wired generically but hasn't been camera-layout tested.
//   - Client: forces SCREEN_MODE_1P so this guest always renders a single
//     fullscreen camera, regardless of the host's local splitscreen.
// No-op if no session is active.
void NetGameplay_ConfigureScreenModeForSession(void);

// Returns the gPlayers[] index a client build's single fullscreen camera
// should follow (NetSession's local slot). Returns 0 (PLAYER_ONE) for
// host/no-session, so this is always safe to call unconditionally from
// spawn_single_player_camera().
int NetGameplay_GetLocalCameraPlayerIndex(void);

// Host only: call when the host presses "Start Race" (see OnlinePlayWindow.cpp).
// Reads the currently selected track (via TrackBrowser), gModeSelection,
// gCCSelection, and gCharacterSelections[0] for the host's own character
// plus every connected guest's character (from their Hello), broadcasts all
// of it to every guest as a single StartRaceMsg, and immediately forces the
// host's own gGamestateNext to RACING - so pressing this one button starts
// the race for the host and every guest at once, instead of everyone
// separately navigating their own local track/mode/character menus.
// No-op if this session isn't the host.
void NetGameplay_HostStartRace(void);

// Client only: call once per frame (wired into thread5_iteration alongside
// NetGameplay_PerFrameInput) to check for a pending StartRace message from
// the host. If one arrived, applies its track/mode/CC/character selections
// and forces gGamestateNext to RACING, skipping this guest's own local
// track/mode/character menus entirely. No-op if nothing is pending, or if
// this session isn't a client.
void NetGameplay_ClientPollStartRace(void);

// Host only: returns the network character id for a connected guest
// occupying this player slot (1..MAX_NET_PLAYERS-1), or -1 if that slot
// isn't a connected network guest (including on a client, or slot 0 - the
// host's own local slot). Used by spawn_players.c right after local spawn
// logic runs, to force every connected network slot to be treated as a real
// human player with its guest's actual chosen character, instead of the CPU
// stand-in the local spawn logic assigns to "extra" slots beyond the host's
// own local player count.
int NetGameplay_GetGuestCharacter(int slot);

// Returns non-zero if a guest should NOT be allowed to transition into RACING
// right now - true whenever connected as a client and the host hasn't actually
// authorized a race via StartRace yet. Call from thread5_iteration's
// gGamestateNext transition check, before it takes effect, so a guest can't
// wander into an unrelated local race (via their own local menus) while
// sitting in an online session - every RACING entry must be authorized by the
// host's StartRace message. Always returns 0 for host/no-session.
int NetGameplay_ShouldBlockRaceTransition(void);

// Call right after letting a RACING transition through (i.e. NOT blocked by
// the above) so the one-shot authorization is consumed - entering RACING
// again afterward (e.g. hitting Retry, or finishing and starting another
// race) requires a fresh StartRace from the host each time.
void NetGameplay_ConsumeRaceAuthorization(void);

#ifdef __cplusplus
}
#endif

#endif // NET_GAMEPLAY_BRIDGE_H
