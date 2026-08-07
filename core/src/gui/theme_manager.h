#pragma once
#include <map>
#include <mutex>
#include <imgui.h>
#include <vector>
#include <string>
#include <json.hpp>

using nlohmann::json;

struct Theme {
    std::string author;
    // Where the theme came from. Empty for the ImGui built-ins, which have no file.
    std::string path;
    // Themes shipped in the resource directory are read only: saving over one would be
    // undone by the next install, so the editor always makes the user save a copy.
    bool readOnly = false;
    json data;
};

// A colour the theme can override that is not part of ImGuiStyle: the waterfall, the
// spectrum overlays, the meters. Registering one here is all that is needed for it to
// be loadable from a theme file, editable in the theme editor and written back out.
struct ThemeColor {
    std::string key;   // key used in the theme JSON
    std::string name;  // label shown in the editor
    ImVec4* value;     // live value the drawing code reads
    ImVec4 def;        // value used when a theme doesn't mention the key
};

struct ThemeColorGroup {
    std::string name;
    std::vector<ThemeColor> colors;
};

class ThemeManager {
public:
    ThemeManager();

    // Themes from the resource directory are read only, the ones from the user
    // directory are not. The user directory is also where saving and importing put
    // new themes, so it has to be set before either can work.
    bool loadThemesFromDir(std::string path, bool readOnly);
    bool loadTheme(std::string path, bool readOnly);
    void clearThemes() { themes.clear(); }
    void setUserThemeDir(std::string path);
    std::string getUserThemeDir() { return userThemeDir; }

    bool applyTheme(std::string name);
    // Applies theme JSON that isn't (or isn't yet) a registered theme. This is what
    // makes the editor live: every colour tweak is applied through here.
    bool applyThemeData(const json& data);
    // Puts every themable colour back to its built-in default. Called by applyThemeData
    // so a theme that omits a key doesn't inherit it from whatever was selected before.
    void resetToDefaults();

    // The current, live colours as a theme document, so the editor can start from
    // what's on screen even for the ImGui built-ins which have no file.
    json dumpLiveTheme(std::string name, std::string author);

    bool saveTheme(std::string name, const json& data, std::string& error);
    bool deleteTheme(std::string name, std::string& error);
    bool exportTheme(const json& data, std::string path, std::string& error);
    // Reads a theme file from anywhere and copies it into the user theme directory.
    // Renames it if the name is taken so importing never silently replaces a theme.
    bool importTheme(std::string path, std::string& nameOut, std::string& error);

    std::vector<std::string> getThemeNames();
    bool themeExists(std::string name);
    const Theme* getTheme(std::string name);

    static const std::vector<ThemeColorGroup>& getImGuiColorGroups();
    const std::vector<ThemeColorGroup>& getCustomColorGroups() { return customColorGroups; }

    static bool decodeRGBA(std::string str, uint8_t out[4]);
    static std::string encodeRGBA(const ImVec4& col);
    static std::string themeFileName(const std::string& name);

    ImVec4 waterfallBg = ImVec4(0.0f, 0.0f, 0.0f, 1.0f);
    ImVec4 fftHoldColor = ImVec4(0.0f, 1.0f, 0.75f, 1.0f);
    ImVec4 squelchColor = ImVec4(1.0f, 0.0f, 0.0f, 1.0f);
    ImVec4 scannerSquelchColor = ImVec4(0.0f, 1.0f, 0.0f, 1.0f);
    ImVec4 clearColor = ImVec4(0.0666f, 0.0666f, 0.0666f, 1.0f);

    // Spectrum overlays that used to be hardcoded in the drawing code.
    ImVec4 fftGridColor = ImVec4(50.0f / 255.0f, 50.0f / 255.0f, 50.0f / 255.0f, 1.0f);
    ImVec4 fftBorderColor = ImVec4(50.0f / 255.0f, 50.0f / 255.0f, 50.0f / 255.0f, 1.0f);
    ImVec4 fftCenterMarkerColor = ImVec4(1.0f, 1.0f, 1.0f, 1.0f);
    ImVec4 vfoSelectedLineColor = ImVec4(1.0f, 0.0f, 0.0f, 1.0f);
    ImVec4 vfoLineColor = ImVec4(1.0f, 1.0f, 0.0f, 1.0f);
    ImVec4 notchColor = ImVec4(1.0f, 0.0f, 0.0f, 127.0f / 255.0f);

    ImVec4 bandPlanTextColor = ImVec4(1.0f, 1.0f, 1.0f, 1.0f);
    ImVec4 bandPlanDefaultColor = ImVec4(1.0f, 1.0f, 1.0f, 1.0f);
    ImVec4 bandPlanDefaultFillColor = ImVec4(1.0f, 1.0f, 1.0f, 100.0f / 255.0f);

    ImVec4 snrMeterColor = ImVec4(0.0f, 136.0f / 255.0f, 1.0f, 1.0f);
    ImVec4 volumeMeterBgLow = ImVec4(9.0f / 255.0f, 136.0f / 255.0f, 9.0f / 255.0f, 1.0f);
    ImVec4 volumeMeterBgHigh = ImVec4(136.0f / 255.0f, 9.0f / 255.0f, 9.0f / 255.0f, 1.0f);
    ImVec4 volumeMeterLow = ImVec4(0.0f, 1.0f, 0.0f, 1.0f);
    ImVec4 volumeMeterHigh = ImVec4(1.0f, 0.0f, 0.0f, 1.0f);
    ImVec4 volumeMeterPeakLow = ImVec4(127.0f / 255.0f, 1.0f, 127.0f / 255.0f, 1.0f);
    ImVec4 volumeMeterPeakHigh = ImVec4(1.0f, 127.0f / 255.0f, 127.0f / 255.0f, 1.0f);

    ImVec4 freqSelectUpColor = ImVec4(1.0f, 0.0f, 0.0f, 75.0f / 255.0f);
    ImVec4 freqSelectDownColor = ImVec4(0.0f, 0.0f, 1.0f, 75.0f / 255.0f);

private:
    // Drops keys this build doesn't know about, isn't a colour, or isn't valid hex
    // RGBA, keeping everything else. src names the file for the log message.
    json sanitizeThemeData(const json& data, const std::string& src);
    void initCustomColors();
    static void maybeInitThemeManager();
    static const std::map<std::string, int>& getImGuiColIds();

    std::vector<ThemeColorGroup> customColorGroups;
    // Flattened index into customColorGroups so applying a theme is a lookup per key
    // rather than a scan of every group.
    std::map<std::string, ThemeColor*> customColorsByKey;

    std::string userThemeDir;
    std::map<std::string, Theme> themes;
};
