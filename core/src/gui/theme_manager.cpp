#include <json.hpp>
#include <gui/theme_manager.h>
#include <imgui_internal.h>
#include <utils/flog.h>
#include <algorithm>
#include <filesystem>
#include <fstream>
#include <cctype>
#include "utils/wstr.h"

ThemeManager::ThemeManager() {
    initCustomColors();
    initSettings();
    fftGradient = defaultGradient();
    for (auto& stop : fftGradient) { stop.id = nextGradientStopId(); }
}

// Dark at the noise floor rising to white at the top, which reads on any theme and is
// an obvious starting point to drag around.
std::vector<GradientStop> ThemeManager::defaultGradient() {
    return {
        { 0.0f, ImVec4(0.04f, 0.09f, 0.20f, 1.0f) },
        { 0.55f, ImVec4(0.12f, 0.59f, 0.98f, 1.0f) },
        { 1.0f, ImVec4(1.0f, 1.0f, 1.0f, 1.0f) },
    };
}

void ThemeManager::normalizeGradient() {
    if (fftGradient.empty()) {
        fftGradient = defaultGradient();
        for (auto& stop : fftGradient) { stop.id = nextGradientStopId(); }
    }
    for (auto& stop : fftGradient) { stop.pos = std::clamp(stop.pos, 0.0f, 1.0f); }
    // Stable, so two stops dragged onto the same position keep the order the user put
    // them in instead of flipping every frame.
    std::stable_sort(fftGradient.begin(), fftGradient.end(),
                     [](const GradientStop& a, const GradientStop& b) { return a.pos < b.pos; });
}

ImVec4 ThemeManager::sampleGradient(float t) const {
    if (fftGradient.empty()) { return ImVec4(0, 0, 0, 0); }
    t = std::clamp(t, 0.0f, 1.0f);

    const GradientStop* prev = &fftGradient.front();
    if (t <= prev->pos) { return prev->color; }

    for (const auto& stop : fftGradient) {
        if (stop.pos < t) {
            prev = &stop;
            continue;
        }
        float span = stop.pos - prev->pos;
        // Two stops at the same position are a hard edge, not a division by zero.
        float f = (span > 0.0f) ? ((t - prev->pos) / span) : 1.0f;
        return ImVec4(prev->color.x + (stop.color.x - prev->color.x) * f,
                      prev->color.y + (stop.color.y - prev->color.y) * f,
                      prev->color.z + (stop.color.z - prev->color.z) * f,
                      prev->color.w + (stop.color.w - prev->color.w) * f);
    }
    return fftGradient.back().color;
}

ImU32 ThemeManager::sampleGradientU32(float t, float alphaMul) const {
    ImVec4 col = sampleGradient(t);
    col.w = std::clamp(col.w * alphaMul, 0.0f, 1.0f);
    return ImGui::ColorConvertFloat4ToU32(col);
}

json ThemeManager::encodeGradient(const std::vector<GradientStop>& stops) {
    json out = json::array();
    for (const auto& stop : stops) {
        json entry = json::object();
        entry["pos"] = roundSetting(stop.pos);
        entry["color"] = encodeRGBA(stop.color);
        out.push_back(entry);
    }
    return out;
}

bool ThemeManager::decodeGradient(const json& val, std::vector<GradientStop>& out) {
    if (!val.is_array() || val.empty()) { return false; }

    std::vector<GradientStop> stops;
    uint8_t rgba[4];
    for (const auto& entry : val) {
        if (!entry.is_object() || !entry.contains("pos") || !entry.contains("color")) { return false; }
        if (!entry["pos"].is_number() || !entry["color"].is_string()) { return false; }
        if (!decodeRGBA(entry["color"].get<std::string>(), rgba)) { return false; }
        GradientStop stop;
        stop.pos = std::clamp(entry["pos"].get<float>(), 0.0f, 1.0f);
        stop.color = ImVec4(rgba[0] / 255.0f, rgba[1] / 255.0f, rgba[2] / 255.0f, rgba[3] / 255.0f);
        stops.push_back(stop);
    }

    out = stops;
    return true;
}

bool ThemeManager::setGradient(const json& val) {
    std::vector<GradientStop> stops;
    if (!decodeGradient(val, stops)) { return false; }
    fftGradient = stops;
    for (auto& stop : fftGradient) { stop.id = nextGradientStopId(); }
    normalizeGradient();
    return true;
}

