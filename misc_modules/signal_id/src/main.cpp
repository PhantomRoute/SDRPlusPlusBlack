#include <imgui.h>
#include <module.h>
#include <gui/gui.h>
#include <gui/style.h>
#include <gui/widgets/bandplan.h>
#include <signal_path/signal_path.h>
#include <config.h>
#include <core.h>
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <deque>
#include <string>
#include <utility>
#include <vector>

SDRPP_MOD_INFO{
    /* Name:            */ "signal_id",
    /* Description:     */ "Measures what the signal under the VFO is doing",
    /* Author:          */ "PhantomRoute",
    /* Version:         */ 0, 1, 0,
    /* Max instances    */ 1
};

// This panel reports what can be measured off the spectrum, and nothing else. It
// deliberately does not guess at what the signal is: a width and a shape are not
// enough to name a mode, and a list of maybes is worse than no answer at all.
// Everything here is either a measurement or a plain description of one.

ConfigManager config;

namespace {
    std::string fmtFreq(double hz) {
        char buf[64];
        if (fabs(hz) >= 1000000.0) { snprintf(buf, sizeof buf, "%.4f MHz", hz / 1000000.0); }
        else if (fabs(hz) >= 1000.0) { snprintf(buf, sizeof buf, "%.3f kHz", hz / 1000.0); }
        else { snprintf(buf, sizeof buf, "%.1f Hz", hz); }
        return std::string(buf);
    }

    std::string fmtWidth(double hz) {
        char buf[64];
        if (hz >= 1000000.0) { snprintf(buf, sizeof buf, "%.3f MHz", hz / 1000000.0); }
        else if (hz >= 1000.0) { snprintf(buf, sizeof buf, "%.2f kHz", hz / 1000.0); }
        else { snprintf(buf, sizeof buf, "%.0f Hz", hz); }
        return std::string(buf);
    }

    std::string fmtTime(double seconds) {
        char buf[64];
        if (seconds < 1.0) { snprintf(buf, sizeof buf, "%.0f ms", seconds * 1000.0); }
        else { snprintf(buf, sizeof buf, "%.1f s", seconds); }
        return std::string(buf);
    }

    void helpMarker(const char* text) {
        ImGui::SameLine();
        ImGui::TextDisabled("(?)");
        if (ImGui::IsItemHovered()) { ImGui::SetTooltip("%s", text); }
    }

    void sectionHeader(const char* title) {
        ImGui::Spacing();
        ImGui::TextDisabled("%s", title);
        ImGui::Separator();
    }
}

class SignalIDModule : public ModuleManager::Instance {
public:
    SignalIDModule(std::string name) {
        this->name = name;

        config.acquire();
        if (config.conf.contains("holdSeconds") && config.conf["holdSeconds"].is_number()) {
            holdSeconds = config.conf["holdSeconds"];
        }
        config.release();
        holdSeconds = std::clamp<float>(holdSeconds, 0.0f, 30.0f);

        tickHandler.ctx = this;
        tickHandler.handler = [](ImGuiContext* gctx, void* ctx) {
            ((SignalIDModule*)ctx)->tick();
        };
        gui::mainWindow.onWaterfallDrawn.bindHandler(&tickHandler);

        gui::menu.registerEntry(name, menuHandler, this, NULL);
    }

    ~SignalIDModule() {
        gui::menu.removeEntry(name);
        gui::mainWindow.onWaterfallDrawn.unbindHandler(&tickHandler);
    }

    void postInit() {}
    void enable() { enabled = true; }
    void disable() { enabled = false; }
    bool isEnabled() { return enabled; }

private:
    struct Measurement {
        bool valid = false;
        double centre = 0.0;      // where the peak is, which is not always the VFO
        double occupied = 0.0;    // width 26 dB below the peak
        double halfPower = 0.0;   // width 3 dB below the peak
        double hzPerBin = 0.0;
        int binsAcross = 0;
        float peakDbfs = 0.0f;
        float noiseDbfs = 0.0f;
        float snr = 0.0f;
        int peaks = 0;            // separate maxima across the occupied width
        double peakSpacing = 0.0; // gap between them, when there are exactly two
        float crestDb = 0.0f;     // peak above the average across the occupied width
        double balance = 0.0;     // -1 all below the peak, 0 even, +1 all above
    };

