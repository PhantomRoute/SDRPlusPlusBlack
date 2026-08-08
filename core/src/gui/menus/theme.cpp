#include <gui/menus/theme.h>
#include <gui/menus/display.h>
#include <gui/menus/vfo_color.h>
#include <gui/gui.h>
#include <core.h>
#include <gui/style.h>
#include <gui/file_dialogs.h>
#include <utils/flog.h>
#include <filesystem>
#include <fstream>
#include <cstring>
#include <algorithm>
#include <imgui.h>

namespace thememenu {
    int themeId;
    std::vector<std::string> themeNames;
    std::string themeNamesTxt;
    std::string userThemeDir;

    // The three ImGui presets aren't files, they're calls into ImGui. They're listed
    // alongside the real themes so they can be picked and used as a starting point.
    const char* IMGUI_PRESETS[] = { "ImGUI Dark", "ImGUI Light", "ImGUI Classic" };

    // Theme editor state. editData is a complete theme document - every key the build
    // knows about - so a saved or exported theme doesn't depend on the defaults of
    // whoever opens it.
    bool editorOpen = false;
    json editData;
    char editName[128] = { 0 };
    char editAuthor[128] = { 0 };
    std::string editStatus;
    bool editStatusIsError = false;

    bool importOpen = false;
    bool exportOpen = false;
    pfd::open_file* importDialog = NULL;
    pfd::save_file* exportDialog = NULL;

    void saveStyle() {
        std::string path = std::string(core::getRoot()) + "/imgui_style_v2.bin";
        std::ofstream file(path, std::ios::binary);
        if (file) {
            ImGuiStyle style = ImGui::GetStyle();
            file.write((char*)&style, sizeof(ImGuiStyle));
        }
    }

