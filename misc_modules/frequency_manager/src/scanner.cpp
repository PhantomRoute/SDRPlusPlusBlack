#include "scanner.h"
#include "frequency_manager.h"  // For FrequencyManagerModule definition
#include <cmath>
#include <cstdio>
#include <algorithm>
#include <utils/flog.h>
#include <gui/gui.h>
#include <gui/style.h>
#include <gui/widgets/level_meter.h>
#include <signal_path/signal_path.h>

namespace {

    const float METER_MIN_DB = 0.0f;
    const float METER_MAX_DB = 40.0f;

    // utils::formatFreq lives in a header that defines it without inline linkage,
    // so including it from a second translation unit of this module would not
    // link. A fixed number of decimals also keeps the readout from jittering as
    // the scan steps.
    std::string fmtFreq(double freq) {
        char buf[64];
        if (fabs(freq) >= 1000000.0) { snprintf(buf, sizeof buf, "%.4f MHz", freq / 1000000.0); }
        else if (fabs(freq) >= 1000.0) { snprintf(buf, sizeof buf, "%.3f kHz", freq / 1000.0); }
        else { snprintf(buf, sizeof buf, "%.0f Hz", freq); }
        return std::string(buf);
    }

    ImVec4 stateColor(Scanner::State state) {
        switch (state) {
        case Scanner::SCAN_SETTLING: return ImVec4(0.35f, 0.75f, 1.0f, 1.0f);
        case Scanner::SCAN_MEASURING: return ImVec4(1.0f, 0.78f, 0.2f, 1.0f);
        case Scanner::SCAN_LISTENING: return ImVec4(0.2f, 1.0f, 0.35f, 1.0f);
        case Scanner::SCAN_HELD: return ImVec4(0.6f, 0.65f, 1.0f, 1.0f);
        default: return ImVec4(0.65f, 0.65f, 0.65f, 1.0f);
        }
    }

    template <class T>
    void saveSetting(const char* key, const T& value) {
        auto& config = getFrequencyManagerConfig();
        config.acquire();
        config.conf["scanner"][key] = value;
        config.release(true);
    }

}

const char* Scanner::stateName(State state) {
    switch (state) {
    case SCAN_SETTLING: return "TUNING";
    case SCAN_MEASURING: return "CHECKING";
    case SCAN_LISTENING: return "LISTENING";
    case SCAN_HELD: return "HELD";
    default: return "IDLE";
    }
}

Scanner::Scanner(FrequencyManagerModule* module) : module(module) {
    // Listen for play state changes
    playStateHandler.ctx = this;
    playStateHandler.handler = [](bool playing, void* ctx) {
        ((Scanner*)ctx)->onPlayStateChange(playing);
    };
    gui::mainWindow.onPlayStateChange.bindHandler(&playStateHandler);

    // Load scanner settings from config
    auto& config = getFrequencyManagerConfig();
    config.acquire();
    if (!config.conf.contains("scanner") || !config.conf["scanner"].is_object()) {
        config.conf["scanner"] = json::object();
    }
    auto& sc = config.conf["scanner"];

    // Every read is guarded and fills in its own default: a config written by an
    // older build is missing the keys added since, and reading one of those out of
    // the json throws.
    auto num = [&sc](const char* key, float def) -> float {
        if (sc.contains(key) && sc[key].is_number()) { return sc[key].get<float>(); }
        sc[key] = def;
        return def;
    };
    auto flag = [&sc](const char* key, bool def) -> bool {
        if (sc.contains(key) && sc[key].is_boolean()) { return sc[key].get<bool>(); }
        sc[key] = def;
        return def;
    };

    dwellMs = num("scanIntervalMs", 250.0f);
    settleMs = num("settleMs", 120.0f);
    listenTimeSec = num("listenTimeSec", 10.0f);
    noiseFloor = num("noiseFloor", 3.0f);
    signalMarginDb = num("signalMarginDb", 4.0f);
    squelchEnabled = flag("squelchEnabled", false);
    carrierHoldMode = flag("carrierHoldMode", false);
    if (sc.contains("skipped") && sc["skipped"].is_array()) {
        for (auto& entry : sc["skipped"]) {
            if (entry.is_string()) { skipped.insert(entry.get<std::string>()); }
        }
    }
    else {
        sc["skipped"] = json::array();
    }

    // An earlier default put -120 in the noise floor, an absolute dBFS level, where
    // the rest of the scanner works in dB over the local noise. Every measurement
    // clears -120, so the first channel always looked busy and the scan stopped
    // dead on it. Repair anything outside the range the UI allows.
    if (!(noiseFloor >= METER_MIN_DB && noiseFloor <= METER_MAX_DB)) { noiseFloor = 3.0f; }
    if (!(signalMarginDb >= 0.0f && signalMarginDb <= METER_MAX_DB)) { signalMarginDb = 4.0f; }
    dwellMs = std::clamp<float>(dwellMs, 20.0f, 10000.0f);
    settleMs = std::clamp<float>(settleMs, 0.0f, 2000.0f);
    listenTimeSec = std::clamp<float>(listenTimeSec, 0.5f, 600.0f);

    config.release(true);
}

