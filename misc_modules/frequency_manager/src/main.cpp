#include <imgui.h>
#include <utils/flog.h>
#include <module.h>
#include <gui/gui.h>
#include <gui/style.h>
#include <core.h>
#include <radio_interface.h>
#include <signal_path/signal_path.h>
#include <vector>
#include <gui/tuner.h>
#include <gui/file_dialogs.h>
#include <utils/freq_formatting.h>
#include <gui/dialogs/dialog_box.h>
#include <fstream>
#include <unordered_map>
#include "utils/wstr.h"
#include "frequency_manager.h"
#include "scanner.h"
#include "../../radio/src/radio_module_interface.h"
#include "../../radio/src/tone_tables.h"
#include <algorithm>
#include <cmath>

SDRPP_MOD_INFO{
    /* Name:            */ "frequency_manager",
    /* Description:     */ "Frequency manager module for SDR++",
    /* Author:          */ "Ryzerth;Zimm",
    /* Version:         */ 0, 3, 0,
    /* Max instances    */ 1
};

ConfigManager config;

std::unordered_map<int, std::string> demodModeList;
std::unordered_map<std::string, int> demodModeListRev;
std::string demodModeListTxt;

// demodModeList only holds the modes currently registered, so a bookmark saved
// against a module that is since disabled - the DSD modes come and go with
// ch_extravhf_decoder - will miss. Looking that up with operator[] inserts a blank
// entry into the shared map while drawing and shows an empty mode; say what
// actually happened instead.
static std::string demodModeName(int demodId) {
    auto it = demodModeList.find(demodId);
    return (it != demodModeList.end()) ? it->second : "Unknown";
}

enum {
    BOOKMARK_DISP_MODE_OFF,
    BOOKMARK_DISP_MODE_TOP,
    BOOKMARK_DISP_MODE_BOTTOM,
    _BOOKMARK_DISP_MODE_COUNT
};

const char* bookmarkDisplayModesTxt = "Off\0Top\0Bottom\0";


ConfigManager &getFrequencyManagerConfig() {
    return config;
}

class FrequencyManagerModule : public ModuleManager::Instance, public TransientBookmarkManager {
public:
    FrequencyManagerModule(std::string name) {
        this->name = name;


        fftRedrawHandler.ctx = this;
        fftRedrawHandler.handler = fftRedraw;
        inputHandler.ctx = this;
        inputHandler.handler = fftInput;
        // The scanner has to keep ticking whether or not this menu is on screen.
        // See scannerTick.
        scannerTickHandler.ctx = this;
        scannerTickHandler.handler = scannerTick;

        gui::menu.registerEntry(name, menuHandler, this, NULL);
        gui::waterfall.onFFTRedraw.bindHandler(&fftRedrawHandler);
        gui::waterfall.onInputProcess.bindHandler(&inputHandler);
        gui::mainWindow.onWaterfallDrawn.bindHandler(&scannerTickHandler);
    }

    // The menu handler only runs while the Frequency Manager section is expanded,
    // because the core only calls it inside its CollapsingHeader. Driving the
    // scanner from there meant collapsing the menu froze it mid scan - no timers,
    // no station changes, and the squelch left muted or unmuted wherever it
    // happened to be. onWaterfallDrawn fires once a frame regardless, so tick it
    // from there and leave only the drawing to the menu.
    static void scannerTick(ImGuiContext* ctx, void* c) {
        FrequencyManagerModule* _this = (FrequencyManagerModule*)c;
        _this->scanner.update(ImGui::GetIO().DeltaTime);
    }

    void *getInterface(const char *name) override {
        if (!strcmp(name, "TransientBookmarkManager")) {
            return (TransientBookmarkManager*)this;
        }
        return nullptr;
    }

    const char *getModesList() override {
        return demodModeListTxt.c_str();
    }

    ~FrequencyManagerModule() {
        gui::menu.removeEntry(name);
        gui::waterfall.onFFTRedraw.unbindHandler(&fftRedrawHandler);
        gui::waterfall.onInputProcess.unbindHandler(&inputHandler);
        gui::mainWindow.onWaterfallDrawn.unbindHandler(&scannerTickHandler);
    }

    void postInit() override {
        config.acquire();
        std::string selList = config.conf["selectedList"];
        bookmarkDisplayMode = config.conf["bookmarkDisplayMode"];
        config.release();

        refreshLists();
        loadByName(selList);
        refreshWaterfallBookmarks(true);
    }

    void enable() override {
        enabled = true;
    }

    void disable() override {
        enabled = false;
    }

    bool isEnabled() override {
        return enabled;
    }

