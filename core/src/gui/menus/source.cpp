#include <gui/menus/source.h>
#include <imgui.h>
#include <gui/gui.h>
#include <core.h>
#include <gui/style.h>
#include <signal_path/signal_path.h>
#include <utils/cty.h>
#include <utils/optionlist.h>
#include <gui/dialogs/dialog_box.h>

namespace sourcemenu {
    int sourceId = 0;
    EventHandler<std::string> sourcesChangedHandler;
    EventHandler<std::string> sourceUnregisterHandler;
    EventHandler<std::string> sourceSelectedHandler;
    OptionList<std::string, std::string> sources;
    std::string selectedSource;

    int decimId = 0;
    OptionList<int, int> decimations;

    bool iqCorrection = false;
    bool invertIQ = false;
    utils::LatLng operatorLatLng = utils::LatLng::invalid();
    char operatorCallsignRaw[30];
    utils::CTY::Callsign callsignFound;


    int offsetId = 0;
    double manualOffset = 0.0;
    std::string selectedOffset;
    double effectiveOffset = 0.0;
    OptionList<std::string, double> offsets;
    std::map<std::string, double> namedOffsets;

    bool showAddOffsetDialog = false;
    char newOffsetName[1024];
    double newOffset = 0.0;

    bool showDelOffsetDialog = false;
    std::string delOffsetName = "";

    // Offset IDs
    enum {
        OFFSET_ID_NONE,
        OFFSET_ID_MANUAL,
        OFFSET_ID_CUSTOM_BASE
    };

    void updateOffset() {
        // Compute the effective offset
        switch (offsetId) {
        case OFFSET_ID_NONE:
            effectiveOffset = 0;
            break;
        case OFFSET_ID_MANUAL:
            effectiveOffset = manualOffset;
            break;
        default:
            effectiveOffset = namedOffsets[offsets.name(offsetId)];
            break;
        }

        // Apply it
        sigpath::sourceManager.setTuningOffset(effectiveOffset);
    }

    void selectOffsetById(int id) {
        // Update the offset mode
        offsetId = id;
        selectedOffset = offsets.name(id);

        // Update the offset
        updateOffset();
    }

    void selectOffsetByName(const std::string& name) {
        // If the name doesn't exist, select 'None'
        if (!offsets.nameExists(name)) {
            selectOffsetById(OFFSET_ID_NONE);
            return;
        }

        // Select using the ID associated with the name
        selectOffsetById(offsets.nameId(name));
    }

    void refreshSources() {
        // Get sources
        auto sourceNames = sigpath::sourceManager.getSourceNames();

        // Define source options
        sources.clear();
        for (auto name : sourceNames) {
            sources.define(name, name, name);
        }
    }

    void selectSource(std::string name) {
        // If there is no source, give up
        if (sources.empty()) {
            sourceId = 0;
            selectedSource.clear();
            return;
        }

        // If a source with the given name doesn't exist, select the first source instead
        if (!sources.valueExists(name)) {
            selectSource(sources.value(0));
            return;
        }

        // Update the GUI variables
        sourceId = sources.valueId(name);
        selectedSource = name;

        // Select the source module
        sigpath::sourceManager.selectSource(name);
    }

    void onSourcesChanged(std::string name, void* ctx) {
        // Update the source list
        refreshSources();

        // Reselect the current source
        selectSource(selectedSource);
    }

    void onSourceUnregister(std::string name, void* ctx) {
        if (name != selectedSource) { return; }

        // The source being listened to is about to stop existing. Nothing used to
        // happen here, so the radio stayed in its playing state with no source under
        // it: the play button read as running, the source combo stayed greyed out
        // behind "Stop the radio to change the source", and the only thing that got
        // it back was the dead-source watchdog five seconds later - which then
        // blamed the device for going away when in fact it had been unloaded on
        // purpose from the Module Manager.
        //
        // Stopping here is the same path the stop button takes. The module has
        // already stopped its own hardware by this point (every source module calls
        // its stop handler before unregistering), so this is bookkeeping the rest of
        // the application needs rather than another attempt to touch the device.
        if (gui::mainWindow.sdrIsRunning()) {
            gui::mainWindow.setPlayState(false);
        }
    }

    void onSourceSelected(std::string name, void* ctx) {
        // Update GUI state to match the actual selected source
        if (sources.valueExists(name)) {
            sourceId = sources.valueId(name);
            selectedSource = name;
        }
    }