    // ---- Peak hold. A signal that transmits in bursts is gone before the numbers
    // can be read, so the measurement is taken from the loudest thing each bin has
    // shown recently rather than from this frame alone.
    std::vector<float> hold;
    double holdCentre = 0.0;
    double holdSpan = 0.0;
    int holdWidth = 0;

    float holdSeconds = 3.0f;
    bool frozen = false;

    Measurement meas;
    double lastPanelDraw = -1000.0;
    std::vector<float> scratch;

    // ---- Settling.
    //
    // The measurement above is taken every frame and every one of its numbers moves
    // a little each time, which is unreadable no matter how steady the signal is.
    // What the panel shows is the median of the last couple of seconds, refreshed
    // four times a second: fast enough to follow a signal, slow enough to read.
    // Medians rather than averages because the peak count and the widths jump about
    // rather than wandering, and one bad frame should not move the number at all.
    static constexpr double SETTLE_WINDOW = 2.0;
    static constexpr double SETTLE_PERIOD = 0.25;
    std::deque<std::pair<double, Measurement>> history;
    std::vector<double> medianScratch;
    Measurement shown;
    bool shownValid = false;
    double lastSettle = 0.0;
    int profileIdx = 1; // remembered so the label does not flap across a threshold
    bool coarse = false;

    // ---- Timing. On or off is decided per frame, then turned into how long the
    // signal stays up and how long it waits between transmissions.
    bool wasOn = false;
    double lastEdge = 0.0;
    std::deque<double> burstLengths;
    std::deque<double> gapLengths;
    int samplesOn = 0;
    int samplesTotal = 0;
    double dutyWindowStart = 0.0;

    // ---- Drift, from where the peak has moved over the last few seconds.
    std::deque<std::pair<double, double>> centreHistory;

    void resetAll() {
        hold.clear();
        holdWidth = 0;
        burstLengths.clear();
        gapLengths.clear();
        centreHistory.clear();
        history.clear();
        shownValid = false;
        samplesOn = 0;
        samplesTotal = 0;
        wasOn = false;
        lastEdge = 0.0;
        dutyWindowStart = ImGui::GetTime();
    }

    // The median of one field over everything measured in the settling window.
    template <class Get>
    double medianOf(Get get) {
        medianScratch.clear();
        for (const auto& entry : history) { medianScratch.push_back(get(entry.second)); }
        if (medianScratch.empty()) { return 0.0; }
        size_t k = medianScratch.size() / 2;
        std::nth_element(medianScratch.begin(), medianScratch.begin() + k, medianScratch.end());
        return medianScratch[k];
    }

