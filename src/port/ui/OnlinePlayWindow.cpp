#include "port/ui/PortMenu.h"
#include "port/ui/UIWidgets.h"
#include "port/network/NetSession.h"

#include <imgui.h>
#include <cstring>

namespace GameUI {
extern std::shared_ptr<PortMenu> mPortMenu;

namespace OnlinePlay {

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

void DrawOnlinePlayPanel(WidgetInfo& info) {
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
        } else {
            if (status == Net::ConnectionStatus::Connected) {
                ImGui::Text("Playing as slot %d", session.GetLocalSlot());
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

} // namespace OnlinePlay
} // namespace GameUI