void ThemeManager::initSettings() {
    choices = {
        { "FFTTraceStyle", "Trace style",
          "The line across the top of the spectrum.\n"
          "Solid: one colour, the ImGui PlotLines colour.\n"
          "Reflection: coloured by the waterfall's colour map, so a peak takes the\n"
          "colour it has in the waterfall below.\n"
          "Gradient: coloured by the theme's own gradient, edited below.",
          { "solid", "reflection", "gradient" },
          { "Solid", "Reflection", "Gradient" },
          &fftTraceStyle, fftTraceStyle },
        { "FFTFillStyle", "Fill style",
          "The area underneath the trace. Same three colour sources, plus None to\n"
          "leave it empty.",
          { "none", "solid", "reflection", "gradient" },
          { "None", "Solid", "Reflection", "Gradient" },
          &fftFillStyle, fftFillStyle },
    };

    sliders = {
        // Not down to zero: a trace nobody can see looks like the spectrum is broken.
        { "FFTTraceIntensity", "Trace intensity", "How opaque the trace line is", &fftTraceIntensity, 0.05f, 1.0f, fftTraceIntensity },
        { "FFTFillIntensity", "Fill intensity", "How opaque the fill under the trace is", &fftFillIntensity, 0.0f, 1.0f, fftFillIntensity },
    };
}

// A float written straight to JSON comes out as 0.20000000298023224, which makes a
// hand edited theme file unpleasant to read for no gain: the sliders are worth three
// decimals at most.
double ThemeManager::roundSetting(float value) {
    return (double)(long long)((value * 1000.0f) + 0.5f) / 1000.0;
}

ThemeChoice* ThemeManager::findChoice(const std::string& key) {
    for (auto& choice : choices) {
        if (choice.key == key) { return &choice; }
    }
    return NULL;
}

ThemeSlider* ThemeManager::findSlider(const std::string& key) {
    for (auto& slider : sliders) {
        if (slider.key == key) { return &slider; }
    }
    return NULL;
}

bool ThemeManager::decodeChoice(const ThemeChoice& choice, const std::string& val, int& out) {
    std::string lower;
    for (char c : val) { lower += (char)std::tolower((unsigned char)c); }
    for (int i = 0; i < (int)choice.options.size(); i++) {
        if (choice.options[i] == lower) {
            out = i;
            return true;
        }
    }
    return false;
}