    void settle(double now) {
        while (!history.empty() && (now - history.front().first) > SETTLE_WINDOW) {
            history.pop_front();
        }
        if ((now - lastSettle) < SETTLE_PERIOD) { return; }
        lastSettle = now;
        // Nothing new to settle: keep showing the last set rather than blanking the
        // panel, which would be another thing flashing on and off.
        if (history.empty()) { return; }

        shown = history.back().second;
        shown.centre = medianOf([](const Measurement& m) { return m.centre; });
        shown.occupied = medianOf([](const Measurement& m) { return m.occupied; });
        shown.halfPower = medianOf([](const Measurement& m) { return m.halfPower; });
        shown.peakDbfs = (float)medianOf([](const Measurement& m) { return (double)m.peakDbfs; });
        shown.noiseDbfs = (float)medianOf([](const Measurement& m) { return (double)m.noiseDbfs; });
        shown.snr = (float)medianOf([](const Measurement& m) { return (double)m.snr; });
        shown.crestDb = (float)medianOf([](const Measurement& m) { return (double)m.crestDb; });
        shown.balance = medianOf([](const Measurement& m) { return m.balance; });
        shown.peaks = (int)llround(medianOf([](const Measurement& m) { return (double)m.peaks; }));
        shown.binsAcross = (int)llround(medianOf([](const Measurement& m) { return (double)m.binsAcross; }));

        // Only the frames that actually saw two peaks have a spacing to report.
        medianScratch.clear();
        for (const auto& entry : history) {
            if (entry.second.peaks == 2 && entry.second.peakSpacing > 0.0) {
                medianScratch.push_back(entry.second.peakSpacing);
            }
        }
        if (medianScratch.size() >= history.size() / 2 && !medianScratch.empty()) {
            size_t k = medianScratch.size() / 2;
            std::nth_element(medianScratch.begin(), medianScratch.begin() + k, medianScratch.end());
            shown.peakSpacing = medianScratch[k];
        }
        else {
            shown.peakSpacing = 0.0;
        }

        // Profile category, with a margin either side of each boundary so a ratio
        // sitting on one does not switch the word back and forth.
        if (shown.halfPower > 0.0) {
            double ratio = shown.occupied / shown.halfPower;
            if (profileIdx != 0 && ratio < 1.45) { profileIdx = 0; }
            else if (profileIdx == 0 && ratio > 1.75) { profileIdx = 1; }
            if (profileIdx != 2 && ratio > 4.4) { profileIdx = 2; }
            else if (profileIdx == 2 && ratio < 3.6) { profileIdx = 1; }
        }

        // Same treatment for the resolution warning. A signal sitting near the
        // threshold dips below it for a frame or two all the time, and a warning
        // that appears and vanishes teaches you to ignore it. It takes a real drop
        // to raise it and a clear recovery to clear it.
        if (!coarse && shown.binsAcross < 6) { coarse = true; }
        else if (coarse && shown.binsAcross >= 10) { coarse = false; }

        shownValid = true;
    }

    void tick() {
        double now = ImGui::GetTime();
        if (!enabled || (now - lastPanelDraw) > 1.0) { return; }
        if (frozen) { return; }

        int dataWidth = 0;
        float* data = gui::waterfall.acquireLatestFFT(dataWidth);
        if (data == NULL) { return; }

        double span = gui::waterfall.getViewBandwidth();
        double centre = gui::waterfall.getViewOffset() + gui::waterfall.getCenterFrequency();

        // A pan, a zoom or a window resize means the held bins no longer stand for
        // the frequencies they were collected at.
        if (dataWidth != holdWidth || span != holdSpan || centre != holdCentre) {
            hold.assign(dataWidth, -INFINITY);
            holdWidth = dataWidth;
            holdSpan = span;
            holdCentre = centre;
            centreHistory.clear();
        }

        float decay = (holdSeconds > 0.05f) ? (90.0f / holdSeconds) * ImGui::GetIO().DeltaTime : 1000.0f;
        for (int i = 0; i < dataWidth; i++) {
            float aged = hold[i] - decay;
            hold[i] = (data[i] > aged) ? data[i] : aged;
        }

        // Whether the signal is up right now is a question about this frame, not
        // about the hold, so it is answered before the hold is measured.
        float liveSnr = liveSignalToNoise(data, dataWidth, span, centre);

        // Everything below reads the hold, not the waterfall's own buffer.
        gui::waterfall.releaseLatestFFT();

        updateTiming(now, liveSnr > 6.0f);
        measure(hold.data(), dataWidth, span, centre);

        if (meas.valid) {
            history.push_back(std::make_pair(now, meas));
            centreHistory.push_back(std::make_pair(now, meas.centre));
            while (!centreHistory.empty() && (now - centreHistory.front().first) > 8.0) {
                centreHistory.pop_front();
            }
        }
        settle(now);
    }