Scanner::~Scanner() {
    stop();
    gui::mainWindow.onPlayStateChange.unbindHandler(&playStateHandler);
}

void Scanner::setBookmarks(const std::vector<std::string>& newBookmarks, const std::map<std::string, FrequencyBookmark>& newMap) {
    bookmarks = newBookmarks;
    bookmarksMap = newMap;

    if (bookmarks.empty()) {
        currentStation.clear();
        haveCurrentBookmark = false;
        currentStationIndex = 0;
        return;
    }

    // Follow the channel we are on by name rather than by index. Editing the list
    // while a scan runs used to stop it dead, or leave the index pointing at a
    // different channel than the one being listened to.
    if (!currentStation.empty()) {
        auto it = std::find(bookmarks.begin(), bookmarks.end(), currentStation);
        if (it != bookmarks.end()) {
            currentStationIndex = (size_t)std::distance(bookmarks.begin(), it);
        }
        else if (isScanning()) {
            // The channel we were parked on is gone. Carry on from where it was.
            if (currentStationIndex >= bookmarks.size()) { currentStationIndex = 0; }
            int next = findScannable((int)currentStationIndex, 1);
            if (next < 0) {
                stop();
                statusMessage = "Nothing left in this list to scan.";
                return;
            }
            gotoStation((size_t)next);
            return;
        }
    }

    if (currentStationIndex >= bookmarks.size()) { currentStationIndex = 0; }
    currentStation = bookmarks[currentStationIndex];
    auto found = bookmarksMap.find(currentStation);
    haveCurrentBookmark = (found != bookmarksMap.end());
    if (haveCurrentBookmark) { currentBookmark = found->second; }
}

bool Scanner::isScannable(const std::string& name) const {
    if (skipped.find(name) != skipped.end()) { return false; }
    auto it = bookmarksMap.find(name);
    if (it == bookmarksMap.end()) { return false; }
    // A bookmark tied to a radio that is not loaded cannot be tuned - applyBookmark
    // does nothing for it - so the scan would sit on the previous channel measuring
    // it over and over.
    if (!it->second.vfoName.empty() && !sigpath::vfoManager.vfoExists(it->second.vfoName)) { return false; }
    return true;
}

int Scanner::findScannable(int from, int dir) const {
    int count = (int)bookmarks.size();
    if (count <= 0) { return -1; }
    if (dir == 0) { dir = 1; }
    for (int i = 1; i <= count; i++) {
        int idx = (((from + (dir * i)) % count) + count) % count;
        if (isScannable(bookmarks[idx])) { return idx; }
    }
    return -1;
}

void Scanner::enterState(State newState) {
    state = newState;
    stateTime = 0.0f;
}

void Scanner::gotoStation(size_t index) {
    if (bookmarks.empty()) { return; }
    if (index >= bookmarks.size()) { index = 0; }

    currentStationIndex = index;
    currentStation = bookmarks[index];
    auto it = bookmarksMap.find(currentStation);
    haveCurrentBookmark = (it != bookmarksMap.end());
    if (haveCurrentBookmark) {
        currentBookmark = it->second;
        applyBookmark(currentBookmark, gui::waterfall.selectedVFO);
    }

    haveLevel = false;
    enterState(SCAN_SETTLING);
}

void Scanner::step(int dir) {
    int next = findScannable((int)currentStationIndex, dir);
    if (next < 0) {
        stop();
        statusMessage = "Every channel in this list is skipped or has no radio.";
        return;
    }
    gotoStation((size_t)next);
}

