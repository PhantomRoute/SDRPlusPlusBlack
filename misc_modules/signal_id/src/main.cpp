#include <imgui.h>
#include <module.h>
#include <gui/gui.h>
#include <gui/style.h>
#include <gui/widgets/bandplan.h>
#include <gui/tuner.h>
#include <signal_path/signal_path.h>
#include <config.h>
#include <core.h>
#include <algorithm>
#include <cmath>
#include <cstdint>
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
//
// It does say what a measurement can be compared with - a channel spacing, a common
// channel width - because that is what turns a number into something a person can
// think with. Those comparisons are always framed as comparisons, never as answers.

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

    // Channel spacings that transmitters are actually placed on. A signal that lands
    // on one of these did not get there by accident, and knowing which grid it sits
    // on says more about what kind of service it is than its width does. Largest
    // first, so the coarsest grid that fits is the one reported.
    const double RASTER_STEPS[] = { 200000.0, 100000.0, 25000.0, 12500.0, 10000.0,
                                    9000.0, 6250.0, 5000.0, 1000.0 };

    // Which grid the frequency falls on, or 0 for none of them. Needs the bin width,
    // because landing within half a bin of a grid line proves nothing when the bins
    // are wider than the spacing.
    double rasterStep(double freq, double hzPerBin) {
        for (double step : RASTER_STEPS) {
            if (hzPerBin > (step / 4.0)) { continue; }

            // How close counts as on the grid. Two things bound it, and getting the
            // relationship backwards makes the row worse than useless.
            //
            // It cannot be tighter than the measurement: the centre is only known to
            // about a bin, so a bin is the floor - with a small floor of its own for
            // very fine bins, where drift alone moves the peak further than that.
            //
            // It must not be a large slice of the step either, and that is the part
            // that was wrong: the tolerance used to be max(bin, 1% of step), so a
            // coarse FFT widened the window rather than narrowing it. At 250 Hz bins
            // that made the 1 kHz grid match roughly half of all frequencies, and the
            // panel reported it as evidence the transmitter had been put there.
            double tolerance = std::max<double>(hzPerBin, step * 0.002);
            if (tolerance > step * 0.02) { tolerance = step * 0.02; }

            double off = fmod(fabs(freq), step);
            double err = std::min<double>(off, step - off);
            if (err <= tolerance) { return step; }
        }
        return 0.0;
    }

    // Something to hold the measured width up against. These are channel plans, not
    // signals: several unrelated things share each one, which is exactly why this is
    // offered as a comparison and never as an answer.
    struct Yardstick {
        double hz;
        const char* what;
    };
    const Yardstick YARDSTICKS[] = {
        { 500.0, "a CW / narrow data channel" },
        { 2800.0, "an SSB voice channel" },
        { 6250.0, "a 6.25 kHz narrow channel" },
        { 9000.0, "an AM broadcast channel (9 kHz)" },
        { 10000.0, "an AM broadcast channel (10 kHz)" },
        { 12500.0, "a 12.5 kHz FM channel" },
        { 25000.0, "a 25 kHz FM channel" },
        { 200000.0, "an FM broadcast channel" },
        { 1250000.0, "a DAB block" },
        { 5000000.0, "a TV / wideband data channel" },
    };

    const char* yardstick(double hz) {
        for (const auto& y : YARDSTICKS) {
            if (hz > (y.hz * 0.8) && hz < (y.hz * 1.2)) { return y.what; }
        }
        return NULL;
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
        if (config.conf.contains("resetOnRetune") && config.conf["resetOnRetune"].is_boolean()) {
            resetOnRetune = config.conf["resetOnRetune"];
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
        // The edges as absolute frequencies, not just the widths. A signal is rarely
        // symmetric about its peak, and the sketch has to draw them where they are.
        double occLeft = 0.0, occRight = 0.0;
        double hpLeft = 0.0, hpRight = 0.0;
        double hzPerBin = 0.0;
        int binsAcross = 0;
        float peakDbfs = 0.0f;
        float noiseDbfs = 0.0f;
        float snr = 0.0f;
        int peaks = 0;            // separate maxima across the occupied width
        double peakSpacing = 0.0; // average gap between them, when there are two or more
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

    // Whether retuning starts the timing history over. On is right when the panel is
    // being used to characterise one signal; off is right when it is being swept
    // along a band as a readout, where starting over at every step means the timing
    // rows never fill in at all.
    bool resetOnRetune = true;

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
    // When the signal came up, kept so the gap between one transmission and the next
    // can be measured directly rather than added up from a burst and a gap. Whether
    // those gaps are all the same length is most of what separates a machine keeping
    // to a schedule from someone talking.
    std::deque<double> onsetTimes;
    int samplesOn = 0;
    int samplesTotal = 0;
    double dutyWindowStart = 0.0;

    // When the signal last went up or down. The burst and gap lengths are durations
    // with no timestamps attached, so this is what says whether they are still
    // describing something that is happening.
    double lastTransition = 0.0;
    // After this long with no transition at all, they are not. Generous, because a
    // beacon on a one minute cycle is a real thing and its numbers should not be
    // thrown away between overs.
    static constexpr double TIMING_EXPIRY = 90.0;

    // ---- The last ten seconds, one entry per frame. Where the peak was answers how
    // far it has drifted; how much the level and the width moved over that span
    // answers two questions the instantaneous numbers cannot - whether the signal is
    // fading, and whether its width breathes with the modulation or sits still.
    struct Trend {
        double time = 0.0;
        double centre = 0.0;
        float level = 0.0f;
        double width = 0.0;
    };
    static constexpr double TREND_WINDOW = 10.0;
    std::deque<Trend> trend;

    // ---- A picture of the signal, which says in one glance what a column of
    // numbers takes a while to say: the shape, where the edges were measured, how far
    // above the noise it stands and whether the VFO is sitting across it.
    std::vector<float> sketch;    // one dB value per column, left to right
    double sketchLo = 0.0;        // frequency at the left edge of the sketch
    double sketchHi = 0.0;        // and at the right
    double lastSketch = 0.0;

    // ---- What the readings are of. Everything above accumulates over seconds to
    // minutes, and none of it means anything once the VFO has been moved to a
    // different signal.
    std::string measuredVfo;
    double measuredFreq = 0.0;

    // When a measurement last succeeded. The panel deliberately keeps showing the
    // last set rather than blanking between transmissions, so without this there is
    // nothing to say whether what is on screen is a second old or ten minutes.
    double lastValidTime = 0.0;

    // What was true of the signal under the VFO: how often it transmitted, for how
    // long, how far it drifted. Moving the VFO makes all of it wrong.
    //
    // Note what is *not* here. The peak hold is anchored to the view, not to the VFO,
    // so sliding the VFO across a band it has already collected takes nothing away
    // from it - which is the whole point of the picture. Nor is `shown` cleared: the
    // last reading stays up while the next one settles, so the panel does not empty
    // itself for a quarter of a second every time a digit is nudged.
    void forgetSignalHistory() {
        burstLengths.clear();
        gapLengths.clear();
        onsetTimes.clear();
        trend.clear();
        history.clear();
        samplesOn = 0;
        samplesTotal = 0;
        wasOn = false;
        lastEdge = 0.0;
        lastTransition = 0.0;
        dutyWindowStart = ImGui::GetTime();
    }

    // The Reset button: that, and the picture with it.
    void resetAll() {
        forgetSignalHistory();
        hold.clear();
        holdWidth = 0;
        sketch.clear();
        shownValid = false;
        lastValidTime = 0.0;
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
        shown.occLeft = medianOf([](const Measurement& m) { return m.occLeft; });
        shown.occRight = medianOf([](const Measurement& m) { return m.occRight; });
        shown.hpLeft = medianOf([](const Measurement& m) { return m.hpLeft; });
        shown.hpRight = medianOf([](const Measurement& m) { return m.hpRight; });
        shown.peakDbfs = (float)medianOf([](const Measurement& m) { return (double)m.peakDbfs; });
        shown.noiseDbfs = (float)medianOf([](const Measurement& m) { return (double)m.noiseDbfs; });
        shown.snr = (float)medianOf([](const Measurement& m) { return (double)m.snr; });
        shown.crestDb = (float)medianOf([](const Measurement& m) { return (double)m.crestDb; });
        shown.balance = medianOf([](const Measurement& m) { return m.balance; });
        shown.peaks = (int)llround(medianOf([](const Measurement& m) { return (double)m.peaks; }));
        shown.binsAcross = (int)llround(medianOf([](const Measurement& m) { return (double)m.binsAcross; }));

        // Only the frames that actually saw more than one peak have a spacing to
        // report.
        medianScratch.clear();
        for (const auto& entry : history) {
            if (entry.second.peaks >= 2 && entry.second.peakSpacing > 0.0) {
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

        // Tuning away makes the accumulated history wrong: the burst lengths, the
        // gaps, the rhythm and the duty cycle are all about one signal, and tuning
        // off a busy repeater onto a dead channel would otherwise leave the panel
        // reporting the repeater's traffic on a frequency where nothing had
        // transmitted at all.
        //
        // Only that history goes. The peak hold, the picture and the last set of
        // numbers stay, because they are about the piece of spectrum on screen and
        // moving the VFO across it does not make any of them untrue.
        {
            double vfoFreq = 0.0, vfoBw = 0.0;
            ImU32 vfoColor = 0;
            if (vfoInfo(vfoFreq, vfoBw, vfoColor)) {
                double moved = fabs(vfoFreq - measuredFreq);
                // Half a channel width, so nudging about inside the signal - or the
                // Tune to it button below - is not a move.
                bool retuned = (gui::waterfall.selectedVFO != measuredVfo) ||
                               (moved > std::max<double>(vfoBw * 0.5, span / (double)dataWidth));
                if (retuned) {
                    // measuredFreq is where this history was started, not where the
                    // VFO was last frame, so crossing a band in small steps adds up
                    // to a move instead of never quite reaching the threshold.
                    measuredVfo = gui::waterfall.selectedVFO;
                    measuredFreq = vfoFreq;
                    if (resetOnRetune) { forgetSignalHistory(); }
                }
            }
        }

        // A pan, a zoom or a window resize means the held bins no longer stand for
        // the frequencies they were collected at.
        if (dataWidth != holdWidth || span != holdSpan || centre != holdCentre) {
            hold.assign(dataWidth, -INFINITY);
            holdWidth = dataWidth;
            holdSpan = span;
            holdCentre = centre;
            trend.clear();
            // The settling window too: those measurements were taken on a different
            // bin grid, and averaging them with the new ones gave a width that was
            // neither one thing nor the other for two seconds after every zoom.
            history.clear();
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
            lastValidTime = now;
            history.push_back(std::make_pair(now, meas));

            Trend t;
            t.time = now;
            t.centre = meas.centre;
            t.level = meas.peakDbfs;
            t.width = meas.occupied;
            trend.push_back(t);
            while (!trend.empty() && (now - trend.front().time) > TREND_WINDOW) {
                trend.pop_front();
            }
        }
        settle(now);

        // Ten times a second. Fast enough to look live, slow enough that the trace
        // can be read rather than watched.
        if ((now - lastSketch) > 0.1) {
            lastSketch = now;
            captureSketch(hold.data(), dataWidth, span / (double)dataWidth, centre - (span / 2.0));
        }
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
                    lastTransition = now;
                    if (on) {
                        onsetTimes.push_back(now);
                        while (onsetTimes.size() > 20) { onsetTimes.pop_front(); }
                    }
                }
            }
            lastEdge = now;
            wasOn = on;
        }
    }

    // The gap from one transmission starting to the next, and how much that gap
    // varies. Returns false until there are enough of them to say anything.
    // Whether the burst and gap history still describes something that is happening.
    // Without this the timing rows go on quoting the last twenty transmissions of a
    // beacon that stopped an hour ago, directly under a State row saying there is
    // nothing above the noise.
    bool timingExpired() const {
        return lastTransition > 0.0 && (ImGui::GetTime() - lastTransition) > TIMING_EXPIRY;
    }

    bool rhythm(double& period, double& spread) const {
        if (timingExpired()) { return false; }
        if (onsetTimes.size() < 4) { return false; }
        double sum = 0.0;
        int count = 0;
        for (size_t i = 1; i < onsetTimes.size(); i++) {
            sum += onsetTimes[i] - onsetTimes[i - 1];
            count++;
        }
        period = sum / (double)count;
        if (period <= 0.0) { return false; }

        double var = 0.0;
        for (size_t i = 1; i < onsetTimes.size(); i++) {
            double d = (onsetTimes[i] - onsetTimes[i - 1]) - period;
            var += d * d;
        }
        // As a fraction of the period, so "regular" means the same thing for
        // something every two seconds and something every two minutes.
        spread = sqrt(var / (double)count) / period;
        return true;
    }

    // Copies the held spectrum around the signal into the sketch, over a window three
    // times the occupied width so there is some context either side of it. Taken from
    // the settled width rather than this frame's, so the window does not breathe in
    // and out while the trace inside it moves.
    void captureSketch(const float* data, int dataWidth, double hzPerBin, double viewStart) {
        // Nothing measurable means nothing to draw. Returning without clearing left
        // the last good trace on screen, so a signal that had stopped went on being
        // pictured under a set of numbers that said it was gone.
        if (!shownValid || shown.occupied <= 0.0) {
            sketch.clear();
            return;
        }

        double half = std::max<double>(shown.occupied * 1.5, hzPerBin * 12.0);
        double lo = shown.centre - half;
        double hi = shown.centre + half;
        int loBin = (int)floor((lo - viewStart) / hzPerBin);
        int hiBin = (int)ceil((hi - viewStart) / hzPerBin);
        loBin = std::clamp<int>(loBin, 0, dataWidth - 1);
        hiBin = std::clamp<int>(hiBin, 0, dataWidth - 1);
        // Same reasoning as the early return above, and it was missed here: pan or
        // zoom until the signal is off the edge of the view and both bins clamp to
        // the same end, leaving nothing to draw. Returning without clearing kept the
        // last good trace on screen against a sketchLo/sketchHi that no longer
        // describe anything, so the picture and its markers disagreed with each other
        // and with the view.
        if ((hiBin - loBin) < 4) {
            sketch.clear();
            return;
        }

        // At most this many columns, taking the loudest bin in each. A max rather
        // than an average, so a narrow carrier inside a wide window does not get
        // averaged down into nothing.
        const int MAX_COLUMNS = 160;
        int span = hiBin - loBin + 1;
        int columns = std::min<int>(span, MAX_COLUMNS);
        sketch.assign(columns, -INFINITY);
        for (int i = 0; i < span; i++) {
            int col = (int)((int64_t)i * columns / span);
            if (col >= columns) { col = columns - 1; }
            float v = data[loBin + i];
            if (v > sketch[col]) { sketch[col] = v; }
        }
        // Every column should have caught at least one bin, since there are never
        // more columns than bins. Belt and braces: one -INFINITY reaching the drawing
        // code turns into a NaN coordinate and takes the whole trace with it.
        for (int i = 1; i < columns; i++) {
            if (!std::isfinite(sketch[i])) { sketch[i] = sketch[i - 1]; }
        }
        if (!std::isfinite(sketch[0])) { sketch[0] = shown.noiseDbfs; }
        sketchLo = viewStart + ((double)loBin * hzPerBin);
        sketchHi = viewStart + ((double)hiBin * hzPerBin);
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

    // The level a quarter of the bins are below, which is as good a noise floor as
    // the spectrum alone can give. Bins from skipFrom to skipTo inclusive are left
    // out; pass an empty range (0, -1) to use all of them. Returns NaN when too few
    // bins are left to mean anything.
    float quietQuarter(const float* data, int dataWidth, int skipFrom, int skipTo) {
        scratch.clear();
        scratch.reserve(dataWidth);
        for (int i = 0; i < dataWidth; i++) {
            if (i >= skipFrom && i <= skipTo) { continue; }
            scratch.push_back(data[i]);
        }
        if (scratch.size() < 16) { return NAN; }
        std::vector<float>::iterator kth = scratch.begin() + (scratch.size() / 4);
        std::nth_element(scratch.begin(), kth, scratch.end());
        return *kth;
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
        // The search window is left out of the floor for the same reason measure()
        // leaves the signal out of its own: this is what decides whether the signal
        // is on, and a floor that rises along with it never lets it read as on.
        float noise = quietQuarter(data, dataWidth, lo, hi);
        if (!std::isfinite(noise)) { noise = quietQuarter(data, dataWidth, 0, -1); }
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
        float noise = quietQuarter(data, dataWidth, 0, -1);
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

        // Stopping at the noise floor as well as at the threshold keeps a weak
        // signal from measuring as the width of the whole search window.
        int leftEdge = 0, rightEdge = 0;
        edgesAt(data, dataWidth, peakBin, std::max<float>(peak - 26.0f, noise + 3.0f), leftEdge, rightEdge);

        // Now that the signal's own bins are known, take the floor again without
        // them. "The quiet quarter of the view" is only the noise while the signal
        // is a small part of what is on screen - and this panel spends its life
        // being told to zoom in, at which point the signal is most of the screen and
        // the floor it is being measured against is largely itself. That reads as a
        // signal 10 or 20 dB weaker than it is, and drags the -26 dB edges in with
        // it. Only ever accepted if it lowers the floor, since leaving bins out
        // cannot honestly raise it.
        float clean = quietQuarter(data, dataWidth, leftEdge, rightEdge);
        if (std::isfinite(clean) && clean < noise) {
            noise = clean;
            edgesAt(data, dataWidth, peakBin, std::max<float>(peak - 26.0f, noise + 3.0f), leftEdge, rightEdge);
        }

        meas.valid = true;
        meas.hzPerBin = hzPerBin;
        meas.peakDbfs = peak;
        meas.noiseDbfs = noise;
        meas.snr = peak - noise;
        meas.centre = viewStart + ((double)peakBin * hzPerBin);

        float floorStop = noise + 3.0f;
        meas.occupied = (double)(rightEdge - leftEdge) * hzPerBin;
        meas.binsAcross = rightEdge - leftEdge;
        meas.occLeft = viewStart + ((double)leftEdge * hzPerBin);
        meas.occRight = viewStart + ((double)rightEdge * hzPerBin);

        int l3 = 0, r3 = 0;
        edgesAt(data, dataWidth, peakBin, std::max<float>(peak - 3.0f, floorStop), l3, r3);
        meas.halfPower = (double)(r3 - l3) * hzPerBin;
        meas.hpLeft = viewStart + ((double)l3 * hzPerBin);
        meas.hpRight = viewStart + ((double)r3 * hzPerBin);

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
        // The average gap, not the total span, so a row of evenly spaced tones reads
        // as one number however many of them there are. Two peaks is the same
        // calculation with a divisor of one.
        if (meas.peaks >= 2 && firstPeakBin >= 0 && lastPeakBin > firstPeakBin) {
            meas.peakSpacing = (double)(lastPeakBin - firstPeakBin) * hzPerBin / (double)(meas.peaks - 1);
        }
    }

    // Walk out from the peak to the first bin below the given level on each side.
    static void edgesAt(const float* data, int dataWidth, int peakBin, float level, int& left, int& right) {
        left = peakBin;
        while (left > 0 && data[left] >= level) { left--; }
        right = peakBin;
        while (right < dataWidth - 1 && data[right] >= level) { right++; }
    }

    static double average(const std::deque<double>& v) {
        if (v.empty()) { return 0.0; }
        double s = 0.0;
        for (double x : v) { s += x; }
        return s / (double)v.size();
    }

    // Hz per second the peak has moved, or 0 if it is sitting still.
    double drift() const {
        if (trend.size() < 8) { return 0.0; }
        double dt = trend.back().time - trend.front().time;
        if (dt < 2.0) { return 0.0; }
        double moved = trend.back().centre - trend.front().centre;
        // Movement of less than one bin is the measurement's own resolution.
        if (fabs(moved) < shown.hzPerBin) { return 0.0; }
        return moved / dt;
    }

    // The spread of one field across the trend window, as the gap between its tenth
    // and ninetieth percentile.
    //
    // Not the plain highest minus lowest. Over ten seconds this is several hundred
    // per-frame measurements, and a single frame caught mid-transition sets the whole
    // range on its own - which is exactly the noise the rest of the panel goes to
    // some trouble to median away. Throwing away the top and bottom tenth leaves a
    // number that says how much the signal actually moved.
    mutable std::vector<double> swingScratch;
    template <class Get>
    bool swingOf(Get get, double& spread) const {
        if (trend.size() < 20) { return false; }
        if ((trend.back().time - trend.front().time) < 3.0) { return false; }

        swingScratch.clear();
        swingScratch.reserve(trend.size());
        for (const auto& t : trend) { swingScratch.push_back(get(t)); }

        size_t lo = swingScratch.size() / 10;
        size_t hi = swingScratch.size() - 1 - lo;
        std::nth_element(swingScratch.begin(), swingScratch.begin() + lo, swingScratch.end());
        double low = swingScratch[lo];
        std::nth_element(swingScratch.begin(), swingScratch.begin() + hi, swingScratch.end());
        spread = swingScratch[hi] - low;
        return true;
    }

    // How far the peak level rose and fell. One of the few things measurable here
    // that says something about the path the signal took rather than about the
    // transmitter that sent it.
    bool levelSwing(double& db) const {
        return swingOf([](const Trend& t) { return (double)t.level; }, db);
    }

    // The same for the occupied width. A width that moves belongs to a transmitter
    // whose bandwidth follows what is being sent; a width that sits still over ten
    // seconds belongs to one sending at a constant rate.
    bool widthSwing(double& hz) const {
        return swingOf([](const Trend& t) { return t.width; }, hz);
    }

    // The selected VFO's centre and width, so the panel can say where the signal sits
    // relative to what is actually being listened to. False when no radio is running.
    bool vfoInfo(double& freq, double& bw, ImU32& color) const {
        const std::string& vfoName = gui::waterfall.selectedVFO;
        if (vfoName.empty()) { return false; }
        auto it = gui::waterfall.vfos.find(vfoName);
        if (it == gui::waterfall.vfos.end() || it->second == NULL) { return false; }
        freq = gui::waterfall.getCenterFrequency() + it->second->centerOffset;
        bw = it->second->bandwidth;
        color = it->second->color;
        return true;
    }

    // How the transmissions are spaced, in words. Kept apart from the drawing so the
    // clipboard summary says exactly what the panel says.
    std::string rhythmText() const {
        double period = 0.0, spread = 0.0;
        if (!rhythm(period, spread)) { return "-"; }
        char buf[96];
        // A tenth either way is about as steady as a hand-measured burst edge gets,
        // so anything inside that is called regular rather than given a number.
        if (spread < 0.1) { snprintf(buf, sizeof buf, "regular, every %s", fmtTime(period).c_str()); }
        else if (spread < 0.35) { snprintf(buf, sizeof buf, "roughly every %s", fmtTime(period).c_str()); }
        else { snprintf(buf, sizeof buf, "irregular, %s on average", fmtTime(period).c_str()); }
        return std::string(buf);
    }

    const char* stateText() const {
        if (samplesTotal < 20) { return "listening..."; }
        double duty = (double)samplesOn / (double)samplesTotal;
        if (duty > 0.97) { return "on all the time"; }
        if (duty < 0.02) { return "nothing above the noise"; }
        return "coming and going";
    }

    // How long since anything could be measured. The panel keeps the last reading on
    // screen between transmissions rather than blinking, so this is what stops that
    // kindness turning into a lie.
    double stale() const {
        if (lastValidTime <= 0.0) { return 0.0; }
        return ImGui::GetTime() - lastValidTime;
    }

    // The one sentence a beginner needs before any of the numbers mean anything.
    std::string headline() const {
        char buf[128];
        double old = stale();
        if (old > 2.0) {
            snprintf(buf, sizeof buf, "Nothing to measure here now. Everything below is %s old.",
                     fmtTime(old).c_str());
            return std::string(buf);
        }
        // A signal that never got 3 dB clear of the noise has no width to report, and
        // "0 Hz wide" reads like a measurement rather than the absence of one.
        if (shown.occupied <= 0.0) {
            snprintf(buf, sizeof buf, "Nothing standing clear of the noise - only %.0f dB above it.", shown.snr);
            return std::string(buf);
        }
        std::string s = fmtWidth(shown.occupied) + " wide";
        if (samplesTotal >= 20) { s += ", "; s += stateText(); }
        snprintf(buf, sizeof buf, ", %.0f dB over the noise", shown.snr);
        s += buf;
        return s;
    }

    // Everything on screen as plain text, for pasting into a post asking what it is.
    std::string summaryText() const {
        char buf[256];
        std::string s = "Signal at " + fmtFreq(shown.centre) + "\n";
        s += "  " + headline() + "\n";
        s += "  Occupied (-26 dB)  " + fmtWidth(shown.occupied) + "\n";
        s += "  Half power (-3 dB) " + fmtWidth(shown.halfPower) + "\n";
        snprintf(buf, sizeof buf, "  Peak %.0f dBFS, noise %.0f dBFS, SNR %.0f dB\n",
                 shown.peakDbfs, shown.noiseDbfs, shown.snr);
        s += buf;
        snprintf(buf, sizeof buf, "  Crest %.0f dB, %d peak(s)", shown.crestDb, shown.peaks);
        s += buf;
        if (shown.peaks >= 2 && shown.peakSpacing > 0.0) { s += ", " + fmtWidth(shown.peakSpacing) + " apart"; }
        s += "\n";

        double moved = 0.0;
        if (widthSwing(moved)) { s += "  Width moved " + fmtWidth(moved) + " over ten seconds\n"; }
        if (levelSwing(moved)) {
            snprintf(buf, sizeof buf, "  Level swung %.0f dB over ten seconds\n", moved);
            s += buf;
        }
        double drifting = drift();
        if (fabs(drifting) >= 1.0) {
            snprintf(buf, sizeof buf, "  Drifting %+.0f Hz/s\n", drifting);
            s += buf;
        }
        s += "  Rhythm: " + rhythmText() + "\n";
        if (samplesTotal >= 20) {
            snprintf(buf, sizeof buf, "  On %.0f%% of the time\n",
                     ((double)samplesOn / (double)samplesTotal) * 100.0);
            s += buf;
        }
        double step = rasterStep(shown.centre, shown.hzPerBin);
        if (step > 0.0) { s += "  Lands on " + fmtWidth(step) + " channel steps\n"; }
        const bandplan::Band_t* band = bandAt(shown.centre);
        if (band != NULL) { s += "  Band plan: " + band->name + " (" + band->type + ")\n"; }
        snprintf(buf, sizeof buf, "  Measured over %d bins of %s\n", shown.binsAcross,
                 fmtWidth(shown.hzPerBin).c_str());
        s += buf;
        return s;
    }

    const bandplan::Band_t* bandAt(double freq) const {
        if (gui::waterfall.bandplan == NULL) { return NULL; }
        for (const auto& band : gui::waterfall.bandplan->bands) {
            if (freq >= band.start && freq <= band.end) { return &band; }
        }
        return NULL;
    }

    // A small picture of the held spectrum around the signal, with the two widths
    // marked where they were measured and the VFO drawn over the top. Reading "6.2
    // kHz at -3 dB, 14.9 kHz at -26 dB" off two rows of text takes a moment; seeing
    // a narrow spike with wide skirts takes none.
    void drawSketch() {
        float height = ImGui::GetTextLineHeight() * 4.0f;
        float width = ImGui::GetContentRegionAvail().x;
        if (width < 32.0f) { return; }
        ImVec2 boxMin = ImGui::GetCursorScreenPos();
        ImVec2 boxMax = ImVec2(boxMin.x + width, boxMin.y + height);
        ImDrawList* dl = ImGui::GetWindowDrawList();

        // The box is drawn even with nothing to put in it, so the rest of the panel
        // does not slide up and down as the signal comes and goes.
        bool haveTrace = (sketch.size() >= 2) && (sketchHi > sketchLo);
        if (!haveTrace) {
            dl->AddRectFilled(boxMin, boxMax, ImGui::GetColorU32(ImGuiCol_FrameBg));
            dl->AddRect(boxMin, boxMax, ImGui::GetColorU32(ImGuiCol_Border));
            ImGui::Dummy(ImVec2(width, height));
            return;
        }

        // Scaled between the noise floor and the peak, not between fixed limits, so
        // a weak signal fills the box just as a strong one does.
        float top = shown.peakDbfs + 3.0f;
        float bottom = shown.noiseDbfs - 5.0f;
        if ((top - bottom) < 10.0f) { top = bottom + 10.0f; }

        auto yOf = [&](float db) {
            float t = (db - bottom) / (top - bottom);
            t = std::clamp<float>(t, 0.0f, 1.0f);
            return boxMax.y - (t * height);
        };
        auto xOf = [&](double hz) {
            double t = (hz - sketchLo) / (sketchHi - sketchLo);
            t = std::clamp<double>(t, 0.0, 1.0);
            return (float)(boxMin.x + (t * width));
        };

        dl->AddRectFilled(boxMin, boxMax, ImGui::GetColorU32(ImGuiCol_FrameBg));
        dl->PushClipRect(boxMin, boxMax, true);

        // The VFO first, underneath everything, so it reads as a background band
        // rather than another feature of the signal.
        double vfoFreq = 0.0, vfoBw = 0.0;
        ImU32 vfoColor = 0;
        if (vfoInfo(vfoFreq, vfoBw, vfoColor) && vfoBw > 0.0) {
            dl->AddRectFilled(ImVec2(xOf(vfoFreq - (vfoBw / 2.0)), boxMin.y),
                              ImVec2(xOf(vfoFreq + (vfoBw / 2.0)), boxMax.y), vfoColor);
        }

        // Where the widths were taken from, as the levels they were taken at.
        ImU32 edgeColor = ImGui::GetColorU32(ImGuiCol_TextDisabled);
        if (shown.occRight > shown.occLeft) {
            float y = yOf(shown.peakDbfs - 26.0f);
            dl->AddLine(ImVec2(xOf(shown.occLeft), y), ImVec2(xOf(shown.occRight), y), edgeColor, style::uiScale);
            dl->AddLine(ImVec2(xOf(shown.occLeft), y - (3.0f * style::uiScale)),
                        ImVec2(xOf(shown.occLeft), y + (3.0f * style::uiScale)), edgeColor, style::uiScale);
            dl->AddLine(ImVec2(xOf(shown.occRight), y - (3.0f * style::uiScale)),
                        ImVec2(xOf(shown.occRight), y + (3.0f * style::uiScale)), edgeColor, style::uiScale);
        }
        if (shown.hpRight > shown.hpLeft) {
            float y = yOf(shown.peakDbfs - 3.0f);
            dl->AddLine(ImVec2(xOf(shown.hpLeft), y), ImVec2(xOf(shown.hpRight), y), edgeColor, style::uiScale);
        }

        // The noise floor, so how far the signal stands above it is visible and not
        // just a number in the STRENGTH section.
        float noiseY = yOf(shown.noiseDbfs);
        dl->AddLine(ImVec2(boxMin.x, noiseY), ImVec2(boxMax.x, noiseY), edgeColor, style::uiScale);

        // The trace itself, last and brightest.
        ImU32 traceColor = ImGui::GetColorU32(ImGuiCol_PlotLines);
        int columns = (int)sketch.size();
        float step = width / (float)(columns - 1);
        for (int i = 0; i + 1 < columns; i++) {
            dl->AddLine(ImVec2(boxMin.x + (step * i), yOf(sketch[i])),
                        ImVec2(boxMin.x + (step * (i + 1)), yOf(sketch[i + 1])),
                        traceColor, 1.5f * style::uiScale);
        }

        dl->PopClipRect();
        dl->AddRect(boxMin, boxMax, ImGui::GetColorU32(ImGuiCol_Border));

        ImGui::Dummy(ImVec2(width, height));
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip("The peak hold around the signal, %s across.\n"
                              "The long line is the noise floor, the two short ones are the -3 dB and\n"
                              "-26 dB widths where they were measured, and the shaded band is the VFO.",
                              fmtWidth(sketchHi - sketchLo).c_str());
        }
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
        ImGui::HelpMarker("Stops measuring, so the numbers stay put while you read them.");
        ImGui::SameLine();
        if (ImGui::Button(("Reset##_sigid_reset_" + name).c_str())) { resetAll(); }
        if (ImGui::IsItemHovered()) { ImGui::SetTooltip("Throw away the peak hold and the timing history"); }
        ImGui::SameLine();
        if (!shownValid) { style::beginDisabled(); }
        if (ImGui::Button(("Copy##_sigid_copy_" + name).c_str()) && shownValid) {
            ImGui::SetClipboardText(summaryText().c_str());
        }
        if (!shownValid) { style::endDisabled(); }
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
            ImGui::SetTooltip("Copy everything below as text, for pasting somewhere you can ask about it");
        }

        ImGui::LeftLabel("Hold");
        ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x - (32.0f * style::uiScale));
        if (ImGui::SliderFloat(("##_sigid_hold_" + name).c_str(), &holdSeconds, 0.0f, 10.0f, "%.1f s")) {
            config.acquire();
            config.conf["holdSeconds"] = holdSeconds;
            config.release(true);
        }
        ImGui::HelpMarker("How long the loudest thing each frequency has shown stays in the\n"
                          "measurement. Raise it to catch a signal that transmits in short\n"
                          "bursts; set it to zero to measure only what is there right now.\n"
                          "This is about the spectrum, not the readout: the numbers below are\n"
                          "always averaged over two seconds and refreshed four times a second.");

        if (ImGui::Checkbox(("Start the timing over when I retune##_sigid_retune_" + name).c_str(), &resetOnRetune)) {
            config.acquire();
            config.conf["resetOnRetune"] = resetOnRetune;
            config.release(true);
        }
        ImGui::HelpMarker("The TIMING rows build up over a minute or two, and they describe whichever\n"
                          "signal was under the VFO while they did. On, moving the VFO more than half\n"
                          "a channel starts them over, so they always describe what you are pointing\n"
                          "at. Off, they keep counting across a retune, which is what you want when\n"
                          "you are stepping along a band and reading the panel as you go.\n"
                          "Either way the picture and the peak hold are left alone - those are about\n"
                          "the spectrum on screen, not about the VFO.");

        if (!shownValid) {
            ImGui::Spacing();
            ImGui::TextDisabled("Nothing measured yet");
            return;
        }

        // The picture and the one sentence version go above everything, because for
        // most of the questions people bring to this panel they are the whole answer
        // and the sections below are only there to check it.
        ImGui::Spacing();
        drawSketch();
        ImGui::PushTextWrapPos(0.0f);
        ImGui::TextUnformatted(headline().c_str());
        ImGui::PopTextWrapPos();

        // Every row below is drawn whether or not it has something to say, and says
        // so with a dash when it does not. Rows that came and went as their value
        // crossed a threshold moved everything under them up and down the panel,
        // which made the whole thing unreadable however steady the signal was.

        // ---- Frequency
        ImGui::SectionHeader("FREQUENCY");
        ImGui::Text("Centre    %s", fmtFreq(shown.centre).c_str());
        if (ImGui::IsItemHovered()) { ImGui::SetTooltip("The peak of the signal, which is not necessarily where the VFO sits"); }

        // Where it is compared with what is being listened to. Being half a channel
        // off is the commonest reason a signal that is plainly there sounds wrong,
        // and nothing in the panel used to say so.
        double vfoFreq = 0.0, vfoBw = 0.0;
        ImU32 vfoColor = 0;
        bool haveVfo = vfoInfo(vfoFreq, vfoBw, vfoColor);
        if (!haveVfo) {
            ImGui::Text("Offset    -");
        }
        else {
            double off = shown.centre - vfoFreq;
            if (fabs(off) < std::max<double>(shown.hzPerBin, 1.0)) { ImGui::Text("Offset    centred on the VFO"); }
            else { ImGui::Text("Offset    %s%s from the VFO", (off > 0.0) ? "+" : "-", fmtWidth(fabs(off)).c_str()); }
        }
        if (ImGui::IsItemHovered()) { ImGui::SetTooltip("How far the peak sits from the middle of the channel you are listening to"); }

        double drifting = drift();
        if (fabs(drifting) >= 1.0) { ImGui::Text("Drift     %+.0f Hz/s", drifting); }
        else { ImGui::Text("Drift     steady"); }
        if (ImGui::IsItemHovered()) { ImGui::SetTooltip("Movement of the peak over the last few seconds. Anything under one\nbin of the current resolution counts as steady."); }

        double step = rasterStep(shown.centre, shown.hzPerBin);
        if (step > 0.0) { ImGui::Text("Channels  on the %s grid", fmtWidth(step).c_str()); }
        else { ImGui::Text("Channels  not on a common grid"); }
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip("Whether the centre lands on one of the spacings transmitters are usually\n"
                              "assigned on - 25, 12.5, 9, 5 kHz and so on. The widest one it fits is the\n"
                              "one shown, so any finer spacing that divides into it fits as well.\n"
                              "Sitting on a grid means it was put there; sitting between them is normal\n"
                              "for anything without a carrier, and for anything the resolution at the\n"
                              "bottom of this panel is too coarse to place.");
        }

        // Acting on the measurement, rather than reading it off and doing it by hand.
        if (!haveVfo) { style::beginDisabled(); }
        float half = (ImGui::GetContentRegionAvail().x - ImGui::GetStyle().ItemSpacing.x) / 2.0f;
        if (ImGui::Button(("Tune to it##_sigid_tune_" + name).c_str(), ImVec2(half, 0)) && haveVfo) {
            tuner::tune(tuner::TUNER_MODE_NORMAL, gui::waterfall.selectedVFO, shown.centre);
        }
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
            ImGui::SetTooltip("Put the VFO on the measured centre");
        }
        ImGui::SameLine();
        if (ImGui::Button(("Match width##_sigid_width_" + name).c_str(), ImVec2(ImGui::GetContentRegionAvail().x, 0)) &&
            haveVfo && shown.occupied > 0.0) {
            auto it = gui::waterfall.vfos.find(gui::waterfall.selectedVFO);
            if (it != gui::waterfall.vfos.end() && it->second != NULL && !it->second->bandwidthLocked) {
                double bw = std::clamp<double>(shown.occupied, it->second->minBandwidth, it->second->maxBandwidth);
                // Set it and announce it the same way dragging the VFO edge does, so
                // the radio hears about it and stores it against the current mode.
                it->second->setBandwidth(bw);
                it->second->onUserChangedBandwidth.emit(bw);
            }
        }
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
            ImGui::SetTooltip("Set the channel width to the measured -26 dB width. Modes with a fixed\nwidth ignore this.");
        }
        if (!haveVfo) { style::endDisabled(); }

        // ---- Width and shape
        ImGui::SectionHeader("WIDTH AND SHAPE");
        ImGui::Text("-26 dB    %s", fmtWidth(shown.occupied).c_str());
        if (ImGui::IsItemHovered()) { ImGui::SetTooltip("How much room the signal takes up: the width measured 26 dB down from the\npeak, where it has fallen to about a four hundredth of its power. This is\nthe number to compare with a channel plan."); }
        ImGui::Text("-3 dB     %s", fmtWidth(shown.halfPower).c_str());
        if (ImGui::IsItemHovered()) { ImGui::SetTooltip("The width of the strong part alone, at half the peak power"); }


        // Not "near": windows.h defines that as an empty macro.
        const char* comparable = yardstick(shown.occupied);
        if (comparable != NULL) { ImGui::Text("Compare   as wide as %s", comparable); }
        else { ImGui::Text("Compare   -"); }
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip("Something to hold the width up against, not an identification. Plenty of\n"
                              "unrelated things share a channel width, and a signal can sit in a channel\n"
                              "far wider than itself.");
        }

        // What the receiver is doing with it. Both numbers are already on screen, but
        // the comparison between them is the one thing here that says something about
        // what is coming out of the speaker rather than what is on the antenna.
        if (!haveVfo || shown.occupied <= 0.0) {
            ImGui::Text("Filter    -");
        }
        else if (vfoBw < (shown.occupied * 0.9)) {
            ImGui::Text("Filter    %s, cutting the edges off", fmtWidth(vfoBw).c_str());
        }
        else if (vfoBw > (shown.occupied * 1.6)) {
            ImGui::Text("Filter    %s, wider than it needs", fmtWidth(vfoBw).c_str());
        }
        else {
            ImGui::Text("Filter    %s, a good fit", fmtWidth(vfoBw).c_str());
        }
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip("Your channel width against the measured one. Narrower and you are hearing\n"
                              "part of the signal, which is why a wide signal in a narrow filter sounds\n"
                              "muffled or breaks up. Wider and you are letting in noise, and whatever\n"
                              "else is in the next channel along. Match width above sets it.");
        }

        // Whether the width sits still. Speech through an FM or SSB rig visibly
        // breathes as the talker does; a transmitter sending at a fixed rate does
        // not, whether or not it has anything to say.
        double widthMoved = 0.0;
        if (!widthSwing(widthMoved)) {
            ImGui::Text("Changes   -");
        }
        else if (widthMoved < std::max<double>(shown.hzPerBin * 2.0, shown.occupied * 0.08)) {
            ImGui::Text("Changes   hardly at all");
        }
        else {
            ImGui::Text("Changes   by %s", fmtWidth(widthMoved).c_str());
        }
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip("How much the occupied width moved over the last ten seconds, ignoring the\n"
                              "odd stray frame. A width that breathes belongs to something whose bandwidth\n"
                              "follows what is being sent, which is what voice does. A width that sits\n"
                              "still belongs to something sending at a constant rate.");
        }

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
        else if (shown.peaks >= 2 && shown.peakSpacing > 0.0) {
            ImGui::Text("Peaks     %d, %s apart", shown.peaks, fmtWidth(shown.peakSpacing).c_str());
        }
        else {
            ImGui::Text("Peaks     %d", shown.peaks);
        }
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip("Separate maxima within 20 dB of the strongest, across the occupied width,\n"
                              "and the average gap between them. Two evenly spaced humps look like keying\n"
                              "between two frequencies; a row of them looks like a multi-carrier signal.");
        }

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
        ImGui::SectionHeader("STRENGTH");
        ImGui::Text("Peak      %.0f dBFS", shown.peakDbfs);
        if (ImGui::IsItemHovered()) { ImGui::SetTooltip("Relative to the full scale of the receiver, so it depends on the gain\nsettings. Only the difference from the noise below means anything on its own."); }
        ImGui::Text("Noise     %.0f dBFS", shown.noiseDbfs);
        if (ImGui::IsItemHovered()) { ImGui::SetTooltip("The quiet quarter of everything on screen, taken as the noise floor"); }
        ImGui::Text("SNR       %.0f dB", shown.snr);
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip("How far the signal stands above the noise. Under about 6 dB nothing here\n"
                              "is worth much; over 20 dB the measurements are as good as the resolution\n"
                              "at the bottom of this panel allows.");
        }

        // Almost everything else in this panel describes the transmitter. This one
        // describes the path the signal took to get here.
        double levelMoved = 0.0;
        if (!levelSwing(levelMoved)) { ImGui::Text("Swing     -"); }
        else if (levelMoved < 3.0) { ImGui::Text("Swing     steady"); }
        else { ImGui::Text("Swing     %.0f dB", levelMoved); }
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip("How far the level rose and fell over the last ten seconds, ignoring the odd\n"
                              "stray frame. A steady level is a signal arriving by a stable path, usually\n"
                              "a local one. A level swinging by 10 or 20 dB is fading, which is what a\n"
                              "signal that has come a long way by ionosphere does - and also what a\n"
                              "transmitter that is moving does.");
        }

        // ---- Timing
        ImGui::SectionHeader("TIMING");
        double duty = (samplesTotal > 0) ? ((double)samplesOn / (double)samplesTotal) : 0.0;
        ImGui::Text("State     %s", stateText());

        if (samplesTotal >= 20) { ImGui::Text("On        %.0f%% of the time", duty * 100.0); }
        else { ImGui::Text("On        -"); }
        if (ImGui::IsItemHovered()) { ImGui::SetTooltip("Over the last twenty seconds or so"); }

        // All three of the rows below describe the last twenty transmissions, and
        // stop meaning anything once those stopped happening.
        bool timingOld = timingExpired();

        if (!burstLengths.empty() && !timingOld) {
            ImGui::Text("Bursts    %s", fmtTime(average(burstLengths)).c_str());
            if (ImGui::IsItemHovered()) {
                double shortest = *std::min_element(burstLengths.begin(), burstLengths.end());
                double longest = *std::max_element(burstLengths.begin(), burstLengths.end());
                ImGui::SetTooltip("How long it stays up, averaged over the last twenty transmissions.\n"
                                  "Shortest %s, longest %s.\n"
                                  "Bursts all the same length are a machine; ones that vary are someone talking.",
                                  fmtTime(shortest).c_str(), fmtTime(longest).c_str());
            }
        }
        else {
            ImGui::Text("Bursts    -");
        }

        if (!gapLengths.empty() && !timingOld) { ImGui::Text("Gaps      %s", fmtTime(average(gapLengths)).c_str()); }
        else { ImGui::Text("Gaps      -"); }
        if (ImGui::IsItemHovered()) { ImGui::SetTooltip("How long it waits between transmissions, averaged the same way"); }

        // Whether it keeps to a schedule, which the average period alone cannot say:
        // one burst every two seconds and a scatter of bursts averaging two seconds
        // apart gave the same number, and they are not the same signal.
        ImGui::Text("Rhythm    %s", rhythmText().c_str());
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip("The time from one transmission starting to the next, and how much it varies.\n"
                              "Regular means every gap was within a tenth of the average - a beacon, a\n"
                              "telemetry link, something on a timer rather than a person.");
        }

        // ---- Where
        ImGui::SectionHeader("WHERE");
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
        ImGui::SectionHeader("RESOLUTION");
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
