#pragma once

#include <memory>
#include "ship/window/gui/GuiWindow.h"

namespace GameUI {
namespace OnlinePlay {

// A standalone floating window (same family as the game's Stats/Console
// windows) bound to its own CVar ("gOnlineLobbyWindowOpen") - shows/hides
// independently of the F1 debug/enhancements overlay entirely. This is what
// gets opened by a real in-game button (see menus.c's Game Select screen)
// instead of requiring the player to know about F1 -> Enhancements -> Online
// Play.
std::shared_ptr<Ship::GuiWindow> CreateOnlineLobbyWindow();

} // namespace OnlinePlay
} // namespace GameUI