void Scanner::start() {
    statusMessage.clear();
    if (bookmarks.empty()) {
        statusMessage = "This list has no channels to scan.";
        return;
    }
    if (gui::waterfall.selectedVFO.empty()) {
        statusMessage = "No radio to tune. Load a radio module first.";
        return;
    }

    size_t from = (currentStationIndex < bookmarks.size()) ? currentStationIndex : 0;
    int idx = isScannable(bookmarks[from]) ? (int)from : findScannable((int)from, 1);
    if (idx < 0) {
        statusMessage = "Every channel in this list is skipped or has no radio.";
        return;
    }

    gotoStation((size_t)idx);
    flog::info("Scanner started");
}

void Scanner::stop() {
    if (state == SCAN_IDLE) { return; }
    state = SCAN_IDLE;
    stateTime = 0.0f;
    statusMessage.clear();
    // Whatever we muted, we unmute. The old code only did this when the squelch
    // checkbox happened to still be on, so turning it off mid scan left the audio
    // muted with no way back other than restarting.
    setMuted(false);
    flog::info("Scanner stopped");
}

void Scanner::setMuted(bool muted) {
    if (muted == weMuted) { return; }
    weMuted = muted;
    sigpath::sinkManager.setAllMuted(muted);
}

void Scanner::onPlayStateChange(bool playing) {
    if (!playing && isScanning()) {
        stop();
        statusMessage = "Stopped, because the radio was stopped.";
    }
}

void Scanner::logHit() {
    Hit hit;
    hit.name = currentStation;
    hit.frequency = haveCurrentBookmark ? currentBookmark.frequency : 0.0;
    hit.level = haveLevel ? level : 0.0f;
    hit.time = ImGui::GetTime();
    hits.push_front(hit);
    while (hits.size() > 8) { hits.pop_back(); }
}

float Scanner::historyAverage() const {
    if (signalHistory.empty()) { return 0.0f; }
    float sum = 0.0f;
    for (const auto& sample : signalHistory) { sum += sample.level; }
    return sum / (float)signalHistory.size();
}

bool Scanner::currentChannel(double& freq, double& bandwidth) const {
    if (isScanning() && haveCurrentBookmark) {
        freq = currentBookmark.frequency;
        bandwidth = currentBookmark.bandwidth;
    }
    else {
        // Not scanning: report on whatever the radio is tuned to, so the meter and
        // the "Set from noise" button mean something before the scan starts.
        const std::string vfoName = gui::waterfall.selectedVFO;
        if (vfoName.empty()) { return false; }
        auto it = gui::waterfall.vfos.find(vfoName);
        if (it == gui::waterfall.vfos.end() || it->second == NULL) { return false; }
        freq = gui::waterfall.getCenterFrequency() + it->second->centerOffset;
        bandwidth = it->second->bandwidth;
    }
    if (!std::isfinite(freq) || !std::isfinite(bandwidth) || bandwidth <= 0.0) { return false; }
    return true;
}