void ThemeManager::initCustomColors() {
    // The names are what the editor lists, so they say what is drawn rather than
    // which variable holds it. The JSON key is still shown as the tooltip.
    customColorGroups = {
        { "Spectrum and waterfall", {
            { "WaterfallBackground", "Waterfall background", &waterfallBg, waterfallBg },
            { "ClearColor", "Window background", &clearColor, clearColor },
            { "FFTGridColor", "Spectrum grid lines", &fftGridColor, fftGridColor },
            { "FFTBorderColor", "Border around the spectrum and waterfall", &fftBorderColor, fftBorderColor },
            { "FFTHoldColor", "Peak hold trace", &fftHoldColor, fftHoldColor },
            { "FFTCenterMarkerColor", "Centre frequency marker", &fftCenterMarkerColor, fftCenterMarkerColor },
        } },
        { "VFOs and squelch", {
            { "VFOSelectedLineColor", "Centre line of the selected VFO", &vfoSelectedLineColor, vfoSelectedLineColor },
            { "VFOLineColor", "Centre line of the other VFOs", &vfoLineColor, vfoLineColor },
            { "SquelchColor", "Squelch threshold marker", &squelchColor, squelchColor },
            { "ScannerSquelchColor", "Scanner trigger level line", &scannerSquelchColor, scannerSquelchColor },
            { "NotchColor", "Notch filter marker", &notchColor, notchColor },
        } },
        { "Bookmarks", {
            { "BookmarkColor", "Bookmark label and marker line", &bookmarkColor, bookmarkColor },
            { "BookmarkWorkedColor", "Bookmark already worked", &bookmarkWorkedColor, bookmarkWorkedColor },
            { "BookmarkMissingColor", "Bookmark whose radio is not loaded", &bookmarkMissingColor, bookmarkMissingColor },
            { "BookmarkTextColor", "Bookmark name, written on the label", &bookmarkTextColor, bookmarkTextColor },
        } },
        { "Band plan", {
            { "BandPlanTextColor", "Band name", &bandPlanTextColor, bandPlanTextColor },
            { "BandPlanDefaultColor", "Outline of a band with no colour of its own", &bandPlanDefaultColor, bandPlanDefaultColor },
            { "BandPlanDefaultFillColor", "Fill of a band with no colour of its own", &bandPlanDefaultFillColor, bandPlanDefaultFillColor },
        } },
        { "Meters", {
            { "SNRMeterColor", "SNR meter bar", &snrMeterColor, snrMeterColor },
            { "MeterOpenColor", "Level meter bar, over the threshold", &meterOpenColor, meterOpenColor },
            { "VolumeMeterLow", "Audio meter bar, below 0 dB", &volumeMeterLow, volumeMeterLow },
            { "VolumeMeterHigh", "Audio meter bar, clipping", &volumeMeterHigh, volumeMeterHigh },
            { "VolumeMeterPeakLow", "Audio meter peak mark, below 0 dB", &volumeMeterPeakLow, volumeMeterPeakLow },
            { "VolumeMeterPeakHigh", "Audio meter peak mark, clipping", &volumeMeterPeakHigh, volumeMeterPeakHigh },
            { "VolumeMeterBgLow", "Audio meter track, below 0 dB", &volumeMeterBgLow, volumeMeterBgLow },
            { "VolumeMeterBgHigh", "Audio meter track, clipping", &volumeMeterBgHigh, volumeMeterBgHigh },
        } },
        { "Frequency readout", {
            { "FreqSelectUpColor", "Upper half of a digit, which raises it", &freqSelectUpColor, freqSelectUpColor },
            { "FreqSelectDownColor", "Lower half of a digit, which lowers it", &freqSelectDownColor, freqSelectDownColor },
        } },
    };

    customColorsByKey.clear();
    for (auto& group : customColorGroups) {
        for (auto& col : group.colors) {
            customColorsByKey[col.key] = &col;
        }
    }
}

void ThemeManager::setUserThemeDir(std::string path) {
    userThemeDir = path;
}

bool ThemeManager::loadThemesFromDir(std::string path, bool readOnly) {
    if (!std::filesystem::is_directory(path)) {
        flog::error("Theme directory doesn't exist: {0}", path);
        return false;
    }
    for (const auto& file : std::filesystem::directory_iterator(path)) {
        std::string _path = file.path().generic_string();
        if (file.path().extension().generic_string() != ".json") {
            continue;
        }
        loadTheme(_path, readOnly);
    }
    return true;
}

bool ThemeManager::loadTheme(std::string path, bool readOnly) {
    maybeInitThemeManager();

    if (!std::filesystem::is_regular_file(path)) {
        flog::error("Theme file doesn't exist: {0}", path);
        return false;
    }

    // Load defaults in theme
    Theme thm;
    thm.author = "--";
    thm.path = path;
    thm.readOnly = readOnly;

    // Load JSON
    json data;
    try {
        std::ifstream file(wstr::str2wstr(path.c_str()));
        file >> data;
        file.close();
    }
    catch (const std::exception& e) {
        flog::error("Theme {0} could not be parsed: {1}", path, e.what());
        return false;
    }

    if (!data.is_object()) {
        flog::error("Theme {0} is not a JSON object", path);
        return false;
    }

    // Load theme name
    if (!data.contains("name")) {
        flog::error("Theme {0} is missing the name parameter", path);
        return false;
    }
    if (!data["name"].is_string()) {
        flog::error("Theme {0} contains invalid name field. Expected string", path);
        return false;
    }
    std::string name = data["name"];

    // A user theme is allowed to shadow a shipped one of the same name: that's how you
    // keep your own edit of "Dark" across upgrades. The reverse would let a new release
    // silently overwrite the user's copy, so it's refused.
    auto existing = themes.find(name);
    if (existing != themes.end()) {
        if (readOnly || !existing->second.readOnly) {
            flog::error("A theme named '{0}' already exists, ignoring {1}", name, path);
            return false;
        }
        flog::warn("User theme '{0}' overrides the built-in one", name);
    }

    // Load theme author if available
    if (data.contains("author")) {
        if (!data["author"].is_string()) {
            flog::error("Theme {0} contains invalid author field. Expected string", path);
            return false;
        }
        thm.author = data["author"];
    }

    thm.data = sanitizeThemeData(data, path);
    themes[name] = thm;

    return true;
}