    void reloadOffsets() {
        // Clear list
        offsets.clear();
        namedOffsets.clear();

        // Define special offset modes
        offsets.define("None", OFFSET_ID_NONE);
        offsets.define("Manual", OFFSET_ID_MANUAL);

        // Acquire the config file
        core::configManager.acquire();

        // Load custom offsets
        auto ofs = core::configManager.conf["offsets"].items();
        for (auto& o : ofs) {
            namedOffsets[o.key()] = (double)o.value();
        }

        // Define custom offsets
        for (auto& [name, offset] : namedOffsets) {
            offsets.define(name, offsets.size());
        }

        // Release the config file
        core::configManager.release();
    }

    void init() {

        sigpath::iqFrontEnd.operatorCallsign.reserve(30);
        sigpath::iqFrontEnd.operatorLocation.reserve(30);
        // Load offset modes
        reloadOffsets();

        // Define decimation values
        decimations.define(1, "None", 1);
        decimations.define(2, "2x", 2);
        decimations.define(4, "4x", 4);
        decimations.define(8, "8x", 8);
        decimations.define(16, "16x", 16);
        decimations.define(32, "32x", 32);
        decimations.define(64, "64x", 64);

        // Acquire the config file
        core::configManager.acquire();

        // Load other settings
        std::string selectedSource = core::configManager.conf["source"];
        manualOffset = core::configManager.conf["manualOffset"];
        std::string selectedOffset = core::configManager.conf["selectedOffset"];
        iqCorrection = core::configManager.conf["iqCorrection"];
        invertIQ = core::configManager.conf["invertIQ"];

        // config.json is hand-editable and writable over the debug endpoint, so
        // the stored callsign can be longer than the buffer. Truncate, and leave
        // room for the terminator std::copy would not have written.
        std::string opcs = core::configManager.conf["operatorCallsign"];
        opcs.resize((std::min)(opcs.size(), sizeof(operatorCallsignRaw) - 1));
        std::copy(opcs.begin(), opcs.end(), operatorCallsignRaw);
        operatorCallsignRaw[opcs.size()] = '\0';
        callsignFound = utils::globalCty.findCallsign(operatorCallsignRaw);

        if (core::configManager.conf.contains("secondsAdjustment")) {
            sigpath::iqFrontEnd.secondsAdjustment = core::configManager.conf["secondsAdjustment"];
        }

        sigpath::iqFrontEnd.operatorCallsign.reserve(30);
        if (callsignFound.dxccname != "") {
            sigpath::iqFrontEnd.operatorCallsign = operatorCallsignRaw;
        }
        sigpath::iqFrontEnd.operatorLocation = core::configManager.conf["operatorLocation"];
        sigpath::iqFrontEnd.operatorLocation.reserve(30);

        auto ll = utils::gridToLatLng(sigpath::iqFrontEnd.operatorLocation);
        if (ll.isValid()) {
            operatorLatLng = ll;
        }

        int decimation = core::configManager.conf["decimation"];
        if (decimations.keyExists(decimation)) {
            decimId = decimations.keyId(decimation);
        }

        // Release the config file
        core::configManager.release();

        // Select the source module
        refreshSources();
        selectSource(selectedSource);

        // Update frontend settings
        sigpath::iqFrontEnd.setDCBlocking(iqCorrection);
        sigpath::iqFrontEnd.setInvertIQ(invertIQ);
        sigpath::iqFrontEnd.setDecimation(decimations.value(decimId));
        selectOffsetByName(selectedOffset);

        // Register handlers
        sourcesChangedHandler.handler = onSourcesChanged;
        sourceUnregisterHandler.handler = onSourceUnregister;
        sourceSelectedHandler.handler = onSourceSelected;
        sigpath::sourceManager.onSourceRegistered.bindHandler(&sourcesChangedHandler);
        sigpath::sourceManager.onSourceUnregister.bindHandler(&sourceUnregisterHandler);
        sigpath::sourceManager.onSourceUnregistered.bindHandler(&sourcesChangedHandler);
        sigpath::sourceManager.onSourceSelected.bindHandler(&sourceSelectedHandler);
    }

