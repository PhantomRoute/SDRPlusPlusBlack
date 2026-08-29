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

// How the live spectrum trace is drawn.
enum FFTTraceStyle {
    FFT_TRACE_SOLID = 0,
    // Coloured by the waterfall colour map, so a peak takes the colour it would have in
    // the waterfall below it - the spectrum reflects the waterfall.
    FFT_TRACE_REFLECTION = 1,
    // Coloured by the theme's own gradient, which is edited in the Theme menu and has
    // nothing to do with the waterfall.
    FFT_TRACE_GRADIENT = 2,
};

// How the area under the live spectrum trace is filled.
enum FFTFillStyle {
    FFT_FILL_NONE = 0,
    FFT_FILL_SOLID = 1,
    FFT_FILL_REFLECTION = 2,
    FFT_FILL_GRADIENT = 3,
};

// One colour stop of the theme's spectrum gradient. pos is where it sits on the
// spectrum's vertical range: 0 at the bottom of the FFT area, 1 at the top.
struct GradientStop {
    float pos;
    ImVec4 color;
    // Identity that survives the list being re-sorted, so dragging a stop past its
    // neighbour doesn't hand the drag to a different row. Runtime only, never stored.
    int id = 0;
};

// A theme value that isn't a colour but a choice between named options. Written to the
// theme file by option name rather than by index, so a file stays readable and doesn't
// break if options are ever added in the middle of the list.
struct ThemeChoice {
    std::string key;                   // key used in the theme JSON
    std::string name;                  // label shown in the menu
    std::string desc;                  // shown as a tooltip; the options need explaining
    std::vector<std::string> options;  // stored lowercase, matched case insensitively
    std::vector<std::string> labels;   // what the combo shows, one per option
    int* value;                        // live value the drawing code reads
    int def;
};

// A theme value that is a plain number, drawn as a slider.
struct ThemeSlider {
    std::string key;
    std::string name;
    std::string desc;
    float* value;
    float min;
    float max;
    float def;
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

    // The non-colour theme values, for the menu that draws them. Everything needed to
    // draw, store and load one is in the entry, so adding a setting is a single edit.
    const std::vector<ThemeChoice>& getChoices() { return choices; }
    const std::vector<ThemeSlider>& getSliders() { return sliders; }

    // Case insensitive lookup of an option name. Returns false for a name this build
    // doesn't have, which is what a theme written by a newer version would contain.
    static bool decodeChoice(const ThemeChoice& choice, const std::string& val, int& out);

    // Slider values are only worth a few decimals, and rounding keeps the theme and
    // config files readable instead of full of float noise.
    static double roundSetting(float value);

    static bool decodeRGBA(std::string str, uint8_t out[4]);
    static std::string encodeRGBA(const ImVec4& col);
    static std::string themeFileName(const std::string& name);

    ImVec4 waterfallBg = ImVec4(0.0f, 0.0f, 0.0f, 1.0f);
    ImVec4 fftHoldColor = ImVec4(0.0f, 1.0f, 0.75f, 1.0f);
    ImVec4 squelchColor = ImVec4(1.0f, 0.0f, 0.0f, 1.0f);
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

    // The bookmark labels and markers the frequency manager draws over the spectrum.
    // These were hardcoded where they are drawn, so there was no way to change them.
    ImVec4 bookmarkColor = ImVec4(1.0f, 1.0f, 0.0f, 1.0f);
    ImVec4 bookmarkWorkedColor = ImVec4(0.0f, 1.0f, 0.0f, 1.0f);
    ImVec4 bookmarkMissingColor = ImVec4(1.0f, 80.0f / 255.0f, 80.0f / 255.0f, 1.0f);
    ImVec4 bookmarkTextColor = ImVec4(0.0f, 0.0f, 0.0f, 1.0f);