bool Scanner::measureChannel(double freq, double bandwidth, float& snrOut, float& noiseOut) {
    int dataWidth = 0;
    float* data = gui::waterfall.acquireLatestFFT(dataWidth);
    if (data == NULL) { return false; } // No FFT yet, and nothing was locked

    bool ok = false;
    double wfWidth = gui::waterfall.getViewBandwidth();
    if (dataWidth >= 16 && wfWidth > 0.0) {
        double wfStart = gui::waterfall.getViewOffset() + gui::waterfall.getCenterFrequency() - (wfWidth / 2.0);
        double binsPerHz = (double)dataWidth / wfWidth;
        int centerBin = (int)((freq - wfStart) * binsPerHz);

        // Only measure a channel that is actually on screen. Clamping one that is
        // not would measure the edge of the display instead and read whatever
        // happens to be there.
        if (centerBin >= 1 && centerBin <= dataWidth - 2) {
            int halfCh = (int)ceil((bandwidth / 2.0) * binsPerHz);
            if (halfCh < 1) { halfCh = 1; }
            int chLow = std::clamp<int>(centerBin - halfCh, 0, dataWidth - 1);
            int chHigh = std::clamp<int>(centerBin + halfCh, 0, dataWidth - 1);

            float peak = -INFINITY;
            for (int i = chLow; i <= chHigh; i++) {
                if (data[i] > peak) { peak = data[i]; }
            }

            // Noise reference: the quiet quarter of the bins either side of the
            // channel. Same idea as the core's SNR meter, so the numbers here are
            // on the same scale as the one next to the frequency display.
            noiseScratch.clear();
            int skirt = std::clamp<int>(halfCh * 4, 8, dataWidth);
            int skirtLow = std::max<int>(centerBin - skirt, 0);
            int skirtHigh = std::min<int>(centerBin + skirt, dataWidth - 1);
            for (int i = skirtLow; i <= skirtHigh; i++) {
                if (i >= chLow && i <= chHigh) { continue; }
                noiseScratch.push_back(data[i]);
            }
            if (noiseScratch.size() < 8) {
                // Zoomed in far enough that the channel fills the view: fall back
                // to everything outside it.
                noiseScratch.clear();
                for (int i = 0; i < dataWidth; i++) {
                    if (i >= chLow && i <= chHigh) { continue; }
                    noiseScratch.push_back(data[i]);
                }
            }
            if (noiseScratch.size() >= 8) {
                size_t kth = noiseScratch.size() / 4;
                std::nth_element(noiseScratch.begin(), noiseScratch.begin() + kth, noiseScratch.end());
                float noise = noiseScratch[kth];
                if (std::isfinite(peak) && std::isfinite(noise)) {
                    snrOut = peak - noise;
                    noiseOut = noise;
                    ok = true;
                }
            }
        }
    }

    gui::waterfall.releaseLatestFFT();
    return ok;
}

void Scanner::sampleLevel(float deltaTime) {
    // Idle and off screen, nothing reads the level, so do not pay for it.
    double now = ImGui::GetTime();
    if (state == SCAN_IDLE && (now - lastRenderTime) > 0.5) {
        haveLevel = false;
        signalHistory.clear();
        return;
    }

    haveLevel = false;

    double freq = 0.0;
    double bandwidth = 0.0;
    if (currentChannel(freq, bandwidth)) {
        float snr = 0.0f;
        float noise = 0.0f;
        if (measureChannel(freq, bandwidth, snr, noise)) {
            level = snr;
            haveLevel = true;
        }
    }
    if (!haveLevel && !gui::waterfall.selectedVFO.empty() &&
        (!isScanning() || !haveCurrentBookmark || currentBookmark.vfoName.empty() || currentBookmark.vfoName == gui::waterfall.selectedVFO)) {
        // The channel is off the visible span, or there is no FFT to read. The core
        // still measures the selected VFO for the SNR meter, so use that.
        float snr = gui::waterfall.selectedVFOSNR;
        if (std::isfinite(snr)) {
            level = snr;
            haveLevel = true;
        }
    }

    if (!haveLevel) { return; }

    meterPeak = std::max<float>(level, meterPeak - (12.0f * deltaTime));

    float stamp = (float)(now * 1000.0);
    signalHistory.push_back({ level, stamp });
    while (!signalHistory.empty() && (stamp - signalHistory.front().time) > 500.0f) {
        signalHistory.pop_front();
    }
}

void Scanner::update(float deltaTime) {
    sampleLevel(deltaTime);

    if (state == SCAN_IDLE) {
        setMuted(false);
        return;
    }

    if (bookmarks.empty()) {
        stop();
        statusMessage = "The list being scanned is now empty.";
        return;
    }

    stateTime += deltaTime;
    float trigger = getTriggerLevel();

    switch (state) {
    case SCAN_SETTLING:
        // The FFT still shows the channel we came from for a frame or two after a
        // retune. Measuring during that window reports the previous channel's
        // signal on this one.
        if ((stateTime * 1000.0f) >= settleMs) {
            enterState(SCAN_MEASURING);
        }
        break;

    case SCAN_MEASURING:
        if (haveLevel && level >= trigger) {
            logHit();
            enterState(SCAN_LISTENING);
        }
        else if ((stateTime * 1000.0f) >= dwellMs) {
            step(1);
        }
        break;

    case SCAN_LISTENING:
        // Carrier hold restarts the countdown for as long as the channel stays up,
        // which turns the listen time into a hang time after it drops. The 2dB of
        // hysteresis stops a signal sitting right on the trigger from flapping.
        if (carrierHoldMode && haveLevel && level >= (trigger - 2.0f)) {
            stateTime = 0.0f;
        }
        if (stateTime >= listenTimeSec) {
            step(1);
        }
        break;

    case SCAN_HELD:
    default:
        break;
    }

    // step() can stop the scan when it runs out of channels to visit.
    if (state == SCAN_IDLE) {
        setMuted(false);
        return;
    }
    setMuted(squelchEnabled && state != SCAN_LISTENING && state != SCAN_HELD);
}

