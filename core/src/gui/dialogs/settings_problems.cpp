#include <gui/dialogs/settings_problems.h>
#include <gui/style.h>
#include <gui/gui.h>
#include <config.h>
#include <core.h>
#include <http_debug_server.h>
#include <imgui.h>
#include <algorithm>
#include <string>
#include <vector>

namespace dialogs {
    namespace {
        struct Group {
            std::string file;
            bool fileReset = false;
            std::vector<std::pair<std::string, int>> lines; // text, times reported
        };

        std::vector<Group> groups;
        bool open = false;
        // The body's height as last drawn, so the dialog is only as tall as it needs.
        float bodyHeight = 0.0f;

        // What a user loses when this file is not what they left, in their terms
        // rather than the file's.
        std::string affects(const std::string& file) {
            static const std::vector<std::pair<const char*, const char*>> known = {
                { "config.json", "Main settings: display, layout, waterfall and spectrum, source, band plan, and which radios and modules are loaded." },
                { "radio_config.json", "Radio settings: mode, bandwidth, squelch, noise blanker and tone squelch for each radio." },
                { "frequency_manager_config.json", "Bookmarks, bookmark lists and scanner settings." },
                { "recorder_config.json", "Recorder settings: what is recorded, the format and the folder." },
                { "noise_reduction_logmmse_config.json", "Noise reduction settings." },
                { "signal_id_config.json", "Signal ID settings." },
                { "rtl_sdr_config.json", "RTL-SDR settings: device, sample rate, gain and Bias-T." },
                { "file_source_config.json", "The file the File source plays." },
                { "QSO log", "Your QSO log." },
                { "colour maps", "Waterfall colour maps." },
            };
            for (auto& [name, text] : known) {
                if (file == name) { return text; }
            }
            const std::string suffix = "_config.json";
            if (file.size() > suffix.size() && file.compare(file.size() - suffix.size(), suffix.size(), suffix) == 0) {
                std::string mod = file.substr(0, file.size() - suffix.size());
                std::replace(mod.begin(), mod.end(), '_', ' ');
                return "Settings for " + mod + ".";
            }
            if (file.find(".json") != std::string::npos) { return "Settings stored in " + file + "."; }
            // Per-instance settings are reported under the instance's name.
            return "Settings for '" + file + "'.";
        }

        void collect() {
            auto fresh = ConfigManager::takeProblems();
            if (fresh.empty()) { return; }
            for (auto& p : fresh) {
                auto g = std::find_if(groups.begin(), groups.end(), [&](const Group& x) { return x.file == p.file; });
                if (g == groups.end()) {
                    groups.push_back(Group{ p.file });
                    g = groups.end() - 1;
                }
                g->fileReset |= p.fileReset;
                auto l = std::find_if(g->lines.begin(), g->lines.end(), [&](const auto& x) { return x.first == p.what; });
                if (l == g->lines.end()) { g->lines.push_back({ p.what, 1 }); }
                else { l->second++; }
            }
            open = true;
        }
    }

    void drawSettingsProblems() {
        collect();
        if (!open) { return; }

        const char* id = "Some settings could not be loaded##settings_problems";
        ImGuiViewport* vp = ImGui::GetMainViewport();
        float width = (std::min)(vp->Size.x * 0.92f, 640.0f * style::uiScale);
        ImGui::SetNextWindowPos(ImVec2(vp->Pos.x + vp->Size.x * 0.5f, vp->Pos.y + vp->Size.y * 0.5f), ImGuiCond_Always, ImVec2(0.5f, 0.5f));
        // A fixed height, so the body can scroll inside it and the buttons stay on
        // screen however much there is to say - a phone held sideways has room for
        // only a few lines.
        const ImGuiStyle& st = ImGui::GetStyle();
        float footer = ImGui::GetFrameHeight() + st.ItemSpacing.y * 3.0f + 1.0f;
        float chrome = ImGui::GetFrameHeight() + st.WindowPadding.y * 2.0f + footer;
        float wanted = bodyHeight > 0.0f ? bodyHeight + chrome : 620.0f * style::uiScale;
        float height = (std::min)(vp->Size.y * 0.92f, wanted);
        ImGui::SetNextWindowSize(ImVec2(width, height), ImGuiCond_Always);
        if (!ImGui::IsPopupOpen(id)) { ImGui::OpenPopup(id); }
        gui::mainWindow.lockWaterfallControls = true;

        // Opaque whatever the theme's popup alpha: this is text to be read, not a hint
        // over the spectrum, and the spectrum's labels showed through it.
        ImVec4 bg = ImGui::GetStyleColorVec4(ImGuiCol_PopupBg);
        bg.w = 1.0f;
        ImGui::PushStyleColor(ImGuiCol_PopupBg, bg);
        bool visible = ImGui::BeginPopupModal(id, NULL, ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoScrollbar);
        ImGui::PopStyleColor();
        if (!visible) { return; }

        bool anyReset = std::any_of(groups.begin(), groups.end(), [](const Group& g) { return g.fileReset; });

        ImGui::BeginChild("##settings_problems_body", ImVec2(0.0f, -footer), false);
        ImGui::PushTextWrapPos(0.0f);
        ImGui::TextUnformatted(anyReset
            ? "SDR++Black is running, but some of your settings were damaged or could not be read. They have been put back to their defaults so the program could start."
            : "SDR++Black is running, but some of your settings could not be read as they were. Those values have been put back to their defaults so the program could start.");
        ImGui::Spacing();
        for (auto& g : groups) {
            ImGui::Spacing();
            ImGui::TextColored(ImGui::GetStyleColorVec4(ImGuiCol_PlotHistogram), "%s", g.file.c_str());
            ImGui::TextDisabled("%s", affects(g.file).c_str());
            ImGui::Indent();
            for (auto& [text, count] : g.lines) {
                std::string line = text;
                if (count > 1) { line += " (" + std::to_string(count) + " times)"; }
                ImGui::Bullet();
                ImGui::TextUnformatted(line.c_str());
            }
            ImGui::Unindent();
        }
        ImGui::Spacing();
        ImGui::Separator();
        ImGui::Spacing();
        ImGui::TextUnformatted("The program will work, but what is listed above is not the way you left it, and anything that depends on it may not behave the way you expect.");
        ImGui::Spacing();
        ImGui::TextUnformatted("You can continue, but it is not recommended if you are using SDR++Black for anything important. The safest thing to do is quit, restore these files from a backup, and start again.");
        ImGui::Spacing();
        ImGui::TextDisabled("Settings folder: %s", core::getRoot());
        ImGui::PopTextWrapPos();
        bodyHeight = ImGui::GetCursorPosY() + st.ItemSpacing.y;
        ImGui::EndChild();

        ImGui::Separator();
        ImGui::Spacing();

        // Quit is the recommended answer, so it comes first. Continuing is deliberately
        // the far button rather than the one under a thumb.
        const ImVec2 size(150.0f * style::uiScale, 0.0f);
        if (ImGui::Button("Quit##settings_problems", size)) {
            open = false;
            groups.clear();
            bodyHeight = 0.0f;
            ImGui::CloseCurrentPopup();
            httpdebug::stopApp();
        }
        ImGui::SameLine(0.0f, (std::max)(20.0f * style::uiScale, ImGui::GetContentRegionAvail().x - size.x * 2.0f));
        if (ImGui::Button("Continue anyway##settings_problems", size)) {
            open = false;
            groups.clear();
            bodyHeight = 0.0f;
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    }
}
