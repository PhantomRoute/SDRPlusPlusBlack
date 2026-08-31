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
#include "bookmark_csv.h"
#include "../../radio/src/radio_module_interface.h"
#include "../../radio/src/tone_tables.h"
#include <algorithm>
#include <cmath>
#include <iterator>
#include <sstream>
#include "gui/brown/imgui-notify/imgui_notify.h"

SDRPP_MOD_INFO{
    /* Name:            */ "frequency_manager",
    /* Description:     */ "Frequency manager module for SDR++",
    /* Author:          */ "Ryzerth;Zimm",
    /* Version:         */ 0, 3, 0,
    /* Max instances    */ 1
};

ConfigManager config;

// How much free text a bookmark's notes can hold. Generous rather than tight: this is
// the field someone copies a repeater's whole entry from a printed list into, and a
// note that gets cut off halfway is worse than no note at all.
static const size_t NOTES_MAX = 8192;

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

// The reverse, for reading a mode back out of a file. Matched without regard to case
// or spacing, because a list someone typed up themselves is as likely to say "nfm" as
// "NFM", and quietly substituting the default mode over a difference in spelling would
// put the radio on the wrong demodulator without saying so.
static bool demodIdByName(const std::string& wanted, int* out) {
    std::string w = csv::normaliseHeader(wanted);
    if (w.empty()) { return false; }
    for (const auto& [nm, id] : demodModeListRev) {
        if (csv::normaliseHeader(nm) == w) {
            *out = id;
            return true;
        }
    }
    return false;
}

enum {
    BOOKMARK_DISP_MODE_OFF,
    BOOKMARK_DISP_MODE_TOP,
    BOOKMARK_DISP_MODE_BOTTOM,
    _BOOKMARK_DISP_MODE_COUNT
};

const char* bookmarkDisplayModesTxt = "Off\0Top\0Bottom\0";