    // The saved style is the scaled one, since that is what is live by the time
    // anything saves it. Reports whether it was applied so init knows not to scale on
    // top of it.
    //
    // v2, not v1: a v1 file was written by the build that scaled the style again on
    // every start, so any of them that a theme change ever saved has uiScale baked
    // into it one or more times over. Not scaling it again stops the compounding but
    // cannot undo what is already in the file, so the old name is abandoned and the
    // style rebuilt from the theme.
    bool loadStyle() {
        std::string path = std::string(core::getRoot()) + "/imgui_style_v2.bin";
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

    // The spectrum styling is part of the theme, but like the waterfall gradient it is
    // also a control of its own, so the config remembers the live value under the same
    // key the theme file uses. Picking a theme overwrites it; changing it by hand
    // overrides the theme until the next theme change.
    static void saveSpectrumStyle() {
        core::configManager.acquire();
        for (const auto& choice : gui::themeManager.getChoices()) {
            int id = std::clamp(*choice.value, 0, (int)choice.options.size() - 1);
            core::configManager.conf[choice.key] = choice.options[id];
        }
        for (const auto& slider : gui::themeManager.getSliders()) {
            core::configManager.conf[slider.key] = ThemeManager::roundSetting(*slider.value);
        }
        core::configManager.release(true);
    }

    static void loadSpectrumStyle() {
        core::configManager.acquire();
        auto& conf = core::configManager.conf;

        for (const auto& choice : gui::themeManager.getChoices()) {
            if (!conf.contains(choice.key) || !conf[choice.key].is_string()) { continue; }
            int decoded;
            if (ThemeManager::decodeChoice(choice, conf[choice.key].get<std::string>(), decoded)) {
                *choice.value = decoded;
            }
        }
        for (const auto& slider : gui::themeManager.getSliders()) {
            if (!conf.contains(slider.key) || !conf[slider.key].is_number()) { continue; }
            *slider.value = std::clamp(conf[slider.key].get<float>(), slider.min, slider.max);
        }

        // The fill used to be the Display menu's "Shadow" checkbox, which is now the
        // "None" fill style. Carry the old setting over rather than silently turning
        // the fill back on for anyone who had it off.
        if (!conf.contains("FFTFillStyle") && conf.contains("showFFTShadows") &&
            conf["showFFTShadows"].is_boolean() && !conf["showFFTShadows"].get<bool>()) {
            gui::themeManager.fftFillStyle = FFT_FILL_NONE;
        }

        core::configManager.release();
    }

    void rebuildThemeList() {
        std::string selected = (themeId >= 0 && themeId < (int)themeNames.size()) ? themeNames[themeId] : "Dark";

        themeNames = gui::themeManager.getThemeNames();
        for (const char* preset : IMGUI_PRESETS) { themeNames.push_back(preset); }

        auto it = std::find(themeNames.begin(), themeNames.end(), selected);
        if (it == themeNames.end()) {
            it = std::find(themeNames.begin(), themeNames.end(), "Dark");
        }
        themeId = (it != themeNames.end()) ? std::distance(themeNames.begin(), it) : 0;

        themeNamesTxt = "";
        for (auto name : themeNames) {
            themeNamesTxt += name;
            themeNamesTxt += '\0';
        }
    }

    // Reloads both directories from scratch. Needed after a delete, since a user theme
    // can shadow a shipped one of the same name and removing it has to bring the
    // original back.
    void reloadThemes(std::string resDir) {
        static std::string savedResDir;
        if (!resDir.empty()) { savedResDir = resDir; }

        gui::themeManager.clearThemes();
        gui::themeManager.loadThemesFromDir(savedResDir + "/themes/", true);
        if (std::filesystem::is_directory(userThemeDir)) {
            gui::themeManager.loadThemesFromDir(userThemeDir, false);
        }
        rebuildThemeList();
    }

    void init(std::string resDir) {
        // User themes live next to the config, not in the resource directory, so they
        // survive an upgrade and don't need write access to the install.
        userThemeDir = std::string(core::getRoot()) + "/themes";
        gui::themeManager.setUserThemeDir(userThemeDir);

        themeId = 0;
        themeNames.clear();
        reloadThemes(resDir);

        // Select Dark theme by default
        auto it = std::find(themeNames.begin(), themeNames.end(), "Dark");
        if (it != themeNames.end()) { themeId = std::distance(themeNames.begin(), it); }

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

        // applyTheme(false): displaymenu::init runs after this and applies the gradient
        // the config remembers, and the colormaps aren't even loaded yet.
        //
        // The waterfall background, the GL clear colour, the FFT hold and the squelch
        // colours live outside ImGuiStyle, so loadStyle cannot restore them - only
        // applying the theme sets them. Without this they sat at their defaults until
        // the user touched the theme combo, which is why picking any theme, even the
        // one already selected, visibly changed the waterfall.
        applyTheme(false);

        // Load saved style if exists
        bool styleLoaded = loadStyle();

        // Apply scaling. The saved style was scaled before it was written, so scaling
        // it again multiplied every padding, spacing and item size by uiScale a second
        // time on each start - which on anything but a 1.0 scale pushed the layout,
        // and the waterfall inside it, out of shape.
        if (!styleLoaded) {
            ImGui::GetStyle().ScaleAllSizes(style::uiScale);
        }
    }

    void applyTheme(bool userSelected) {
        std::string name = (themeId >= 0 && themeId < (int)themeNames.size()) ? themeNames[themeId] : "Dark";

        if (name == "ImGUI Dark") {
            ImGui::StyleColorsDark();
            gui::themeManager.resetToDefaults();
        }
        else if (name == "ImGUI Light") {
            ImGui::StyleColorsLight();
            gui::themeManager.resetToDefaults();
        }
        else if (name == "ImGUI Classic") {
            ImGui::StyleColorsClassic();
            gui::themeManager.resetToDefaults();
        }
        else {
            gui::themeManager.applyTheme(name);
        }

        // A theme names its waterfall gradient and its spectrum styling, so picking one
        // switches them and makes those the live choices the config remembers. Changing
        // either by hand afterwards overrides it until the next theme change - one
        // setting, one place it is stored, no argument between the theme file and the
        // config at startup.
        if (userSelected) {
            if (!gui::themeManager.colorMap.empty() &&
                !displaymenu::setColorMapByName(gui::themeManager.colorMap)) {
                flog::warn("Theme '{0}' asks for colormap '{1}', which isn't installed", name, gui::themeManager.colorMap);
            }
            saveSpectrumStyle();
        }
        else {
            // Only re-applying the theme that is already selected, so what the config
            // holds wins: at startup it is the user's last choice, and after the editor
            // closes it is whatever they had before the preview started.
            loadSpectrumStyle();
        }

        core::configManager.acquire();
        core::configManager.conf["theme"] = name;
        core::configManager.release(true);
    }

    static void setStatus(const std::string& msg, bool error) {
        editStatus = msg;
        editStatusIsError = error;
    }

    static void copyToBuf(char* buf, size_t size, const std::string& str) {
        strncpy(buf, str.c_str(), size - 1);
        buf[size - 1] = 0;
    }

    // Snapshots what is currently on screen. Because applyTheme() has already run, the
    // live colours are the selected theme plus the defaults for anything it omits, so
    // this gives a complete document that matches the theme exactly.
    static void openEditor() {
        std::string name = (themeId >= 0 && themeId < (int)themeNames.size()) ? themeNames[themeId] : "Dark";
        const Theme* thm = gui::themeManager.getTheme(name);
        std::string author = thm ? thm->author : "--";

        // Editing a shipped theme or an ImGui preset produces a copy, so suggest a name
        // that is free instead of one the user will be told they can't save over.
        std::string editableName = name;
        if (!thm || thm->readOnly) {
            editableName = name + " (custom)";
            for (int i = 2; gui::themeManager.themeExists(editableName); i++) {
                editableName = name + " (custom " + std::to_string(i) + ")";
            }
        }

        // The Theme menu's Color Map combo is the one control for the gradient, so the
        // editor just records whatever it currently says rather than duplicating it.
        gui::themeManager.colorMap = displaymenu::getColorMapName();
        editData = gui::themeManager.dumpLiveTheme(editableName, author);
        copyToBuf(editName, sizeof editName, editableName);
        copyToBuf(editAuthor, sizeof editAuthor, author);
        editStatus = "";
        editorOpen = true;
    }

    static ImVec4 getEditColor(const std::string& key) {
        uint8_t rgba[4] = { 0, 0, 0, 255 };
        if (editData.contains(key) && editData[key].is_string()) {
            ThemeManager::decodeRGBA(editData[key].get<std::string>(), rgba);
        }
        return ImVec4(rgba[0] / 255.0f, rgba[1] / 255.0f, rgba[2] / 255.0f, rgba[3] / 255.0f);
    }

    static bool drawColorGroup(const ThemeColorGroup& group, bool defaultOpen) {
        bool changed = false;
        if (!ImGui::CollapsingHeader(group.name.c_str(), defaultOpen ? ImGuiTreeNodeFlags_DefaultOpen : 0)) {
            return false;
        }
        for (const auto& col : group.colors) {
            ImVec4 value = getEditColor(col.key);
            if (ImGui::ColorEdit4(("##theme_edit_" + col.key).c_str(), (float*)&value,
                                  ImGuiColorEditFlags_NoInputs | ImGuiColorEditFlags_AlphaBar |
                                      ImGuiColorEditFlags_AlphaPreviewHalf)) {
                editData[col.key] = ThemeManager::encodeRGBA(value);
                changed = true;
            }
            ImGui::SameLine();
            ImGui::TextUnformatted(col.name.c_str());
            if (ImGui::IsItemHovered()) { ImGui::SetTooltip("%s", col.key.c_str()); }
        }
        return changed;
    }

    // Combos and sliders for the parts of the theme that aren't colours. Generic, so a
    // new setting only has to be added to the theme manager's tables to appear here.
    static void drawSpectrumStyle() {
        float menuWidth = ImGui::GetContentRegionAvail().x;
        bool changed = false;

        for (const auto& choice : gui::themeManager.getChoices()) {
            std::string items;
            for (const auto& label : choice.labels) {
                items += label;
                items += '\0';
            }
            ImGui::LeftLabel(choice.name.c_str());
            ImGui::SetNextItemWidth(menuWidth - ImGui::GetCursorPosX());
            changed |= ImGui::Combo(("##theme_choice_" + choice.key).c_str(), choice.value, items.c_str());
        }

        for (const auto& slider : gui::themeManager.getSliders()) {
            ImGui::LeftLabel(slider.name.c_str());
            ImGui::SetNextItemWidth(menuWidth - ImGui::GetCursorPosX());
            changed |= ImGui::SliderFloat(("##theme_slider_" + slider.key).c_str(), slider.value,
                                          slider.min, slider.max, "%.2f");
        }

        if (changed) { saveSpectrumStyle(); }
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
            // Otherwise the editor would still be holding the previous theme, and the
            // next colour tweak would silently put all of it back.
            if (editorOpen) { openEditor(); }
        }

        if (ImGui::Button("Customize##theme_customize", ImVec2(menuWidth / 2 - style::uiScale * 2, 0))) {
            openEditor();
        }
        ImGui::SameLine();
        if (ImGui::Button("Import##theme_import", ImVec2(ImGui::GetContentRegionAvail().x, 0)) && !importOpen) {
            importOpen = true;
            importDialog = new pfd::open_file("Import theme", "", { "Theme files (*.json)", "*.json", "All Files", "*" });
        }

        // The waterfall gradient and the per-VFO colours used to live in the Display
        // menu and in a top level section of their own. They are colour settings, so
        // this is where anyone looking for them will look.
        ImGui::Spacing();
        displaymenu::drawColorMapSelector();

        ImGui::Spacing();
        if (ImGui::CollapsingHeader("Spectrum##theme_spectrum", ImGuiTreeNodeFlags_DefaultOpen)) {
            drawSpectrumStyle();
        }

        ImGui::Spacing();
        if (ImGui::CollapsingHeader("VFO colors##theme_vfo_colors", ImGuiTreeNodeFlags_DefaultOpen)) {
            vfo_color_menu::draw(ctx);
        }
    }