    void addOffset(const std::string& name, double offset) {
        // Acquire the config file
        core::configManager.acquire();

        // Define a new offset
        core::configManager.conf["offsets"][name] = offset;

        // Acquire the config file
        core::configManager.release(true);

        // Reload the offsets
        reloadOffsets();

        // Attempt to re-select the same one
        selectOffsetByName(selectedOffset);
    }

    void delOffset(const std::string& name) {
        // Acquire the config file
        core::configManager.acquire();

        // Define a new offset
        core::configManager.conf["offsets"].erase(name);

        // Acquire the config file
        core::configManager.release(true);

        // Reload the offsets
        reloadOffsets();

        // Attempt to re-select the same one
        selectOffsetByName(selectedOffset);
    }

    bool addOffsetDialog() {
        bool open = true;
        gui::mainWindow.lockWaterfallControls = true;

        float menuWidth = ImGui::GetContentRegionAvail().x;

        const char* id = "Add offset##sdrpp_add_offset_dialog_";
        ImGui::OpenPopup(id);

        if (ImGui::BeginPopup(id, ImGuiWindowFlags_NoResize)) {
            ImGui::LeftLabel("Name");
            ImGui::SetNextItemWidth(menuWidth - ImGui::GetCursorPosX());
            ImGui::InputText("##sdrpp_add_offset_name", newOffsetName, 1023);

            ImGui::LeftLabel("Offset");
            ImGui::SetNextItemWidth(menuWidth - ImGui::GetCursorPosX());
            ImGui::InputDouble("##sdrpp_add_offset_offset", &newOffset);

            bool nameExists = offsets.nameExists(newOffsetName);
            bool reservedName = !strcmp(newOffsetName, "None") || !strcmp(newOffsetName, "Manual");
            bool denyApply = !newOffsetName[0] || nameExists || reservedName;

            if (nameExists) {
                ImGui::TextColored(ImVec4(1.0f, 0.0f, 0.0f, 1.0f), "An offset with the given name already exists.");
            }
            else if (reservedName) {
                ImGui::TextColored(ImVec4(1.0f, 0.0f, 0.0f, 1.0f), "The given name is reserved.");
            }

            if (denyApply) { style::beginDisabled(); }
            if (ImGui::Button("Apply")) {
                addOffset(newOffsetName, newOffset);
                open = false;
            }
            if (denyApply) { style::endDisabled(); }
            ImGui::SameLine();
            if (ImGui::Button("Cancel")) {
                open = false;
            }
            ImGui::EndPopup();
        }
        return open;
    }

    // This menu holds four unrelated things: which radio to listen with, what is
    // done to its samples, how its frequencies are corrected, and who is operating
    // it. Without something to separate them the callsign box reads like a setting
    // of the SDR.
    void draw(void* ctx) {
        float itemWidth = ImGui::GetContentRegionAvail().x;
        float lineHeight = ImGui::GetTextLineHeightWithSpacing();
        float spacing = lineHeight - ImGui::GetTextLineHeight();
        bool running = gui::mainWindow.sdrIsRunning();

        if (running) { style::beginDisabled(); }

        ImGui::SetNextItemWidth(itemWidth);
        if (ImGui::Combo("##source", &sourceId, sources.txt)) {
            std::string newSource = sources.value(sourceId);
            selectSource(newSource);
            core::configManager.acquire();
            core::configManager.conf["source"] = newSource;
            core::configManager.release(true);
        }

        if (running) { style::endDisabled(); }
        // A disabled item is not "hovered" unless asked for explicitly, and this is
        // exactly the case where the reason is worth showing.
        if (running && ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
            ImGui::SetTooltip("Stop the radio to change the source");
        }

        // An empty combo with nothing said about it is the first thing a new
        // install shows if no source module got loaded.
        if (sources.empty()) {
            ImGui::PushTextWrapPos(0.0f);
            ImGui::TextColored(ImVec4(1.0f, 0.7f, 0.3f, 1.0f), "No sources are loaded. Add one under Module Manager, then restart.");
            ImGui::PopTextWrapPos();
        }

        // Rescan for devices while the menu is on screen and the radio is stopped,
        // so plugging an SDR in with the application already open is enough for it
        // to show up. The rate limiting and the enumeration itself live in the
        // source manager, which keeps the expensive part off this thread.
        if (!running) {
            sigpath::sourceManager.pollDeviceChanges();
        }

        sigpath::sourceManager.showSelectedMenu();

        ImGui::SectionHeader("SIGNAL");

        if (ImGui::Checkbox("IQ correction##_sdrpp_iq_corr", &iqCorrection)) {
            sigpath::iqFrontEnd.setDCBlocking(iqCorrection);
            core::configManager.acquire();
            core::configManager.conf["iqCorrection"] = iqCorrection;
            core::configManager.release(true);
        }
        ImGui::HelpMarker("Removes the DC spike sitting in the middle of the spectrum on most\nSDRs. Leave it on unless you are looking at something exactly at\nthe centre frequency.");

        if (ImGui::Checkbox("Invert IQ##_sdrpp_inv_iq", &invertIQ)) {
            sigpath::iqFrontEnd.setInvertIQ(invertIQ);
            core::configManager.acquire();
            core::configManager.conf["invertIQ"] = invertIQ;
            core::configManager.release(true);
        }
        ImGui::HelpMarker("Mirrors the spectrum left to right. Turn it on if USB and LSB come out\nthe wrong way round, which some hardware and some recordings need.");

        if (running) { style::beginDisabled(); }
        ImGui::LeftLabel("Decimation");
        ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x - (32.0f * style::uiScale));
        if (ImGui::Combo("##source_decim", &decimId, decimations.txt)) {
            sigpath::iqFrontEnd.setDecimation(decimations.value(decimId));
            core::configManager.acquire();
            core::configManager.conf["decimation"] = decimations.key(decimId);
            core::configManager.release(true);
        }
        if (running) { style::endDisabled(); }
        ImGui::HelpMarker("Divides the sample rate before anything else sees it: less spectrum on\nscreen, proportionally less CPU. Set while the radio is stopped.");