    void updateTiming(double now, bool on) {
        if (dutyWindowStart <= 0.0) { dutyWindowStart = now; }
        samplesTotal++;
        if (on) { samplesOn++; }
        // A rolling window, so a signal that stopped ten minutes ago does not hold
        // the percentage down for the rest of the session.
        if ((now - dutyWindowStart) > 20.0) {
            samplesTotal /= 2;
            samplesOn /= 2;
            dutyWindowStart = now - 10.0;
        }

        if (on != wasOn) {
            if (lastEdge > 0.0) {
                double held = now - lastEdge;
                // Flicker on the threshold is noise, not a transmission.
                if (held > 0.03) {
                    std::deque<double>& into = wasOn ? burstLengths : gapLengths;
                    into.push_back(held);
                    while (into.size() > 20) { into.pop_front(); }
                }
            }
            lastEdge = now;
            wasOn = on;
        }
    }

    // Where to look for the signal: around the VFO if there is one, and wide enough
    // to take in a signal broader than the VFO itself.
    void searchWindow(double hzPerBin, double viewStart, double viewCentre, int dataWidth, int& lo, int& hi) {
        double target = viewCentre;
        double searchHalf = (double)dataWidth * hzPerBin / 4.0;
        const std::string vfoName = gui::waterfall.selectedVFO;
        auto vfoIt = gui::waterfall.vfos.find(vfoName);
        if (!vfoName.empty() && vfoIt != gui::waterfall.vfos.end() && vfoIt->second != NULL) {
            target = gui::waterfall.getCenterFrequency() + vfoIt->second->centerOffset;
            searchHalf = std::max<double>(vfoIt->second->bandwidth * 3.0, hzPerBin * 16.0);
        }
        int centreBin = (int)((target - viewStart) / hzPerBin);
        int halfBins = (int)(searchHalf / hzPerBin);
        if (halfBins < 4) { halfBins = 4; }
        lo = std::clamp<int>(centreBin - halfBins, 0, dataWidth - 1);
        hi = std::clamp<int>(centreBin + halfBins, 0, dataWidth - 1);
    }

    // The strongest bin near the VFO against the noise floor, from the live frame.
    float liveSignalToNoise(const float* data, int dataWidth, double span, double viewCentre) {
        if (data == NULL || dataWidth < 32 || span <= 0.0) { return 0.0f; }
        double hzPerBin = span / (double)dataWidth;
        double viewStart = viewCentre - (span / 2.0);

        int lo = 0, hi = dataWidth - 1;
        searchWindow(hzPerBin, viewStart, viewCentre, dataWidth, lo, hi);

        float peak = -INFINITY;
        for (int i = lo; i <= hi; i++) {
            if (data[i] > peak) { peak = data[i]; }
        }
        scratch.assign(data, data + dataWidth);
        std::vector<float>::iterator kth = scratch.begin() + (scratch.size() / 4);
        std::nth_element(scratch.begin(), kth, scratch.end());
        float noise = *kth;
        if (!std::isfinite(peak) || !std::isfinite(noise)) { return 0.0f; }
        return peak - noise;
    }