void Scanner::render() {
    // The label carries the state so a collapsed section still says whether a scan
    // is running. Fixed ### id, so collapsing it does not follow the label around.
    char header[96];
    if (isScanning()) {
        snprintf(header, sizeof header, "Scanner - %s###freq_mgr_scanner", stateName(state));
    }
    else {
        snprintf(header, sizeof header, "Scanner###freq_mgr_scanner");
    }
    if (!ImGui::CollapsingHeader(header, ImGuiTreeNodeFlags_DefaultOpen)) { return; }
    lastRenderTime = ImGui::GetTime();

    drawTransport();
    drawStatus();
    drawMeter();
    drawActivity();
    drawSettings();
}

void Scanner::drawTransport() {
    float width = ImGui::GetContentRegionAvail().x;
    float spacing = ImGui::GetStyle().ItemSpacing.x;

    if (!isScanning()) {
        if (ImGui::Button("Start scan##scanner_start", ImVec2(width, 0))) { start(); }
        if (!statusMessage.empty()) {
            ImGui::PushTextWrapPos(0.0f);
            ImGui::TextColored(ImVec4(1.0f, 0.7f, 0.3f, 1.0f), "%s", statusMessage.c_str());
            ImGui::PopTextWrapPos();
        }
        if (!skipped.empty()) {
            ImGui::Text("%d skipped", (int)skipped.size());
            ImGui::SameLine();
            if (ImGui::Button("Clear##scanner_clear_skipped")) {
                skipped.clear();
                saveSetting("skipped", json::array());
            }
        }
        return;
    }

    float btnWidth = (width - (spacing * 3.0f)) / 4.0f;
    if (ImGui::Button("Stop##scanner_stop", ImVec2(btnWidth, 0))) { stop(); }
    ImGui::SameLine();
    if (ImGui::Button("<##scanner_prev", ImVec2(btnWidth, 0))) { step(-1); }
    if (ImGui::IsItemHovered()) { style::tooltip("Back to the previous channel"); }
    ImGui::SameLine();
    if (state == SCAN_HELD) {
        if (ImGui::Button("Resume##scanner_hold", ImVec2(btnWidth, 0))) {
            enterState(stateBeforeHold == SCAN_LISTENING ? SCAN_LISTENING : SCAN_MEASURING);
        }
    }
    else {
        if (ImGui::Button("Hold##scanner_hold", ImVec2(btnWidth, 0))) {
            stateBeforeHold = state;
            enterState(SCAN_HELD);
        }
    }
    if (ImGui::IsItemHovered()) { style::tooltip("Stay on this channel until you resume"); }
    ImGui::SameLine();
    if (ImGui::Button(">##scanner_next", ImVec2(btnWidth, 0))) { step(1); }
    if (ImGui::IsItemHovered()) { style::tooltip("Move on to the next channel"); }

    if (ImGui::Button("Skip this channel##scanner_skip", ImVec2(width, 0))) {
        if (!currentStation.empty()) {
            skipped.insert(currentStation);
            json arr = json::array();
            for (const auto& name : skipped) { arr.push_back(name); }
            saveSetting("skipped", arr);
            step(1);
        }
    }
    if (ImGui::IsItemHovered()) { style::tooltip("Leave this channel out of the scan from now on"); }
}