// Checks the contents of every colour but only drops the offending key. A theme
// written by a newer version, or one that simply has a typo in it, should still load
// with everything else intact instead of being refused as a whole.
json ThemeManager::sanitizeThemeData(const json& data, const std::string& src) {
    json cleaned = json::object();
    uint8_t ret[4];
    for (auto const& [param, val] : data.items()) {
        if (param == "name" || param == "author") {
            cleaned[param] = val;
            continue;
        }
        if (param == GRADIENT_KEY) {
            std::vector<GradientStop> stops;
            if (!decodeGradient(val, stops)) {
                flog::warn("Theme {0}: field {1} is not a list of gradient stops, ignoring it", src, param);
                continue;
            }
            // Written back sorted and clamped, so the file matches what it will do.
            std::stable_sort(stops.begin(), stops.end(),
                             [](const GradientStop& a, const GradientStop& b) { return a.pos < b.pos; });
            cleaned[param] = encodeGradient(stops);
            continue;
        }
        if (const ThemeSlider* slider = findSlider(param)) {
            if (!val.is_number()) {
                flog::warn("Theme {0}: field {1} is not a number, ignoring it", src, param);
                continue;
            }
            cleaned[param] = roundSetting(std::clamp(val.get<float>(), slider->min, slider->max));
            continue;
        }
        if (const ThemeChoice* choice = findChoice(param)) {
            int dummy;
            if (!val.is_string() || !decodeChoice(*choice, val.get<std::string>(), dummy)) {
                flog::warn("Theme {0} contains invalid {1} field, ignoring it", src, param);
                continue;
            }
            cleaned[param] = val;
            continue;
        }
        if (!val.is_string()) {
            flog::warn("Theme {0}: field {1} is not a string, ignoring it", src, param);
            continue;
        }
        if (param == COLOR_MAP_KEY) {
            // Not a colour, and not checked against the loaded colormaps: those load
            // after the themes do. An unknown name is ignored when it is applied.
            cleaned[param] = val;
            continue;
        }
        if (!customColorsByKey.count(param) && !getImGuiColIds().count(param)) {
            flog::warn("Theme {0} contains unknown {1} field, ignoring it", src, param);
            continue;
        }
        if (!decodeRGBA(val.get<std::string>(), ret)) {
            flog::warn("Theme {0} contains invalid {1} field. Expected hex RGBA color", src, param);
            continue;
        }
        cleaned[param] = val;
    }
    return cleaned;
}

void ThemeManager::resetToDefaults() {
    colorMap.clear();
    fftGradient = defaultGradient();
    for (auto& stop : fftGradient) { stop.id = nextGradientStopId(); }
    for (auto& choice : choices) { *choice.value = choice.def; }
    for (auto& slider : sliders) { *slider.value = slider.def; }
    for (auto& group : customColorGroups) {
        for (auto& col : group.colors) {
            *col.value = col.def;
        }
    }
}

bool ThemeManager::applyThemeData(const json& data) {
    maybeInitThemeManager();

    ImGui::StyleColorsDark();

    auto& style = ImGui::GetStyle();

    style.WindowRounding = 0.0f;
    style.ChildRounding = 0.0f;
    style.FrameRounding = 0.0f;
    style.GrabRounding = 0.0f;
    style.PopupRounding = 0.0f;
    style.ScrollbarRounding = 0.0f;

    // Anything the theme doesn't mention goes back to its default rather than staying
    // at whatever the previously selected theme left behind.
    resetToDefaults();

    if (!data.is_object()) { return false; }

    ImVec4* colors = style.Colors;
    uint8_t ret[4];
    const auto& imguiIds = getImGuiColIds();

    for (auto const& [param, val] : data.items()) {
        if (param == "name" || param == "author") { continue; }
        if (param == GRADIENT_KEY) {
            setGradient(val);
            continue;
        }
        if (ThemeSlider* slider = findSlider(param)) {
            if (val.is_number()) { *slider->value = std::clamp(val.get<float>(), slider->min, slider->max); }
            continue;
        }
        if (ThemeChoice* choice = findChoice(param)) {
            int decoded;
            if (val.is_string() && decodeChoice(*choice, val.get<std::string>(), decoded)) {
                *choice->value = decoded;
            }
            continue;
        }
        if (!val.is_string()) { continue; }
        if (param == COLOR_MAP_KEY) {
            colorMap = val.get<std::string>();
            continue;
        }
        if (!decodeRGBA(val.get<std::string>(), ret)) { continue; }

        ImVec4 col((float)ret[0] / 255.0f, (float)ret[1] / 255.0f, (float)ret[2] / 255.0f, (float)ret[3] / 255.0f);

        auto customIt = customColorsByKey.find(param);
        if (customIt != customColorsByKey.end()) {
            *customIt->second->value = col;
            continue;
        }

        auto imguiIt = imguiIds.find(param);
        if (imguiIt != imguiIds.end()) {
            colors[imguiIt->second] = col;
            continue;
        }
    }

    return true;
}