    std::string handleDebugCommand(const std::string& cmd, const std::string& args) override {
        if (cmd == "get_lists") {
            json lists = json::array();
            for (const auto& listName : listNames) {
                lists.push_back(listName);
            }
            return json{{"lists", lists}}.dump();
        }
        if (cmd == "get_current_list") {
            return json{{"current_list", selectedListName}}.dump();
        }
        if (cmd == "set_current_list") {
            if (std::find(listNames.begin(), listNames.end(), args) != listNames.end()) {
                loadByName(args);
                config.acquire();
                config.conf["selectedList"] = selectedListName;
                config.release(true);
                return json{{"status", "ok"}, {"current_list", selectedListName}}.dump();
            }
            return json{{"error", "list not found: " + args}}.dump();
        }
        if (cmd == "get_bookmarks") {
            json bms = json::array();
            auto radio = (RadioModuleInterface *)core::moduleManager.getInterface(gui::waterfall.selectedVFO, "RadioModuleInterface");
            for (const auto& [name, bm] : bookmarks) {
                json bookmark;
                bookmark["name"] = name;
                bookmark["frequency"] = bm.frequency;
                bookmark["bandwidth"] = bm.bandwidth;
                if (bm.modeIndex < 0) {
                    bookmark["mode"] = "Unspecified";
                } else if (radio) {
                    DemodID demodId = radio->getDemodByIndex(bm.modeIndex);
                    bookmark["mode"] = demodModeList.count(demodId) ?
                        demodModeList[demodId] : "Unknown";
                } else {
                    bookmark["mode"] = "Unknown";
                }
                bookmark["mode_index"] = bm.modeIndex;
                bookmark["vfo"] = bm.vfoName;
                bookmark["vfo_available"] = bm.vfoName.empty() || sigpath::vfoManager.vfoExists(bm.vfoName);
                bms.push_back(bookmark);
            }
            return json{{"bookmarks", bms}, {"list", selectedListName}}.dump();
        }
        if (cmd == "add_bookmark") {
            size_t pos1 = args.find('|');
            if (pos1 == std::string::npos) {
                return json{{"error", "invalid format. Use: name|frequency|bandwidth|mode"}}.dump();
            }
            size_t pos2 = args.find('|', pos1 + 1);
            if (pos2 == std::string::npos) {
                return json{{"error", "invalid format. Use: name|frequency|bandwidth|mode"}}.dump();
            }
            size_t pos3 = args.find('|', pos2 + 1);
            if (pos3 == std::string::npos) {
                return json{{"error", "invalid format. Use: name|frequency|bandwidth|mode"}}.dump();
            }
            std::string bmName = args.substr(0, pos1);
            std::string freqStr = args.substr(pos1 + 1, pos2 - pos1 - 1);
            std::string bwStr = args.substr(pos2 + 1, pos3 - pos2 - 1);
            std::string modeStr = args.substr(pos3 + 1);
            if (bmName.empty()) {
                return json{{"error", "bookmark name cannot be empty"}}.dump();
            }
            if (bookmarks.find(bmName) != bookmarks.end()) {
                return json{{"error", "bookmark already exists: " + bmName}}.dump();
            }
            try {
                double frequency = std::stod(freqStr);
                double bandwidth = std::stod(bwStr);
                int modeIndex = 0;
                auto radio = (RadioModuleInterface *)core::moduleManager.getInterface(gui::waterfall.selectedVFO, "RadioModuleInterface");
                if (radio) {
                    updateModeList(radio);
                }
                try {
                    modeIndex = std::stoi(modeStr);
                } catch (...) {
                    if (demodModeListRev.find(modeStr) != demodModeListRev.end()) {
                        modeIndex = demodModeListRev[modeStr];
                    } else {
                        return json{{"error", "unknown mode: " + modeStr}}.dump();
                    }
                }
                FrequencyBookmark fbm;
                fbm.frequency = frequency;
                fbm.bandwidth = bandwidth;
                fbm.modeIndex = modeIndex;
                fbm.selected = false;
                fbm.vfoName = gui::waterfall.selectedVFO;
                bookmarks[bmName] = fbm;
                saveByName(selectedListName);
                return json{{"status", "ok"}, {"name", bmName}, {"frequency", frequency}, {"bandwidth", bandwidth}, {"mode_index", modeIndex}}.dump();
            } catch (const std::exception& e) {
                return json{{"error", std::string("parse error: ") + e.what()}}.dump();
            }
        }
        if (cmd == "remove_bookmark") {
            if (args.empty()) {
                return json{{"error", "bookmark name required"}}.dump();
            }
            auto it = bookmarks.find(args);
            if (it == bookmarks.end()) {
                return json{{"error", "bookmark not found: " + args}}.dump();
            }
            bookmarks.erase(it);
            saveByName(selectedListName);
            return json{{"status", "ok"}, {"removed", args}}.dump();
        }
        if (cmd == "apply_bookmark") {
            if (args.empty()) {
                return json{{"error", "bookmark name required"}}.dump();
            }
            auto it = bookmarks.find(args);
            if (it == bookmarks.end()) {
                return json{{"error", "bookmark not found: " + args}}.dump();
            }
            FrequencyBookmark& bm = it->second;
            std::string targetVfo = bm.vfoName.empty() ? gui::waterfall.selectedVFO : bm.vfoName;
            applyBookmark(bm, gui::waterfall.selectedVFO);
            bm.selected = false;
            return json{{"status", "ok"}, {"applied", args}, {"frequency", bm.frequency}, {"bandwidth", bm.bandwidth}, {"vfo", targetVfo.empty() ? "center" : targetVfo}}.dump();
        }
        if (cmd == "get_scanner_status") {
            return json{{"scanning", scanner.isScanning()}, {"current_station", scanner.getCurrentStation()}, {"bookmark_count", (int)bookmarks.size()}}.dump();
        }
        if (cmd == "start_scanner") {
            if (bookmarks.empty()) {
                return json{{"error", "no bookmarks to scan"}}.dump();
            }
            scanner.startScanner();
            return json{{"status", "ok"}, {"scanning", true}}.dump();
        }
        if (cmd == "stop_scanner") {
            scanner.stopScanner();
            return json{{"status", "ok"}, {"scanning", false}}.dump();
        }
        return json{{"error", "unknown command: " + cmd}}.dump();
    }

private:
    // Combo strings for the tone pickers, built once from the shared tables.
    static const char* ctcssToneNames() {
        static const std::string names = [] {
            std::string s;
            char buf[16];
            for (int i = 0; i < tonedetect::CTCSS_TONE_COUNT; i++) {
                snprintf(buf, sizeof buf, "%.1f Hz", tonedetect::CTCSS_TONES[i]);
                s += buf;
                s += '\0';
            }
            s += '\0';
            return s;
        }();
        return names.c_str();
    }

    static const std::vector<int>& dcsCodes() {
        static const std::vector<int> codes = tonedetect::dcsCodeList();
        return codes;
    }

    static const char* dcsCodeNames() {
        static const std::string names = [] {
            std::string s;
            char buf[16];
            for (int c : dcsCodes()) {
                snprintf(buf, sizeof buf, "D%03d", c);
                s += buf;
                s += '\0';
            }
            s += '\0';
            return s;
        }();
        return names.c_str();
    }

    // The tone rows of the edit dialog. Only shown for NFM, since that is the only
    // demodulator the radio will accept tone settings for - offering them against
    // an SSB bookmark would just be a control that does nothing.
    void drawToneRows() {
        // The bookmark's own radio when it names one, since that is what recalling it
        // will drive; only fall back to the selected VFO for a legacy bookmark.
        std::string vfo = editedBookmark.vfoName.empty() ? gui::waterfall.selectedVFO : editedBookmark.vfoName;
        auto radio = (RadioModuleInterface*)core::moduleManager.getInterface(vfo, "RadioModuleInterface");
        if (radio == nullptr || editedBookmark.modeIndex < 0) { return; }
        if (radio->getDemodByIndex(editedBookmark.modeIndex) != RADIO_DEMOD_NFM) { return; }

        // Showing the rows is what makes this bookmark one that carries tone
        // settings, so the flag is set here rather than waiting for the round trip
        // through the config file - otherwise a bookmark just added would not apply
        // its own tone until the list was next reloaded.
        editedBookmark.hasTone = true;

        RadioToneSettings& t = editedBookmark.tone;

        ImGui::TableNextRow();
        ImGui::TableSetColumnIndex(0);
        ImGui::LeftLabel("Tone squelch");
        ImGui::TableSetColumnIndex(1);
        ImGui::SetNextItemWidth(200);
        if (ImGui::Checkbox(("##freq_manager_edit_tonesq" + name).c_str(), &t.squelchEnabled)) {
            if (t.squelchEnabled && t.mode == 0) { t.mode = 1; }
        }

        if (t.squelchEnabled) {
            ImGui::TableNextRow();
            ImGui::TableSetColumnIndex(0);
            ImGui::LeftLabel("Type");
            ImGui::TableSetColumnIndex(1);
            ImGui::SetNextItemWidth(200);
            int modeSel = (t.mode == 2) ? 1 : 0;
            if (ImGui::Combo(("##freq_manager_edit_tonetype" + name).c_str(), &modeSel, "CTCSS\0DCS\0")) {
                t.mode = modeSel ? 2 : 1;
            }

            ImGui::TableNextRow();
            ImGui::TableSetColumnIndex(0);
            if (t.mode == 2) {
                ImGui::LeftLabel("Code");
                ImGui::TableSetColumnIndex(1);
                ImGui::SetNextItemWidth(110);
                const auto& codes = dcsCodes();
                auto it = std::find(codes.begin(), codes.end(), t.dcsCode);
                int codeId = (it == codes.end()) ? 0 : (int)std::distance(codes.begin(), it);
                if (ImGui::Combo(("##freq_manager_edit_dcs" + name).c_str(), &codeId, dcsCodeNames())) {
                    t.dcsCode = codes[codeId];
                }
                ImGui::SameLine();
                int pol = t.dcsInverted ? 1 : 0;
                ImGui::SetNextItemWidth(85);
                if (ImGui::Combo(("##freq_manager_edit_dcspol" + name).c_str(), &pol, "Normal\0Invert\0")) {
                    t.dcsInverted = (pol != 0);
                }
            }
            else {
                ImGui::LeftLabel("Tone");
                ImGui::TableSetColumnIndex(1);
                ImGui::SetNextItemWidth(200);
                int toneId = 0;
                for (int i = 0; i < tonedetect::CTCSS_TONE_COUNT; i++) {
                    if (std::fabs(tonedetect::CTCSS_TONES[i] - t.ctcssFreq) < 0.05f) { toneId = i; break; }
                }
                if (ImGui::Combo(("##freq_manager_edit_ctcss" + name).c_str(), &toneId, ctcssToneNames())) {
                    t.ctcssFreq = tonedetect::CTCSS_TONES[toneId];
                }
            }
        }

        ImGui::TableNextRow();
        ImGui::TableSetColumnIndex(0);
        ImGui::LeftLabel("Remove tone");
        ImGui::TableSetColumnIndex(1);
        ImGui::SetNextItemWidth(200);
        ImGui::Checkbox(("##freq_manager_edit_tonefilt" + name).c_str(), &t.filterEnabled);

        ImGui::TableNextRow();
        ImGui::TableSetColumnIndex(0);
        ImGui::LeftLabel("Identify tone");
        ImGui::TableSetColumnIndex(1);
        ImGui::SetNextItemWidth(200);
        ImGui::Checkbox(("##freq_manager_edit_toneid" + name).c_str(), &t.identifyEnabled);
    }

