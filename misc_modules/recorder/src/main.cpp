#include <imgui.h>
#include <module.h>
#include <dsp/types.h>
#include <dsp/stream.h>
#include <dsp/bench/peak_level_meter.h>
#include <dsp/sink/handler_sink.h>
#include <dsp/routing/splitter.h>
#include <dsp/audio/volume.h>
#include <dsp/convert/stereo_to_mono.h>
#include <thread>
#include <ctime>
#include <gui/gui.h>
#include <filesystem>
#include <system_error>
#include <signal_path/signal_path.h>
#include <config.h>
#include <gui/style.h>
#include <gui/widgets/volume_meter.h>
#include <regex>
#include <gui/widgets/folder_select.h>
#include <recorder_interface.h>
#include <core.h>
#include <radio_interface.h>
#include <utils/optionlist.h>
#include <utils/wav.h>
#include <radio_module_interface.h>

#define CONCAT(a, b) ((std::string(a) + b).c_str())

#define SILENCE_LVL 10e-6

SDRPP_MOD_INFO{
    /* Name:            */ "recorder",
    /* Description:     */ "Recorder module for SDR++",
    /* Author:          */ "Ryzerth",
    /* Version:         */ 0, 3, 0,
    /* Max instances    */ -1
};

ConfigManager config;

namespace {
    std::string formatBytes(uint64_t bytes) {
        const char* units[] = { "B", "KB", "MB", "GB", "TB" };
        double value = (double)bytes;
        int unit = 0;
        while (value >= 1024.0 && unit < 4) {
            value /= 1024.0;
            unit++;
        }
        char buf[64];
        snprintf(buf, sizeof buf, (unit > 0 && value < 10.0) ? "%.1f %s" : "%.0f %s", value, units[unit]);
        return std::string(buf);
    }

    // HH:MM:SS, for a running clock.
    std::string formatClock(uint64_t seconds) {
        char buf[32];
        snprintf(buf, sizeof buf, "%02d:%02d:%02d", (int)(seconds / 3600), (int)((seconds / 60) % 60), (int)(seconds % 60));
        return std::string(buf);
    }

    // Rounded and in words, for "about this long".
    std::string formatSpan(uint64_t seconds) {
        char buf[64];
        if (seconds < 90) { snprintf(buf, sizeof buf, "%d seconds", (int)seconds); }
        else if (seconds < 5400) { snprintf(buf, sizeof buf, "%d minutes", (int)(seconds / 60)); }
        else if (seconds < 172800) { snprintf(buf, sizeof buf, "%.1f hours", (double)seconds / 3600.0); }
        else { snprintf(buf, sizeof buf, "%.1f days", (double)seconds / 86400.0); }
        return std::string(buf);
    }
}

class RecorderModule : public ModuleManager::Instance {
public:
    RecorderModule(std::string name) : folderSelect("%ROOT%/recordings") {
        this->name = name;
        root = std::string(core::getRoot());
        strcpy(nameTemplate, "$t_$f_$h-$m-$s_$d-$M-$y");

        // Define option lists
        containers.define("WAV", wav::FORMAT_WAV);
        // containers.define("RF64", wav::FORMAT_RF64); // Disabled for now
        sampleTypes.define(wav::SAMP_TYPE_UINT8, "Uint8", wav::SAMP_TYPE_UINT8);
        sampleTypes.define(wav::SAMP_TYPE_INT16, "Int16", wav::SAMP_TYPE_INT16);
        sampleTypes.define(wav::SAMP_TYPE_INT32, "Int32", wav::SAMP_TYPE_INT32);
        sampleTypes.define(wav::SAMP_TYPE_FLOAT32, "Float32", wav::SAMP_TYPE_FLOAT32);


        // Load default config for option lists
        containerId = containers.valueId(wav::FORMAT_WAV);
        sampleTypeId = sampleTypes.valueId(wav::SAMP_TYPE_INT16);

        // Load config
        config.acquire();
        if (config.conf[name].contains("mode")) {
            recMode = config.conf[name]["mode"];
        }
        if (config.conf[name].contains("recPath")) {
            folderSelect.setPath(config.conf[name]["recPath"]);
        }
        if (config.conf[name].contains("container") && containers.keyExists(config.conf[name]["container"])) {
            containerId = containers.keyId(config.conf[name]["container"]);
        }
        if (config.conf[name].contains("sampleType") && sampleTypes.keyExists(config.conf[name]["sampleType"])) {
            sampleTypeId = sampleTypes.keyId(config.conf[name]["sampleType"]);
        }
        if (config.conf[name].contains("audioStream")) {
            selectedStreamName = config.conf[name]["audioStream"];
        }
        if (config.conf[name].contains("audioVolume")) {
            audioVolume = config.conf[name]["audioVolume"];
        }
        if (config.conf[name].contains("stereo")) {
            stereo = config.conf[name]["stereo"];
        }
        if (config.conf[name].contains("ignoreSilence")) {
            ignoreSilence = config.conf[name]["ignoreSilence"];
        }
        if (config.conf[name].contains("nameTemplate")) {
            std::string _nameTemplate = config.conf[name]["nameTemplate"];
            if (_nameTemplate.length() > sizeof(nameTemplate)-1) {
                _nameTemplate = _nameTemplate.substr(0, sizeof(nameTemplate)-1);
            }
            strcpy(nameTemplate, _nameTemplate.c_str());
        }
        config.release();

        // Init audio path
        volume.init(NULL, audioVolume, false);
        splitter.init(&volume.out);
        splitter.bindStream(&meterStream);
        splitter.origin = "Recorder(misc_modules).splitter";
        meter.init(&meterStream);
        s2m.init(&stereoStream);

        // Init sinks
        basebandSink.init(NULL, complexHandler, this);
        stereoSink.init(&stereoStream, stereoHandler, this);
        monoSink.init(&s2m.out, monoHandler, this);

        gui::menu.registerEntry(name, menuHandler, this);
        core::modComManager.registerInterface("recorder", name, moduleInterfaceHandler, this);
    }