    void measure(const float* data, int dataWidth, double span, double viewCentre) {
        meas = Measurement();
        if (data == NULL || dataWidth < 32 || span <= 0.0) { return; }

        double hzPerBin = span / (double)dataWidth;
        double viewStart = viewCentre - (span / 2.0);

        int lo = 0, hi = dataWidth - 1;
        searchWindow(hzPerBin, viewStart, viewCentre, dataWidth, lo, hi);
        if (hi - lo < 4) { return; }

        // Noise floor from the quiet quarter of the whole view. Taking it from the
        // search window would measure the signal against itself.
        scratch.assign(data, data + dataWidth);
        std::vector<float>::iterator kth = scratch.begin() + (scratch.size() / 4);
        std::nth_element(scratch.begin(), kth, scratch.end());
        float noise = *kth;
        if (!std::isfinite(noise)) { return; }

        int peakBin = lo;
        float peak = -INFINITY;
        for (int i = lo; i <= hi; i++) {
            if (data[i] > peak) {
                peak = data[i];
                peakBin = i;
            }
        }
        if (!std::isfinite(peak)) { return; }

        meas.valid = true;
        meas.hzPerBin = hzPerBin;
        meas.peakDbfs = peak;
        meas.noiseDbfs = noise;
        meas.snr = peak - noise;
        meas.centre = viewStart + ((double)peakBin * hzPerBin);

        // Stopping at the noise floor as well as at the threshold keeps a weak
        // signal from measuring as the width of the whole search window.
        float floorStop = noise + 3.0f;
        int leftEdge = 0, rightEdge = 0;
        edgesAt(data, dataWidth, peakBin, std::max<float>(peak - 26.0f, floorStop), leftEdge, rightEdge);
        meas.occupied = (double)(rightEdge - leftEdge) * hzPerBin;
        meas.binsAcross = rightEdge - leftEdge;

        int l3 = 0, r3 = 0;
        edgesAt(data, dataWidth, peakBin, std::max<float>(peak - 3.0f, floorStop), l3, r3);
        meas.halfPower = (double)(r3 - l3) * hzPerBin;

        // Everything below describes the shape between the -26 dB edges.
        double sum = 0.0;
        double lowSide = 0.0, highSide = 0.0;
        int count = 0;
        int firstPeakBin = -1, lastPeakBin = -1;
        float peakFloor = peak - 20.0f;
        for (int i = std::max<int>(leftEdge, 1); i <= std::min<int>(rightEdge, dataWidth - 2); i++) {
            sum += data[i];
            count++;
            // Energy either side of the peak, added up in linear terms so a strong
            // bin counts for more than a quiet one.
            double lin = pow(10.0, data[i] / 10.0);
            if (i < peakBin) { lowSide += lin; }
            else if (i > peakBin) { highSide += lin; }

            if (data[i] >= peakFloor && data[i] >= data[i - 1] && data[i] > data[i + 1]) {
                meas.peaks++;
                if (firstPeakBin < 0) { firstPeakBin = i; }
                lastPeakBin = i;
            }
        }
        if (count > 0) { meas.crestDb = peak - (float)(sum / (double)count); }
        if ((lowSide + highSide) > 0.0) { meas.balance = (highSide - lowSide) / (highSide + lowSide); }
        if (meas.peaks == 2 && firstPeakBin >= 0 && lastPeakBin > firstPeakBin) {
            meas.peakSpacing = (double)(lastPeakBin - firstPeakBin) * hzPerBin;
        }
    }

    // Walk out from the peak to the first bin below the given level on each side.
    static void edgesAt(const float* data, int dataWidth, int peakBin, float level, int& left, int& right) {
        left = peakBin;
        while (left > 0 && data[left] >= level) { left--; }
        right = peakBin;
        while (right < dataWidth - 1 && data[right] >= level) { right++; }
    }

    const bandplan::Band_t* bandAt(double freq) {
        if (gui::waterfall.bandplan == NULL) { return NULL; }
        for (const auto& band : gui::waterfall.bandplan->bands) {
            if (freq >= band.start && freq <= band.end) { return &band; }
        }
        return NULL;
    }

    static double average(const std::deque<double>& v) {
        if (v.empty()) { return 0.0; }
        double s = 0.0;
        for (double x : v) { s += x; }
        return s / (double)v.size();
    }

    // Hz per second the peak has moved, or 0 if it is sitting still.
    double drift() const {
        if (centreHistory.size() < 8) { return 0.0; }
        double dt = centreHistory.back().first - centreHistory.front().first;
        if (dt < 2.0) { return 0.0; }
        double moved = centreHistory.back().second - centreHistory.front().second;
        // Movement of less than one bin is the measurement's own resolution.
        if (fabs(moved) < shown.hzPerBin) { return 0.0; }
        return moved / dt;
    }

    static void menuHandler(void* ctx) {
        SignalIDModule* _this = (SignalIDModule*)ctx;
        _this->draw();
    }