    bool bookmarkEditDialog() {
        bool open = true;
        gui::mainWindow.lockWaterfallControls = true;

        std::string id = "Edit##freq_manager_edit_popup_" + name;
        ImGui::OpenPopup(id.c_str());

        char nameBuf[1024];
        strcpy(nameBuf, editedBookmarkName.c_str());

        if (ImGui::BeginPopup(id.c_str(), ImGuiWindowFlags_NoResize)) {
            ImGui::BeginTable(("freq_manager_edit_table" + name).c_str(), 2);

            ImGui::TableNextRow();
            ImGui::TableSetColumnIndex(0);
            ImGui::LeftLabel("Name");
            ImGui::TableSetColumnIndex(1);
            ImGui::SetNextItemWidth(200);
            if (ImGui::InputText(("##freq_manager_edit_name" + name).c_str(), nameBuf, 1023)) {
                editedBookmarkName = nameBuf;
            }

            ImGui::TableNextRow();
            ImGui::TableSetColumnIndex(0);
            ImGui::LeftLabel("Frequency");
            ImGui::TableSetColumnIndex(1);
            ImGui::SetNextItemWidth(200);
            ImGui::InputDouble(("##freq_manager_edit_freq" + name).c_str(), &editedBookmark.frequency);

            ImGui::TableNextRow();
            ImGui::TableSetColumnIndex(0);
            ImGui::LeftLabel("Bandwidth");
            ImGui::TableSetColumnIndex(1);
            ImGui::SetNextItemWidth(200);
            ImGui::InputDouble(("##freq_manager_edit_bw" + name).c_str(), &editedBookmark.bandwidth);

            ImGui::TableNextRow();
            ImGui::TableSetColumnIndex(0);
            ImGui::LeftLabel("Mode");
            ImGui::TableSetColumnIndex(1);
            ImGui::SetNextItemWidth(200);

            ImGui::Combo(("##freq_manager_edit_mode" + name).c_str(), &editedBookmark.modeIndex, demodModeListTxt.c_str());

            drawToneRows();

            ImGui::EndTable();

            bool applyDisabled = (strlen(nameBuf) == 0) || (bookmarks.find(editedBookmarkName) != bookmarks.end() && editedBookmarkName != firstEditedBookmarkName);
            if (applyDisabled) { style::beginDisabled(); }
            if (ImGui::Button("Apply")) {
                open = false;

                // If editing, delete the original one
                if (editOpen) {
                    bookmarks.erase(firstEditedBookmarkName);
                }
                bookmarks[editedBookmarkName] = editedBookmark;

                saveByName(selectedListName);
            }
            if (applyDisabled) { style::endDisabled(); }
            ImGui::SameLine();
            if (ImGui::Button("Cancel")) {
                open = false;
            }
            ImGui::EndPopup();
        }
        return open;
    }

    bool newListDialog() {
        bool open = true;
        gui::mainWindow.lockWaterfallControls = true;

        float menuWidth = ImGui::GetContentRegionAvail().x;

        std::string id = "New##freq_manager_new_popup_" + name;
        ImGui::OpenPopup(id.c_str());

        char nameBuf[1024];
        strcpy(nameBuf, editedListName.c_str());

        if (ImGui::BeginPopup(id.c_str(), ImGuiWindowFlags_NoResize)) {
            ImGui::LeftLabel("Name");
            ImGui::SetNextItemWidth(menuWidth - ImGui::GetCursorPosX());
            if (ImGui::InputText(("##freq_manager_edit_name" + name).c_str(), nameBuf, 1023)) {
                editedListName = nameBuf;
            }

            bool alreadyExists = (std::find(listNames.begin(), listNames.end(), editedListName) != listNames.end());

            if (strlen(nameBuf) == 0 || alreadyExists) { style::beginDisabled(); }
            if (ImGui::Button("Apply")) {
                open = false;

                config.acquire();
                if (renameListOpen) {
                    config.conf["lists"][editedListName] = config.conf["lists"][firstEditedListName];
                    config.conf["lists"].erase(firstEditedListName);
                }
                else {
                    config.conf["lists"][editedListName]["showOnWaterfall"] = true;
                    config.conf["lists"][editedListName]["bookmarks"] = json::object();
                }
                refreshWaterfallBookmarks(false);
                config.release(true);
                refreshLists();
                loadByName(editedListName);
            }
            if (strlen(nameBuf) == 0 || alreadyExists) { style::endDisabled(); }
            ImGui::SameLine();
            if (ImGui::Button("Cancel")) {
                open = false;
            }
            ImGui::EndPopup();
        }
        return open;
    }

    bool selectListsDialog() {
        gui::mainWindow.lockWaterfallControls = true;

        float menuWidth = ImGui::GetContentRegionAvail().x;

        std::string id = "Select lists##freq_manager_sel_popup_" + name;
        ImGui::OpenPopup(id.c_str());

        bool open = true;

        if (ImGui::BeginPopup(id.c_str(), ImGuiWindowFlags_NoResize)) {
            // No need to lock config since we're not modifying anything and there's only one instance
            for (auto [listName, list] : config.conf["lists"].items()) {
                bool shown = list["showOnWaterfall"];
                if (ImGui::Checkbox((listName + "##freq_manager_sel_list_").c_str(), &shown)) {
                    config.acquire();
                    config.conf["lists"][listName]["showOnWaterfall"] = shown;
                    refreshWaterfallBookmarks(false);
                    config.release(true);
                }
            }

            if (ImGui::Button("Ok")) {
                open = false;
            }
            ImGui::EndPopup();
        }
        return open;
    }

    void refreshLists() {
        listNames.clear();
        listNamesTxt = "";

        config.acquire();
        for (auto [_name, list] : config.conf["lists"].items()) {
            listNames.push_back(_name);
            listNamesTxt += _name;
            listNamesTxt += '\0';
        }
        config.release();
    }