    ~RecorderModule() {
        std::lock_guard<std::recursive_mutex> lck(recMtx);
        core::modComManager.unregisterInterface(name);
        gui::menu.removeEntry(name);
        stop();
        deselectStream();
        sigpath::sinkManager.onStreamRegistered.unbindHandler(&onStreamRegisteredHandler);
        sigpath::sinkManager.onStreamUnregister.unbindHandler(&onStreamUnregisterHandler);
        meter.stop();
    }

    void postInit() {
        // Enumerate streams
        audioStreams.clear();
        auto names = sigpath::sinkManager.getStreamNames();
        for (const auto& name : names) {
            audioStreams.define(name, name, name);
        }

        // Bind stream register/unregister handlers
        onStreamRegisteredHandler.ctx = this;
        onStreamRegisteredHandler.handler = streamRegisteredHandler;
        sigpath::sinkManager.onStreamRegistered.bindHandler(&onStreamRegisteredHandler);
        onStreamUnregisterHandler.ctx = this;
        onStreamUnregisterHandler.handler = streamUnregisterHandler;
        sigpath::sinkManager.onStreamUnregister.bindHandler(&onStreamUnregisterHandler);

        // Select the stream
        selectStream(selectedStreamName);
    }

    void enable() {
        enabled = true;
    }

    void disable() {
        enabled = false;
    }

    bool isEnabled() {
        return enabled;
    }

    // Why the Record button is greyed out, or an empty string when it is not.
    // Everything that can stop a recording from starting is decided here, so the
    // button and start() cannot disagree about it.
    std::string recordBlockedReason() {
        if (!folderSelect.pathIsValid()) { return "That folder does not exist. Pick one that does."; }
        if (recMode == RECORDER_MODE_AUDIO) {
            if (selectedStreamName.empty()) { return "No audio stream to record. Start a radio first."; }
        }
        if (currentSamplerate() == 0) { return "The sample rate is not known yet. Start the radio first."; }
        return "";
    }

    // The rate the recording would run at, whether or not one is running. Used for
    // the size estimate as well as by start().
    uint64_t currentSamplerate() {
        if (recording) { return samplerate; }
        if (recMode == RECORDER_MODE_AUDIO) {
            if (selectedStreamName.empty()) { return 0; }
            return sigpath::sinkManager.getStreamSampleRate(selectedStreamName);
        }
        return sigpath::iqFrontEnd.getSampleRate();
    }

    int currentChannels() {
        return (recMode == RECORDER_MODE_AUDIO && !stereo) ? 1 : 2;
    }

    // The name the next recording would get. Rebuilt when something it depends on
    // changes, and at most twice a second otherwise, because building it runs nine
    // regex replacements and the clock fields in it only tick once a second.
    const std::string& fileNamePreview() {
        std::string key = std::string(nameTemplate) + "|" + std::to_string(recMode) + "|" + selectedStreamName;
        double now = ImGui::GetTime();
        if (key != previewKey || (now - previewTime) > 0.5) {
            previewKey = key;
            previewTime = now;
            std::string vfoName = (recMode == RECORDER_MODE_AUDIO) ? selectedStreamName : "";
            previewName = genFileName(nameTemplate, recMode, vfoName) + ".wav";
            previewPath = expandString(folderSelect.path + "/" + previewName);
        }
        return previewName;
    }