        ImGui::SectionHeader("FREQUENCY OFFSET");

        ImGui::LeftLabel("Mode");
        ImGui::SetNextItemWidth(itemWidth - ImGui::GetCursorPosX() - 2.0f * (lineHeight + 1.5f * spacing));
        if (ImGui::Combo("##_sdrpp_offset", &offsetId, offsets.txt)) {
            selectOffsetById(offsetId);
            core::configManager.acquire();
            core::configManager.conf["selectedOffset"] = offsets.key(offsetId);
            core::configManager.release(true);
        }
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip("For a transverter or converter: added to every frequency you tune, so\nthe display reads what is on the antenna. None if the SDR is connected\nstraight to it.");
        }
        ImGui::SameLine();
        ImGui::SetCursorPosX(ImGui::GetCursorPosX() - spacing);
        if (offsetId < OFFSET_ID_CUSTOM_BASE) { ImGui::BeginDisabled(); }
        if (ImGui::Button("-##_sdrpp_offset_del_", ImVec2(lineHeight + 0.5f * spacing, 0))) {
            delOffsetName = selectedOffset;
            showDelOffsetDialog = true;
        }
        if (offsetId < OFFSET_ID_CUSTOM_BASE) { ImGui::EndDisabled(); }
        ImGui::SameLine();
        ImGui::SetCursorPosX(ImGui::GetCursorPosX() - spacing);
        if (ImGui::Button("+##_sdrpp_offset_add_", ImVec2(lineHeight + 0.5f * spacing, 0))) {
            strcpy(newOffsetName, "New Offset");
            showAddOffsetDialog = true;
        }

        // Offset delete confirmation
        if (ImGui::GenericDialog("sdrpp_del_offset_confirm", showDelOffsetDialog, GENERIC_DIALOG_BUTTONS_YES_NO, []() {
                ImGui::Text("Deleting offset named \"%s\". Are you sure?", delOffsetName.c_str());
            }) == GENERIC_DIALOG_BUTTON_YES) {
            delOffset(delOffsetName);
        }

        // Offset add diaglog
        if (showAddOffsetDialog) { showAddOffsetDialog = addOffsetDialog(); }

        // Only the Manual entry is an editable value. The others show the offset
        // the chosen entry stands for, and used to be an identical looking box that
        // simply refused to take a keypress.
        ImGui::LeftLabel(offsetId == OFFSET_ID_MANUAL ? "Offset (Hz)" : "Offset (Hz), fixed");
        ImGui::FillWidth();
        if (offsetId == OFFSET_ID_MANUAL) {
            if (ImGui::InputDouble("##freq_offset", &manualOffset, 1.0, 100.0)) {
                updateOffset();
                core::configManager.acquire();
                core::configManager.conf["manualOffset"] = manualOffset;
                core::configManager.release(true);
            }
        }
        else {
            style::beginDisabled();
            ImGui::InputDouble("##freq_offset", &effectiveOffset, 1.0, 100.0);
            style::endDisabled();
        }
        if (offsetId != OFFSET_ID_MANUAL && ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
            ImGui::SetTooltip("Pick Manual above to type an offset, or + to save a named one");
        }

        ImGui::SectionHeader("OPERATOR");

        ImGui::LeftLabel("Callsign");
        ImGui::FillWidth();
        if (ImGui::InputText("##_my_callsign", operatorCallsignRaw, 12)) {
            if (strlen(operatorCallsignRaw) >= 3) {
                callsignFound = utils::globalCty.findCallsign(operatorCallsignRaw);
                if (callsignFound.dxccname != "") {
                    sigpath::iqFrontEnd.operatorCallsign.resize(strlen(sigpath::iqFrontEnd.operatorCallsign.data()));
                    sigpath::iqFrontEnd.operatorCallsign = operatorCallsignRaw;
                }
                else {
                    sigpath::iqFrontEnd.operatorCallsign = "";
                }
                core::configManager.acquire();
                core::configManager.conf["operatorCallsign"] = sigpath::iqFrontEnd.operatorCallsign;
                core::configManager.release(true);
            }
            else {
                callsignFound.dxccname = "[invalid]";
                sigpath::iqFrontEnd.operatorCallsign = "";
                core::configManager.acquire();
                core::configManager.conf["operatorCallsign"] = "";
                core::configManager.release(true);
            }
        }
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip("Yours. Nothing here changes how the radio behaves; it is what the\ndecoders and reporting features put in what they send.");
        }
        // Empty before anything has been typed, rather than reading as a failure.
        if (operatorCallsignRaw[0] == 0) {
            ImGui::TextDisabled("DXCC: not set");
        }
        else if (callsignFound.dxccname.empty() || callsignFound.dxccname == "[invalid]") {
            ImGui::TextColored(ImVec4(1.0f, 0.7f, 0.3f, 1.0f), "DXCC: not recognised");
        }
        else {
            ImGui::Text("DXCC: %s", callsignFound.dxccname.c_str());
        }

        ImGui::LeftLabel("Grid square");
        auto gwSize = ImGui::CalcTextSize("WWWWWWWW");
        ImGui::SetNextItemWidth(gwSize.x);
        if (ImGui::InputTextWithHint("##_my_grid", "IO91wm", sigpath::iqFrontEnd.operatorLocation.data(), 8)) {
            sigpath::iqFrontEnd.operatorLocation.resize(strlen(sigpath::iqFrontEnd.operatorLocation.data()));
            core::configManager.acquire();
            core::configManager.conf["operatorLocation"] = sigpath::iqFrontEnd.operatorLocation;
            core::configManager.release(true);

            operatorLatLng = utils::gridToLatLng(sigpath::iqFrontEnd.operatorLocation);
        }
        ImGui::SameLine();
        if (operatorLatLng.isValid()) {
            ImGui::TextDisabled("%+02.2f %+02.2f", operatorLatLng.lat, operatorLatLng.lon);
        }
        else if (sigpath::iqFrontEnd.operatorLocation.empty()) {
            ImGui::TextDisabled("not set");
        }
        else {
            ImGui::TextColored(ImVec4(1.0f, 0.7f, 0.3f, 1.0f), "not a grid square");
        }

        // This was the last row of OPERATOR, under the callsign and the grid square,
        // where it read as another thing about the person at the radio. It is not:
        // it is what the time-slotted decoders count seconds from.
        ImGui::SectionHeader("CLOCK");

        ImGui::LeftLabel("Correction");
        ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x - (32.0f * style::uiScale));
        if (ImGui::SliderInt("##_sdrpp_time_correction", &sigpath::iqFrontEnd.secondsAdjustment, -15, 15, "%d s")) {
            core::configManager.acquire();
            core::configManager.conf["secondsAdjustment"] = sigpath::iqFrontEnd.secondsAdjustment;
            core::configManager.release(true);
        }
        ImGui::HelpMarker("Shifts the clock the time-slotted decoders work from. FT8 and the like\nneed the computer to be within a second or so of real time; leave this at\n0 unless decoding fails and you know the clock is out.");
    }
}