    void refreshWaterfallBookmarks(bool lockConfig) override {
        auto radio = (RadioModuleInterface *)core::moduleManager.getInterface(gui::waterfall.selectedVFO, "RadioModuleInterface");
        if (lockConfig) { config.acquire(); }
        waterfallBookmarks.clear();
        for (auto [listName, list] : config.conf["lists"].items()) {
            if (!((bool)list["showOnWaterfall"])) { continue; }
            WaterfallBookmark wbm;
            wbm.listName = listName;
            for (auto [bookmarkName, bm] : config.conf["lists"][listName]["bookmarks"].items()) {
                wbm.bookmarkName = bookmarkName;
                wbm.bookmark.frequency = config.conf["lists"][listName]["bookmarks"][bookmarkName]["frequency"];
                wbm.bookmark.bandwidth = config.conf["lists"][listName]["bookmarks"][bookmarkName]["bandwidth"];
                int mode = config.conf["lists"][listName]["bookmarks"][bookmarkName]["mode"];
                wbm.bookmark.modeIndex = (radio != nullptr) ? radio->getDemodIndex(mode) : -1;
                wbm.bookmark.vfoName = config.conf["lists"][listName]["bookmarks"][bookmarkName].value("vfo", "");
                wbm.bookmark.hasTone = hasTone(config.conf["lists"][listName]["bookmarks"][bookmarkName]);
                wbm.bookmark.tone = loadTone(config.conf["lists"][listName]["bookmarks"][bookmarkName]);
                wbm.bookmark.selected = false;
                wbm.notValidAfter = 0;
                wbm.extraInfo = "";
                wbm.worked = false;
                waterfallBookmarks.push_back(wbm);
            }
        }
        auto ctm = currentTimeMillis();
        for (auto &tr: transientBookmarks) {
            if (ctm < tr.notValidAfter) {
                waterfallBookmarks.push_back(tr);
            }
        }
        if (lockConfig) { config.release(); }
    }

    void loadFirst() {
        if (listNames.size() > 0) {
            loadByName(listNames[0]);
            return;
        }
        selectedListName = "";
        selectedListId = 0;
    }

    void loadByName(std::string listName) {
        bookmarks.clear();
        if (std::find(listNames.begin(), listNames.end(), listName) == listNames.end()) {
            selectedListName = "";
            selectedListId = 0;
            loadFirst();
            return;
        }
        selectedListId = std::distance(listNames.begin(), std::find(listNames.begin(), listNames.end(), listName));
        selectedListName = listName;
        auto radio = (RadioModuleInterface *)core::moduleManager.getInterface(gui::waterfall.selectedVFO, "RadioModuleInterface");
        config.acquire();
        for (auto [bmName, bm] : config.conf["lists"][listName]["bookmarks"].items()) {
            FrequencyBookmark fbm;
            fbm.frequency = bm["frequency"];
            fbm.bandwidth = bm["bandwidth"];
            fbm.modeIndex = (radio != nullptr) ? radio->getDemodIndex(bm["mode"]) : -1;
            fbm.vfoName = bm.value("vfo", "");
            fbm.hasTone = hasTone(bm);
            fbm.tone = loadTone(bm);
            fbm.selected = false;
            bookmarks[bmName] = fbm;
        }
        config.release();
    }

    // Written for every NFM bookmark and no other, so one saved by this version
    // always says what it wants - including "everything off". The alternative,
    // writing it only when something is switched on, makes "no tone" and "saved
    // before tones existed" the same thing on disk, and then recalling any old
    // bookmark would quietly clear whatever the radio was set to.
    static void saveTone(json& bm, const RadioToneSettings& t) {
        bm["tone"]["squelch"] = t.squelchEnabled;
        bm["tone"]["mode"] = t.mode;
        bm["tone"]["ctcss"] = t.ctcssFreq;
        bm["tone"]["dcsCode"] = t.dcsCode;
        bm["tone"]["dcsInverted"] = t.dcsInverted;
        bm["tone"]["filter"] = t.filterEnabled;
        bm["tone"]["identify"] = t.identifyEnabled;
    }

    // Absent on every bookmark saved before tones existed, hence the flag: those are
    // recalled without touching the radio's tone settings. Individual keys are
    // optional too, so a block written by a future version that drops one still
    // loads.
    static bool hasTone(const json& bm) {
        return bm.contains("tone") && bm["tone"].is_object();
    }

    static RadioToneSettings loadTone(const json& bm) {
        RadioToneSettings t;
        if (!hasTone(bm)) { return t; }
        const json& j = bm["tone"];
        t.squelchEnabled = j.value("squelch", false);
        t.mode = j.value("mode", 0);
        t.ctcssFreq = j.value("ctcss", 100.0f);
        t.dcsCode = j.value("dcsCode", 23);
        t.dcsInverted = j.value("dcsInverted", false);
        t.filterEnabled = j.value("filter", false);
        t.identifyEnabled = j.value("identify", false);
        return t;
    }

    void updateModeList(RadioModuleInterface *radio) {
        demodModeList.clear();
        demodModeListRev.clear();
        demodModeListTxt = "";
        if (radio == nullptr) { return; } // VFO without RadioModuleInterface (e.g. TETRA Demodulator)
        for (auto m : radio->radioModes) {
            demodModeList[m.second] = m.first;
            demodModeListRev[m.first] = m.second;
            demodModeListTxt += m.first;
            demodModeListTxt += std::string("\0", 1);
        }
    }

    void saveByName(std::string listName) {
        auto radio = (RadioModuleInterface *)core::moduleManager.getInterface(gui::waterfall.selectedVFO, "RadioModuleInterface");
        config.acquire();
        config.conf["lists"][listName]["bookmarks"] = json::object();
        for (auto [bmName, bm] : bookmarks) {
            config.conf["lists"][listName]["bookmarks"][bmName]["frequency"] = bm.frequency;
            config.conf["lists"][listName]["bookmarks"][bmName]["bandwidth"] = bm.bandwidth;
            DemodID demodId = (radio != nullptr) ? radio->getDemodByIndex(bm.modeIndex) : (DemodID)-1;
            flog::info("bm.modeIndex={}, demodId={}", (int)bm.modeIndex, (int)demodId);
            config.conf["lists"][listName]["bookmarks"][bmName]["mode"] = demodId;
            config.conf["lists"][listName]["bookmarks"][bmName]["vfo"] = bm.vfoName;
            if (demodId == RADIO_DEMOD_NFM) {
                saveTone(config.conf["lists"][listName]["bookmarks"][bmName], bm.tone);
            }
        }
        refreshWaterfallBookmarks(false);
        config.release(true);
    }