    // Rate limited: this is a filesystem call and the panel redraws every frame.
    uint64_t freeSpace() {
        double now = ImGui::GetTime();
        if ((now - spaceTime) > 1.0) {
            spaceTime = now;
            std::error_code ec;
            auto info = std::filesystem::space(std::filesystem::path(expandString(folderSelect.path)), ec);
            cachedFreeSpace = ec ? 0 : (uint64_t)info.available;
        }
        return cachedFreeSpace;
    }

    int bytesPerFrame() {
        int bits = 16;
        switch (sampleTypes[sampleTypeId]) {
        case wav::SAMP_TYPE_UINT8: bits = 8; break;
        case wav::SAMP_TYPE_INT16: bits = 16; break;
        case wav::SAMP_TYPE_INT32: bits = 32; break;
        case wav::SAMP_TYPE_FLOAT32: bits = 32; break;
        default: break;
        }
        return (bits / 8) * currentChannels();
    }

    void start() {
        std::lock_guard<std::recursive_mutex> lck(recMtx);
        if (recording) { return; }

        // Used to fail silently here, leaving the panel showing "Idle" as though
        // the button had not been pressed at all.
        lastError = recordBlockedReason();
        if (!lastError.empty()) { return; }

        // Configure the wav writer
        samplerate = currentSamplerate();
        writer.setFormat(containers[containerId]);
        writer.setChannels(currentChannels());
        writer.setSampleType(sampleTypes[sampleTypeId]);
        writer.setSamplerate(samplerate);

        // Open file
        std::string vfoName = (recMode == RECORDER_MODE_AUDIO) ? selectedStreamName : "";
        std::string extension = ".wav";
        std::string expandedPath = expandString(folderSelect.path + "/" + genFileName(nameTemplate, recMode, vfoName) + extension);
        if (!writer.open(expandedPath)) {
            flog::error("Failed to open file for recording: {0}", expandedPath);
            lastError = "Could not open " + expandedPath;
            return;
        }
        currentPath = expandedPath;
        recBytesPerFrame = bytesPerFrame();

        // Open audio stream or baseband
        if (recMode == RECORDER_MODE_AUDIO) {
            // Start correct path depending on 
            if (stereo) {
                stereoSink.start();
            }
            else {
                s2m.start();
                monoSink.start();
            }
            splitter.bindStream(&stereoStream);
        }
        else {
            // Create and bind IQ stream
            basebandStream = new dsp::stream<dsp::complex_t>();
            basebandSink.setInput(basebandStream);
            basebandSink.start();
            sigpath::iqFrontEnd.bindIQStream(basebandStream);
        }

        recording = true;
    }

    void stop() {
        std::lock_guard<std::recursive_mutex> lck(recMtx);
        if (!recording) { return; }

        // Close audio stream or baseband
        if (recMode == RECORDER_MODE_AUDIO) {
            splitter.unbindStream(&stereoStream);
            monoSink.stop();
            stereoSink.stop();
            s2m.stop();
            
        }
        else {
            // Unbind and destroy IQ stream
            sigpath::iqFrontEnd.unbindIQStream(basebandStream);
            basebandSink.stop();
            delete basebandStream;
        }

        // Close file
        writer.close();
        
        recording = false;
    }

private:
    static void menuHandler(void* ctx) {
        RecorderModule* _this = (RecorderModule*)ctx;
        float menuWidth = ImGui::GetContentRegionAvail().x;

        // The panel was one run of controls from the mode switch to the record
        // button, so the container and sample type - which decide the file - read as
        // if they belonged to the audio meters below them.
        ImGui::SectionHeader("WHAT TO RECORD");

        // Recording mode
        if (_this->recording) { style::beginDisabled(); }
        ImGui::BeginGroup();
        ImGui::Columns(2, CONCAT("RecorderModeColumns##_", _this->name), false);
        if (ImGui::RadioButton(CONCAT("Baseband##_recorder_mode_", _this->name), _this->recMode == RECORDER_MODE_BASEBAND)) {
            _this->recMode = RECORDER_MODE_BASEBAND;
            config.acquire();
            config.conf[_this->name]["mode"] = _this->recMode;
            config.release(true);
        }
        ImGui::NextColumn();
        if (ImGui::RadioButton(CONCAT("Audio##_recorder_mode_", _this->name), _this->recMode == RECORDER_MODE_AUDIO)) {
            _this->recMode = RECORDER_MODE_AUDIO;
            config.acquire();
            config.conf[_this->name]["mode"] = _this->recMode;
            config.release(true);
        }
        ImGui::Columns(1, CONCAT("EndRecorderModeColumns##_", _this->name), false);
        ImGui::EndGroup();
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
            style::tooltip("Baseband records the raw IQ of the whole visible spectrum, which is\n"
                              "large but can be replayed and retuned later. Audio records what you\n"
                              "are listening to on one stream.");
        }