    static void handleDialogs() {
        if (importOpen && importDialog->ready()) {
            importOpen = false;
            std::vector<std::string> paths = importDialog->result();
            if (!paths.empty()) {
                std::string name, error;
                if (gui::themeManager.importTheme(paths[0], name, error)) {
                    rebuildThemeList();
                    auto it = std::find(themeNames.begin(), themeNames.end(), name);
                    if (it != themeNames.end()) {
                        themeId = std::distance(themeNames.begin(), it);
                        applyTheme();
                    }
                    // Show what was imported, both so the result is visible and so it
                    // can be tweaked straight away.
                    openEditor();
                    setStatus("Imported '" + name + "'", false);
                    flog::info("Imported theme '{0}' from {1}", name, paths[0]);
                }
                else {
                    if (!editorOpen) { openEditor(); }
                    setStatus(error, true);
                    flog::error("Could not import theme from {0}: {1}", paths[0], error);
                }
            }
            delete importDialog;
            importDialog = NULL;
        }

        if (exportOpen && exportDialog->ready()) {
            exportOpen = false;
            std::string path = exportDialog->result();
            if (!path.empty()) {
                std::string error;
                if (gui::themeManager.exportTheme(editData, path, error)) {
                    setStatus("Exported to " + path, false);
                }
                else {
                    setStatus(error, true);
                }
            }
            delete exportDialog;
            exportDialog = NULL;
        }
    }