    static void menuHandler(void* ctx) {
        FrequencyManagerModule* _this = (FrequencyManagerModule*)ctx;

        if (demodModeList.empty()) {
            auto radio = (RadioModuleInterface *)core::moduleManager.getInterface(gui::waterfall.selectedVFO, "RadioModuleInterface");
            _this->updateModeList(radio);
        }

        float menuWidth = ImGui::GetContentRegionAvail().x;

        // TODO: Replace with something that won't iterate every frame
        std::vector<std::string> selectedNames;
        for (auto& [name, bm] : _this->bookmarks) {
            if (bm.selected) { selectedNames.push_back(name); }
        }

        float lineHeight = ImGui::GetTextLineHeightWithSpacing();

        float btnSize = ImGui::CalcTextSize("Rename").x + 8;
        ImGui::SetNextItemWidth(menuWidth - 24 - (2 * lineHeight) - btnSize);
        if (ImGui::Combo(("##freq_manager_list_sel" + _this->name).c_str(), &_this->selectedListId, _this->listNamesTxt.c_str())) {
            _this->loadByName(_this->listNames[_this->selectedListId]);
            config.acquire();
            config.conf["selectedList"] = _this->selectedListName;
            config.release(true);
        }
        ImGui::SameLine();
        if (_this->listNames.size() == 0) { style::beginDisabled(); }
        if (ImGui::Button(("Rename##_freq_mgr_ren_lst_" + _this->name).c_str(), ImVec2(btnSize, 0))) {
            _this->firstEditedListName = _this->listNames[_this->selectedListId];
            _this->editedListName = _this->firstEditedListName;
            _this->renameListOpen = true;
        }
        if (_this->listNames.size() == 0) { style::endDisabled(); }
        ImGui::SameLine();
        if (ImGui::Button(("+##_freq_mgr_add_lst_" + _this->name).c_str(), ImVec2(lineHeight, 0))) {
            // Find new unique default name
            if (std::find(_this->listNames.begin(), _this->listNames.end(), "New List") == _this->listNames.end()) {
                _this->editedListName = "New List";
            }
            else {
                char buf[64];
                for (int i = 1; i < 1000; i++) {
                    snprintf(buf, sizeof buf, "New List (%d)", i);
                    if (std::find(_this->listNames.begin(), _this->listNames.end(), buf) == _this->listNames.end()) { break; }
                }
                _this->editedListName = buf;
            }
            _this->newListOpen = true;
        }
        ImGui::SameLine();
        if (_this->selectedListName == "") { style::beginDisabled(); }
        if (ImGui::Button(("-##_freq_mgr_del_lst_" + _this->name).c_str(), ImVec2(lineHeight, 0))) {
            _this->deleteListOpen = true;
        }
        if (_this->selectedListName == "") { style::endDisabled(); }

        // Update scanner with current bookmarks
        std::vector<std::string> bookmarkNames;
        for (auto& [name, bm] : _this->bookmarks) {
            bookmarkNames.push_back(name);
        }
        _this->scanner.setBookmarks(bookmarkNames, _this->bookmarks);
        
        // Render only - the scanner is ticked from scannerTick, so that collapsing
        // this menu does not stop it.
        _this->scanner.render();

        // List delete confirmation
        if (ImGui::GenericDialog(("freq_manager_del_list_confirm" + _this->name).c_str(), _this->deleteListOpen, GENERIC_DIALOG_BUTTONS_YES_NO, [_this]() {
                ImGui::Text("Deleting list named \"%s\". Are you sure?", _this->selectedListName.c_str());
            }) == GENERIC_DIALOG_BUTTON_YES) {
            config.acquire();
            config.conf["lists"].erase(_this->selectedListName);
            _this->refreshWaterfallBookmarks(false);
            config.release(true);
            _this->refreshLists();
            _this->selectedListId = std::clamp<int>(_this->selectedListId, 0, _this->listNames.size());
            if (_this->listNames.size() > 0) {
                _this->loadByName(_this->listNames[_this->selectedListId]);
            }
            else {
                _this->selectedListName = "";
            }
        }

        if (_this->selectedListName == "") { style::beginDisabled(); }
        //Draw buttons on top of the list
        ImGui::BeginTable(("freq_manager_btn_table" + _this->name).c_str(), 3);
        ImGui::TableNextRow();

        ImGui::TableSetColumnIndex(0);
        auto radio = (RadioModuleInterface *)core::moduleManager.getInterface(gui::waterfall.selectedVFO, "RadioModuleInterface");
        if (ImGui::Button(("Add##_freq_mgr_add_" + _this->name).c_str(), ImVec2(ImGui::GetContentRegionAvail().x, 0))) {
            // If there's no VFO selected, just save the center freq


            _this->updateModeList(radio);


            if (gui::waterfall.selectedVFO == "") {
                _this->editedBookmark.frequency = gui::waterfall.getCenterFrequency();
                _this->editedBookmark.bandwidth = 0;
            }
            else {
                _this->editedBookmark.frequency = gui::waterfall.getCenterFrequency() + sigpath::vfoManager.getOffset(gui::waterfall.selectedVFO);
                _this->editedBookmark.bandwidth = sigpath::vfoManager.getBandwidth(gui::waterfall.selectedVFO);
            }
            _this->editedBookmark.modeIndex = (radio != nullptr) ? radio->getDemodIndex(radio->getSelectedDemodId()) : -1;
            _this->editedBookmark.vfoName = gui::waterfall.selectedVFO;
            _this->editedBookmark.selected = false;
            // Take the tone the radio is set to right now, so bookmarking a repeater
            // you have just tuned in captures its tone without retyping it.
            _this->editedBookmark.tone = RadioToneSettings();
            if (gui::waterfall.selectedVFO != "") {
                core::modComManager.callInterface(gui::waterfall.selectedVFO, RADIO_IFACE_CMD_GET_TONE_SETTINGS,
                                                  NULL, &_this->editedBookmark.tone);
            }


            _this->createOpen = true;

            // Find new unique default name
            if (_this->bookmarks.find("New Bookmark") == _this->bookmarks.end()) {
                _this->editedBookmarkName = "New Bookmark";
            }
            else {
                char buf[64];
                for (int i = 1; i < 1000; i++) {
                    snprintf(buf, sizeof buf, "New Bookmark (%d)", i);
                    if (_this->bookmarks.find(buf) == _this->bookmarks.end()) { break; }
                }
                _this->editedBookmarkName = buf;
            }
        }

        ImGui::TableSetColumnIndex(1);
        if (selectedNames.size() == 0 && _this->selectedListName != "") { style::beginDisabled(); }
        if (ImGui::Button(("Remove##_freq_mgr_rem_" + _this->name).c_str(), ImVec2(ImGui::GetContentRegionAvail().x, 0))) {
            _this->deleteBookmarksOpen = true;
        }
        if (selectedNames.size() == 0 && _this->selectedListName != "") { style::endDisabled(); }
        ImGui::TableSetColumnIndex(2);
        if (selectedNames.size() != 1 && _this->selectedListName != "") { style::beginDisabled(); }
        if (ImGui::Button(("Edit##_freq_mgr_edt_" + _this->name).c_str(), ImVec2(ImGui::GetContentRegionAvail().x, 0))) {
            auto radio = (RadioModuleInterface *)core::moduleManager.getInterface(gui::waterfall.selectedVFO, "RadioModuleInterface");
            _this->updateModeList(radio);
            _this->editOpen = true;
            _this->editedBookmark = _this->bookmarks[selectedNames[0]];
            _this->editedBookmarkName = selectedNames[0];
            _this->firstEditedBookmarkName = selectedNames[0];
        }
        if (selectedNames.size() != 1 && _this->selectedListName != "") { style::endDisabled(); }

        ImGui::EndTable();

        // Bookmark delete confirm dialog
        // List delete confirmation
        if (ImGui::GenericDialog(("freq_manager_del_list_confirm" + _this->name).c_str(), _this->deleteBookmarksOpen, GENERIC_DIALOG_BUTTONS_YES_NO, [_this]() {
                ImGui::TextUnformatted("Deleting selected bookmaks. Are you sure?");
            }) == GENERIC_DIALOG_BUTTON_YES) {
            for (auto& _name : selectedNames) { _this->bookmarks.erase(_name); }
            _this->saveByName(_this->selectedListName);
        }

        // Bookmark list
        if (ImGui::BeginTable(("freq_manager_bkm_table" + _this->name).c_str(), 2, ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY, ImVec2(0, 200.0f * style::uiScale))) {
            ImGui::TableSetupColumn("Name");
            ImGui::TableSetupColumn("Bookmark");
            ImGui::TableSetupScrollFreeze(2, 1);
            ImGui::TableHeadersRow();
            for (auto& [name, bm] : _this->bookmarks) {
                bool vfoMissing = !bm.vfoName.empty() && !sigpath::vfoManager.vfoExists(bm.vfoName);
                ImGui::TableNextRow();
                ImGui::TableSetColumnIndex(0);
                ImVec2 min = ImGui::GetCursorPos();

                if (vfoMissing) { style::beginDisabled(); }
                if (ImGui::Selectable((name + "##_freq_mgr_bkm_name_" + _this->name).c_str(), &bm.selected, ImGuiSelectableFlags_SpanAllColumns | ImGuiSelectableFlags_SelectOnClick)) {
                    // if shift or control isn't pressed, deselect all others
                    if (!ImGui::GetIO().KeyShift && !ImGui::GetIO().KeyCtrl) {
                        for (auto& [_name, _bm] : _this->bookmarks) {
                            if (name == _name) { continue; }
                            _bm.selected = false;
                        }
                    }
                }
                if (vfoMissing) {
                    style::endDisabled();
                    if (ImGui::IsItemHovered()) {
                        ImGui::SetTooltip("Radio \"%s\" is not available", bm.vfoName.c_str());
                    }
                }
                if (ImGui::TableGetHoveredColumn() >= 0 && ImGui::IsItemHovered() && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) {
                    applyBookmark(bm, gui::waterfall.selectedVFO);
                }

                ImGui::TableSetColumnIndex(1);
                if (vfoMissing) {
                    ImGui::Text("%s ", utils::formatFreq(bm.frequency).c_str());
                    ImGui::SameLine();
                    ImGui::TextColored(ImVec4(1.0f, 0.25f, 0.25f, 1.0f), "X");
                } else {
                    ImGui::Text("%s %s", utils::formatFreq(bm.frequency).c_str(), (radio != nullptr) ? demodModeName(radio->getDemodByIndex(bm.modeIndex)).c_str() : "");
                }
//		std::string modeStr = (radio != nullptr && bm.modeIndex >= 0) ? demodModeList[radio->getDemodByIndex(bm.modeIndex)] : "DIGITAL";
//		ImGui::Text("%s %s", utils::formatFreq(bm.frequency).c_str(), modeStr.c_str());

                ImVec2 max = ImGui::GetCursorPos();
            }
            ImGui::EndTable();
        }


        if (selectedNames.size() != 1 && _this->selectedListName != "") { style::beginDisabled(); }
        if (ImGui::Button(("Apply##_freq_mgr_apply_" + _this->name).c_str(), ImVec2(menuWidth, 0))) {
            FrequencyBookmark& bm = _this->bookmarks[selectedNames[0]];
            applyBookmark(bm, gui::waterfall.selectedVFO);
            bm.selected = false;
        }
        if (selectedNames.size() != 1 && _this->selectedListName != "") { style::endDisabled(); }

        //Draw import and export buttons
        ImGui::BeginTable(("freq_manager_bottom_btn_table" + _this->name).c_str(), 2);
        ImGui::TableNextRow();

        ImGui::TableSetColumnIndex(0);
        if (ImGui::Button(("Import##_freq_mgr_imp_" + _this->name).c_str(), ImVec2(ImGui::GetContentRegionAvail().x, 0)) && !_this->importOpen) {
            _this->importOpen = true;
            _this->importDialog = new pfd::open_file("Import bookmarks", "", { "JSON Files (*.json)", "*.json", "All Files", "*" }, pfd::opt::multiselect);
        }

        ImGui::TableSetColumnIndex(1);
        if (selectedNames.size() == 0 && _this->selectedListName != "") { style::beginDisabled(); }
        if (ImGui::Button(("Export##_freq_mgr_exp_" + _this->name).c_str(), ImVec2(ImGui::GetContentRegionAvail().x, 0)) && !_this->exportOpen) {
            _this->exportedBookmarks = json::object();
            config.acquire();
            for (auto& _name : selectedNames) {
                _this->exportedBookmarks["bookmarks"][_name] = config.conf["lists"][_this->selectedListName]["bookmarks"][_name];
            }
            config.release();
            _this->exportOpen = true;
            _this->exportDialog = new pfd::save_file("Export bookmarks", "", { "JSON Files (*.json)", "*.json", "All Files", "*" });
        }
        if (selectedNames.size() == 0 && _this->selectedListName != "") { style::endDisabled(); }
        ImGui::EndTable();

        if (ImGui::Button(("Select displayed lists##_freq_mgr_exp_" + _this->name).c_str(), ImVec2(menuWidth, 0))) {
            _this->selectListsOpen = true;
        }

        ImGui::LeftLabel("Bookmark display mode");
        ImGui::SetNextItemWidth(menuWidth - ImGui::GetCursorPosX());
        if (ImGui::Combo(("##_freq_mgr_dms_" + _this->name).c_str(), &_this->bookmarkDisplayMode, bookmarkDisplayModesTxt)) {
            config.acquire();
            config.conf["bookmarkDisplayMode"] = _this->bookmarkDisplayMode;
            config.release(true);
        }

        if (_this->selectedListName == "") { style::endDisabled(); }

        if (_this->createOpen) {
            _this->createOpen = _this->bookmarkEditDialog();
        }

        if (_this->editOpen) {
            _this->editOpen = _this->bookmarkEditDialog();
        }

        if (_this->newListOpen) {
            _this->newListOpen = _this->newListDialog();
        }

        if (_this->renameListOpen) {
            _this->renameListOpen = _this->newListDialog();
        }

        if (_this->selectListsOpen) {
            _this->selectListsOpen = _this->selectListsDialog();
        }

        // Handle import and export
        if (_this->importOpen && _this->importDialog->ready()) {
            _this->importOpen = false;
            std::vector<std::string> paths = _this->importDialog->result();
            if (paths.size() > 0 && _this->listNames.size() > 0) {
                _this->importBookmarks(paths[0]);
            }
            delete _this->importDialog;
        }
        if (_this->exportOpen && _this->exportDialog->ready()) {
            _this->exportOpen = false;
            std::string path = _this->exportDialog->result();
            if (path != "") {
                _this->exportBookmarks(path);
            }
            delete _this->exportDialog;
        }
    }