    ImVec4 snrMeterColor = ImVec4(0.0f, 136.0f / 255.0f, 1.0f, 1.0f);
    // Fill of a level meter once the level has passed the threshold beside it. Shared
    // by the squelch and the scanner so that "this one is open" reads the same
    // wherever it appears.
    ImVec4 meterOpenColor = ImVec4(0.0f, 1.0f, 0.0f, 1.0f);
    ImVec4 volumeMeterBgLow = ImVec4(9.0f / 255.0f, 136.0f / 255.0f, 9.0f / 255.0f, 1.0f);
    ImVec4 volumeMeterBgHigh = ImVec4(136.0f / 255.0f, 9.0f / 255.0f, 9.0f / 255.0f, 1.0f);
    ImVec4 volumeMeterLow = ImVec4(0.0f, 1.0f, 0.0f, 1.0f);
    ImVec4 volumeMeterHigh = ImVec4(1.0f, 0.0f, 0.0f, 1.0f);
    ImVec4 volumeMeterPeakLow = ImVec4(127.0f / 255.0f, 1.0f, 127.0f / 255.0f, 1.0f);
    ImVec4 volumeMeterPeakHigh = ImVec4(1.0f, 127.0f / 255.0f, 127.0f / 255.0f, 1.0f);

    ImVec4 freqSelectUpColor = ImVec4(1.0f, 0.0f, 0.0f, 75.0f / 255.0f);
    ImVec4 freqSelectDownColor = ImVec4(0.0f, 0.0f, 1.0f, 75.0f / 255.0f);

    // The waterfall gradient the theme asks for, by name, or empty if it doesn't name
    // one. Not an ImVec4 like the rest, and not applied here: the colormaps are loaded
    // after the theme is, and the menu owns the config key that remembers the live
    // choice, so thememenu reads this and applies it.
    static constexpr const char* COLOR_MAP_KEY = "ColorMap";
    std::string colorMap;

    // The theme's own spectrum gradient, used by the trace and fill styles that ask for
    // it. Kept sorted by position; sampleGradient relies on that.
    static constexpr const char* GRADIENT_KEY = "FFTGradient";
    std::vector<GradientStop> fftGradient;

    static std::vector<GradientStop> defaultGradient();
    // Sorts and clamps after an edit. Also what makes a hand written theme with its
    // stops in any order behave.
    void normalizeGradient();
    // A fresh id, so a stop added by the editor can be told apart from its neighbours.
    int nextGradientStopId() { return ++lastGradientStopId; }

    // t is 0 at the bottom of the spectrum and 1 at the top. Flat below the first stop
    // and above the last one rather than fading out, so a gradient that doesn't span
    // the full range still fills.
    ImVec4 sampleGradient(float t) const;
    ImU32 sampleGradientU32(float t, float alphaMul) const;

    static json encodeGradient(const std::vector<GradientStop>& stops);
    // Returns false for anything that isn't a usable list of stops, leaving out alone.
    static bool decodeGradient(const json& val, std::vector<GradientStop>& out);
    // Replaces the live gradient from a theme or config value, ids and sorting included.
    // Returns false and keeps the current gradient if the value isn't usable.
    bool setGradient(const json& val);

    // Spectrum trace and fill styling. Part of the theme like the colours are, but not
    // colours, so they live in the choice/slider tables below rather than in a group.
    int fftTraceStyle = FFT_TRACE_SOLID;
    int fftFillStyle = FFT_FILL_SOLID;
    float fftTraceIntensity = 1.0f;
    // 0.2 is what the old hardcoded FFT shadow used, so the default look is unchanged.
    float fftFillIntensity = 0.2f;

private:
    // Drops keys this build doesn't know about, isn't a colour, or isn't valid hex
    // RGBA, keeping everything else. src names the file for the log message.
    json sanitizeThemeData(const json& data, const std::string& src);
    void initCustomColors();
    void initSettings();
    static void maybeInitThemeManager();
    static const std::map<std::string, int>& getImGuiColIds();

    ThemeChoice* findChoice(const std::string& key);
    ThemeSlider* findSlider(const std::string& key);

    std::vector<ThemeChoice> choices;
    std::vector<ThemeSlider> sliders;
    int lastGradientStopId = 0;

    std::vector<ThemeColorGroup> customColorGroups;
    // Flattened index into customColorGroups so applying a theme is a lookup per key
    // rather than a scan of every group.
    std::map<std::string, ThemeColor*> customColorsByKey;

    std::string userThemeDir;
    std::map<std::string, Theme> themes;
};