    void draw() {
        lastPanelDraw = ImGui::GetTime();

        if (gui::waterfall.selectedVFO.empty()) {
            ImGui::TextDisabled("No VFO. Start a radio and tune to the signal.");
        }

        ImGui::Checkbox(("Freeze##_sigid_freeze_" + name).c_str(), &frozen);
        helpMarker("Stops measuring, so the numbers stay put while you read them.");
        ImGui::SameLine();
        if (ImGui::Button(("Reset##_sigid_reset_" + name).c_str())) { resetAll(); }
        if (ImGui::IsItemHovered()) { ImGui::SetTooltip("Throw away the peak hold and the timing history"); }

        ImGui::LeftLabel("Hold");
        ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x - (32.0f * style::uiScale));
        if (ImGui::SliderFloat(("##_sigid_hold_" + name).c_str(), &holdSeconds, 0.0f, 10.0f, "%.1f s")) {
            config.acquire();
            config.conf["holdSeconds"] = holdSeconds;
            config.release(true);
        }
        helpMarker("How long the loudest thing each frequency has shown stays in the\n"
                   "measurement. Raise it to catch a signal that transmits in short\n"
                   "bursts; set it to zero to measure only what is there right now.\n"
                   "This is about the spectrum, not the readout: the numbers below are\n"
                   "always averaged over two seconds and refreshed four times a second.");

        if (!shownValid) {
            ImGui::Spacing();
            ImGui::TextDisabled("Nothing measured yet");
            return;
        }

        // Every row below is drawn whether or not it has something to say, and says
        // so with a dash when it does not. Rows that came and went as their value
        // crossed a threshold moved everything under them up and down the panel,
        // which made the whole thing unreadable however steady the signal was.

        // ---- Frequency
        sectionHeader("FREQUENCY");
        ImGui::Text("Centre    %s", fmtFreq(shown.centre).c_str());
        if (ImGui::IsItemHovered()) { ImGui::SetTooltip("The peak of the signal, which is not necessarily where the VFO sits"); }

        double drifting = drift();
        if (fabs(drifting) >= 1.0) { ImGui::Text("Drift     %+.0f Hz/s", drifting); }
        else { ImGui::Text("Drift     steady"); }
        if (ImGui::IsItemHovered()) { ImGui::SetTooltip("Movement of the peak over the last few seconds. Anything under one\nbin of the current resolution counts as steady."); }

        // ---- Width and shape
        sectionHeader("WIDTH AND SHAPE");
        ImGui::Text("-26 dB    %s", fmtWidth(shown.occupied).c_str());
        ImGui::Text("-3 dB     %s", fmtWidth(shown.halfPower).c_str());

