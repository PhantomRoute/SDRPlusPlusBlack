#include <imgui.h>
#include <imgui_internal.h>
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
#include <memory>
#include <cfloat>
#include <gui/gui.h>
#include <filesystem>
#include <system_error>
#include <signal_path/signal_path.h>
#include <config.h>
#include <gui/style.h>
#include <regex>
#include <gui/widgets/folder_select.h>
#include <recorder_interface.h>
#include <core.h>
#include <radio_interface.h>
#include <utils/optionlist.h>
#include <utils/wav.h>
#include <radio_module_interface.h>
#include "audio_file.h"

#define CONCAT(a, b) ((std::string(a) + b).c_str())

#define SILENCE_LVL 10e-6

SDRPP_MOD_INFO{
    /* Name:            */ "recorder",
    /* Description:     */ "Recorder module for SDR++",
    /* Author:          */ "Ryzerth",
    /* Version:         */ 0, 4, 0,
    /* Max instances    */ -1
};

ConfigManager config;

namespace {
    // What an audio recording is written as. Baseband is always WAV: it is kept exactly
    // as it arrived, so it can be replayed and retuned later.
    enum AudioFormat {
        AUDIO_FORMAT_WAV = 0,
        AUDIO_FORMAT_FLAC,
        AUDIO_FORMAT_MP3,
        AUDIO_FORMAT_COUNT
    };
    const char* AUDIO_FORMAT_KEYS[AUDIO_FORMAT_COUNT] = { "WAV", "FLAC", "MP3" };
    const char* AUDIO_FORMAT_EXT[AUDIO_FORMAT_COUNT] = { ".wav", ".flac", ".mp3" };

    const int FLAC_BITS[] = { 16, 24 };
    const int FLAC_BITS_COUNT = 2;
    const int MP3_BITRATES[] = { 32, 48, 64, 96, 128, 160, 192, 256, 320 };
    const int MP3_BITRATES_COUNT = 9;

    // The level meter's scale, in dB below full scale.
    const float METER_MIN_DB = -60.0f;
    const float METER_MAX_DB = 0.0f;
    // Where the bar turns from green to amber, and from amber to red.
    const float METER_WARN_DB = -18.0f;
    const float METER_HOT_DB = -6.0f;
    // The gain slider's bottom stop, which means off.
    const float GAIN_OFF_DB = -60.0f;

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

    std::string formatRate(uint64_t rate) {
        char buf[48];
        if (rate >= 1000000) { snprintf(buf, sizeof buf, "%.3g MS/s", (double)rate / 1000000.0); }
        else if (rate % 1000 == 0) { snprintf(buf, sizeof buf, "%d kHz", (int)(rate / 1000)); }
        else { snprintf(buf, sizeof buf, "%.1f kHz", (double)rate / 1000.0); }
        return std::string(buf);
    }

    ImU32 withAlpha(ImVec4 c, float a) {
        c.w *= a;
        return ImGui::GetColorU32(c);
    }

    // A peak meter channel: jumps up at once, falls back at a steady rate, and keeps a
    // mark at the highest recent peak for a moment so a short one can still be read.
    struct MeterChannel {
        float level = -120.0f;
        float hold = -120.0f;
        double holdTime = 0.0;
        double clipTime = -1000.0;