void Scanner::drawStatus() {
    float width = ImGui::GetContentRegionAvail().x;
    ImVec4 color = stateColor(state);

    ImGui::TextColored(color, "%s", stateName(state));
    ImGui::SameLine();
    if (isScanning() && !bookmarks.empty()) {
        ImGui::Text("%d/%d", (int)currentStationIndex + 1, (int)bookmarks.size());
    }
    else {
        ImGui::TextDisabled("%d channels", (int)bookmarks.size());
    }

    ImGui::PushTextWrapPos(0.0f);
    ImGui::TextUnformatted(currentStation.empty() ? "--" : currentStation.c_str());
    ImGui::PopTextWrapPos();

    if (haveCurrentBookmark) {
        ImGui::PushFont(style::mediumFont);
        ImGui::TextColored(color, "%s", fmtFreq(currentBookmark.frequency).c_str());
        ImGui::PopFont();
    }

    // What the scanner is waiting for, and how much longer it will wait.
    float fraction = 0.0f;
    char overlay[64] = { 0 };
    switch (state) {
    case SCAN_SETTLING:
        fraction = (settleMs > 0.0f) ? ((stateTime * 1000.0f) / settleMs) : 1.0f;
        snprintf(overlay, sizeof overlay, "tuning");
        break;
    case SCAN_MEASURING:
        fraction = (dwellMs > 0.0f) ? ((stateTime * 1000.0f) / dwellMs) : 1.0f;
        snprintf(overlay, sizeof overlay, "quiet, moving on in %.1fs", std::max<float>(0.0f, (dwellMs / 1000.0f) - stateTime));
        break;
    case SCAN_LISTENING:
        fraction = (listenTimeSec > 0.0f) ? (1.0f - (stateTime / listenTimeSec)) : 0.0f;
        snprintf(overlay, sizeof overlay, "%s %.1fs", carrierHoldMode ? "hang" : "leaving in", std::max<float>(0.0f, listenTimeSec - stateTime));
        break;
    case SCAN_HELD:
        fraction = 1.0f;
        snprintf(overlay, sizeof overlay, "held");
        break;
    default:
        fraction = 0.0f;
        snprintf(overlay, sizeof overlay, "not scanning");
        break;
    }
    ImGui::PushStyleColor(ImGuiCol_PlotHistogram, color);
    ImGui::ProgressBar(std::clamp<float>(fraction, 0.0f, 1.0f), ImVec2(width, 0), overlay);
    ImGui::PopStyleColor();
}

void Scanner::drawMeter() {
    ImGui::LevelMeterStyle look;
    look.fillClosed = gui::themeManager.snrMeterColor;
    look.fillOpen = gui::themeManager.meterOpenColor;
    look.threshold = ImGui::GetStyleColorVec4(ImGuiCol_Text);

    // The widget edits the trigger, which is where the two settings behind it meet.
    // Only the floor moves with the drag; the margin is what the user set it to.
    float trigger = getTriggerLevel();
    ImGui::LevelMeterResult r = ImGui::LevelMeter("##scanner_meter", METER_MIN_DB, METER_MAX_DB,
                                                 haveLevel, level, meterPeak,
                                                 haveLevel && level >= trigger,
                                                 &trigger, look);
    if (r.changed) {
        noiseFloor = std::clamp<float>(trigger - signalMarginDb, METER_MIN_DB, METER_MAX_DB);
    }
    // Written back on release, so a drag does not touch the config file per frame.
    if (r.released) { saveSetting("noiseFloor", noiseFloor); }
    if (r.hovered) { style::tooltip("Signal on the current channel, in dB over the noise around it.\nDrag to set the level a channel has to beat."); }
}

void Scanner::drawActivity() {
    if (hits.empty()) { return; }
    if (!ImGui::TreeNode("Recent activity##scanner_hits")) { return; }
    double now = ImGui::GetTime();
    int index = 0;
    for (const auto& hit : hits) {
        char label[192];
        snprintf(label, sizeof label, "%s  %s  %.0f dB  %.0fs ago##scanner_hit_%d",
                 hit.name.c_str(), fmtFreq(hit.frequency).c_str(), hit.level,
                 std::max<double>(0.0, now - hit.time), index++);
        if (ImGui::Selectable(label)) {
            auto it = bookmarksMap.find(hit.name);
            if (it != bookmarksMap.end()) {
                stop();
                applyBookmark(it->second, gui::waterfall.selectedVFO);
            }
        }
        if (ImGui::IsItemHovered()) { style::tooltip("Stop the scan and go back to this channel"); }
    }
    ImGui::TreePop();
}