    struct Drawn {
        ImRect rect;
        int index;
    };

    std::vector<Drawn> rects;

    static void fftRedraw(ImGui::WaterFall::FFTRedrawArgs args, void* ctx) {

        FrequencyManagerModule* _this = (FrequencyManagerModule*)ctx;

        _this->rects.clear();
        if (_this->scanner.isScanning() && _this->scanner.isSquelchEnabled()) {
            double scanBPos = args.max.y - ((_this->scanner.getNoiseFloor() + _this->scanner.getSignalMarginDb() - gui::waterfall.getFFTMin()) * (args.max.y - args.min.y) / (gui::waterfall.getFFTMax() - gui::waterfall.getFFTMin()));
            if (scanBPos >= args.min.y && scanBPos <= args.max.y) {
                args.window->DrawList->AddLine(ImVec2(args.min.x, roundf(scanBPos)), ImVec2(args.max.x, roundf(scanBPos)), ImGui::ColorConvertFloat4ToU32(gui::themeManager.scannerSquelchColor), 1.0);
            }
        }


        if (_this->bookmarkDisplayMode == BOOKMARK_DISP_MODE_OFF) { return; }


        int index = -1;
        auto ctm = currentTimeMillis();
        for (auto const &bm : _this->waterfallBookmarks) {
            index++;

            if (bm.notValidAfter && ctm > bm.notValidAfter) { continue; }

            bool vfoMissing = !bm.bookmark.vfoName.empty() && !sigpath::vfoManager.vfoExists(bm.bookmark.vfoName);

            double centerXpos = args.min.x + std::round((bm.bookmark.frequency - args.lowFreq) * args.freqToPixelRatio);

            ImVec2 nameSize = ImGui::CalcTextSize(bm.bookmarkName.c_str());
            ImVec2 rectMin;
            float layoutOverlapStep = nameSize.y + 1;
            if (_this->bookmarkDisplayMode == BOOKMARK_DISP_MODE_TOP) {
                rectMin = ImVec2(centerXpos - (nameSize.x / 2) - 5, args.min.y);
            } else {
                rectMin = ImVec2(centerXpos - (nameSize.x / 2) - 5, args.max.y - nameSize.y);
                layoutOverlapStep = -layoutOverlapStep;
            }

            ImVec2 rectMax = ImVec2(centerXpos + (nameSize.x / 2) + 5, rectMin.y + nameSize.y);
            ImVec2 clampedRectMin = ImVec2(std::clamp<double>(rectMin.x, args.min.x, args.max.x), rectMin.y);
            ImVec2 clampedRectMax = ImVec2(std::clamp<double>(rectMax.x, args.min.x, args.max.x), rectMax.y);

            if (clampedRectMax.x - clampedRectMin.x > 0) {
                auto newRect = ImRect{clampedRectMin, clampedRectMax};
                // Step the label past anything already placed. Each pass pushes it one
                // row further from the edge it started at, so it settles after at most
                // one row per label already drawn - that is the bound. It also stops
                // as soon as the label leaves the band, since the check below is going
                // to drop it anyway and there is nothing left to collide with.
                for (size_t attempt = 0; attempt <= _this->rects.size(); attempt++) {
                    bool overlaps = false;
                    for (const auto &existing: _this->rects) {
                        if (existing.rect.Overlaps(newRect)) {
                            overlaps = true;
                            break;
                        }
                    }
                    if (!overlaps) { break; }
                    if (newRect.Min.y < args.min.y || newRect.Max.y > args.max.y) { break; }
                    newRect.Min.y += layoutOverlapStep;
                    newRect.Max.y += layoutOverlapStep;
                }
                clampedRectMax = newRect.Max;
                clampedRectMin = newRect.Min;
                rectMin.y = clampedRectMin.y;
                rectMax.y = clampedRectMax.y;
                // Inclusive at the bottom edge: in BOTTOM mode the first label sits
                // flush against args.max.y, so an exclusive test culled every
                // bookmark. Culled labels never reach _this->rects either, so the
                // next one started at the same y and was culled in turn, which is
                // why the whole set vanished rather than just the bottom row.
                if (clampedRectMin.y < args.min.y || clampedRectMax.y > args.max.y) {
                    continue; // dont draw at all.
                }
                args.window->DrawList->AddRectFilled(clampedRectMin, clampedRectMax, bm.worked ? IM_COL32(0, 255, 0, 255) : (vfoMissing ? IM_COL32(255, 80, 80, 255) : IM_COL32(255, 255, 0, 255)));
                _this->rects.emplace_back(Drawn { newRect, index } );
            }
            if (rectMin.x >= args.min.x && rectMax.x <= args.max.x) {
                if (vfoMissing) {
                    // Red cross over the label: the radio this bookmark belongs to is gone
                    ImVec2 crossCenter = ImVec2(centerXpos, rectMin.y + (nameSize.y / 2.0f));
                    float crossHalf = nameSize.y * 0.35f;
                    args.window->DrawList->AddLine(ImVec2(crossCenter.x - crossHalf, crossCenter.y - crossHalf), ImVec2(crossCenter.x + crossHalf, crossCenter.y + crossHalf), IM_COL32(255, 0, 0, 255), 2.0f);
                    args.window->DrawList->AddLine(ImVec2(crossCenter.x - crossHalf, crossCenter.y + crossHalf), ImVec2(crossCenter.x + crossHalf, crossCenter.y - crossHalf), IM_COL32(255, 0, 0, 255), 2.0f);
                } else {
                    args.window->DrawList->AddText(ImVec2(centerXpos - (nameSize.x / 2), rectMin.y), IM_COL32(0, 0, 0, 255), bm.bookmarkName.c_str());
                }
            }
            if (bm.bookmark.frequency >= args.lowFreq && bm.bookmark.frequency <= args.highFreq) {
                args.window->DrawList->AddLine(ImVec2(centerXpos, args.min.y), ImVec2(centerXpos, args.max.y), bm.worked ? IM_COL32(0, 255, 0, 255) : IM_COL32(255, 255, 0, 255));
            }

        }
    }