        void update(float linearPeak, float dt, double now) {
            float db = (linearPeak > 1e-6f) ? 20.0f * log10f(linearPeak) : -120.0f;
            level = std::max<float>(db, level - (24.0f * dt));
            if (db >= hold) {
                hold = db;
                holdTime = now;
            }
            else if ((now - holdTime) > 1.5) {
                hold = std::max<float>(level, hold - (20.0f * dt));
            }
            if (linearPeak >= 0.999f) { clipTime = now; }
        }
    };
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
        // Read defensively: a hand edited file, or one from an older build, is as
        // likely to have these missing or of the wrong type as right.
        auto& conf = config.conf[name];
        if (conf.contains("audioFormat") && conf["audioFormat"].is_string()) {
            std::string key = conf["audioFormat"];
            for (int i = 0; i < AUDIO_FORMAT_COUNT; i++) {
                if (key == AUDIO_FORMAT_KEYS[i]) { audioFormat = i; }
            }
        }
        if (conf.contains("flacBits") && conf["flacBits"].is_number_integer()) {
            int bits = conf["flacBits"];
            for (int i = 0; i < FLAC_BITS_COUNT; i++) {
                if (FLAC_BITS[i] == bits) { flacBitsId = i; }
            }
        }
        if (conf.contains("mp3Bitrate") && conf["mp3Bitrate"].is_number_integer()) {
            int kbps = conf["mp3Bitrate"];
            for (int i = 0; i < MP3_BITRATES_COUNT; i++) {
                if (MP3_BITRATES[i] == kbps) { mp3BitrateId = i; }
            }
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

    // The format the next recording is written in. Baseband is WAV whatever audio is set to.
    int effectiveFormat() {
        return (recMode == RECORDER_MODE_AUDIO) ? audioFormat : AUDIO_FORMAT_WAV;
    }

    // The name the next recording would get. Rebuilt when something it depends on
    // changes, and at most twice a second otherwise, because building it runs nine
    // regex replacements and the clock fields in it only tick once a second.
    const std::string& fileNamePreview() {
        std::string key = std::string(nameTemplate) + "|" + std::to_string(recMode) + "|" + selectedStreamName + "|" + std::to_string(effectiveFormat());
        double now = ImGui::GetTime();
        if (key != previewKey || (now - previewTime) > 0.5) {
            previewKey = key;
            previewTime = now;
            std::string vfoName = (recMode == RECORDER_MODE_AUDIO) ? selectedStreamName : "";
            previewName = genFileName(nameTemplate, recMode, vfoName) + AUDIO_FORMAT_EXT[effectiveFormat()];
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

    // Bytes a second the chosen format writes at this rate. For FLAC, which depends on
    // what is being recorded, the uncompressed size - the most it can come to.
    double bytesPerSecond(uint64_t rate) {
        switch (effectiveFormat()) {
        case AUDIO_FORMAT_MP3: return (double)MP3_BITRATES[mp3BitrateId] * 1000.0 / 8.0;
        case AUDIO_FORMAT_FLAC: return (double)rate * (double)(FLAC_BITS[flacBitsId] / 8) * (double)currentChannels();
        default: return (double)rate * (double)bytesPerFrame();
        }
    }

    const char* sampleTypeName() {
        switch (sampleTypes[sampleTypeId]) {
        case wav::SAMP_TYPE_UINT8: return "8-bit";
        case wav::SAMP_TYPE_INT32: return "32-bit";
        case wav::SAMP_TYPE_FLOAT32: return "32-bit float";
        default: return "16-bit";
        }
    }

    // "MP3 128 kbps, 48 kHz mono", for the line under the Record button.
    std::string formatSummary(uint64_t rate) {
        char buf[128];
        const char* layout = (recMode == RECORDER_MODE_BASEBAND) ? "IQ" : (currentChannels() == 1 ? "mono" : "stereo");
        std::string rateStr = formatRate(rate);
        switch (effectiveFormat()) {
        case AUDIO_FORMAT_MP3: snprintf(buf, sizeof buf, "MP3 %d kbps, %s %s", MP3_BITRATES[mp3BitrateId], rateStr.c_str(), layout); break;
        case AUDIO_FORMAT_FLAC: snprintf(buf, sizeof buf, "FLAC %d-bit, %s %s", FLAC_BITS[flacBitsId], rateStr.c_str(), layout); break;
        default: snprintf(buf, sizeof buf, "WAV %s, %s %s", sampleTypeName(), rateStr.c_str(), layout); break;
        }
        return buf;
    }

    void start() {
        std::lock_guard<std::recursive_mutex> lck(recMtx);
        if (recording) { return; }

        // Used to fail silently here, leaving the panel showing "Idle" as though
        // the button had not been pressed at all.
        lastError = recordBlockedReason();
        if (!lastError.empty()) { return; }

        samplerate = currentSamplerate();
        recFormat = effectiveFormat();
        std::string vfoName = (recMode == RECORDER_MODE_AUDIO) ? selectedStreamName : "";
        std::string expandedPath = expandString(folderSelect.path + "/" + genFileName(nameTemplate, recMode, vfoName) + AUDIO_FORMAT_EXT[recFormat]);

        if (recFormat == AUDIO_FORMAT_WAV) {
            encoded.reset();
            writer.setFormat(containers[containerId]);
            writer.setChannels(currentChannels());
            writer.setSampleType(sampleTypes[sampleTypeId]);
            writer.setSamplerate(samplerate);
            if (!writer.open(expandedPath)) {
                flog::error("Failed to open file for recording: {0}", expandedPath);
                lastError = "Could not open " + expandedPath;
                return;
            }
            recBytesPerFrame = bytesPerFrame();
        }
        else {
            std::shared_ptr<recorder_audio::EncodedFile> file;
            if (recFormat == AUDIO_FORMAT_FLAC) { file = std::make_shared<recorder_audio::FlacFile>(FLAC_BITS[flacBitsId]); }
            else { file = std::make_shared<recorder_audio::Mp3File>(MP3_BITRATES[mp3BitrateId]); }
            if (!file->open(expandedPath, currentChannels(), (int)samplerate)) {
                flog::error("Failed to start recording to {0}: {1}", expandedPath, file->lastError());
                lastError = file->lastError();
                return;
            }
            encoded = file;
        }
        currentPath = expandedPath;
        recBytesPerSecond = bytesPerSecond(samplerate);

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

        // Close file. The sinks above are stopped, so nothing is still writing to it.
        // An encoded file is kept, closed, until the next recording starts: the panel
        // still reads its size and whether a write failed.
        if (encoded) { encoded->close(); }
        else { writer.close(); }

        recording = false;
    }

private:
    // ---- Reading the recording back, for the panel.

    uint64_t recordedFrames() {
        return encoded ? encoded->framesWritten() : (uint64_t)writer.getSamplesWritten();
    }

    uint64_t recordedBytes() {
        return encoded ? encoded->bytesWritten() : (uint64_t)writer.getSamplesWritten() * (uint64_t)recBytesPerFrame;
    }

    // ---- Drawing

    // A row of buttons that act as one choice, the chosen one in the theme's accent
    // colour. Returns true when the choice changed.
    static bool segmented(const char* id, const char* const* labels, int count, int& value) {
        float width = ImGui::GetContentRegionAvail().x;
        float gap = 2.0f * style::uiScale;
        float each = (width - (gap * (float)(count - 1))) / (float)count;
        bool changed = false;
        for (int i = 0; i < count; i++) {
            if (i > 0) { ImGui::SameLine(0.0f, gap); }
            bool active = (value == i);
            if (active) {
                ImVec4 fill = ImGui::GetStyleColorVec4(ImGuiCol_CheckMark);
                fill.w = 0.55f;
                ImGui::PushStyleColor(ImGuiCol_Button, fill);
                ImGui::PushStyleColor(ImGuiCol_ButtonHovered, fill);
                ImGui::PushStyleColor(ImGuiCol_ButtonActive, fill);
            }
            if (ImGui::Button((std::string(labels[i]) + "##" + id + std::to_string(i)).c_str(), ImVec2(each, 0)) && !active) {
                value = i;
                changed = true;
            }
            if (active) { ImGui::PopStyleColor(3); }
        }
        return changed;
    }

    // The Record / Stop button: tall, with a dot or a square drawn beside the word, and
    // red while recording so it cannot be mistaken from across the room.
    bool transportButton(const char* id, bool isRecording, bool disabled, float width) {
        const float s = style::uiScale;
        float height = ImGui::GetFrameHeight() * 1.6f;
        ImVec2 pos = ImGui::GetCursorScreenPos();
        bool pressed = ImGui::InvisibleButton(id, ImVec2(width, height)) && !disabled;
        bool hovered = ImGui::IsItemHovered() && !disabled;
        bool held = ImGui::IsItemActive() && !disabled;

        ImDrawList* dl = ImGui::GetWindowDrawList();
        ImVec2 corner(pos.x + width, pos.y + height);
        ImU32 fill;
        if (isRecording) {
            ImVec4 red = held ? ImVec4(0.55f, 0.13f, 0.13f, 1.0f) : hovered ? ImVec4(0.75f, 0.2f, 0.2f, 1.0f) : ImVec4(0.64f, 0.16f, 0.16f, 1.0f);
            fill = ImGui::GetColorU32(red);
        }
        else {
            fill = ImGui::GetColorU32(held ? ImGuiCol_ButtonActive : hovered ? ImGuiCol_ButtonHovered : ImGuiCol_Button);
        }
        float rounding = std::max<float>(ImGui::GetStyle().FrameRounding, 4.0f * s);
        dl->AddRectFilled(pos, corner, fill, rounding);

        const char* label = isRecording ? "Stop" : "Record";
        ImVec2 textSize = ImGui::CalcTextSize(label);
        float icon = ImGui::GetFontSize() * 0.62f;
        float spacing = 8.0f * s;
        float totalW = icon + spacing + textSize.x;
        float x = pos.x + ((width - totalW) / 2.0f);
        float cy = pos.y + (height / 2.0f);
        ImU32 textCol = isRecording ? ImGui::GetColorU32(ImVec4(1.0f, 1.0f, 1.0f, 1.0f)) : ImGui::GetColorU32(ImGuiCol_Text);
        if (isRecording) {
            dl->AddRectFilled(ImVec2(x, cy - (icon / 2.0f)), ImVec2(x + icon, cy + (icon / 2.0f)), textCol, 1.5f * s);
        }
        else {
            ImVec4 dot = disabled ? ImVec4(0.9f, 0.3f, 0.3f, 0.35f) : ImVec4(0.93f, 0.27f, 0.25f, 1.0f);
            dl->AddCircleFilled(ImVec2(x + (icon / 2.0f), cy), icon / 2.0f, ImGui::GetColorU32(dot), 20);
        }
        dl->AddText(ImVec2(x + icon + spacing, cy - (textSize.y / 2.0f)), textCol, label);
        return pressed;
    }

    void drawSource() {
        ImGui::SectionHeader("RECORD");

        if (recording) { style::beginDisabled(); }
        // Audio first: it is what most recordings are.
        const char* modes[2] = { "Audio", "Baseband" };
        int modeIdx = (recMode == RECORDER_MODE_AUDIO) ? 0 : 1;
        if (segmented(CONCAT("recorder_mode_", name), modes, 2, modeIdx) && !recording) {
            recMode = (modeIdx == 0) ? RECORDER_MODE_AUDIO : RECORDER_MODE_BASEBAND;
            config.acquire();
            config.conf[name]["mode"] = recMode;
            config.release(true);
        }
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
            style::tooltip("Audio records what you are listening to on one stream. Baseband records the\n"
                           "raw IQ of the whole visible spectrum, which is large but can be replayed\n"
                           "and retuned later.");
        }

        // Which stream is the other half of "what to record".
        if (recMode == RECORDER_MODE_AUDIO) {
            ImGui::LeftLabel("Stream");
            ImGui::FillWidth();
            if (ImGui::Combo(CONCAT("##_recorder_stream_", name), &streamId, audioStreams.txt)) {
                selectStream(audioStreams.value(streamId));
                config.acquire();
                config.conf[name]["audioStream"] = audioStreams.key(streamId);
                config.release(true);
            }
            if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
                style::tooltip("Which audio output to record. Each radio and each secondary output is\nits own stream.");
            }
        }
        if (recording) { style::endDisabled(); }
    }

    void drawTransport() {
        std::lock_guard<std::recursive_mutex> lck(recMtx);
        const float s = style::uiScale;
        float width = ImGui::GetContentRegionAvail().x;
        ImGui::Dummy(ImVec2(0.0f, 2.0f * s));

        // The guard used to be worked out and then ignored, so pressing Record with a
        // bad folder did nothing at all and the panel went on saying "Idle".
        std::string blocked = recordBlockedReason();
        if (!recording) {
            if (transportButton(CONCAT("##_recorder_rec_", name), false, !blocked.empty(), width)) { start(); }
        }
        else {
            if (transportButton(CONCAT("##_recorder_rec_", name), true, false, width)) { stop(); }
        }

        if (!recording) {
            if (!blocked.empty()) {
                ImGui::PushTextWrapPos(0.0f);
                ImGui::TextColored(ImVec4(1.0f, 0.7f, 0.3f, 1.0f), "%s", blocked.c_str());
                ImGui::PopTextWrapPos();
                return;
            }
            if (!lastError.empty()) {
                ImGui::PushTextWrapPos(0.0f);
                ImGui::TextColored(ImVec4(1.0f, 0.5f, 0.4f, 1.0f), "%s", lastError.c_str());
                ImGui::PopTextWrapPos();
            }
            else if (!currentPath.empty()) {
                ImGui::PushTextWrapPos(0.0f);
                ImGui::TextDisabled("Saved %s, %s", std::filesystem::path(currentPath).filename().string().c_str(), formatBytes(recordedBytes()).c_str());
                ImGui::PopTextWrapPos();
                if (ImGui::IsItemHovered()) { style::tooltip("%s", currentPath.c_str()); }
                if (encoded && encoded->writeFailed()) {
                    ImGui::TextColored(ImVec4(1.0f, 0.5f, 0.4f, 1.0f), "Part of it could not be written. Is the disk full?");
                }
            }
            drawSummary(currentSamplerate());
            return;
        }

        // ---- Recording: the clock large, with a light that says whether audio is
        // actually going into the file.
        uint64_t frames = recordedFrames();
        uint64_t seconds = (samplerate > 0) ? (frames / samplerate) : 0;
        uint64_t bytes = recordedBytes();
        bool waiting = ignoreSilence && ignoringSilence;
        double now = ImGui::GetTime();

        ImDrawList* dl = ImGui::GetWindowDrawList();
        ImVec2 pos = ImGui::GetCursorScreenPos();
        ImGui::PushFont(style::mediumFont);
        float clockH = ImGui::GetTextLineHeight();
        std::string clock = formatClock(seconds);
        ImVec2 clockSize = ImGui::CalcTextSize(clock.c_str());
        ImGui::PopFont();

        float dotR = 5.0f * s;
        ImVec4 red(0.93f, 0.27f, 0.25f, 1.0f);
        ImVec4 amber(1.0f, 0.72f, 0.25f, 1.0f);
        // A slow pulse while writing; steady amber while skip silence is holding off.
        float pulse = waiting ? 1.0f : (0.55f + (0.45f * (float)(0.5 + (0.5 * cos(now * 4.0)))));
        ImVec2 dotC(pos.x + dotR + (1.0f * s), pos.y + (clockH / 2.0f));
        dl->AddCircleFilled(dotC, dotR, withAlpha(waiting ? amber : red, pulse), 16);

        const char* state = waiting ? "WAITING" : "REC";
        ImGui::PushFont(style::tinyFont);
        ImVec2 stateSize = ImGui::CalcTextSize(state);
        float stateX = dotC.x + dotR + (6.0f * s);
        dl->AddText(ImVec2(stateX, pos.y + ((clockH - stateSize.y) / 2.0f)), ImGui::GetColorU32(waiting ? amber : red), state);
        ImGui::PopFont();

        float clockX = stateX + stateSize.x + (8.0f * s);
        dl->AddText(style::mediumFont, style::mediumFont->FontSize, ImVec2(clockX, pos.y), ImGui::GetColorU32(ImGuiCol_Text), clock.c_str());

        std::string size = formatBytes(bytes);
        ImVec2 sizeSize = ImGui::CalcTextSize(size.c_str());
        float sizeX = pos.x + width - sizeSize.x;
        if (sizeX > clockX + clockSize.x + (8.0f * s)) {
            dl->AddText(ImVec2(sizeX, pos.y + ((clockH - sizeSize.y) / 2.0f)), ImGui::GetColorU32(ImGuiCol_Text), size.c_str());
        }
        ImGui::Dummy(ImVec2(width, clockH));
        if (waiting && ImGui::IsItemHovered()) {
            style::tooltip("Skip silence is on and the audio is silent, so nothing is being written.\nThe clock carries on when the audio does.");
        }

        ImGui::PushTextWrapPos(0.0f);
        ImGui::TextDisabled("%s", std::filesystem::path(currentPath).filename().string().c_str());
        ImGui::PopTextWrapPos();
        if (ImGui::IsItemHovered()) { style::tooltip("Writing to\n%s", currentPath.c_str()); }

        // How much room is left, and the things that need doing something about.
        uint64_t freeBytes = freeSpace();
        double perSecond = recBytesPerSecond;
        if (recFormat == AUDIO_FORMAT_FLAC && seconds >= 10 && bytes > 0) {
            // FLAC's rate depends on the audio; once there is some, go by what it has done.
            perSecond = (double)bytes / (double)seconds;
        }
        bool lowOnSpace = (freeBytes > 0) && (perSecond > 0.0) && (((double)freeBytes / perSecond) < 300.0);
        ImGui::PushFont(style::tinyFont);
        if (freeBytes > 0 && !lowOnSpace) {
            ImGui::TextDisabled("%s | %s free", formatSummary(samplerate).c_str(), formatBytes(freeBytes).c_str());
        }
        else {
            ImGui::TextDisabled("%s", formatSummary(samplerate).c_str());
        }
        ImGui::PopFont();

        // A full RIFF container drops everything written after it without a word, so
        // this is the only sign the recording has stopped growing.
        if (!encoded && writer.isFull()) {
            ImGui::PushTextWrapPos(0.0f);
            ImGui::TextColored(ImVec4(1.0f, 0.3f, 0.3f, 1.0f), "File hit the 4 GB WAV limit and has stopped growing. Stop and start a new one.");
            ImGui::PopTextWrapPos();
        }
        if (encoded && encoded->writeFailed()) {
            ImGui::PushTextWrapPos(0.0f);
            ImGui::TextColored(ImVec4(1.0f, 0.3f, 0.3f, 1.0f), "Writing to the file failed, so nothing more is reaching it. Is the disk full?");
            ImGui::PopTextWrapPos();
        }
        if (lowOnSpace) {
            ImGui::PushTextWrapPos(0.0f);
            ImGui::TextColored(ImVec4(1.0f, 0.5f, 0.4f, 1.0f), "%s free, about %s left",
                               formatBytes(freeBytes).c_str(), formatSpan((uint64_t)((double)freeBytes / perSecond)).c_str());
            ImGui::PopTextWrapPos();
        }
    }

    // What the next recording will be, and how fast it fills a disk. Baseband at a few
    // MS/s fills one far faster than anyone expects.
    void drawSummary(uint64_t rate) {
        if (rate == 0) { return; }
        double perMinute = bytesPerSecond(rate) * 60.0;
        std::string size = formatBytes((uint64_t)perMinute);
        ImGui::PushFont(style::tinyFont);
        if (effectiveFormat() == AUDIO_FORMAT_FLAC) {
            ImGui::TextDisabled("%s | up to %s a minute", formatSummary(rate).c_str(), size.c_str());
        }
        else {
            ImGui::TextDisabled("%s | %s a minute", formatSummary(rate).c_str(), size.c_str());
        }
        ImGui::PopFont();
        if (ImGui::IsItemHovered()) {
            if (effectiveFormat() == AUDIO_FORMAT_WAV) {
                style::tooltip("A WAV file cannot go past 4 GB. At this rate that is about %s.",
                               formatSpan((uint64_t)(4294967296.0 / std::max<double>(1.0, perMinute / 60.0))).c_str());
            }
            else if (effectiveFormat() == AUDIO_FORMAT_FLAC) {
                style::tooltip("FLAC's size depends on the audio. Quiet or simple audio packs down a long\n"
                               "way; noise hardly at all. It never comes to more than the same WAV would.");
            }
        }
    }

    void drawLevel() {
        const float s = style::uiScale;
        ImGui::SectionHeader("LEVEL");

        double now = ImGui::GetTime();
        float dt = std::clamp<float>(ImGui::GetIO().DeltaTime, 0.0f, 0.25f);
        dsp::stereo_t raw = meter.getLevel();
        meter.resetLevel();
        meterL.update(raw.l, dt, now);
        meterR.update(raw.r, dt, now);
        // A mono file is the two channels mixed, so its meter is the louder of the two.
        MeterChannel mono;
        if (!stereo) {
            mono = (meterL.level >= meterR.level) ? meterL : meterR;
            mono.hold = std::max<float>(meterL.hold, meterR.hold);
            mono.clipTime = std::max<double>(meterL.clipTime, meterR.clipTime);
        }

        float width = ImGui::GetContentRegionAvail().x;
        ImDrawList* dl = ImGui::GetWindowDrawList();
        ImVec2 origin = ImGui::GetCursorScreenPos();

        ImGui::PushFont(style::tinyFont);
        ImFont* tiny = ImGui::GetFont();
        float tinySize = ImGui::GetFontSize();
        float labelW = std::max<float>(ImGui::CalcTextSize("L").x, ImGui::CalcTextSize("R").x) + (6.0f * s);
        const char* clipLabel = "CLIP";
        ImVec2 clipText = ImGui::CalcTextSize(clipLabel);
        ImGui::PopFont();

        int rows = stereo ? 2 : 1;
        float barH = (stereo ? 7.0f : 12.0f) * s;
        float rowGap = 3.0f * s;
        float barsH = (barH * (float)rows) + (rowGap * (float)(rows - 1));
        float clipW = clipText.x + (10.0f * s);
        float barX0 = origin.x + (stereo ? labelW : 0.0f);
        float barX1 = origin.x + width - clipW - (6.0f * s);
        float barW = std::max<float>(10.0f, barX1 - barX0);
        auto xOf = [&](float db) {
            float t = (std::clamp<float>(db, METER_MIN_DB, METER_MAX_DB) - METER_MIN_DB) / (METER_MAX_DB - METER_MIN_DB);
            return barX0 + (t * barW);
        };

        ImVec4 green(0.25f, 0.78f, 0.38f, 1.0f);
        ImVec4 amber(0.96f, 0.74f, 0.22f, 1.0f);
        ImVec4 red(0.93f, 0.28f, 0.25f, 1.0f);
        struct Zone { float from, to; ImVec4 col; };
        const Zone zones[3] = { { METER_MIN_DB, METER_WARN_DB, green }, { METER_WARN_DB, METER_HOT_DB, amber }, { METER_HOT_DB, METER_MAX_DB, red } };

        const MeterChannel* channels[2] = { stereo ? &meterL : &mono, &meterR };
        const char* names[2] = { "L", "R" };
        float rounding = 2.0f * s;
        for (int r = 0; r < rows; r++) {
            const MeterChannel& m = *channels[r];
            float y0 = origin.y + ((barH + rowGap) * (float)r);
            float y1 = y0 + barH;
            if (stereo) {
                dl->AddText(tiny, tinySize, ImVec2(origin.x, y0 + ((barH - tinySize) / 2.0f)), ImGui::GetColorU32(ImGuiCol_TextDisabled), names[r]);
            }
            dl->AddRectFilled(ImVec2(barX0, y0), ImVec2(barX0 + barW, y1), ImGui::GetColorU32(ImGuiCol_FrameBg), rounding);
            // Every zone faintly, so the scale reads before anything is playing; then
            // the lit part up to the level.
            for (const Zone& z : zones) {
                float zx0 = xOf(z.from), zx1 = xOf(z.to);
                dl->AddRectFilled(ImVec2(zx0, y0), ImVec2(zx1, y1), withAlpha(z.col, 0.14f));
                float litTo = std::min<float>(zx1, xOf(m.level));
                if (m.level > METER_MIN_DB && litTo > zx0) {
                    dl->AddRectFilled(ImVec2(zx0, y0), ImVec2(litTo, y1), ImGui::GetColorU32(z.col));
                }
            }
            if (m.hold > METER_MIN_DB) {
                float hx = xOf(m.hold);
                // In the text colour, so it shows against the lit bar as well as the dark.
                dl->AddRectFilled(ImVec2(std::max<float>(barX0, hx - (1.0f * s)), y0), ImVec2(std::min<float>(barX0 + barW, hx + (1.0f * s)), y1), ImGui::GetColorU32(ImGuiCol_Text));
            }
        }

        // Clip light: lit for two seconds after any sample reaches full scale.
        double lastClip = std::max<double>(meterL.clipTime, meterR.clipTime);
        bool clipLit = (now - lastClip) < 2.0;
        ImVec2 clipMin(origin.x + width - clipW, origin.y);
        ImVec2 clipMax(origin.x + width, origin.y + barsH);
        if (clipLit) { dl->AddRectFilled(clipMin, clipMax, ImGui::GetColorU32(red), rounding); }
        else { dl->AddRect(clipMin, clipMax, ImGui::GetColorU32(ImGuiCol_Border), rounding); }
        dl->AddText(tiny, tinySize, ImVec2(clipMin.x + ((clipW - clipText.x) / 2.0f), origin.y + ((barsH - tinySize) / 2.0f)),
                    clipLit ? ImGui::GetColorU32(ImVec4(1.0f, 1.0f, 1.0f, 1.0f)) : ImGui::GetColorU32(ImGuiCol_TextDisabled), clipLabel);

        // The scale under the bars, leaving out any label that would run into the last.
        float scaleY = origin.y + barsH + (2.0f * s);
        const float marks[] = { -60.0f, -48.0f, -36.0f, -24.0f, -18.0f, -12.0f, -6.0f, -3.0f, 0.0f };
        float lastRight = -1e9f;
        for (float db : marks) {
            char txt[8];
            snprintf(txt, sizeof txt, "%.0f", db);
            float tw = tiny->CalcTextSizeA(tinySize, FLT_MAX, 0.0f, txt).x;
            float cx = xOf(db);
            float tx = std::clamp<float>(cx - (tw / 2.0f), barX0, barX0 + barW - tw);
            if (tx < lastRight + (4.0f * s)) { continue; }
            dl->AddLine(ImVec2(cx, scaleY), ImVec2(cx, scaleY + (2.0f * s)), ImGui::GetColorU32(ImGuiCol_TextDisabled));
            dl->AddText(tiny, tinySize, ImVec2(tx, scaleY + (2.0f * s)), ImGui::GetColorU32(ImGuiCol_TextDisabled), txt);
            lastRight = tx + tw;
        }

        float totalH = barsH + (4.0f * s) + tinySize;
        ImGui::InvisibleButton(CONCAT("##_recorder_meter_", name), ImVec2(width, totalH));
        if (ImGui::IsItemHovered()) {
            float peak = stereo ? std::max<float>(meterL.hold, meterR.hold) : mono.hold;
            if (peak > METER_MIN_DB) {
                style::tooltip("Peak %.1f dBFS\n\nWhat goes into the file, after the gain below. Keep the peaks out of the\n"
                               "red: CLIP lights when a sample reaches full scale, and anything louder than\n"
                               "that is cut off in the file.", peak);
            }
            else {
                style::tooltip("What goes into the file, after the gain below. Nothing is coming through.");
            }
        }

        // Gain, in dB. Stored as the plain multiplier it always was, so an existing
        // setting carries over.
        ImGui::LeftLabel("Gain");
        ImGui::FillWidth();
        float gainDb = (audioVolume > 0.0f) ? std::max<float>(GAIN_OFF_DB, 20.0f * log10f(audioVolume)) : GAIN_OFF_DB;
        if (ImGui::SliderFloat(CONCAT("##_recorder_vol_", name), &gainDb, GAIN_OFF_DB, 0.0f, (gainDb <= GAIN_OFF_DB) ? "off" : "%.1f dB")) {
            audioVolume = (gainDb <= GAIN_OFF_DB) ? 0.0f : powf(10.0f, gainDb / 20.0f);
            volume.setVolume(audioVolume);
            config.acquire();
            config.conf[name]["audioVolume"] = audioVolume;
            config.release(true);
        }
        if (ImGui::IsItemHovered()) {
            style::tooltip("Gain applied to what is written to the file, not to what you hear.\nCtrl+click to type a value.");
        }
    }

    void drawFile() {
        ImGui::SectionHeader("FILE");
        float markerRoom = 30.0f * style::uiScale;

        if (recording) { style::beginDisabled(); }

        if (folderSelect.render("##_recorder_fold_" + name)) {
            if (folderSelect.pathIsValid()) {
                config.acquire();
                config.conf[name]["recPath"] = folderSelect.path;
                config.release(true);
            }
        }

        ImGui::LeftLabel("Name");
        ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x - markerRoom);
        if (ImGui::InputText(CONCAT("##_recorder_name_template_", name), nameTemplate, 1023)) {
            config.acquire();
            config.conf[name]["nameTemplate"] = nameTemplate;
            config.release(true);
        }
        ImGui::HelpMarker("$t   audio or baseband\n"
                          "$f   frequency, in Hz\n"
                          "$r   mode (NFM, USB, ...)\n"
                          "$h $m $s   hour, minute, second\n"
                          "$d $M $y   day, month, year\n"
                          "Anything else is kept as typed. The extension is added for you.");

        // The file the template produces next. While recording, the one being written
        // is shown up by the clock instead.
        if (!recording) {
            std::string shown = fileNamePreview();
            ImGui::PushTextWrapPos(0.0f);
            ImGui::TextDisabled("%s", shown.c_str());
            ImGui::PopTextWrapPos();
            if (ImGui::IsItemHovered()) { style::tooltip("Next recording goes to\n%s", previewPath.c_str()); }
        }

        // ---- Format
        if (recMode == RECORDER_MODE_AUDIO) {
            ImGui::LeftLabel("Format");
            ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x - markerRoom);
            if (ImGui::Combo(CONCAT("##_recorder_format_", name), &audioFormat, "WAV\0FLAC\0MP3\0")) {
                audioFormat = std::clamp<int>(audioFormat, 0, AUDIO_FORMAT_COUNT - 1);
                config.acquire();
                config.conf[name]["audioFormat"] = AUDIO_FORMAT_KEYS[audioFormat];
                config.release(true);
            }
            ImGui::HelpMarker("WAV: uncompressed, exactly what came in.\n"
                              "FLAC: lossless - the same audio as WAV, in less space.\n"
                              "MP3: lossy - much smaller, and plays anywhere, but some detail is gone for good.");
        }
        else {
            ImGui::LeftLabel("Format");
            ImGui::AlignTextToFramePadding();
            ImGui::TextDisabled("WAV");
            ImGui::HelpMarker("Baseband is always WAV. It is kept exactly as it arrived, so it can be\nreplayed and retuned later; a compressed format would lose that.");
        }