// The label and the marker line share a colour, so a worked bookmark or one whose
// radio is gone reads the same in both. All three come from the theme; they used to
// be written into the drawing code as literals, which left no way to change them.
static ImU32 bookmarkLabelColor(bool worked, bool vfoMissing) {
    if (worked) { return ImGui::ColorConvertFloat4ToU32(gui::themeManager.bookmarkWorkedColor); }
    if (vfoMissing) { return ImGui::ColorConvertFloat4ToU32(gui::themeManager.bookmarkMissingColor); }
    return ImGui::ColorConvertFloat4ToU32(gui::themeManager.bookmarkColor);
}

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
        // Handing the list over from the menu handler meant the scanner had no
        // channels at all until the Frequency Manager section had been expanded
        // once, and never saw an edit made while it was collapsed.
        std::vector<std::string> bookmarkNames;
        bookmarkNames.reserve(_this->bookmarks.size());
        for (auto& [name, bm] : _this->bookmarks) {
            bookmarkNames.push_back(name);
        }
        _this->scanner.setBookmarks(bookmarkNames, _this->bookmarks);
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
            return json{{"scanning", scanner.isScanning()}, {"state", Scanner::stateName(scanner.getState())}, {"current_station", scanner.getCurrentStation()}, {"bookmark_count", (int)bookmarks.size()}}.dump();
        }
        if (cmd == "start_scanner") {
            if (bookmarks.empty()) {
                return json{{"error", "no bookmarks to scan"}}.dump();
            }
            scanner.startScanner();
            // The scan can refuse to start - no radio loaded, every channel
            // skipped - so report what actually happened rather than "ok".
            if (!scanner.isScanning()) {
                return json{{"error", scanner.getStatusMessage()}, {"scanning", false}}.dump();
            }
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

    // The accept list is carried in plain types across the module boundary, so it
    // comes back as RadioToneListEntry rather than as the ToneKey that knows how to
    // compare and name itself.
    static tonedetect::ToneKey toneKeyOf(const RadioToneListEntry& e) {
        tonedetect::ToneKey k;
        k.kind = (e.kind == (int)tonedetect::ToneKey::DCS) ? tonedetect::ToneKey::DCS : tonedetect::ToneKey::CTCSS;
        k.ctcssFreq = e.ctcssFreq;
        k.dcsCode = e.dcsCode;
        k.dcsInverted = e.dcsInverted;
        return k;
    }

    static void toneListRemove(RadioToneSettings& t, int index) {
        if (index < 0 || index >= t.listCount) { return; }
        for (int i = index; i < t.listCount - 1; i++) { t.list[i] = t.list[i + 1]; }
        t.list[--t.listCount] = RadioToneListEntry();
    }

    // Duplicate check before the capacity check, matching tonedetect::Target::addKey:
    // a code already in the list is not something a full list should refuse.
    static void toneListAdd(RadioToneSettings& t, const RadioToneListEntry& e) {
        for (int i = 0; i < t.listCount; i++) {
            if (toneKeyOf(t.list[i]) == toneKeyOf(e)) { return; }
        }
        if (t.listCount >= RADIO_TONE_LIST_MAX) { return; }
        t.list[t.listCount++] = e;
    }

    // The accept list's rows of the edit dialog: what the bookmark opens for, with a
    // way to take each one out, and a picker to add another. A business channel split
    // across areas carries a different code per area, and this is where a bookmark for
    // one records all of them.
    void drawToneListRows(RadioToneSettings& t) {
        for (int i = 0; i < t.listCount; i++) {
            ImGui::TableNextRow();
            ImGui::TableSetColumnIndex(0);
            if (i == 0) { ImGui::LeftLabel("Codes"); }
            ImGui::TableSetColumnIndex(1);
            if (ImGui::Button(("X##freq_manager_edit_tonedel" + std::to_string(i) + "_" + name).c_str())) {
                toneListRemove(t, i);
                // The rows after it have shifted down, so stop rather than keep drawing
                // against indices that have moved.
                break;
            }
            ImGui::SameLine();
            ImGui::TextUnformatted(toneKeyOf(t.list[i]).label().c_str());
        }
        if (t.listCount == 0) {
            ImGui::TableNextRow();
            ImGui::TableSetColumnIndex(0);
            ImGui::LeftLabel("Codes");
            ImGui::TableSetColumnIndex(1);
            // An empty list mutes the channel outright, which is worth saying rather
            // than leaving to be discovered on air.
            ImGui::TextDisabled("Empty - nothing opens the squelch.");
        }

        ImGui::TableNextRow();
        ImGui::TableSetColumnIndex(0);
        ImGui::LeftLabel("Add");
        ImGui::TableSetColumnIndex(1);
        int addKind = (editedToneAdd.kind == (int)tonedetect::ToneKey::DCS) ? 1 : 0;
        ImGui::SetNextItemWidth(addKind ? 75 : 85);
        if (ImGui::Combo(("##freq_manager_edit_toneaddkind" + name).c_str(), &addKind, "CTCSS\0DCS\0")) {
            editedToneAdd.kind = addKind ? (int)tonedetect::ToneKey::DCS : (int)tonedetect::ToneKey::CTCSS;
        }
        ImGui::SameLine();
        if (addKind) {
            ImGui::SetNextItemWidth(75);
            const auto& codes = dcsCodes();
            auto it = std::find(codes.begin(), codes.end(), editedToneAdd.dcsCode);
            int codeId = (it == codes.end()) ? 0 : (int)std::distance(codes.begin(), it);
            if (ImGui::Combo(("##freq_manager_edit_toneaddcode" + name).c_str(), &codeId, dcsCodeNames())) {
                editedToneAdd.dcsCode = codes[codeId];
            }
            ImGui::SameLine();
            ImGui::SetNextItemWidth(75);
            int pol = editedToneAdd.dcsInverted ? 1 : 0;
            if (ImGui::Combo(("##freq_manager_edit_toneaddpol" + name).c_str(), &pol, "Normal\0Invert\0")) {
                editedToneAdd.dcsInverted = (pol != 0);
            }
        }
        else {
            ImGui::SetNextItemWidth(115);
            int toneId = 0;
            for (int i = 0; i < tonedetect::CTCSS_TONE_COUNT; i++) {
                if (std::fabs(tonedetect::CTCSS_TONES[i] - editedToneAdd.ctcssFreq) < 0.05f) { toneId = i; break; }
            }
            if (ImGui::Combo(("##freq_manager_edit_toneaddtone" + name).c_str(), &toneId, ctcssToneNames())) {
                editedToneAdd.ctcssFreq = tonedetect::CTCSS_TONES[toneId];
            }
        }

        ImGui::TableNextRow();
        ImGui::TableSetColumnIndex(1);
        bool full = t.listCount >= RADIO_TONE_LIST_MAX;
        if (full) { style::beginDisabled(); }
        if (ImGui::Button(("Add code##freq_manager_edit_toneadd" + name).c_str())) {
            toneListAdd(t, editedToneAdd);
        }
        if (full) { style::endDisabled(); }
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
            // Offset by one, because mode 0 is "off" and the checkbox above is what
            // sets that; the combo only chooses between the modes that are on.
            int modeSel = (t.mode >= 1 && t.mode <= 4) ? (t.mode - 1) : 0;
            if (ImGui::Combo(("##freq_manager_edit_tonetype" + name).c_str(), &modeSel, "CTCSS\0DCS\0Any tone\0Custom list\0")) {
                t.mode = modeSel + 1;
            }

            if (t.mode == 3) {
                ImGui::TableNextRow();
                ImGui::TableSetColumnIndex(1);
                ImGui::TextDisabled("Opens for any CTCSS tone or DCS code.");
            }
            else if (t.mode == 4) {
                drawToneListRows(t);
            }
            else if (t.mode == 2) {
                ImGui::TableNextRow();
                ImGui::TableSetColumnIndex(0);
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
                ImGui::TableNextRow();
                ImGui::TableSetColumnIndex(0);
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

        ImGui::TableNextRow();
        ImGui::TableSetColumnIndex(0);
        ImGui::LeftLabel("Close on end of TX");
        ImGui::TableSetColumnIndex(1);
        ImGui::SetNextItemWidth(200);
        ImGui::Checkbox(("##freq_manager_edit_tonetail" + name).c_str(), &t.tailCloseEnabled);
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

            // Outside the table and full width: a two column layout gives a note about
            // forty characters of room, which is not enough to be worth typing into.
            ImGui::LeftLabel("Notes");
            if (ImGui::IsItemHovered()) {
                style::tooltip("Anything worth remembering about the channel. Kept with the\n"
                                  "bookmark, shown when you hover it in the list, and carried\n"
                                  "through export and import.");
            }
            {
                // ImGui edits a fixed buffer, so the text has to be copied in and back
                // out again - the name field above works the same way.
                char notesBuf[NOTES_MAX];
                size_t n = editedBookmark.notes.size();
                if (n >= sizeof(notesBuf)) { n = sizeof(notesBuf) - 1; }
                memcpy(notesBuf, editedBookmark.notes.c_str(), n);
                notesBuf[n] = 0;
                if (ImGui::InputTextMultiline(("##freq_manager_edit_notes" + name).c_str(), notesBuf, sizeof(notesBuf),
                                              ImVec2(-1.0f, ImGui::GetTextLineHeight() * 4.0f))) {
                    editedBookmark.notes = notesBuf;
                }
            }

            bool applyDisabled = (strlen(nameBuf) == 0) || (bookmarks.find(editedBookmarkName) != bookmarks.end() && editedBookmarkName != firstEditedBookmarkName);
            if (applyDisabled) { style::beginDisabled(); }
            // Named for what it does. There used to be three buttons called "Apply" in
            // this module doing three unrelated things, and the one on the menu behind
            // this dialog tunes the radio.
            if (ImGui::Button(editOpen ? "Save" : "Create")) {
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
            if (ImGui::Button(renameListOpen ? "Rename" : "Create")) {
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

    // Whether a bookmark can be tuned at all. One whose radio is not loaded cannot:
    // applyBookmark does nothing for it, so stepping onto it would look like the button
    // had missed.
    bool bookmarkIsTunable(const FrequencyBookmark& bm) const {
        return bm.vfoName.empty() || sigpath::vfoManager.vfoExists(bm.vfoName);
    }

    // Cheap enough to ask every frame, which the disabled state of the step buttons
    // does. Building the name list for that instead would copy every string in the
    // list once a frame to answer a yes or no.
    bool anyBookmarkTunable() {
        for (auto& [bmName, bm] : bookmarks) {
            if (bookmarkIsTunable(bm)) { return true; }
        }
        return false;
    }

    // The bookmarks that can actually be tuned, in the order the list draws them.
    std::vector<std::string> tunableBookmarkNames() {
        std::vector<std::string> names;
        for (auto& [bmName, bm] : bookmarks) {
            if (!bookmarkIsTunable(bm)) { continue; }
            names.push_back(bmName);
        }
        return names;
    }

    // What the radio is tuned to now, or NAN when there is no VFO to ask.
    double currentTunedFrequency() {
        if (gui::waterfall.selectedVFO.empty()) { return NAN; }
        return gui::waterfall.getCenterFrequency() + sigpath::vfoManager.getOffset(gui::waterfall.selectedVFO);
    }

    // Remembered by name, so that editing the list around it does not leave the
    // position pointing at a different channel than the one being listened to.
    void setNavBookmark(const std::string& bmName) {
        navBookmarkName = bmName;
    }

    // One channel along the list from wherever the radio is, and tune it.
    //
    // dir is +1 for further down the list and -1 for further up, which is what the
    // arrows say and which way the list runs on screen.
    //
    // The position is the last channel tuned from this panel - that is what makes a
    // run of presses walk the list one at a time rather than jumping back to the same
    // place. But if the radio has since been tuned by hand onto some other channel in
    // the list, that is where the operator actually is, so the step is taken from
    // there instead. Failing both, it comes in from the end it is heading away from:
    // the first press of Next lands on the top of the list, of Previous on the bottom.
    void stepBookmark(int dir) {
        std::vector<std::string> names = tunableBookmarkNames();
        if (names.empty()) { return; }
        int count = (int)names.size();

        // Stepping while a scan is running would put the radio somewhere the scanner
        // moves it straight back off. Taking manual control of the channel is a good
        // enough statement that the scan is over.
        if (scanner.isScanning()) { scanner.stopScanner(); }

        std::string from = navBookmarkName;
        double tuned = currentTunedFrequency();
        auto onFrequency = [&](const std::string& bmName) {
            auto it = bookmarks.find(bmName);
            return it != bookmarks.end() && std::isfinite(tuned) && fabs(it->second.frequency - tuned) < 1.0;
        };
        if (!onFrequency(from)) {
            from.clear();
            for (const auto& bmName : names) {
                if (onFrequency(bmName)) { from = bmName; break; }
            }
        }

        int index;
        auto it = std::find(names.begin(), names.end(), from);
        if (from.empty() || it == names.end()) {
            index = (dir > 0) ? 0 : count - 1;
        }
        else {
            // Wraps, so neither end of the list is a button that does nothing.
            index = (int)std::distance(names.begin(), it) + dir;
            index = ((index % count) + count) % count;
        }

        setNavBookmark(names[index]);
        scrollToNav = true;
        // The selection follows, because it is what says where you are in the list and
        // what Edit and Remove act on. Anything else would leave the highlight behind.
        for (auto& [bmName, bm] : bookmarks) { bm.selected = (bmName == navBookmarkName); }
        applyBookmark(bookmarks[navBookmarkName], gui::waterfall.selectedVFO);
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
        // A position in the old list means nothing in the new one.
        navBookmarkName.clear();
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
            fbm.notes = bm.value("notes", "");
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
        bm["tone"]["tailClose"] = t.tailCloseEnabled;
        // Written whatever the mode, so that a bookmark toggled to a single code and
        // back keeps the list of codes that was worked out on the channel.
        json list = json::array();
        for (int i = 0; i < t.listCount && i < RADIO_TONE_LIST_MAX; i++) {
            json e;
            e["kind"] = t.list[i].kind;
            e["ctcss"] = t.list[i].ctcssFreq;
            e["dcsCode"] = t.list[i].dcsCode;
            e["dcsInverted"] = t.list[i].dcsInverted;
            list.push_back(e);
        }
        bm["tone"]["list"] = list;
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
        // Bookmarks written before this existed get the same default a fresh channel
        // does, rather than silently having the behaviour turned off.
        t.tailCloseEnabled = j.value("tailClose", true);
        if (j.contains("list") && j["list"].is_array()) {
            // Capped rather than assumed to fit: bookmark files get hand edited and
            // shared around, and the destination is a fixed size array.
            for (const auto& e : j["list"]) {
                if (!e.is_object()) { continue; }
                if (t.listCount >= RADIO_TONE_LIST_MAX) { break; }
                RadioToneListEntry& out = t.list[t.listCount++];
                out.kind = e.value("kind", 0);
                out.ctcssFreq = e.value("ctcss", 100.0f);
                out.dcsCode = e.value("dcsCode", 23);
                out.dcsInverted = e.value("dcsInverted", false);
            }
        }
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
            config.conf["lists"][listName]["bookmarks"][bmName]["notes"] = bm.notes;
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

        // The scanner sits in the middle of this panel, between choosing a list and
        // working on the bookmarks in it, so without these two headings the row of
        // buttons under the scanner looks like it belongs to the scanner.
        ImGui::SectionHeader("LIST");

        float btnSize = ImGui::CalcTextSize("Rename").x + 8;
        ImGui::SetNextItemWidth(menuWidth - 24 - (2 * lineHeight) - btnSize);
        if (ImGui::Combo(("##freq_manager_list_sel" + _this->name).c_str(), &_this->selectedListId, _this->listNamesTxt.c_str())) {
            _this->loadByName(_this->listNames[_this->selectedListId]);
            config.acquire();
            config.conf["selectedList"] = _this->selectedListName;
            config.release(true);
        }
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
            style::tooltip("Which list of bookmarks to work on. Lists are separate sets of\n"
                           "channels - one for the local repeaters, one for airband, one for\n"
                           "a rally weekend - and each can be shown on the waterfall or not.");
        }
        ImGui::SameLine();
        if (_this->listNames.size() == 0) { style::beginDisabled(); }
        if (ImGui::Button(("Rename##_freq_mgr_ren_lst_" + _this->name).c_str(), ImVec2(btnSize, 0))) {
            _this->firstEditedListName = _this->listNames[_this->selectedListId];
            _this->editedListName = _this->firstEditedListName;
            _this->renameListOpen = true;
        }
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) { style::tooltip("Rename the selected list."); }
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
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) { style::tooltip("Start a new, empty list."); }
        ImGui::SameLine();
        if (_this->selectedListName == "") { style::beginDisabled(); }
        if (ImGui::Button(("-##_freq_mgr_del_lst_" + _this->name).c_str(), ImVec2(lineHeight, 0))) {
            _this->deleteListOpen = true;
        }
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) { style::tooltip("Delete the selected list and every bookmark in it."); }
        if (_this->selectedListName == "") { style::endDisabled(); }

        // Render only - the scanner is fed its channels and ticked from
        // scannerTick, so that collapsing this menu does not stop it.
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

        ImGui::SectionHeader("BOOKMARKS");

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

        if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
            style::tooltip("Save what the radio is tuned to now as a new bookmark - frequency,\n"
                           "mode, bandwidth and the tone settings it is using.");
        }
        ImGui::TableSetColumnIndex(1);
        if (selectedNames.size() == 0 && _this->selectedListName != "") { style::beginDisabled(); }
        if (ImGui::Button(("Remove##_freq_mgr_rem_" + _this->name).c_str(), ImVec2(ImGui::GetContentRegionAvail().x, 0))) {
            _this->deleteBookmarksOpen = true;
        }
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) { style::tooltip("Delete the selected bookmarks."); }
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
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
            style::tooltip("Change the selected bookmark - including its notes.\n"
                           "Select exactly one to use this.");
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
            // Said once at the top of the list rather than on every row, where it
            // would fight with the notes tooltip for the same hover.
            if (ImGui::IsItemHovered()) {
                style::tooltip("Double click a bookmark to tune to it. Click to select, shift or\n"
                               "ctrl click for several. A * means it has notes - hover to read them.");
            }
            for (auto& [name, bm] : _this->bookmarks) {
                bool vfoMissing = !bm.vfoName.empty() && !sigpath::vfoManager.vfoExists(bm.vfoName);
                ImGui::TableNextRow();
                // Mark the channel the scanner is on, so the list says where the
                // scan is instead of leaving it to be read off the frequency.
                if (_this->scanner.isScanning() && name == _this->scanner.getCurrentStation()) {
                    bool listening = (_this->scanner.getState() == Scanner::SCAN_LISTENING);
                    ImGui::TableSetBgColor(ImGuiTableBgTarget_RowBg1,
                                           ImGui::ColorConvertFloat4ToU32(listening ? ImVec4(0.15f, 0.45f, 0.2f, 1.0f) : ImVec4(0.4f, 0.33f, 0.1f, 1.0f)));
                }
                ImGui::TableSetColumnIndex(0);
                ImVec2 min = ImGui::GetCursorPos();

                // Stepping to a channel that is off the visible part of the list would
                // otherwise move the radio with nothing on screen to show for it.
                if (_this->scrollToNav && name == _this->navBookmarkName) {
                    ImGui::SetScrollHereY(0.5f);
                    _this->scrollToNav = false;
                }

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
                if (vfoMissing) { style::endDisabled(); }
                // One tooltip for the row, so a bookmark that is both missing its radio
                // and carries notes says both rather than whichever was checked first.
                if (ImGui::IsItemHovered() && (vfoMissing || !bm.notes.empty())) {
                    if (style::beginTooltip()) {
                        if (vfoMissing) {
                            ImGui::Text("Radio \"%s\" is not available", bm.vfoName.c_str());
                            if (!bm.notes.empty()) { ImGui::Separator(); }
                        }
                        if (!bm.notes.empty()) {
                            // Wrapped rather than left to run off the screen: notes are
                            // free text and some of them are paragraphs.
                            ImGui::PushTextWrapPos(400.0f * style::uiScale);
                            ImGui::TextUnformatted(bm.notes.c_str());
                            ImGui::PopTextWrapPos();
                        }
                        style::endTooltip();
                    }
                }
                if (ImGui::TableGetHoveredColumn() >= 0 && ImGui::IsItemHovered() && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) {
                    _this->setNavBookmark(name);
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
                // A quiet marker, so that notes can be found without hovering every row
                // in the list to look for them.
                if (!bm.notes.empty()) {
                    ImGui::SameLine();
                    ImGui::TextDisabled("*");
                }
//		std::string modeStr = (radio != nullptr && bm.modeIndex >= 0) ? demodModeList[radio->getDemodByIndex(bm.modeIndex)] : "DIGITAL";
//		ImGui::Text("%s %s", utils::formatFreq(bm.frequency).c_str(), modeStr.c_str());

                ImVec2 max = ImGui::GetCursorPos();
            }
            ImGui::EndTable();
        }


        // Step through the list a channel at a time, without having to find the next
        // one and double click it. The arrows read the way the list runs on screen:
        // Next moves further down it, Previous back up.
        {
            bool canStep = _this->anyBookmarkTunable() && _this->selectedListName != "";
            if (!canStep) { style::beginDisabled(); }

            ImGui::BeginTable(("freq_manager_nav_table" + _this->name).c_str(), 2);
            ImGui::TableNextRow();

            ImGui::TableSetColumnIndex(0);
            if (ImGui::Button(("Previous##_freq_mgr_nav_prev_" + _this->name).c_str(), ImVec2(ImGui::GetContentRegionAvail().x, 0))) {
                _this->stepBookmark(-1);
            }
            if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
                style::tooltip("Tune the channel above this one in the list, wrapping round at the top.\n"
                               "Stops a scan if one is running.");
            }

            ImGui::TableSetColumnIndex(1);
            if (ImGui::Button(("Next##_freq_mgr_nav_next_" + _this->name).c_str(), ImVec2(ImGui::GetContentRegionAvail().x, 0))) {
                _this->stepBookmark(1);
            }
            if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
                style::tooltip("Tune the channel below this one in the list, wrapping round at the bottom.\n"
                               "Stops a scan if one is running.");
            }

            ImGui::EndTable();

            // Which channel the buttons will step from. Without it the first press
            // after tuning by hand looks like it came from nowhere.
            if (!_this->navBookmarkName.empty() && _this->bookmarks.count(_this->navBookmarkName)) {
                ImGui::TextDisabled("On %s", _this->navBookmarkName.c_str());
            }
            else {
                ImGui::TextDisabled("Not on a channel");
            }

            if (!canStep) { style::endDisabled(); }
        }

        if (selectedNames.size() != 1 && _this->selectedListName != "") { style::beginDisabled(); }
        if (ImGui::Button(("Tune to bookmark##_freq_mgr_apply_" + _this->name).c_str(), ImVec2(menuWidth, 0))) {
            FrequencyBookmark& bm = _this->bookmarks[selectedNames[0]];
            _this->setNavBookmark(selectedNames[0]);
            applyBookmark(bm, gui::waterfall.selectedVFO);
            bm.selected = false;
        }
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
            style::tooltip("Puts the radio on the selected bookmark - frequency, mode, bandwidth\n"
                           "and its tone settings. Double clicking a row in the list does the same.");
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
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
            style::tooltip("Read a list exported from another copy of this program. Bookmarks\n"
                           "whose names are already in the list are left alone.\n"
                           "For a spreadsheet, use Import CSV below.");
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
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
            style::tooltip("Write the selected bookmarks in this program's own format, which\n"
                           "keeps everything exactly. Select some first.\n"
                           "For something a spreadsheet can open, use Export CSV below.");
        }
        if (selectedNames.size() == 0 && _this->selectedListName != "") { style::endDisabled(); }

        // Second row: the same two things in the format a spreadsheet can open.
        ImGui::TableNextRow();
        ImGui::TableSetColumnIndex(0);
        if (ImGui::Button(("Import CSV##_freq_mgr_impcsv_" + _this->name).c_str(), ImVec2(ImGui::GetContentRegionAvail().x, 0)) && !_this->importCsvOpen) {
            _this->importCsvOpen = true;
            _this->importCsvDialog = new pfd::open_file("Import bookmarks from CSV", "", { "CSV Files (*.csv)", "*.csv", "All Files", "*" });
        }
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
            style::tooltip("Reads a spreadsheet into the selected list. Needs at least a\n"
                              "'name' and a 'frequency' column; every other column is optional\n"
                              "and the order does not matter. Bookmarks already in the list are\n"
                              "updated, new ones are added, and nothing is deleted.");
        }

        ImGui::TableSetColumnIndex(1);
        if (ImGui::Button(("Export CSV##_freq_mgr_expcsv_" + _this->name).c_str(), ImVec2(ImGui::GetContentRegionAvail().x, 0)) && !_this->exportCsvOpen) {
            // The selection if there is one, the whole list otherwise - exporting a
            // frequency list usually means all of it, and having to select every row
            // first would be a chore.
            _this->exportCsvNames = selectedNames;
            if (_this->exportCsvNames.empty()) {
                for (auto& [bmName, bm] : _this->bookmarks) { _this->exportCsvNames.push_back(bmName); }
            }
            _this->exportCsvOpen = true;
            _this->exportCsvDialog = new pfd::save_file("Export bookmarks to CSV", "", { "CSV Files (*.csv)", "*.csv", "All Files", "*" });
        }
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
            style::tooltip("Writes the selected bookmarks, or the whole list if none are\n"
                              "selected, as a spreadsheet - frequency, mode, tone settings and\n"
                              "notes, one channel per row.");
        }
        ImGui::EndTable();

        if (ImGui::Button(("Select displayed lists##_freq_mgr_exp_" + _this->name).c_str(), ImVec2(menuWidth, 0))) {
            _this->selectListsOpen = true;
        }
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
            style::tooltip("Choose which lists draw their bookmarks on the waterfall. A list can\n"
                           "be kept without having it marked up on the spectrum.");
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

        if (_this->importCsvOpen && _this->importCsvDialog->ready()) {
            _this->importCsvOpen = false;
            std::vector<std::string> paths = _this->importCsvDialog->result();
            if (paths.size() > 0 && _this->listNames.size() > 0) {
                _this->reportCsvImport(_this->importBookmarksCsv(paths[0]));
            }
            delete _this->importCsvDialog;
            _this->importCsvDialog = NULL;
        }
        if (_this->exportCsvOpen && _this->exportCsvDialog->ready()) {
            _this->exportCsvOpen = false;
            std::string path = _this->exportCsvDialog->result();
            if (path != "") {
                // The file chooser does not always put the extension on, and a
                // frequency list saved without one will not open in a spreadsheet by
                // double clicking it.
                if (path.size() < 4 || csv::normaliseHeader(path.substr(path.size() - 4)) != ".csv") {
                    path += ".csv";
                }
                if (_this->exportBookmarksCsv(path, _this->exportCsvNames)) {
                    ImGui::InsertNotification({ ImGuiToastType_Success, 5000,
                                                "Exported %d bookmark(s).", (int)_this->exportCsvNames.size() });
                }
                else {
                    ImGui::InsertNotification({ ImGuiToastType_Error, 6000, "Could not write that file." });
                }
            }
            delete _this->exportCsvDialog;
            _this->exportCsvDialog = NULL;
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

        // Which channel the scan is on, where it goes next, and the level it is
        // comparing against. The line used to be plotted at the trigger level as if
        // it were an absolute dBFS value, when it is dB over the local noise, so it
        // sat far off the top of the chart and was never visible.
        _this->scanner.drawWaterfallOverlay(args);


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
                args.window->DrawList->AddRectFilled(clampedRectMin, clampedRectMax, bookmarkLabelColor(bm.worked, vfoMissing));
                _this->rects.emplace_back(Drawn { newRect, index } );
            }
            if (rectMin.x >= args.min.x && rectMax.x <= args.max.x) {
                if (vfoMissing) {
                    // Cross over the label: the radio this bookmark belongs to is gone.
                    // Drawn in the label's own text colour, so it stays legible against
                    // whatever colour the theme gives the label underneath it.
                    ImU32 crossColor = ImGui::ColorConvertFloat4ToU32(gui::themeManager.bookmarkTextColor);
                    ImVec2 crossCenter = ImVec2(centerXpos, rectMin.y + (nameSize.y / 2.0f));
                    float crossHalf = nameSize.y * 0.35f;
                    args.window->DrawList->AddLine(ImVec2(crossCenter.x - crossHalf, crossCenter.y - crossHalf), ImVec2(crossCenter.x + crossHalf, crossCenter.y + crossHalf), crossColor, 2.0f);
                    args.window->DrawList->AddLine(ImVec2(crossCenter.x - crossHalf, crossCenter.y + crossHalf), ImVec2(crossCenter.x + crossHalf, crossCenter.y - crossHalf), crossColor, 2.0f);
                } else {
                    args.window->DrawList->AddText(ImVec2(centerXpos - (nameSize.x / 2), rectMin.y), ImGui::ColorConvertFloat4ToU32(gui::themeManager.bookmarkTextColor), bm.bookmarkName.c_str());
                }
            }
            if (bm.bookmark.frequency >= args.lowFreq && bm.bookmark.frequency <= args.highFreq) {
                args.window->DrawList->AddLine(ImVec2(centerXpos, args.min.y), ImVec2(centerXpos, args.max.y), bookmarkLabelColor(bm.worked, vfoMissing));
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

        if (style::beginTooltip()) {
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
            style::endTooltip();
        }
    }

    json exportedBookmarks;
    bool importOpen = false;
    bool exportOpen = false;
    pfd::open_file* importDialog;
    pfd::save_file* exportDialog;

    bool importCsvOpen = false;
    bool exportCsvOpen = false;
    pfd::open_file* importCsvDialog = NULL;
    pfd::save_file* exportCsvDialog = NULL;
    // Which bookmarks the export was asked for, taken when the button was pressed
    // rather than when the dialog comes back - the selection can change while a modal
    // file chooser is up.
    std::vector<std::string> exportCsvNames;

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
            fbm.notes = bm.value("notes", "");
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

    // ---- CSV ----
    //
    // The JSON above is for passing a list to another copy of this program. CSV is for
    // the operator's own list: it opens in a spreadsheet, so the frequencies someone
    // has already typed up over the years can come in, and the ones in here can go out
    // and be sorted, filtered and printed like any other table.

    static std::string csvModeName(RadioModuleInterface* radio, int modeIndex) {
        if (radio == nullptr || modeIndex < 0) { return ""; }
        return demodModeName(radio->getDemodByIndex(modeIndex));
    }

    bool exportBookmarksCsv(const std::string& path, const std::vector<std::string>& names) {
        auto radio = (RadioModuleInterface*)core::moduleManager.getInterface(gui::waterfall.selectedVFO, "RadioModuleInterface");
        // Binary, because the rows already end in CRLF as the format requires, and a
        // text mode stream on Windows would turn each of those into CRCRLF.
        std::ofstream fs(wstr::str2wstr(path), std::ios::binary);
        if (!fs.is_open()) {
            flog::error("Could not open '{0}' for writing", path);
            return false;
        }

        fs << csv::row(bmcsv::columns());
        for (const auto& n : names) {
            auto it = bookmarks.find(n);
            if (it == bookmarks.end()) { continue; }
            const FrequencyBookmark& bm = it->second;
            const RadioToneSettings& t = bm.tone;
            std::vector<std::string> f = {
                n,
                bmcsv::fmtNumber(bm.frequency),
                bmcsv::fmtNumber(bm.bandwidth),
                csvModeName(radio, bm.modeIndex),
                bm.vfoName,
                bmcsv::fmtToneMode(t.mode),
                bmcsv::fmtNumber(t.ctcssFreq),
                std::to_string(t.dcsCode),
                bmcsv::fmtBool(t.dcsInverted),
                bmcsv::fmtBool(t.squelchEnabled),
                bmcsv::fmtBool(t.filterEnabled),
                bmcsv::fmtBool(t.identifyEnabled),
                bmcsv::fmtBool(t.tailCloseEnabled),
                bmcsv::encodeToneList(t),
                bm.notes
            };
            fs << csv::row(f);
        }
        fs.close();
        return true;
    }

    struct CsvImportResult {
        bool opened = false;
        bool headerFound = false;
        int added = 0;
        int updated = 0;
        int skipped = 0; // rows that carried no usable name or frequency
    };

    // Adds what is new and updates what is already there, matching on the bookmark
    // name. Updating rather than skipping is the point of the round trip: export the
    // list, fix it in a spreadsheet, bring it back. Nothing is ever deleted - a
    // bookmark the file does not mention is left exactly as it was - so the worst a
    // wrong file can do is add entries and change ones that share a name.
    CsvImportResult importBookmarksCsv(const std::string& path) {
        CsvImportResult res;
        std::ifstream fs(wstr::str2wstr(path), std::ios::binary);
        if (!fs.is_open()) {
            flog::error("Could not open '{0}'", path);
            return res;
        }
        res.opened = true;
        std::string text((std::istreambuf_iterator<char>(fs)), std::istreambuf_iterator<char>());
        fs.close();

        std::vector<std::vector<std::string>> rows = csv::parse(text);

        // The first row that has anything on it is the heading. Columns are matched by
        // name, so a file with its columns in another order, or with only some of
        // them, still works - which matters because the most useful file to import is
        // usually one this program did not write.
        size_t headerRow = 0;
        while (headerRow < rows.size() && csv::rowIsBlank(rows[headerRow])) { headerRow++; }
        if (headerRow >= rows.size()) { return res; }

        std::map<std::string, int> col;
        for (size_t i = 0; i < rows[headerRow].size(); i++) {
            col[csv::normaliseHeader(rows[headerRow][i])] = (int)i;
        }
        // Without these two there is no bookmark to make, and treating the first data
        // row as a heading would silently swallow it.
        if (!col.count("name") || !col.count("frequency")) { return res; }
        res.headerFound = true;

        auto field = [&](const std::vector<std::string>& row, const char* key) -> std::string {
            auto it = col.find(key);
            if (it == col.end() || it->second >= (int)row.size()) { return std::string(); }
            return row[it->second];
        };

        auto radio = (RadioModuleInterface*)core::moduleManager.getInterface(gui::waterfall.selectedVFO, "RadioModuleInterface");
        int defaultModeIndex = (radio != nullptr) ? radio->getDemodIndex(radio->getSelectedDemodId()) : -1;
        bool fileHasTone = col.count("tonemode") || col.count("ctcss") || col.count("dcscode");

        for (size_t r = headerRow + 1; r < rows.size(); r++) {
            const std::vector<std::string>& row = rows[r];
            if (csv::rowIsBlank(row)) { continue; }

            std::string bmName = field(row, "name");
            double freq = 0.0;
            if (bmName.empty() || !bmcsv::parseNumber(field(row, "frequency"), &freq)) {
                res.skipped++;
                continue;
            }

            FrequencyBookmark fbm;
            fbm.frequency = freq;
            fbm.bandwidth = 0.0;
            bmcsv::parseNumber(field(row, "bandwidth"), &fbm.bandwidth);
            fbm.vfoName = field(row, "vfo");
            fbm.notes = field(row, "notes");
            fbm.selected = false;

            int demodId = 0;
            fbm.modeIndex = (radio != nullptr && demodIdByName(field(row, "mode"), &demodId))
                                ? radio->getDemodIndex((DemodID)demodId)
                                : defaultModeIndex;

            // Only claim the file said something about tones if it actually had a
            // column for them. Otherwise the flag stays false and recalling the
            // bookmark leaves the radio's tone settings alone, which is what a file
            // from somewhere else means.
            fbm.hasTone = fileHasTone;
            RadioToneSettings& t = fbm.tone;
            t.mode = bmcsv::parseToneMode(field(row, "tonemode"));
            double ctcss = 100.0;
            if (bmcsv::parseNumber(field(row, "ctcss"), &ctcss)) { t.ctcssFreq = (float)ctcss; }
            double dcs = 23.0;
            if (bmcsv::parseNumber(field(row, "dcscode"), &dcs)) { t.dcsCode = (int)dcs; }
            t.dcsInverted = bmcsv::parseBool(field(row, "dcsinvert"), false);
            t.squelchEnabled = bmcsv::parseBool(field(row, "tonesquelch"), false);
            t.filterEnabled = bmcsv::parseBool(field(row, "tonefilter"), false);
            t.identifyEnabled = bmcsv::parseBool(field(row, "toneidentify"), false);
            t.tailCloseEnabled = bmcsv::parseBool(field(row, "tonetailclose"), true);
            bmcsv::decodeToneList(field(row, "tonelist"), t);

            if (bookmarks.find(bmName) != bookmarks.end()) { res.updated++; }
            else { res.added++; }
            bookmarks[bmName] = fbm;
        }

        if (res.added || res.updated) { saveByName(selectedListName); }
        return res;
    }

    void reportCsvImport(const CsvImportResult& res) {
        if (!res.opened) {
            ImGui::InsertNotification({ ImGuiToastType_Error, 6000, "Could not open that file." });
            return;
        }
        if (!res.headerFound) {
            ImGui::InsertNotification({ ImGuiToastType_Error, 8000,
                                        "That file has no usable heading row.\n"
                                        "A CSV needs at least a 'name' and a 'frequency' column." });
            return;
        }
        if (!res.added && !res.updated) {
            ImGui::InsertNotification({ ImGuiToastType_Warning, 6000,
                                        "Nothing imported - %d row(s) had no name or no frequency.", res.skipped });
            return;
        }
        // Skipped rows are worth a mention even on success: a file that half imported
        // is exactly the case where silence would be mistaken for everything working.
        if (res.skipped) {
            ImGui::InsertNotification({ ImGuiToastType_Warning, 8000,
                                        "Imported %d new and updated %d.\n%d row(s) skipped for having no name or no frequency.",
                                        res.added, res.updated, res.skipped });
        }
        else {
            ImGui::InsertNotification({ ImGuiToastType_Success, 5000,
                                        "Imported %d new and updated %d.", res.added, res.updated });
        }
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
    // What the accept list's pickers are showing, which is not part of the bookmark
    // until it is added.
    RadioToneListEntry editedToneAdd;

    std::vector<std::string> listNames;
    std::string listNamesTxt = "";
    std::string selectedListName = "";
    int selectedListId = 0;

    std::string editedListName;
    std::string firstEditedListName;

    std::vector<WaterfallBookmark> waterfallBookmarks;
    Scanner scanner{this};

    // Where the up/down buttons think the radio is in the list. Empty until something
    // has been tuned from here, which the buttons read as "not on a channel yet" and
    // step in from whichever end they are heading away from.
    std::string navBookmarkName;
    // Set when a step moves the position, so the list scrolls the new channel into
    // view on the frame it is drawn.
    bool scrollToNav = false;

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
            // Setting the mode tears the demodulator down and builds a new one even
            // when it is the mode already selected, which the scanner would do on
            // every hop between two bookmarks that share a mode. Ask first.
            int currentMode = -1;
            core::modComManager.callInterface(targetVfo, RADIO_IFACE_CMD_GET_MODE, NULL, &currentMode);
            if (currentMode != mode) {
                core::modComManager.callInterface(targetVfo, RADIO_IFACE_CMD_SET_MODE, &mode, NULL);
            }
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
    def["scanner"]["scanIntervalMs"] = 250.0f;
    def["scanner"]["settleMs"] = 120.0f;
    def["scanner"]["listenTimeSec"] = 10.0f;
    // These are in dB over the local noise, the same scale as the SNR meter and
    // the one the scanner clamps to 0..40, not absolute dBFS. -120 used to live
    // here, which made every station clear the detection threshold instantly.
    def["scanner"]["noiseFloor"] = 3.0f;
    def["scanner"]["signalMarginDb"] = 4.0f;
    def["scanner"]["squelchEnabled"] = false;
    def["scanner"]["carrierHoldMode"] = false;
    def["scanner"]["skipped"] = json::array();

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