    bool mouseAlreadyDown = false;
    bool mouseClickedInLabel = false;
    static void fftInput(ImGui::WaterFall::InputHandlerArgs args, void* ctx) {
        FrequencyManagerModule* _this = (FrequencyManagerModule*)ctx;
        if (_this->bookmarkDisplayMode == BOOKMARK_DISP_MODE_OFF) { return; }

        if (_this->mouseClickedInLabel) {
            if (!ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
                _this->mouseClickedInLabel = false;
            }
            gui::waterfall.inputHandled = true;
            return;
        }

        // First check that the mouse clicked outside of any label. Also get the bookmark that's hovered
        bool inALabel = false;
        WaterfallBookmark hoveredBookmark;
        std::string hoveredBookmarkName;

        // IsMouseHoveringRect doesn't know that a floating window drawn over the
        // waterfall owns the mouse, so without this gate a click landing on one
        // reached the bookmark label underneath and retuned the radio. Leaving
        // inALabel false rather than returning early keeps the button bookkeeping
        // below running, so releasing the mouse over such a window still clears state.
        if (gui::waterfall.mouseOverWaterfallWindow) {
            for(auto &d: _this->rects) {
                if (ImGui::IsMouseHoveringRect(d.rect.Min, d.rect.Max)) {
                    // rects is rebuilt from waterfallBookmarks on every redraw, so the
                    // index should always be live. Check it regardless - the cost of
                    // being wrong here is an out of bounds read on a vector.
                    if (d.index < 0 || d.index >= (int)_this->waterfallBookmarks.size()) { continue; }
                    inALabel = true;
                    hoveredBookmark = _this->waterfallBookmarks[d.index];
                    hoveredBookmarkName = hoveredBookmark.bookmarkName;
                    break;
                }
            }
        }

        // Check if mouse was already down
        if (ImGui::IsMouseClicked(ImGuiMouseButton_Left) && !inALabel) {
            _this->mouseAlreadyDown = true;
        }
        if (!ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
            _this->mouseAlreadyDown = false;
            _this->mouseClickedInLabel = false;
        }

        // If yes, cancel
        if (_this->mouseAlreadyDown || !inALabel) { return; }

        gui::waterfall.inputHandled = true;
        auto radio = (RadioModuleInterface *)core::moduleManager.getInterface(gui::waterfall.selectedVFO, "RadioModuleInterface");

        if (ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
            _this->mouseClickedInLabel = true;
            applyBookmark(hoveredBookmark.bookmark, gui::waterfall.selectedVFO);
        }

        ImGui::BeginTooltip();
        ImGui::TextUnformatted(hoveredBookmarkName.c_str());
        ImGui::Separator();
        ImGui::Text("List: %s", hoveredBookmark.listName.c_str());
        ImGui::Text("Frequency: %s", utils::formatFreq(hoveredBookmark.bookmark.frequency).c_str());
        ImGui::Text("Bandwidth: %s", utils::formatFreq(hoveredBookmark.bookmark.bandwidth).c_str());
        if (!hoveredBookmark.bookmark.vfoName.empty() && !sigpath::vfoManager.vfoExists(hoveredBookmark.bookmark.vfoName)) {
            ImGui::TextColored(ImVec4(1.0f, 0.25f, 0.25f, 1.0f), "Radio \"%s\" is not available", hoveredBookmark.bookmark.vfoName.c_str());
        } else {
            ImGui::Text("Mode: %s", (radio != nullptr) ? demodModeName(radio->getDemodByIndex(hoveredBookmark.bookmark.modeIndex)).c_str() : "");
        }
        ImGui::EndTooltip();
    }