        // Which stream is the other half of "what to record", so it belongs here
        // rather than three sections down under the meters.
        if (_this->recMode == RECORDER_MODE_AUDIO) {
            ImGui::LeftLabel("Stream");
            ImGui::FillWidth();
            if (ImGui::Combo(CONCAT("##_recorder_stream_", _this->name), &_this->streamId, _this->audioStreams.txt)) {
                _this->selectStream(_this->audioStreams.value(_this->streamId));
                config.acquire();
                config.conf[_this->name]["audioStream"] = _this->audioStreams.key(_this->streamId);
                config.release(true);
            }
            if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
                style::tooltip("Which audio output to record. Each radio and each secondary output is\nits own stream.");
            }
        }

        ImGui::SectionHeader("FILE");

        // Recording path
        if (_this->folderSelect.render("##_recorder_fold_" + _this->name)) {
            if (_this->folderSelect.pathIsValid()) {
                config.acquire();
                config.conf[_this->name]["recPath"] = _this->folderSelect.path;
                config.release(true);
            }
        }

        ImGui::LeftLabel("Name template");
        ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x - (30.0f * style::uiScale));
        if (ImGui::InputText(CONCAT("##_recorder_name_template_", _this->name), _this->nameTemplate, 1023)) {
            config.acquire();
            config.conf[_this->name]["nameTemplate"] = _this->nameTemplate;
            config.release(true);
        }
        ImGui::SameLine();
        ImGui::TextDisabled("(?)");
        if (ImGui::IsItemHovered()) {
            style::tooltip("$t   audio or baseband\n"
                              "$f   frequency, in Hz\n"
                              "$r   mode (NFM, USB, ...)\n"
                              "$h $m $s   hour, minute, second\n"
                              "$d $M $y   day, month, year\n"
                              "Anything else is kept as typed. .wav is added for you.");
        }

        // One line for the file, in the same place whether it is the one being
        // written or the one the template would produce next. The full path is a
        // tooltip rather than another line of text.
        {
            bool live = _this->recording && !_this->currentPath.empty();
            std::string shown = live ? std::filesystem::path(_this->currentPath).filename().string()
                                     : _this->fileNamePreview();
            ImGui::PushTextWrapPos(0.0f);
            ImGui::TextDisabled("%s", shown.c_str());
            ImGui::PopTextWrapPos();
            if (ImGui::IsItemHovered()) {
                style::tooltip("%s\n%s", live ? "Writing to" : "Next recording goes to",
                                  live ? _this->currentPath.c_str() : _this->previewPath.c_str());
            }
        }

        ImGui::LeftLabel("Container");
        ImGui::FillWidth();
        if (ImGui::Combo(CONCAT("##_recorder_container_", _this->name), &_this->containerId, _this->containers.txt)) {
            config.acquire();
            config.conf[_this->name]["container"] = _this->containers.key(_this->containerId);
            config.release(true);
        }

        ImGui::LeftLabel("Sample type");
        ImGui::FillWidth();
        if (ImGui::Combo(CONCAT("##_recorder_st_", _this->name), &_this->sampleTypeId, _this->sampleTypes.txt)) {
            config.acquire();
            config.conf[_this->name]["sampleType"] = _this->sampleTypes.key(_this->sampleTypeId);
            config.release(true);
        }

        // Baseband at a few Msps fills a disk far faster than anyone expects, and
        // the format choices in front of this double or halve it.
        uint64_t rate = _this->currentSamplerate();
        if (rate > 0) {
            double bytesPerMinute = (double)rate * (double)_this->bytesPerFrame() * 60.0;
            ImGui::TextDisabled("%s per minute at %.0f kS/s", formatBytes((uint64_t)bytesPerMinute).c_str(), (double)rate / 1000.0);
            if (ImGui::IsItemHovered()) {
                style::tooltip("A WAV file cannot go past 4 GB. At this rate that is about %s.",
                                  formatSpan((uint64_t)(4294967296.0 / std::max<double>(1.0, bytesPerMinute / 60.0))).c_str());
            }
        }

        // Stereo doubles the size of the file and Skip silence decides what goes into
        // it, so both belong with the format above rather than under the meters.
        if (_this->recMode == RECORDER_MODE_AUDIO) {
            if (ImGui::Checkbox(CONCAT("Stereo##_recorder_stereo_", _this->name), &_this->stereo)) {
                config.acquire();
                config.conf[_this->name]["stereo"] = _this->stereo;
                config.release(true);
            }
            ImGui::HelpMarker("Two channels instead of one. Twice the file for no more information\nunless the source really is stereo, as broadcast FM is.");
        }

        if (_this->recording) { style::endDisabled(); }

        // Outside the block above on purpose: unlike everything else about the file,
        // this one is decided per buffer as it is written, so it can be turned on and
        // off part way through a recording.
        if (_this->recMode == RECORDER_MODE_AUDIO) {
            if (ImGui::Checkbox(CONCAT("Skip silence##_recorder_ignore_silence_", _this->name), &_this->ignoreSilence)) {
                config.acquire();
                config.conf[_this->name]["ignoreSilence"] = _this->ignoreSilence;
                config.release(true);
            }
            ImGui::HelpMarker("Write nothing while the audio is below about -100 dB, so a quiet\n"
                              "channel does not fill the file. The recording clock stops with it,\n"
                              "so the file has no gaps and no idea how long the silence was.");
        }

        // The meters and the gain that feeds them are the one part of this panel that
        // can be touched while it is recording, which is easier to see when they are
        // not interleaved with four settings that cannot.
        if (_this->recMode == RECORDER_MODE_AUDIO) {
            ImGui::SectionHeader("LEVEL");

            _this->updateAudioMeter(_this->audioLvl);
            ImGui::FillWidth();
            ImGui::VolumeMeter(_this->audioLvl.l, _this->audioLvl.l, -60, 10);
            ImGui::FillWidth();
            ImGui::VolumeMeter(_this->audioLvl.r, _this->audioLvl.r, -60, 10);

            ImGui::FillWidth();
            if (ImGui::SliderFloat(CONCAT("##_recorder_vol_", _this->name), &_this->audioVolume, 0, 1, "")) {
                _this->volume.setVolume(_this->audioVolume);
                config.acquire();
                config.conf[_this->name]["audioVolume"] = _this->audioVolume;
                config.release(true);
            }
            if (ImGui::IsItemHovered()) {
                style::tooltip("Gain applied to what is written to the file, not to what you hear.\nKeep the meters off the right hand end.");
            }
        }

        ImGui::Separator();

        // Record button. The guard used to be worked out here and then ignored, so
        // pressing Record with a bad folder did nothing at all and the panel went
        // on saying "Idle".
        std::string blocked = _this->recordBlockedReason();
        if (!_this->recording) {
            if (!blocked.empty()) { style::beginDisabled(); }
            if (ImGui::Button(CONCAT("Record##_recorder_rec_", _this->name), ImVec2(menuWidth, 0))) {
                _this->start();
            }
            if (!blocked.empty()) { style::endDisabled(); }

            if (!blocked.empty()) {
                ImGui::PushTextWrapPos(0.0f);
                ImGui::TextColored(ImVec4(1.0f, 0.7f, 0.3f, 1.0f), "%s", blocked.c_str());
                ImGui::PopTextWrapPos();
            }
            else if (!_this->lastError.empty()) {
                ImGui::PushTextWrapPos(0.0f);
                ImGui::TextColored(ImVec4(1.0f, 0.5f, 0.4f, 1.0f), "%s", _this->lastError.c_str());
                ImGui::PopTextWrapPos();
            }
            else if (!_this->currentPath.empty()) {
                ImGui::TextDisabled("Saved  %s", std::filesystem::path(_this->currentPath).filename().string().c_str());
                if (ImGui::IsItemHovered()) { style::tooltip("%s", _this->currentPath.c_str()); }
            }
            else {
                ImGui::TextDisabled("Idle  --:--:--");
            }
        }
        else {
            if (ImGui::Button(CONCAT("Stop##_recorder_rec_", _this->name), ImVec2(menuWidth, 0))) {
                _this->stop();
            }

            uint64_t written = _this->writer.getSamplesWritten();
            uint64_t seconds = (_this->samplerate > 0) ? (written / _this->samplerate) : 0;
            uint64_t bytes = written * (uint64_t)_this->recBytesPerFrame;

            // State, elapsed, size and headroom on one line. Only the things that
            // need doing something about get a line of their own.
            uint64_t freeBytes = _this->freeSpace();
            double perSecond = (double)_this->samplerate * (double)_this->recBytesPerFrame;
            bool lowOnSpace = (freeBytes > 0) && (perSecond > 0.0) && (((double)freeBytes / perSecond) < 300.0);

            if (_this->ignoreSilence && _this->ignoringSilence) {
                ImGui::TextColored(ImVec4(1.0f, 1.0f, 0.0f, 1.0f), "Paused  %s", formatClock(seconds).c_str());
            }
            else {
                ImGui::TextColored(ImVec4(1.0f, 0.3f, 0.3f, 1.0f), "Recording  %s", formatClock(seconds).c_str());
            }
            ImGui::SameLine();
            ImGui::TextDisabled("%s", formatBytes(bytes).c_str());
            if (freeBytes > 0 && !lowOnSpace) {
                ImGui::SameLine();
                ImGui::TextDisabled("%s free", formatBytes(freeBytes).c_str());
            }

            // A full RIFF container drops everything written after it without a
            // word, so this is the only sign the recording has stopped growing.
            if (_this->writer.isFull()) {
                ImGui::PushTextWrapPos(0.0f);
                ImGui::TextColored(ImVec4(1.0f, 0.3f, 0.3f, 1.0f), "File hit the 4 GB WAV limit and has stopped growing. Stop and start a new one.");
                ImGui::PopTextWrapPos();
            }
            if (lowOnSpace) {
                ImGui::PushTextWrapPos(0.0f);
                ImGui::TextColored(ImVec4(1.0f, 0.5f, 0.4f, 1.0f), "%s free, about %s left",
                                   formatBytes(freeBytes).c_str(), formatSpan((uint64_t)((double)freeBytes / perSecond)).c_str());
                ImGui::PopTextWrapPos();
            }
        }
    }

    void selectStream(std::string name) {
        std::lock_guard<std::recursive_mutex> lck(recMtx);
        deselectStream();

        if (audioStreams.empty()) {
            selectedStreamName.clear();
            return;
        }
        else if (!audioStreams.keyExists(name)) {
            selectStream(audioStreams.key(0));
            return;
        }

        audioStream = sigpath::sinkManager.bindStream(name);
        if (!audioStream) { return; }
        selectedStreamName = name;
        audioStream->origin = "recorder(module).audioStream";
        streamId = audioStreams.keyId(name);
        volume.setInput(audioStream);
        startAudioPath();
    }

    void deselectStream() {
        std::lock_guard<std::recursive_mutex> lck(recMtx);
        if (selectedStreamName.empty() || !audioStream) {
            selectedStreamName.clear();
            return;
        }
        if (recording && recMode == RECORDER_MODE_AUDIO) { stop(); }
        stopAudioPath();
        sigpath::sinkManager.unbindStream(selectedStreamName, audioStream);
        selectedStreamName.clear();
        audioStream = NULL;
    }

    void startAudioPath() {
        volume.start();
        splitter.start();
        meter.start();
    }

    void stopAudioPath() {
        volume.stop();
        splitter.stop();
        meter.stop();
    }

    static void streamRegisteredHandler(std::string name, void* ctx) {
        RecorderModule* _this = (RecorderModule*)ctx;

        // Add new stream to the list
        _this->audioStreams.define(name, name, name);

        // If no stream is selected, select new stream. If not, update the menu ID. 
        if (_this->selectedStreamName.empty()) {
            _this->selectStream(name);
        }
        else {
            _this->streamId = _this->audioStreams.keyId(_this->selectedStreamName);
        }
    }

    static void streamUnregisterHandler(std::string name, void* ctx) {
        RecorderModule* _this = (RecorderModule*)ctx;

        // Remove stream from list
        _this->audioStreams.undefineKey(name);

        // If the stream is in used, deselect it and reselect default. Otherwise, update ID.
        if (_this->selectedStreamName == name) {
            _this->selectStream("");
        }
        else {
            _this->streamId = _this->audioStreams.keyId(_this->selectedStreamName);
        }
    }

    void updateAudioMeter(dsp::stereo_t& lvl) {
        // Note: Yes, using the natural log is on purpose, it just gives a more beautiful result.
        double frameTime = 1.0 / ImGui::GetIO().Framerate;
        lvl.l = std::clamp<float>(lvl.l - (frameTime * 50.0), -90.0f, 10.0f);
        lvl.r = std::clamp<float>(lvl.r - (frameTime * 50.0), -90.0f, 10.0f);
        dsp::stereo_t rawLvl = meter.getLevel();
        meter.resetLevel();
        dsp::stereo_t dbLvl = { 10.0f * logf(rawLvl.l), 10.0f * logf(rawLvl.r) };
        if (dbLvl.l > lvl.l) { lvl.l = dbLvl.l; }
        if (dbLvl.r > lvl.r) { lvl.r = dbLvl.r; }
    }

    std::string genFileName(std::string templ, int recMode, std::string name) {
        // Get data
        time_t now = time(0);
        tm* ltm = localtime(&now);
        char buf[1024];
        double freq = gui::waterfall.getCenterFrequency();
        if (gui::waterfall.vfos.find(name) != gui::waterfall.vfos.end()) {
            freq += gui::waterfall.vfos[name]->generalOffset;
        }

        // Select the recording type string
        std::string type = (recMode == RECORDER_MODE_AUDIO) ? "audio" : "baseband";

        // Format to string
        char freqStr[128];
        char hourStr[128];
        char minStr[128];
        char secStr[128];
        char dayStr[128];
        char monStr[128];
        char yearStr[128];

        std::string modeStr = (recMode == RECORDER_MODE_AUDIO) ? "Unknown" : "IQ";
        snprintf(freqStr, sizeof freqStr, "%.0lfHz", freq);
        snprintf(hourStr, sizeof hourStr, "%02d", ltm->tm_hour);
        snprintf(minStr, sizeof minStr, "%02d", ltm->tm_min);
        snprintf(secStr, sizeof secStr, "%02d", ltm->tm_sec);
        snprintf(dayStr, sizeof dayStr, "%02d", ltm->tm_mday);
        snprintf(monStr, sizeof monStr, "%02d", ltm->tm_mon + 1);
        snprintf(yearStr, sizeof yearStr, "%02d", ltm->tm_year + 1900);
        // The radio's own mode table covers modes added by other modules too. A
        // second lookup used to run after this one through a fixed map of the eight
        // built in modes, and std::map::operator[] on a mode that is not in it -
        // DSD, from ch_extravhf_decoder, is 0x1301 - inserts a null const char*,
        // which then went straight into regex_replace as the replacement string.
        auto radio = (RadioModuleInterface *)core::moduleManager.getInterface(name, "RadioModuleInterface");
        if (radio) {
            int demodId = radio->getSelectedDemodId();
            for (int q = 0; q < radio->radioModes.size(); q++) {
                if (radio->radioModes[q].second == demodId) { modeStr = radio->radioModes[q].first; }
            }
        }

        // Replace in template
        templ = std::regex_replace(templ, std::regex("\\$t"), type);
        templ = std::regex_replace(templ, std::regex("\\$f"), freqStr);
        templ = std::regex_replace(templ, std::regex("\\$h"), hourStr);
        templ = std::regex_replace(templ, std::regex("\\$m"), minStr);
        templ = std::regex_replace(templ, std::regex("\\$s"), secStr);
        templ = std::regex_replace(templ, std::regex("\\$d"), dayStr);
        templ = std::regex_replace(templ, std::regex("\\$M"), monStr);
        templ = std::regex_replace(templ, std::regex("\\$y"), yearStr);
        templ = std::regex_replace(templ, std::regex("\\$r"), modeStr);
        return templ;
    }

    std::string expandString(std::string input) {
        input = std::regex_replace(input, std::regex("%ROOT%"), root);
        return std::regex_replace(input, std::regex("//"), "/");
    }

    static void complexHandler(dsp::complex_t* data, int count, void* ctx) {
        RecorderModule* _this = (RecorderModule*)ctx;
        _this->writer.write((float*)data, count);
    }

    static void stereoHandler(dsp::stereo_t* data, int count, void* ctx) {
        RecorderModule* _this = (RecorderModule*)ctx;
        if (_this->ignoreSilence) {
            float absMax = 0.0f;
            float* _data = (float*)data;
            int _count = count * 2;
            for (int i = 0; i < _count; i++) {
                float val = fabsf(_data[i]);
                if (val > absMax) { absMax = val; }
            }
            _this->ignoringSilence = (absMax < SILENCE_LVL);
            if (_this->ignoringSilence) { return; }
        }
        _this->writer.write((float*)data, count);
    }

    static void monoHandler(float* data, int count, void* ctx) {
        RecorderModule* _this = (RecorderModule*)ctx;
        if (_this->ignoreSilence) {
            float absMax = 0.0f;
            for (int i = 0; i < count; i++) {
                float val = fabsf(data[i]);
                if (val > absMax) { absMax = val; }
            }
            _this->ignoringSilence = (absMax < SILENCE_LVL);
            if (_this->ignoringSilence) { return; }
        }
        _this->writer.write(data, count);
    }

    std::string handleDebugCommand(const std::string& cmd, const std::string& args) {
        if (cmd == "start") {
            if (!recording) { start(); }
            return recording ? "{\"status\":\"recording\"}" : "{\"status\":\"failed\"}";
        }
        if (cmd == "stop") {
            if (recording) { stop(); }
            return "{\"status\":\"stopped\"}";
        }
        if (cmd == "status") {
            return "{\"recording\":" + std::string(recording ? "true" : "false") + "}";
        }
        return "{}";
    }

    static void moduleInterfaceHandler(int code, void* in, void* out, void* ctx) {
        RecorderModule* _this = (RecorderModule*)ctx;
        std::lock_guard lck(_this->recMtx);
        if (code == RECORDER_IFACE_CMD_GET_MODE) {
            int* _out = (int*)out;
            *_out = _this->recMode;
        }
        else if (code == RECORDER_IFACE_CMD_SET_MODE) {
            if (_this->recording) { return; }
            int* _in = (int*)in;
            _this->recMode = std::clamp<int>(*_in, 0, 1);
        }
        else if (code == RECORDER_IFACE_CMD_START) {
            if (!_this->recording) { _this->start(); }
        }
        else if (code == RECORDER_IFACE_CMD_STOP) {
            if (_this->recording) { _this->stop(); }
        }
    }

    std::string name;
    bool enabled = true;
    std::string root;
    char nameTemplate[1024];

    OptionList<std::string, wav::Format> containers;
    OptionList<int, wav::SampleType> sampleTypes;
    FolderSelect folderSelect;

    int recMode = RECORDER_MODE_AUDIO;
    int containerId;
    int sampleTypeId;
    bool stereo = false;
    std::string selectedStreamName = "";
    float audioVolume = 1.0f;
    bool ignoreSilence = false;
    dsp::stereo_t audioLvl = { -100.0f, -100.0f };

    bool recording = false;
    bool ignoringSilence = false;

    std::string lastError;   // why the last attempt to record did not take
    std::string currentPath; // file being written right now
    int recBytesPerFrame = 4;

    std::string previewKey;
    std::string previewName;
    std::string previewPath;
    double previewTime = -1000.0;
    uint64_t cachedFreeSpace = 0;
    double spaceTime = -1000.0;

    wav::Writer writer;
    std::recursive_mutex recMtx;
    dsp::stream<dsp::complex_t>* basebandStream;
    dsp::stream<dsp::stereo_t> stereoStream;
    dsp::sink::Handler<dsp::complex_t> basebandSink;
    dsp::sink::Handler<dsp::stereo_t> stereoSink;
    dsp::sink::Handler<float> monoSink;

    OptionList<std::string, std::string> audioStreams;
    int streamId = 0;
    dsp::stream<dsp::stereo_t>* audioStream = NULL;
    dsp::audio::Volume volume;
    dsp::routing::Splitter<dsp::stereo_t> splitter;
    dsp::stream<dsp::stereo_t> meterStream;
    dsp::bench::PeakLevelMeter<dsp::stereo_t> meter;
    dsp::convert::StereoToMono s2m;

    uint64_t samplerate = 48000;

    EventHandler<std::string> onStreamRegisteredHandler;
    EventHandler<std::string> onStreamUnregisterHandler;

};

MOD_EXPORT void _INIT_() {
    // Create default recording directory
    std::string root = std::string(core::getRoot());
    if (!std::filesystem::exists(root + "/recordings")) {
        flog::warn("Recordings directory does not exist, creating it");
        if (!std::filesystem::create_directory(root + "/recordings")) {
            flog::error("Could not create recordings directory");
        }
    }
    json def = json({});
    config.setPath(root + "/recorder_config.json");
    config.load(def);
    config.enableAutoSave();
}

MOD_EXPORT ModuleManager::Instance* _CREATE_INSTANCE_(std::string name) {
    return new RecorderModule(name);
}

MOD_EXPORT void _DELETE_INSTANCE_(ModuleManager::Instance* inst) {
    delete (RecorderModule*)inst;
}

MOD_EXPORT void _END_() {
    config.disableAutoSave();
    config.save();
}