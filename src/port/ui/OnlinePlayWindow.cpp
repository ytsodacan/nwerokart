#include "port/ui/PortMenu.h"
#include "port/ui/UIWidgets.h"
#include "port/ui/OnlinePlayWindow.h"
#include "port/network/NetSession.h"
#include "port/network/NetGameplayBridge.h"

#include <imgui.h>
#include <cstring>

namespace GameUI {
extern std::shared_ptr<PortMenu> mPortMenu;

namespace OnlinePlay {

static const char* sCharacterNames[8] = { "Mario", "Luigi", "Yoshi", "Toad", "DK", "Wario", "Peach", "Bowser" };

// This client's in-progress character pick, sent to the host via
// SelectCharacter whenever it changes - lets each guest run their own
// character select instead of always defaulting to whatever Hello sent at
// connect time (character 0 / Mario).
static uint8_t sLocalCharacterSelection = 0;
static bool sLocalReady = false;

// Default relay - your always-on VPS relay, not a per-host tunnel. Editable in
// case you ever want to point at a different relay (self-hosted test instance, etc).
static char sRelayUrl[128] = "wss://neurorelay.sillyprootsoda.com";
static char sRoomCodeInput[16] = "";

const char* StatusLabel(Net::ConnectionStatus status) {
    switch (status) {
        case Net::ConnectionStatus::Idle:
            return "Not connected";
        case Net::ConnectionStatus::Connecting:
            return "Connecting...";
        case Net::ConnectionStatus::Connected:
            return "Connected";
        case Net::ConnectionStatus::Failed:
            return "Failed";
        case Net::ConnectionStatus::Disconnected:
            return "Disconnected";
    }
    return "Unknown";
}

void DrawOnlinePlayPanelContent(); // forward decl - defined below, called by both entry points above

void DrawOnlinePlayPanel(WidgetInfo& info) {
    using namespace UIWidgets;
    DrawOnlinePlayPanelContent();
}

// Actual panel contents, independent of the sidebar-widget vs standalone-window
// distinction - shared by both DrawOnlinePlayPanel() (sidebar widget, inside the
// F1 overlay) and OnlineLobbyWindow (standalone window, opened by a real in-game
// button - see menus.c's Game Select screen).
void DrawOnlinePlayPanelContent() {
    using namespace UIWidgets;
    Net::NetSession& session = Net::NetSession::Instance();
    const Net::Role role = session.GetRole();
    const Net::ConnectionStatus status = session.GetStatus();

    ImGui::TextWrapped(
        "Race with friends over the internet. One person hosts, everyone else joins "
        "with the room code - no port forwarding needed.");
    ImGui::Spacing();

    ImGui::TextUnformatted("Relay Address");
    ImGui::SetNextItemWidth(-1.0f);
    ImGui::InputText("##RelayUrl", sRelayUrl, sizeof(sRelayUrl));
    ImGui::Spacing();
    ImGui::Separator();
    ImGui::Spacing();

    if (role == Net::Role::None) {
        // -------- Host --------
        ImGui::SeparatorText("Host a Game");
        UIWidgets::ButtonOptions hostOptions = UIWidgets::ButtonOptions().Tooltip(
            "Starts a room on the relay and gives you a code to share with friends.");
        if (UIWidgets::Button("Host Game", hostOptions)) {
            std::string error;
            if (!session.StartHost(sRelayUrl, error)) {
                // error already stored in NetSession's last-error, surfaced below
            }
        }

        ImGui::Spacing();
        ImGui::Spacing();

        // -------- Join --------
        ImGui::SeparatorText("Join a Game");
        ImGui::TextUnformatted("Room Code");
        ImGui::SetNextItemWidth(120.0f);
        ImGui::InputText("##RoomCode", sRoomCodeInput, sizeof(sRoomCodeInput),
                          ImGuiInputTextFlags_CharsUppercase | ImGuiInputTextFlags_CallbackCharFilter,
                          UIWidgets::TextFilters::FilterAlphaNum);
        ImGui::SameLine();
        UIWidgets::ButtonOptions joinOptions =
            UIWidgets::ButtonOptions().Size(UIWidgets::Sizes::Inline).Tooltip("Joins the host's room by code.");
        bool canJoin = sRoomCodeInput[0] != '\0';
        if (!canJoin) {
            joinOptions.disabled = true;
        }
        if (UIWidgets::Button("Join Game", joinOptions) && canJoin) {
            std::string error;
            session.Connect(sRelayUrl, sRoomCodeInput, error);
        }
    } else {
        // -------- Active session (host or client) --------
        ImGui::SeparatorText(role == Net::Role::Host ? "Hosting" : "Joined Game");
        ImGui::Text("Status: %s", StatusLabel(status));

        if (role == Net::Role::Host) {
            std::string roomCode = session.GetRoomCode();
            if (!roomCode.empty()) {
                ImGui::Spacing();
                ImGui::TextUnformatted("Share this code with your friends:");
                ImGui::SetWindowFontScale(1.6f);
                ImGui::TextColored(UIWidgets::ColorValues.at(UIWidgets::Colors::Neuro), "%s", roomCode.c_str());
                ImGui::SetWindowFontScale(1.0f);
                if (ImGui::Button("Copy Code")) {
                    ImGui::SetClipboardText(roomCode.c_str());
                }
            }
            ImGui::Spacing();
            ImGui::Text("Players connected: %d", session.GetConnectedPlayerCount());

            ImGui::Spacing();
            ImGui::SeparatorText("Guests");
            for (int slot = 1; slot < Net::MAX_NET_PLAYERS; slot++) {
                uint8_t characterId = 0;
                if (!session.GetGuestCharacter(slot, characterId)) {
                    continue; // not connected
                }
                bool ready = false;
                session.GetGuestReady(slot, ready);
                const char* characterName = characterId < 8 ? sCharacterNames[characterId] : "?";
                ImGui::Text("Slot %d: %s - %s", slot, characterName, ready ? "Ready" : "Picking...");
            }

            ImGui::Spacing();
            UIWidgets::ButtonOptions startRaceOptions = UIWidgets::ButtonOptions().Tooltip(
                "Sends every connected guest straight into a race with your current track/mode/character "
                "selections - skips their local menus entirely.");
            if (UIWidgets::Button("Start Race", startRaceOptions)) {
                NetGameplay_HostStartRace();
            }
        } else {
            if (status == Net::ConnectionStatus::Connected) {
                ImGui::Text("Playing as slot %d", session.GetLocalSlot());

                ImGui::Spacing();
                ImGui::SeparatorText("Pick Your Character");
                for (int i = 0; i < 8; i++) {
                    if (i > 0) {
                        ImGui::SameLine();
                    }
                    bool selected = (sLocalCharacterSelection == i);
                    if (selected) {
                        ImGui::PushStyleColor(ImGuiCol_Button, UIWidgets::ColorValues.at(UIWidgets::Colors::Neuro));
                    }
                    if (ImGui::Button(sCharacterNames[i])) {
                        sLocalCharacterSelection = static_cast<uint8_t>(i);
                        sLocalReady = true;
                        session.SendCharacterSelect(sLocalCharacterSelection, sLocalReady);
                    }
                    if (selected) {
                        ImGui::PopStyleColor();
                    }
                }
                ImGui::Spacing();
                ImGui::Text("Waiting for the host to start the race...");
            }
        }

        ImGui::Spacing();
        UIWidgets::ButtonOptions leaveOptions = UIWidgets::ButtonOptions().Color(UIWidgets::Colors::Red);
        if (UIWidgets::Button(role == Net::Role::Host ? "Stop Hosting" : "Disconnect", leaveOptions)) {
            if (role == Net::Role::Host) {
                session.StopHost();
            } else {
                session.Disconnect();
            }
            sRoomCodeInput[0] = '\0';
        }
    }

    if (status == Net::ConnectionStatus::Failed) {
        ImGui::Spacing();
        ImGui::PushStyleColor(ImGuiCol_Text, UIWidgets::ColorValues.at(UIWidgets::Colors::Red));
        ImGui::TextWrapped("%s", session.GetLastError().c_str());
        ImGui::PopStyleColor();
    }
}

void RegisterOnlinePlayWidgets() {
    mPortMenu->AddSidebarEntry("Enhancements", "Online Play", 1);
    WidgetPath path = { "Enhancements", "Online Play", SECTION_COLUMN_1 };

    mPortMenu->AddWidget(path, "OnlinePlayPanel", WIDGET_CUSTOM).CustomFunction(DrawOnlinePlayPanel);
}

static RegisterMenuInitFunc initFunc(RegisterOnlinePlayWidgets);

// ---------------------------------------------------------------------------
// Standalone window - opened by a real in-game button (Z on the Game Select
// screen), independent of the F1 debug/enhancements overlay entirely.
// ---------------------------------------------------------------------------
class OnlineLobbyWindow : public Ship::GuiWindow {
  public:
    using GuiWindow::GuiWindow;

  protected:
    void InitElement() override {}
    void UpdateElement() override {}
    void DrawElement() override {
        DrawOnlinePlayPanelContent();
    }
};

std::shared_ptr<Ship::GuiWindow> CreateOnlineLobbyWindow() {
    return std::make_shared<OnlineLobbyWindow>("gOnlineLobbyWindowOpen", false, "Online Multiplayer",
                                                ImVec2(420, 520));
}

} // namespace OnlinePlay
} // namespace GameUI