void Scanner::drawSettings() {
    if (!ImGui::TreeNode("Settings##scanner_settings")) { return; }

    // Leave room on each row for the (?) marker after the field.
    float markerRoom = 32.0f * style::uiScale;

    // These are two separate questions - what makes the scan stop, and how long it
    // waits at each step - and they were interleaved. Settle in particular sat at
    // the bottom under the two behaviour switches, three rows away from the other
    // two times it belongs with.
    ImGui::SectionHeader("WHAT COUNTS AS BUSY");

    ImGui::LeftLabel("Noise floor (dB)");
    ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x - markerRoom);
    if (ImGui::InputFloat("##scanner_noise_floor", &noiseFloor, 1.0f, 5.0f, "%.1f")) {
        noiseFloor = std::clamp<float>(noiseFloor, METER_MIN_DB, METER_MAX_DB);
        saveSetting("noiseFloor", noiseFloor);
    }
    ImGui::HelpMarker("Where the noise sits on this band. A channel counts as busy at floor + margin,\nwhich is the line drawn on the meter.");

    ImGui::LeftLabel("Margin (dB)");
    ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x - markerRoom);
    if (ImGui::InputFloat("##scanner_signal_margin", &signalMarginDb, 0.5f, 2.0f, "%.1f")) {
        signalMarginDb = std::clamp<float>(signalMarginDb, 0.0f, METER_MAX_DB);
        saveSetting("signalMarginDb", signalMarginDb);
    }
    ImGui::HelpMarker("How far over the noise floor a channel has to be. Raise it if the scan stops\non nothing, lower it if it walks past weak signals.");

    if (ImGui::Button("Set floor from what I hear now##scanner_use_current", ImVec2(ImGui::GetContentRegionAvail().x, 0))) {
        if (!signalHistory.empty()) {
            noiseFloor = std::clamp<float>(historyAverage(), METER_MIN_DB, METER_MAX_DB);
            saveSetting("noiseFloor", noiseFloor);
        }
    }
    if (ImGui::IsItemHovered()) { style::tooltip("Take the last half second of the current channel as the noise floor.\nTune to an empty channel first."); }

    ImGui::SectionHeader("TIMING");

    ImGui::LeftLabel("Wait (ms)");
    ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x - markerRoom);
    if (ImGui::InputFloat("##scanner_dwell", &dwellMs, 50.0f, 250.0f, "%.0f")) {
        dwellMs = std::clamp<float>(dwellMs, 20.0f, 10000.0f);
        saveSetting("scanIntervalMs", dwellMs);
    }
    ImGui::HelpMarker("How long to give a quiet channel a chance to come alive before moving on.");

    ImGui::LeftLabel("Settle (ms)");
    ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x - markerRoom);
    if (ImGui::InputFloat("##scanner_settle", &settleMs, 20.0f, 100.0f, "%.0f")) {
        settleMs = std::clamp<float>(settleMs, 0.0f, 2000.0f);
        saveSetting("settleMs", settleMs);
    }
    ImGui::HelpMarker("Ignore the signal for this long after each hop, while the spectrum catches up.\nRaise it if the scan keeps stopping on channels that turn out to be empty.");

    ImGui::LeftLabel(carrierHoldMode ? "Hang (s)" : "Listen (s)");
    ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x - markerRoom);
    if (ImGui::InputFloat("##scanner_listen", &listenTimeSec, 1.0f, 5.0f, "%.1f")) {
        listenTimeSec = std::clamp<float>(listenTimeSec, 0.5f, 600.0f);
        saveSetting("listenTimeSec", listenTimeSec);
    }
    ImGui::HelpMarker(carrierHoldMode ? "How long to wait after the signal drops before moving on."
                                      : "How long to stay on a busy channel before moving on.");

    if (ImGui::Checkbox("Stay while the signal lasts##scanner_carrier_hold", &carrierHoldMode)) {
        saveSetting("carrierHoldMode", carrierHoldMode);
    }
    ImGui::HelpMarker("Stay parked for as long as the channel is busy instead of leaving after a fixed\ntime. The listen time then becomes the hang time after the signal drops.");

    ImGui::SectionHeader("WHILE SCANNING");

    if (ImGui::Checkbox("Mute while searching##scanner_squelch", &squelchEnabled)) {
        saveSetting("squelchEnabled", squelchEnabled);
        if (!squelchEnabled) { setMuted(false); }
    }
    ImGui::HelpMarker("Mute the audio while the scan is hopping, so you only hear the channels it stops on.");

    if (!skipped.empty()) {
        ImGui::Separator();
        ImGui::Text("Skipped (%d)", (int)skipped.size());
        std::string unskip;
        for (const auto& name : skipped) {
            if (ImGui::SmallButton(("x##scanner_unskip_" + name).c_str())) { unskip = name; }
            ImGui::SameLine();
            ImGui::TextUnformatted(name.c_str());
        }
        if (!unskip.empty()) {
            skipped.erase(unskip);
            json arr = json::array();
            for (const auto& name : skipped) { arr.push_back(name); }
            saveSetting("skipped", arr);
        }
    }

    ImGui::TreePop();
}