    void drawEditor() {
        handleDialogs();
        if (!editorOpen) { return; }

        ImGui::SetNextWindowSize(ImVec2(420 * style::uiScale, 560 * style::uiScale), ImGuiCond_FirstUseEver);
        if (!ImGui::Begin("Theme editor##theme_editor", &editorOpen)) {
            ImGui::End();
            // Collapsing the window keeps it open; closing it drops the live preview.
            if (!editorOpen) { applyTheme(false); }
            return;
        }

        float fieldWidth = ImGui::GetContentRegionAvail().x - 80 * style::uiScale;

        ImGui::LeftLabel("Name");
        ImGui::SetNextItemWidth(fieldWidth);
        ImGui::InputText("##theme_edit_name", editName, sizeof editName);

        ImGui::LeftLabel("Author");
        ImGui::SetNextItemWidth(fieldWidth);
        ImGui::InputText("##theme_edit_author", editAuthor, sizeof editAuthor);

        ImGui::Separator();

        // Leave room for the separator, the button row and up to two lines of status,
        // measured rather than guessed so it survives a font or UI scale change.
        float reserved = ImGui::GetFrameHeightWithSpacing() + (ImGui::GetTextLineHeightWithSpacing() * 2.0f) +
                         (ImGui::GetStyle().ItemSpacing.y * 2.0f);

        bool changed = false;
        ImGui::BeginChild("##theme_edit_colors", ImVec2(0, -reserved), false);
        for (const auto& group : gui::themeManager.getCustomColorGroups()) {
            changed |= drawColorGroup(group, true);
        }
        for (const auto& group : ThemeManager::getImGuiColorGroups()) {
            changed |= drawColorGroup(group, false);
        }
        ImGui::EndChild();

        if (changed) {
            // Live preview: everything on screen redraws with the new colours from the
            // next frame, so you can judge a waterfall colour against the waterfall.
            gui::themeManager.applyThemeData(editData);
        }

        ImGui::Separator();

        std::string name = editName;
        editData["name"] = name;
        editData["author"] = std::string(editAuthor);
        // Re-read every frame so changing the gradient or the spectrum styling in the
        // menu while the editor is open is picked up by Save and Export.
        std::string liveColorMap = displaymenu::getColorMapName();
        if (!liveColorMap.empty()) { editData[ThemeManager::COLOR_MAP_KEY] = liveColorMap; }
        for (const auto& choice : gui::themeManager.getChoices()) {
            int id = std::clamp(*choice.value, 0, (int)choice.options.size() - 1);
            editData[choice.key] = choice.options[id];
        }
        for (const auto& slider : gui::themeManager.getSliders()) {
            editData[slider.key] = ThemeManager::roundSetting(*slider.value);
        }

        const Theme* existing = gui::themeManager.getTheme(name);
        bool canSave = !name.empty() && (existing == NULL || !existing->readOnly);
        bool canDelete = existing != NULL && !existing->readOnly;

        if (!canSave) { style::beginDisabled(); }
        if (ImGui::Button("Save##theme_edit_save")) {
            std::string error;
            if (gui::themeManager.saveTheme(name, editData, error)) {
                rebuildThemeList();
                auto it = std::find(themeNames.begin(), themeNames.end(), name);
                if (it != themeNames.end()) {
                    themeId = std::distance(themeNames.begin(), it);
                    applyTheme();
                }
                setStatus("Saved '" + name + "'", false);
            }
            else {
                setStatus(error, true);
            }
        }
        if (!canSave) { style::endDisabled(); }

        ImGui::SameLine();
        if (ImGui::Button("Export##theme_edit_export") && !exportOpen) {
            exportOpen = true;
            exportDialog = new pfd::save_file("Export theme", ThemeManager::themeFileName(name),
                                              { "Theme files (*.json)", "*.json", "All Files", "*" });
        }

        ImGui::SameLine();
        if (ImGui::Button("Revert##theme_edit_revert")) {
            // false: the editor never edits the gradient, so reverting must not move it.
            applyTheme(false);
            openEditor();
        }

        ImGui::SameLine();
        if (!canDelete) { style::beginDisabled(); }
        if (ImGui::Button("Delete##theme_edit_delete")) {
            std::string error;
            std::string deleted = name;
            if (gui::themeManager.deleteTheme(name, error)) {
                reloadThemes("");
                applyTheme();
                editorOpen = false;
                flog::info("Deleted theme '{0}'", deleted);
            }
            else {
                setStatus(error, true);
            }
        }
        if (!canDelete) { style::endDisabled(); }

        if (existing != NULL && existing->readOnly) {
            ImGui::TextWrapped("'%s' is built in. Change the name to save your own copy.", name.c_str());
        }
        else if (!editStatus.empty()) {
            if (editStatusIsError) {
                ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 0.4f, 0.4f, 1.0f));
            }
            ImGui::TextWrapped("%s", editStatus.c_str());
            if (editStatusIsError) { ImGui::PopStyleColor(); }
        }

        ImGui::End();

        // Closing without saving throws the live preview away and puts the selected
        // theme back, so an experiment can't leak into the next session. Not the
        // gradient though: that is the menu's control, not the editor's.
        if (!editorOpen) { applyTheme(false); }
    }
}
