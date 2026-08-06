#include <gui/menus/theme.h>
#include <gui/gui.h>
#include <core.h>
#include <gui/style.h>
#include <utils/flog.h>
#include <fstream>
#include <cstring>
#include <imgui.h>

namespace thememenu {
    int themeId;
    std::vector<std::string> themeNames;
    std::string themeNamesTxt;

    void saveStyle() {
        std::string path = std::string(core::getRoot()) + "/imgui_style_v1.bin";
        std::ofstream file(path, std::ios::binary);
        if (file) {
            ImGuiStyle style = ImGui::GetStyle();
            file.write((char*)&style, sizeof(ImGuiStyle));
        }
    }

    // The saved style is the scaled one, since that is what is live by the time
    // anything saves it. Reports whether it was applied so init knows not to scale on
    // top of it.
    bool loadStyle() {
        std::string path = std::string(core::getRoot()) + "/imgui_style_v1.bin";
        std::ifstream file(path, std::ios::binary);
        if (!file) { return false; }
        ImGuiStyle style;
        file.read((char*)&style, sizeof(ImGuiStyle));
        // A truncated file leaves the rest of the struct uninitialised, and a style
        // full of garbage paddings lays the whole UI out somewhere off screen.
        if ((size_t)file.gcount() != sizeof(ImGuiStyle)) {
            flog::warn("Ignoring incomplete saved ImGui style: {0}", path);
            return false;
        }
        ImGui::GetStyle() = style;
        return true;
    }

    void init(std::string resDir) {
        // TODO: Not hardcode theme directory
        gui::themeManager.loadThemesFromDir(resDir + "/themes/");

        // Select Dark theme by default
        themeNames = gui::themeManager.getThemeNames();
        themeNames.push_back("ImGUI Dark");
        themeNames.push_back("ImGUI Light");
        themeNames.push_back("ImGUI Classic");
        auto it = std::find(themeNames.begin(), themeNames.end(), "Dark");
        themeId = std::distance(themeNames.begin(), it);

        // Load saved theme from config if exists
        core::configManager.acquire();
        if (core::configManager.conf.contains("theme")) {
            std::string savedTheme = core::configManager.conf["theme"];
            auto it2 = std::find(themeNames.begin(), themeNames.end(), savedTheme);
            if (it2 != themeNames.end()) {
                themeId = std::distance(themeNames.begin(), it2);
            }
        }
        core::configManager.release();

        // The waterfall background, the GL clear colour, the FFT hold and the squelch
        // colours live outside ImGuiStyle, so loadStyle cannot restore them - only
        // applying the theme sets them. Without this they sat at their defaults until
        // the user touched the theme combo, which is why picking any theme, even the
        // one already selected, visibly changed the waterfall.
        applyTheme();

        // Load saved style if exists
        bool styleLoaded = loadStyle();

        // Apply scaling. The saved style was scaled before it was written, so scaling
        // it again multiplied every padding, spacing and item size by uiScale a second
        // time on each start - which on anything but a 1.0 scale pushed the layout,
        // and the waterfall inside it, out of shape.
        if (!styleLoaded) {
            ImGui::GetStyle().ScaleAllSizes(style::uiScale);
        }

        themeNamesTxt = "";
        for (auto name : themeNames) {
            themeNamesTxt += name;
            themeNamesTxt += '\0';
        }
    }

    void applyTheme() {
        if (themeNames[themeId] == "ImGUI Dark") {
            ImGui::StyleColorsDark();
        }
        else if (themeNames[themeId] == "ImGUI Light") {
            ImGui::StyleColorsLight();
        }
        else if (themeNames[themeId] == "ImGUI Classic") {
            ImGui::StyleColorsClassic();
        }
        else {
            gui::themeManager.applyTheme(themeNames[themeId]);
        }
        core::configManager.acquire();
        core::configManager.conf["theme"] = themeNames[themeId];
        core::configManager.release(true);
    }

    void draw(void* ctx) {
        float menuWidth = ImGui::GetContentRegionAvail().x;
        ImGui::LeftLabel("Theme");
        ImGui::SetNextItemWidth(menuWidth - ImGui::GetCursorPosX());
        ImGuiStyle styleBefore = ImGui::GetStyle();
        if (ImGui::Combo("##theme_select_combo", &themeId, themeNamesTxt.c_str())) {
            applyTheme();
            ImGuiStyle styleAfter = ImGui::GetStyle();
            if (memcmp(&styleBefore, &styleAfter, sizeof(ImGuiStyle)) != 0) {
                saveStyle();
            }
        }
    }
}