        int format = effectiveFormat();
        if (format == AUDIO_FORMAT_WAV) {
            ImGui::LeftLabel("Sample type");
            ImGui::FillWidth();
            if (ImGui::Combo(CONCAT("##_recorder_st_", name), &sampleTypeId, sampleTypes.txt)) {
                config.acquire();
                config.conf[name]["sampleType"] = sampleTypes.key(sampleTypeId);
                config.release(true);
            }
        }
        else if (format == AUDIO_FORMAT_FLAC) {
            ImGui::LeftLabel("Bit depth");
            ImGui::FillWidth();
            if (ImGui::Combo(CONCAT("##_recorder_flac_bits_", name), &flacBitsId, "16-bit\0" "24-bit\0")) {
                config.acquire();
                config.conf[name]["flacBits"] = FLAC_BITS[flacBitsId];
                config.release(true);
            }
            if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
                style::tooltip("16-bit is more than the audio from a receiver needs. 24-bit only makes a\nbigger file of it.");
            }
        }
        else if (format == AUDIO_FORMAT_MP3) {
            ImGui::LeftLabel("Bitrate");
            ImGui::FillWidth();
            const char* bitrates = "32 kbps\0" "48 kbps\0" "64 kbps\0" "96 kbps\0" "128 kbps\0" "160 kbps\0" "192 kbps\0" "256 kbps\0" "320 kbps\0";
            if (ImGui::Combo(CONCAT("##_recorder_mp3_rate_", name), &mp3BitrateId, bitrates)) {
                config.acquire();
                config.conf[name]["mp3Bitrate"] = MP3_BITRATES[mp3BitrateId];
                config.release(true);
            }
            if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
                style::tooltip("64 kbps is plenty for speech from a scanner in mono. Music, or broadcast\nFM in stereo, wants 128 kbps or more.");
            }
            ImGui::PushTextWrapPos(0.0f);
            ImGui::TextColored(ImVec4(1.0f, 0.72f, 0.3f, 1.0f), "MP3 is lossy.");
            ImGui::SameLine();
            ImGui::TextDisabled("Fine for listening back, but a digital mode or data decoded from the recording later may not survive it. Use WAV or FLAC for that.");
            ImGui::PopTextWrapPos();
        }

        // Stereo doubles the audio in the file, so it belongs with the format.
        if (recMode == RECORDER_MODE_AUDIO) {
            if (ImGui::Checkbox(CONCAT("Stereo##_recorder_stereo_", name), &stereo)) {
                config.acquire();
                config.conf[name]["stereo"] = stereo;
                config.release(true);
            }
            ImGui::HelpMarker("Two channels instead of one. Twice the file for no more information\nunless the source really is stereo, as broadcast FM is.");
        }

        if (recording) { style::endDisabled(); }

        // Outside the block above on purpose: unlike everything else about the file,
        // this one is decided per buffer as it is written, so it can be turned on and
        // off part way through a recording.
        if (recMode == RECORDER_MODE_AUDIO) {
            if (ImGui::Checkbox(CONCAT("Skip silence##_recorder_ignore_silence_", name), &ignoreSilence)) {
                config.acquire();
                config.conf[name]["ignoreSilence"] = ignoreSilence;
                config.release(true);
            }
            ImGui::HelpMarker("Write nothing while the audio is below about -100 dB, so a quiet\n"
                              "channel does not fill the file. The recording clock stops with it,\n"
                              "so the file has no gaps and no idea how long the silence was.");
        }
    }

    static void menuHandler(void* ctx) {
        RecorderModule* _this = (RecorderModule*)ctx;
        // What to record, then the button and what it is doing, then the level going
        // into the file, then how the file is written - roughly the order they are
        // reached for, with the one thing used while recording near the top.
        _this->drawSource();
        _this->drawTransport();
        if (_this->recMode == RECORDER_MODE_AUDIO) { _this->drawLevel(); }
        _this->drawFile();
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
        // keyId() throws on a name it cannot find, so it is only asked about a name
        // that is in the list - see the note in streamUnregisterHandler.
        if (_this->selectedStreamName.empty()) {
            _this->selectStream(name);
        }
        else if (_this->audioStreams.keyExists(_this->selectedStreamName)) {
            _this->streamId = _this->audioStreams.keyId(_this->selectedStreamName);
        }
    }

    static void streamUnregisterHandler(std::string name, void* ctx) {
        RecorderModule* _this = (RecorderModule*)ctx;

        // Remove stream from list
        _this->audioStreams.undefineKey(name);

        // If the stream is in used, deselect it and reselect default. Otherwise, update ID.
        //
        // The else branch asks for the id of whatever is still selected, which is the
        // empty string when nothing is - selectStream() leaves it that way if binding
        // failed. keyId() throws on a name it cannot find, and this runs from a stream
        // unregister event, so the exception came out of the middle of a radio being
        // torn down rather than anywhere it could be handled.
        if (_this->selectedStreamName == name) {
            _this->selectStream("");
        }
        else if (_this->audioStreams.keyExists(_this->selectedStreamName)) {
            _this->streamId = _this->audioStreams.keyId(_this->selectedStreamName);
        }
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

    // The sinks feeding these are stopped before the file is closed or replaced, so the
    // file they write to cannot change under them.
    void writeAudio(float* data, int frames) {
        if (encoded) { encoded->write(data, frames); }
        else { writer.write(data, frames); }
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
        _this->writeAudio((float*)data, count);
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
        _this->writeAudio(data, count);
    }

    std::string handleDebugCommand(const std::string& cmd, const std::string& args) {
        if (cmd == "start") {
            if (!recording) { start(); }
            if (recording) { return "{\"status\":\"recording\"}"; }
            json err;
            err["status"] = "failed";
            err["error"] = lastError;
            return err.dump();
        }
        if (cmd == "stop") {
            if (recording) { stop(); }
            return "{\"status\":\"stopped\"}";
        }
        if (cmd == "status") {
            std::lock_guard<std::recursive_mutex> lck(recMtx);
            json st;
            st["recording"] = recording;
            st["format"] = AUDIO_FORMAT_KEYS[recording ? recFormat : effectiveFormat()];
            st["path"] = currentPath;
            st["frames"] = recordedFrames();
            st["bytes"] = recordedBytes();
            st["writeFailed"] = (bool)(encoded && encoded->writeFailed());
            return st.dump();
        }
        // Audio format for the next recording: WAV, FLAC or MP3.
        if (cmd == "set_format") {
            if (recording) { return "{\"error\":\"recording\"}"; }
            for (int i = 0; i < AUDIO_FORMAT_COUNT; i++) {
                if (args == AUDIO_FORMAT_KEYS[i]) {
                    audioFormat = i;
                    config.acquire();
                    config.conf[name]["audioFormat"] = AUDIO_FORMAT_KEYS[i];
                    config.release(true);
                    return "{\"status\":\"ok\"}";
                }
            }
            return "{\"error\":\"unknown format\"}";
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
    int audioFormat = AUDIO_FORMAT_WAV;
    int flacBitsId = 0;     // 16-bit
    int mp3BitrateId = 4;   // 128 kbps
    bool stereo = false;
    std::string selectedStreamName = "";
    float audioVolume = 1.0f;
    bool ignoreSilence = false;
    MeterChannel meterL, meterR;

    bool recording = false;
    bool ignoringSilence = false;

    std::string lastError;   // why the last attempt to record did not take
    std::string currentPath; // file being written right now
    int recBytesPerFrame = 4;
    int recFormat = AUDIO_FORMAT_WAV;   // what the current or last recording was written as
    double recBytesPerSecond = 0.0;

    std::string previewKey;
    std::string previewName;
    std::string previewPath;
    double previewTime = -1000.0;
    uint64_t cachedFreeSpace = 0;
    double spaceTime = -1000.0;

    wav::Writer writer;
    // The FLAC or MP3 file of an audio recording in one of those; null for WAV.
    std::shared_ptr<recorder_audio::EncodedFile> encoded;
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
    // Settings kept per instance; removed with the instance. See ModuleManager.
    core::moduleManager.forgetSettingsOnDelete(&config, "recorder");
}

MOD_EXPORT ModuleManager::Instance* _CREATE_INSTANCE_(std::string name) {
    return new RecorderModule(name);
}

MOD_EXPORT void _DELETE_INSTANCE_(ModuleManager::Instance* inst) {
    delete (RecorderModule*)inst;
}

MOD_EXPORT void _END_() {
    core::moduleManager.stopForgettingSettings(&config);
    config.disableAutoSave();
    config.save();
}