bool ThemeManager::applyTheme(std::string name) {
    maybeInitThemeManager();

    auto it = themes.find(name);
    if (it == themes.end()) {
        flog::error("Unknown theme: {0}", name);
        return false;
    }

    return applyThemeData(it->second.data);
}

json ThemeManager::dumpLiveTheme(std::string name, std::string author) {
    json data = json::object();
    data["name"] = name;
    data["author"] = author;

    const ImVec4* colors = ImGui::GetStyle().Colors;
    for (auto const& [key, id] : getImGuiColIds()) {
        data[key] = encodeRGBA(colors[id]);
    }
    for (auto const& group : customColorGroups) {
        for (auto const& col : group.colors) {
            data[col.key] = encodeRGBA(*col.value);
        }
    }
    for (auto const& choice : choices) {
        int id = std::clamp(*choice.value, 0, (int)choice.options.size() - 1);
        data[choice.key] = choice.options[id];
    }
    for (auto const& slider : sliders) {
        data[slider.key] = roundSetting(*slider.value);
    }
    data[GRADIENT_KEY] = encodeGradient(fftGradient);
    if (!colorMap.empty()) { data[COLOR_MAP_KEY] = colorMap; }
    return data;
}

std::string ThemeManager::themeFileName(const std::string& name) {
    std::string out;
    for (char c : name) {
        // Theme names are free text but end up as a path, so keep the file name to
        // characters that are safe on every platform we build for.
        if (std::isalnum((unsigned char)c) || c == '-' || c == '_' || c == ' ') {
            out += (char)std::tolower((unsigned char)c);
        }
        else {
            out += '_';
        }
    }
    if (out.empty()) { out = "theme"; }
    return out + ".json";
}

bool ThemeManager::saveTheme(std::string name, const json& data, std::string& error) {
    if (userThemeDir.empty()) {
        error = "No user theme directory configured";
        return false;
    }
    if (name.empty()) {
        error = "The theme needs a name";
        return false;
    }

    auto existing = themes.find(name);
    if (existing != themes.end() && existing->second.readOnly) {
        error = "'" + name + "' is a built-in theme, save it under a different name";
        return false;
    }

    std::error_code ec;
    std::filesystem::create_directories(userThemeDir, ec);
    if (!std::filesystem::is_directory(userThemeDir)) {
        error = "Could not create " + userThemeDir;
        return false;
    }

    json out = data;
    out["name"] = name;

    // Reuse the file the theme already lives in, so saving twice doesn't leave a second
    // copy behind under a differently sanitized name.
    std::string path;
    if (existing != themes.end() && !existing->second.path.empty()) {
        path = existing->second.path;
    }
    else {
        // Two names can sanitize to the same file ("A!" and "A?" both give "a_.json"),
        // which would have the second save quietly clobber the first theme's file.
        std::string base = themeFileName(name);
        path = userThemeDir + "/" + base;
        for (int i = 2; std::filesystem::exists(path); i++) {
            path = userThemeDir + "/" + std::to_string(i) + "_" + base;
        }
    }

    std::ofstream file(wstr::str2wstr(path.c_str()));
    if (!file) {
        error = "Could not write " + path;
        return false;
    }
    file << out.dump(4);
    file.close();

    Theme thm;
    thm.author = out.contains("author") && out["author"].is_string() ? out["author"].get<std::string>() : "--";
    thm.path = path;
    thm.readOnly = false;
    thm.data = out;
    themes[name] = thm;

    return true;
}