void Scanner::drawWaterfallOverlay(const ImGui::WaterFall::FFTRedrawArgs& args) {
    if (state == SCAN_IDLE) { return; }
    ImDrawList* draw = args.window->DrawList;
    ImVec4 color = stateColor(state);

    // Where the scan will go next, as a tick under each channel of the list.
    for (const auto& name : bookmarks) {
        auto it = bookmarksMap.find(name);
        if (it == bookmarksMap.end()) { continue; }
        double freq = it->second.frequency;
        if (freq < args.lowFreq || freq > args.highFreq) { continue; }
        float x = (float)(args.min.x + ((freq - args.lowFreq) * args.freqToPixelRatio));
        if (x < args.min.x || x > args.max.x) { continue; }
        bool active = isScannable(name);
        ImU32 tick = ImGui::ColorConvertFloat4ToU32(active ? ImVec4(1.0f, 1.0f, 1.0f, 0.6f) : ImVec4(1.0f, 0.35f, 0.35f, 0.5f));
        float size = 4.0f * style::uiScale;
        draw->AddTriangleFilled(ImVec2(x - size, args.max.y), ImVec2(x + size, args.max.y), ImVec2(x, args.max.y - (size * 1.5f)), tick);
    }

    // The channel being checked right now.
    if (haveCurrentBookmark) {
        double bandwidth = (currentBookmark.bandwidth > 0.0) ? currentBookmark.bandwidth : 0.0;
        double low = currentBookmark.frequency - (bandwidth / 2.0);
        double high = currentBookmark.frequency + (bandwidth / 2.0);
        if (high >= args.lowFreq && low <= args.highFreq) {
            float x0 = (float)(args.min.x + ((low - args.lowFreq) * args.freqToPixelRatio));
            float x1 = (float)(args.min.x + ((high - args.lowFreq) * args.freqToPixelRatio));
            if ((x1 - x0) < 2.0f) {
                float mid = (x0 + x1) / 2.0f;
                x0 = mid - 1.0f;
                x1 = mid + 1.0f;
            }
            x0 = std::clamp<float>(x0, args.min.x, args.max.x);
            x1 = std::clamp<float>(x1, args.min.x, args.max.x);

            ImVec4 fillColor = color;
            fillColor.w = 0.18f;
            draw->AddRectFilled(ImVec2(x0, args.min.y), ImVec2(x1, args.max.y), ImGui::ColorConvertFloat4ToU32(fillColor));
            ImU32 edge = ImGui::ColorConvertFloat4ToU32(color);
            draw->AddLine(ImVec2(x0, args.min.y), ImVec2(x0, args.max.y), edge, 1.0f);
            draw->AddLine(ImVec2(x1, args.min.y), ImVec2(x1, args.max.y), edge, 1.0f);

            char label[64];
            if (state == SCAN_LISTENING) {
                snprintf(label, sizeof label, "LISTENING %.0fs", std::max<float>(0.0f, listenTimeSec - stateTime));
            }
            else {
                snprintf(label, sizeof label, "%s", stateName(state));
            }
            ImVec2 labelSize = ImGui::CalcTextSize(label);
            float labelX = std::clamp<float>(((x0 + x1) / 2.0f) - (labelSize.x / 2.0f), args.min.x, std::max<float>(args.min.x, args.max.x - labelSize.x));
            float labelY = args.max.y - labelSize.y - (8.0f * style::uiScale);
            draw->AddText(ImVec2(labelX, labelY), edge, label);
        }
    }

}
