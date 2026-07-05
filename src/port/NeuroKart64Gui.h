#pragma once

#include <libultraship.h>
#include "ship/window/gui/Gui.h"
#include "ship/window/Window.h"

class Gui; // <-- forward declare
//class Window;

namespace Ship {
    class NeuroKart64Gui : public Gui {
      public:
        NeuroKart64Gui() : Gui() {}
        NeuroKart64Gui(std::vector<std::shared_ptr<GuiWindow>> guiWindows) : Gui(guiWindows) {}

      protected:
        virtual void DrawMenu() override;
    };
}