bool ThemeManager::deleteTheme(std::string name, std::string& error) {
    auto it = themes.find(name);
    if (it == themes.end()) {
        error = "Unknown theme: " + name;
        return false;
    }
    if (it->second.readOnly) {
        error = "'" + name + "' is a built-in theme and can't be deleted";
        return false;
    }

    std::string path = it->second.path;
    themes.erase(it);

    if (!path.empty()) {
        std::error_code ec;
        std::filesystem::remove(path, ec);
        if (ec) {
            error = "Removed from the list but could not delete " + path;
            return false;
        }
    }

    // A user theme can shadow a shipped one of the same name, so the caller reloads
    // every theme directory afterwards to bring the original back.
    return true;
}

bool ThemeManager::exportTheme(const json& data, std::string path, std::string& error) {
    std::ofstream file(wstr::str2wstr(path.c_str()));
    if (!file) {
        error = "Could not write " + path;
        return false;
    }
    file << data.dump(4);
    file.close();
    return true;
}

bool ThemeManager::importTheme(std::string path, std::string& nameOut, std::string& error) {
    if (userThemeDir.empty()) {
        error = "No user theme directory configured";
        return false;
    }

    json data;
    try {
        std::ifstream file(wstr::str2wstr(path.c_str()));
        if (!file) {
            error = "Could not read " + path;
            return false;
        }
        file >> data;
        file.close();
    }
    catch (const std::exception& e) {
        error = std::string("Not a valid theme file: ") + e.what();
        return false;
    }

    if (!data.is_object() || !data.contains("name") || !data["name"].is_string()) {
        error = "Not a valid theme file: missing the name field";
        return false;
    }

    // Never replace a theme the user already has. Importing "Dark" from a friend gives
    // you "Dark (2)" and both stay selectable.
    std::string name = data["name"];
    std::string candidate = name;
    for (int i = 2; themes.count(candidate); i++) {
        candidate = name + " (" + std::to_string(i) + ")";
    }

    // Same validation as a theme already on disk gets, so keys this build doesn't
    // understand are dropped rather than written straight back out again.
    json cleaned = sanitizeThemeData(data, path);
    cleaned["name"] = candidate;

    if (!saveTheme(candidate, cleaned, error)) { return false; }

    nameOut = candidate;
    return true;
}

bool ThemeManager::decodeRGBA(std::string str, uint8_t out[4]) {
    if (str.length() != 9 || str[0] != '#' || !std::all_of(str.begin() + 1, str.end(), ::isxdigit)) {
        return false;
    }
    out[0] = std::stoi(str.substr(1, 2), NULL, 16);
    out[1] = std::stoi(str.substr(3, 2), NULL, 16);
    out[2] = std::stoi(str.substr(5, 2), NULL, 16);
    out[3] = std::stoi(str.substr(7, 2), NULL, 16);
    return true;
}

std::string ThemeManager::encodeRGBA(const ImVec4& col) {
    auto chan = [](float v) {
        return (int)(std::clamp(v, 0.0f, 1.0f) * 255.0f + 0.5f);
    };
    char buf[16];
    snprintf(buf, sizeof buf, "#%02X%02X%02X%02X", chan(col.x), chan(col.y), chan(col.z), chan(col.w));
    return std::string(buf);
}

std::vector<std::string> ThemeManager::getThemeNames() {
    std::vector<std::string> names;
    for (auto& [name, theme] : themes) { names.push_back(name); }
    return names;
}

bool ThemeManager::themeExists(std::string name) {
    return themes.find(name) != themes.end();
}

const Theme* ThemeManager::getTheme(std::string name) {
    auto it = themes.find(name);
    if (it == themes.end()) { return NULL; }
    return &it->second;
}

static std::vector<ThemeColorGroup> IMGUI_COL_GROUPS;
static std::map<std::string, int> IMGUI_COL_IDS;

const std::map<std::string, int>& ThemeManager::getImGuiColIds() {
    maybeInitThemeManager();
    return IMGUI_COL_IDS;
}