        const char* profileNames[] = { "flat topped", "rounded", "sharply peaked" };
        if (shown.halfPower > 0.0) {
            ImGui::Text("Profile   %s  (%.1fx)", profileNames[profileIdx], shown.occupied / shown.halfPower);
        }
        else {
            ImGui::Text("Profile   -");
        }
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip("The -26 dB width divided by the -3 dB width. Near 1 means the energy is\n"
                              "spread evenly across the whole band; a large number means most of it is\n"
                              "concentrated in the middle.");
        }

        if (shown.peaks <= 0) {
            ImGui::Text("Peaks     -");
        }
        else if (shown.peaks == 2 && shown.peakSpacing > 0.0) {
            ImGui::Text("Peaks     2, %s apart", fmtWidth(shown.peakSpacing).c_str());
        }
        else {
            ImGui::Text("Peaks     %d", shown.peaks);
        }
        if (ImGui::IsItemHovered()) { ImGui::SetTooltip("Separate maxima within 20 dB of the strongest, across the occupied width"); }

        ImGui::Text("Crest     %.0f dB", shown.crestDb);
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip("How far the peak stands above the average across the occupied width.\n"
                              "A few dB means a flat block of energy; a lot means one dominant spike.");
        }

        if (shown.balance > 0.25) { ImGui::Text("Balance   mostly above the peak"); }
        else if (shown.balance < -0.25) { ImGui::Text("Balance   mostly below the peak"); }
        else { ImGui::Text("Balance   even"); }
        if (ImGui::IsItemHovered()) { ImGui::SetTooltip("Which side of the peak the energy sits on"); }

        // ---- Strength
        sectionHeader("STRENGTH");
        ImGui::Text("Peak      %.0f dBFS", shown.peakDbfs);
        ImGui::Text("Noise     %.0f dBFS", shown.noiseDbfs);
        ImGui::Text("SNR       %.0f dB", shown.snr);

        // ---- Timing
        sectionHeader("TIMING");
        double duty = (samplesTotal > 0) ? ((double)samplesOn / (double)samplesTotal) : 0.0;
        if (samplesTotal < 20) { ImGui::Text("State     listening..."); }
        else if (duty > 0.97) { ImGui::Text("State     on all the time"); }
        else if (duty < 0.02) { ImGui::Text("State     nothing above the noise"); }
        else { ImGui::Text("State     coming and going"); }

        if (samplesTotal >= 20) { ImGui::Text("On        %.0f%% of the time", duty * 100.0); }
        else { ImGui::Text("On        -"); }

        if (!burstLengths.empty()) { ImGui::Text("Bursts    %s", fmtTime(average(burstLengths)).c_str()); }
        else { ImGui::Text("Bursts    -"); }

        if (!gapLengths.empty()) { ImGui::Text("Gaps      %s", fmtTime(average(gapLengths)).c_str()); }
        else { ImGui::Text("Gaps      -"); }

        if (!burstLengths.empty() && !gapLengths.empty()) {
            ImGui::Text("Period    %s", fmtTime(average(burstLengths) + average(gapLengths)).c_str());
        }
        else {
            ImGui::Text("Period    -");
        }
        if (ImGui::IsItemHovered()) { ImGui::SetTooltip("Averaged over the last twenty transmissions"); }

        // ---- Where
        sectionHeader("WHERE");
        const bandplan::Band_t* band = bandAt(shown.centre);
        ImGui::PushTextWrapPos(0.0f);
        if (band != NULL) {
            ImGui::Text("%s", band->name.c_str());
            ImGui::TextDisabled("%s, %s band plan", band->type.c_str(),
                                gui::waterfall.bandplan ? gui::waterfall.bandplan->name.c_str() : "current");
        }
        else if (gui::waterfall.bandplan == NULL) {
            ImGui::TextDisabled("No band plan selected");
            ImGui::TextDisabled(" ");
        }
        else {
            ImGui::TextDisabled("Not in the current band plan");
            ImGui::TextDisabled(" ");
        }
        ImGui::PopTextWrapPos();

        // ---- How much the measurement is worth. One line either way: a warning
        // that wrapped onto three lines moved everything above it every time the
        // zoom changed.
        sectionHeader("RESOLUTION");
        if (coarse) {
            ImGui::TextColored(ImVec4(1.0f, 0.7f, 0.3f, 1.0f), "%d bins, %s each - too coarse",
                               shown.binsAcross, fmtWidth(shown.hzPerBin).c_str());
            if (ImGui::IsItemHovered()) {
                ImGui::SetTooltip("Too few bins across the signal to measure its width or shape. Zoom in,\n"
                                  "or raise the FFT size. Held on until there are comfortably enough again,\n"
                                  "so a momentary dip does not flash a warning at you.");
            }
        }
        else {
            ImGui::TextDisabled("%d bins, %s each", shown.binsAcross, fmtWidth(shown.hzPerBin).c_str());
        }
    }

    std::string name;
    bool enabled = true;
    EventHandler<ImGuiContext*> tickHandler;
};

MOD_EXPORT void _INIT_() {
    config.setPath(std::string(core::getRoot()) + "/signal_id_config.json");
    config.load(json::object());
    config.enableAutoSave();
}

MOD_EXPORT ModuleManager::Instance* _CREATE_INSTANCE_(std::string name) {
    return new SignalIDModule(name);
}

MOD_EXPORT void _DELETE_INSTANCE_(void* instance) {
    delete (SignalIDModule*)instance;
}

MOD_EXPORT void _END_() {
    config.disableAutoSave();
    config.save();
}