    json exportedBookmarks;
    bool importOpen = false;
    bool exportOpen = false;
    pfd::open_file* importDialog;
    pfd::save_file* exportDialog;

    void importBookmarks(std::string path) {
        std::ifstream fs(wstr::str2wstr(path));
        json importBookmarks;
        fs >> importBookmarks;

        auto radio = (RadioModuleInterface *)core::moduleManager.getInterface(gui::waterfall.selectedVFO, "RadioModuleInterface");

        if (!importBookmarks.contains("bookmarks")) {
            flog::error("File does not contains any bookmarks");
            return;
        }

        if (!importBookmarks["bookmarks"].is_object()) {
            flog::error("Bookmark attribute is invalid");
            return;
        }

        // Load every bookmark
        for (auto const [_name, bm] : importBookmarks["bookmarks"].items()) {
            if (bookmarks.find(_name) != bookmarks.end()) {
                flog::warn("Bookmark with the name '{0}' already exists in list, skipping", _name);
                continue;
            }
            FrequencyBookmark fbm;
            fbm.frequency = bm["frequency"];
            fbm.bandwidth = bm["bandwidth"];
            fbm.modeIndex = (radio != nullptr) ? radio->getDemodIndex(bm["mode"]) : -1;
            fbm.vfoName = bm.value("vfo", "");
            fbm.selected = false;
            bookmarks[_name] = fbm;
        }
        saveByName(selectedListName);

        fs.close();
    }

    void exportBookmarks(std::string path) {
        std::ofstream fs(wstr::str2wstr(path));
        fs << exportedBookmarks;
        fs.close();
    }

    std::string name;
    bool enabled = true;
    bool createOpen = false;
    bool editOpen = false;
    bool newListOpen = false;
    bool renameListOpen = false;
    bool selectListsOpen = false;

    bool deleteListOpen = false;
    bool deleteBookmarksOpen = false;

    EventHandler<ImGui::WaterFall::FFTRedrawArgs> fftRedrawHandler;

    EventHandler<ImGuiContext*> scannerTickHandler;
    EventHandler<ImGui::WaterFall::InputHandlerArgs> inputHandler;

    std::map<std::string, FrequencyBookmark> bookmarks;

    std::string editedBookmarkName = "";
    std::string firstEditedBookmarkName = "";
    FrequencyBookmark editedBookmark;

    std::vector<std::string> listNames;
    std::string listNamesTxt = "";
    std::string selectedListName = "";
    int selectedListId = 0;

    std::string editedListName;
    std::string firstEditedListName;

    std::vector<WaterfallBookmark> waterfallBookmarks;
    Scanner scanner{this};

    int bookmarkDisplayMode = 0;
};

void applyBookmark(FrequencyBookmark bm, std::string vfoName) {
    // A bookmark may remember the radio/VFO it was saved for. If it does, apply
    // to that radio instead of whatever VFO is currently selected.
    std::string targetVfo = bm.vfoName.empty() ? vfoName : bm.vfoName;
    if (targetVfo == "") {
        // TODO: Replace with proper tune call
        gui::waterfall.setCenterFrequency(bm.frequency);
        gui::waterfall.centerFreqMoved = true;
        return;
    }
    // Radio no longer exists (module not loaded): bookmark is disabled, do nothing
    if (!sigpath::vfoManager.vfoExists(targetVfo)) { return; }
    for(auto x: core::moduleManager.instances) {
        ModuleManager::Instance *pInstance = x.second.instance;
        auto radio = (RadioModuleInterface *)pInstance->getInterface("RadioModuleInterface");
        if (radio && x.first == targetVfo) {
            int mode = radio->getDemodByIndex(bm.modeIndex);
            float bandwidth = bm.bandwidth;
            core::modComManager.callInterface(targetVfo, RADIO_IFACE_CMD_SET_MODE, &mode, NULL);
            core::modComManager.callInterface(targetVfo, RADIO_IFACE_CMD_SET_BANDWIDTH, &bandwidth, NULL);
            // After the mode, since the radio ignores tone settings unless the
            // demodulator it just switched to is one that carries them. Skipped
            // entirely for a bookmark saved before tones existed, so recalling an
            // old list leaves whatever the radio is set to alone.
            if (bm.hasTone) {
                RadioToneSettings tone = bm.tone;
                core::modComManager.callInterface(targetVfo, RADIO_IFACE_CMD_SET_TONE_SETTINGS, &tone, NULL);
            }
        }
    }
    tuner::tune(tuner::TUNER_MODE_NORMAL, targetVfo, bm.frequency);
}



MOD_EXPORT void _INIT_() {
    json def = json({});
    def["selectedList"] = "General";
    def["bookmarkDisplayMode"] = BOOKMARK_DISP_MODE_TOP;
    def["lists"]["General"]["showOnWaterfall"] = true;
    def["lists"]["General"]["bookmarks"] = json::object();
    
    // Scanner defaults
    def["scanner"] = json::object();
    def["scanner"]["scanIntervalMs"] = 100.0f;
    def["scanner"]["listenTimeSec"] = 10.0f;
    // These are on the SNR meter's scale, the same one Scanner::render clamps to
    // 0..40, not absolute dBFS. -120 used to live here, which made every station
    // clear the detection threshold instantly.
    def["scanner"]["noiseFloor"] = 3.0f;
    def["scanner"]["signalMarginDb"] = 4.0f;
    def["scanner"]["squelchEnabled"] = false;
    def["scanner"]["carrierHoldMode"] = false;

    config.setPath(std::string(core::getRoot()) + "/frequency_manager_config.json");
    config.load(def);
    config.enableAutoSave();

    // Check if of list and convert if they're the old type
    config.acquire();
    if (!config.conf.contains("bookmarkDisplayMode")) {
        config.conf["bookmarkDisplayMode"] = BOOKMARK_DISP_MODE_TOP;
    }
    for (auto [listName, list] : config.conf["lists"].items()) {
        if (list.contains("bookmarks") && list.contains("showOnWaterfall") && list["showOnWaterfall"].is_boolean()) { continue; }
        json newList;
        newList = json::object();
        newList["showOnWaterfall"] = true;
        newList["bookmarks"] = list;
        config.conf["lists"][listName] = newList;
    }
    config.release(true);
}

MOD_EXPORT ModuleManager::Instance* _CREATE_INSTANCE_(std::string name) {
    return new FrequencyManagerModule(name);
}

MOD_EXPORT void _DELETE_INSTANCE_(void* instance) {
    delete (FrequencyManagerModule*)instance;
}

MOD_EXPORT void _END_() {
    config.disableAutoSave();
    config.save();
}