const std::vector<ThemeColorGroup>& ThemeManager::getImGuiColorGroups() {
    maybeInitThemeManager();
    return IMGUI_COL_GROUPS;
}

void ThemeManager::maybeInitThemeManager() {
    if (IMGUI_COL_IDS.size() > 0) { return; }

    // Grouped the way the editor shows them. The ImGui colours have no ImVec4* of
    // their own: they live in ImGuiStyle::Colors, so the editor looks them up by key.
    IMGUI_COL_GROUPS = {
        { "Text & background", {
            { "Text", "Text", NULL, ImVec4() },
            { "TextDisabled", "Disabled text", NULL, ImVec4() },
            { "TextSelectedBg", "Selected text background", NULL, ImVec4() },
            { "WindowBg", "Window background", NULL, ImVec4() },
            { "ChildBg", "Child background", NULL, ImVec4() },
            { "PopupBg", "Popup background", NULL, ImVec4() },
            { "MenuBarBg", "Menu bar background", NULL, ImVec4() },
            { "Border", "Border", NULL, ImVec4() },
            { "BorderShadow", "Border shadow", NULL, ImVec4() },
        } },
        { "Controls", {
            { "FrameBg", "Control background", NULL, ImVec4() },
            { "FrameBgHovered", "Control background (hovered)", NULL, ImVec4() },
            { "FrameBgActive", "Control background (active)", NULL, ImVec4() },
            { "Button", "Button", NULL, ImVec4() },
            { "ButtonHovered", "Button (hovered)", NULL, ImVec4() },
            { "ButtonActive", "Button (active)", NULL, ImVec4() },
            { "CheckMark", "Check mark", NULL, ImVec4() },
            { "SliderGrab", "Slider grab", NULL, ImVec4() },
            { "SliderGrabActive", "Slider grab (active)", NULL, ImVec4() },
            { "Header", "Header", NULL, ImVec4() },
            { "HeaderHovered", "Header (hovered)", NULL, ImVec4() },
            { "HeaderActive", "Header (active)", NULL, ImVec4() },
            { "Separator", "Separator", NULL, ImVec4() },
            { "SeparatorHovered", "Separator (hovered)", NULL, ImVec4() },
            { "SeparatorActive", "Separator (active)", NULL, ImVec4() },
        } },
        { "Windows & tabs", {
            { "TitleBg", "Title bar", NULL, ImVec4() },
            { "TitleBgActive", "Title bar (active)", NULL, ImVec4() },
            { "TitleBgCollapsed", "Title bar (collapsed)", NULL, ImVec4() },
            { "ScrollbarBg", "Scrollbar background", NULL, ImVec4() },
            { "ScrollbarGrab", "Scrollbar grab", NULL, ImVec4() },
            { "ScrollbarGrabHovered", "Scrollbar grab (hovered)", NULL, ImVec4() },
            { "ScrollbarGrabActive", "Scrollbar grab (active)", NULL, ImVec4() },
            { "ResizeGrip", "Resize grip", NULL, ImVec4() },
            { "ResizeGripHovered", "Resize grip (hovered)", NULL, ImVec4() },
            { "ResizeGripActive", "Resize grip (active)", NULL, ImVec4() },
            { "Tab", "Tab", NULL, ImVec4() },
            { "TabHovered", "Tab (hovered)", NULL, ImVec4() },
            { "TabActive", "Tab (active)", NULL, ImVec4() },
            { "TabUnfocused", "Tab (unfocused)", NULL, ImVec4() },
            { "TabUnfocusedActive", "Tab (unfocused, active)", NULL, ImVec4() },
        } },
        { "Plots & tables", {
            { "PlotLines", "FFT trace", NULL, ImVec4() },
            { "PlotLinesHovered", "Plot lines (hovered)", NULL, ImVec4() },
            { "PlotHistogram", "Histogram", NULL, ImVec4() },
            { "PlotHistogramHovered", "Histogram (hovered)", NULL, ImVec4() },
            { "TableHeaderBg", "Table header", NULL, ImVec4() },
            { "TableBorderStrong", "Table border (strong)", NULL, ImVec4() },
            { "TableBorderLight", "Table border (light)", NULL, ImVec4() },
            { "TableRowBg", "Table row", NULL, ImVec4() },
            { "TableRowBgAlt", "Table row (alternate)", NULL, ImVec4() },
        } },
        { "Navigation & overlays", {
            { "DragDropTarget", "Drag and drop target", NULL, ImVec4() },
            { "NavHighlight", "Navigation highlight", NULL, ImVec4() },
            { "NavWindowingHighlight", "Window switch highlight", NULL, ImVec4() },
            { "NavWindowingDimBg", "Window switch dim", NULL, ImVec4() },
            { "ModalWindowDimBg", "Modal dim", NULL, ImVec4() },
        } },
    };

    IMGUI_COL_IDS = {
        { "Text", ImGuiCol_Text },
        { "TextDisabled", ImGuiCol_TextDisabled },
        { "WindowBg", ImGuiCol_WindowBg },
        { "ChildBg", ImGuiCol_ChildBg },
        { "PopupBg", ImGuiCol_PopupBg },
        { "Border", ImGuiCol_Border },
        { "BorderShadow", ImGuiCol_BorderShadow },
        { "FrameBg", ImGuiCol_FrameBg },
        { "FrameBgHovered", ImGuiCol_FrameBgHovered },
        { "FrameBgActive", ImGuiCol_FrameBgActive },
        { "TitleBg", ImGuiCol_TitleBg },
        { "TitleBgActive", ImGuiCol_TitleBgActive },
        { "TitleBgCollapsed", ImGuiCol_TitleBgCollapsed },
        { "MenuBarBg", ImGuiCol_MenuBarBg },
        { "ScrollbarBg", ImGuiCol_ScrollbarBg },
        { "ScrollbarGrab", ImGuiCol_ScrollbarGrab },
        { "ScrollbarGrabHovered", ImGuiCol_ScrollbarGrabHovered },
        { "ScrollbarGrabActive", ImGuiCol_ScrollbarGrabActive },
        { "CheckMark", ImGuiCol_CheckMark },
        { "SliderGrab", ImGuiCol_SliderGrab },
        { "SliderGrabActive", ImGuiCol_SliderGrabActive },
        { "Button", ImGuiCol_Button },
        { "ButtonHovered", ImGuiCol_ButtonHovered },
        { "ButtonActive", ImGuiCol_ButtonActive },
        { "Header", ImGuiCol_Header },
        { "HeaderHovered", ImGuiCol_HeaderHovered },
        { "HeaderActive", ImGuiCol_HeaderActive },
        { "Separator", ImGuiCol_Separator },
        { "SeparatorHovered", ImGuiCol_SeparatorHovered },
        { "SeparatorActive", ImGuiCol_SeparatorActive },
        { "ResizeGrip", ImGuiCol_ResizeGrip },
        { "ResizeGripHovered", ImGuiCol_ResizeGripHovered },
        { "ResizeGripActive", ImGuiCol_ResizeGripActive },
        { "Tab", ImGuiCol_Tab },
        { "TabHovered", ImGuiCol_TabHovered },
        { "TabActive", ImGuiCol_TabActive },
        { "TabUnfocused", ImGuiCol_TabUnfocused },
        { "TabUnfocusedActive", ImGuiCol_TabUnfocusedActive },
        { "PlotLines", ImGuiCol_PlotLines },
        { "PlotLinesHovered", ImGuiCol_PlotLinesHovered },
        { "PlotHistogram", ImGuiCol_PlotHistogram },
        { "PlotHistogramHovered", ImGuiCol_PlotHistogramHovered },
        { "TableHeaderBg", ImGuiCol_TableHeaderBg },
        { "TableBorderStrong", ImGuiCol_TableBorderStrong },
        { "TableBorderLight", ImGuiCol_TableBorderLight },
        { "TableRowBg", ImGuiCol_TableRowBg },
        { "TableRowBgAlt", ImGuiCol_TableRowBgAlt },
        { "TextSelectedBg", ImGuiCol_TextSelectedBg },
        { "DragDropTarget", ImGuiCol_DragDropTarget },
        { "NavHighlight", ImGuiCol_NavHighlight },
        { "NavWindowingHighlight", ImGuiCol_NavWindowingHighlight },
        { "NavWindowingDimBg", ImGuiCol_NavWindowingDimBg },
        { "ModalWindowDimBg", ImGuiCol_ModalWindowDimBg }
    };
}
