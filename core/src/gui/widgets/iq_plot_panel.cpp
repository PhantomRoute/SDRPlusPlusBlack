#ifndef IMGUI_DEFINE_MATH_OPERATORS
#define IMGUI_DEFINE_MATH_OPERATORS
#endif

#include <gui/widgets/iq_plot_panel.h>
#include <imgui.h>
#include <imgui/imgui_internal.h>
#include <gui/gui.h>
#include <gui/style.h>
#include <core.h>
#include <utils/event.h>
#include <signal_path/signal_path.h>
#include <dsp/processor.h>
#include <vector>
#include <mutex>
#include <atomic>
#include <string>
#include <algorithm>
#include <cmath>
#include <cstring>

namespace {
    // Points kept for the plot. Enough for a dense cloud, few enough to draw every frame.
    const int SNAPSHOT_POINTS = 4096;
    // Points taken from each block that passes, spread evenly across it.
    const int POINTS_PER_BLOCK = 512;
    // Full scale is a magnitude of 1.0.
    const float CLIP_LEVEL = 0.98f;
    // Samples in a row kept for the scope's sample view: consecutive, not thinned, so the
    // shape of the waveform - flattened tops, I and Q a quarter turn apart - survives.
    // About a tenth of a second at 2.4 MS/s, and over five seconds at 48 kHz.
    const int SCOPE_RING = 262144;
    // How many of them the view can show at once, from a few cycles to the whole ring.
    const int SCOPE_SPANS[] = { 64, 128, 256, 512, 1024, 2048, 4096, 8192, 16384, 32768, 65536, 131072, 262144 };
    const int SCOPE_SPAN_COUNT = 13;
    const int SCOPE_SPAN_DEFAULT = 4;
    // Blocks remembered for the scope's envelope view, which is what shows bursts.
    const int ENVELOPE_BLOCKS = 32768;
    const float ENVELOPE_SPANS[] = { 0.5f, 1.0f, 2.0f, 5.0f, 10.0f, 30.0f, 60.0f };
    const int ENVELOPE_SPAN_COUNT = 7;
    const int ENVELOPE_SPAN_DEFAULT = 2;
    const double IQ_PI = 3.14159265358979323846;

    enum IQView { IQ_VIEW_SCATTER = 0, IQ_VIEW_SCOPE };
    enum ScopeSpan { SPAN_SAMPLES = 0, SPAN_ENVELOPE };

    struct EnvelopeBlock {
        float peakI = 0.0f;
        float peakQ = 0.0f;
        int count = 0;
    };

    // Sits in the IQ front end's preprocessor chain. Passes every sample straight on,
    // and keeps a thinned copy for the plot. It is only enabled while the panel is
    // showing, so the chain pays for the copy only then.
    class IQTap : public dsp::Processor<dsp::complex_t, dsp::complex_t> {
        using base_type = dsp::Processor<dsp::complex_t, dsp::complex_t>;
    public:
        IQTap() {
            ring.resize(SNAPSHOT_POINTS);
            scope.resize(SCOPE_RING);
            envelope.resize(ENVELOPE_BLOCKS);
        }

        int run() override {
            int count = base_type::_in->read();
            if (count < 0) { return -1; }
            const dsp::complex_t* in = base_type::_in->readBuf;
            memcpy(base_type::out.writeBuf, in, (size_t)count * sizeof(dsp::complex_t));
            capture(in, count);
            base_type::_in->flush();
            if (!base_type::out.swap(count)) { return -1; }
            return count;
        }

        // Copies the latest points out, oldest first, along with the peak magnitude seen
        // over every sample since the last call. Returns how many points there were.
        int snapshot(std::vector<dsp::complex_t>& out, float& peak) {
            std::lock_guard<std::mutex> lck(snapMtx);
            out.resize(filled);
            for (int i = 0; i < filled; i++) {
                out[i] = ring[(pos - filled + i + SNAPSHOT_POINTS) % SNAPSHOT_POINTS];
            }
            peak = peakSinceRead;
            peakSinceRead = 0.0f;
            return filled;
        }

        // Whether to keep every sample for the scope. Only while its sample view is up:
        // at a few megasamples a second the copy is not free.
        std::atomic<bool> scopeCapture{ false };
        // Paused: the scope's samples and the envelope stop moving, so the span can still
        // be changed to look closer or further out at the moment Pause was pressed.
        std::atomic<bool> frozen{ false };

        // Forgets the scope's samples, so what it shows next time is not joined onto
        // samples from whenever it last looked.
        void clearScope() {
            std::lock_guard<std::mutex> lck(snapMtx);
            scopeFilled = 0;
            scopePos = 0;
        }

        // The newest want consecutive samples, oldest first, or as many as there are.
        int scopeSnapshot(std::vector<dsp::complex_t>& out, int want) {
            std::lock_guard<std::mutex> lck(snapMtx);
            int n = std::min<int>(want, scopeFilled);
            out.resize(n);
            for (int i = 0; i < n; i++) {
                out[i] = scope[(scopePos - n + i + SCOPE_RING) % SCOPE_RING];
            }
            return n;
        }

        // The per-block peaks covering the last seconds of signal, oldest first.
        int envelopeSnapshot(std::vector<EnvelopeBlock>& out, double seconds, double sampleRate) {
            std::lock_guard<std::mutex> lck(snapMtx);
            double wantSamples = seconds * std::max<double>(sampleRate, 1.0);
            double have = 0.0;
            int n = 0;
            while (n < envFilled && have < wantSamples) {
                have += envelope[(envPos - 1 - n + ENVELOPE_BLOCKS) % ENVELOPE_BLOCKS].count;
                n++;
            }
            out.resize(n);
            for (int i = 0; i < n; i++) {
                out[i] = envelope[(envPos - n + i + ENVELOPE_BLOCKS) % ENVELOPE_BLOCKS];
            }
            return n;
        }

        void clear() {
            std::lock_guard<std::mutex> lck(snapMtx);
            filled = 0;
            pos = 0;
            peakSinceRead = 0.0f;
            scopeFilled = 0;
            scopePos = 0;
            envFilled = 0;
            envPos = 0;
        }

    private:
        void capture(const dsp::complex_t* in, int count) {
            // The peak comes from every sample, not only the ones kept: a clip lasting a
            // single sample is exactly the kind that thinning would miss.
            float peakSq = 0.0f;
            float peakI = 0.0f, peakQ = 0.0f;
            for (int i = 0; i < count; i++) {
                float m = (in[i].re * in[i].re) + (in[i].im * in[i].im);
                if (m > peakSq) { peakSq = m; }
                peakI = std::max<float>(peakI, fabsf(in[i].re));
                peakQ = std::max<float>(peakQ, fabsf(in[i].im));
            }
            int stride = std::max<int>(1, count / POINTS_PER_BLOCK);

            std::lock_guard<std::mutex> lck(snapMtx);
            float peak = sqrtf(peakSq);
            if (peak > peakSinceRead) { peakSinceRead = peak; }
            for (int i = 0; i < count; i += stride) {
                ring[pos] = in[i];
                pos = (pos + 1) % SNAPSHOT_POINTS;
                if (filled < SNAPSHOT_POINTS) { filled++; }
            }
            if (frozen.load(std::memory_order_relaxed)) { return; }
            // Every sample, in order, while the scope wants them.
            if (scopeCapture.load(std::memory_order_relaxed)) {
                int start = std::max<int>(0, count - SCOPE_RING);
                int remaining = count - start;
                while (remaining > 0) {
                    int chunk = std::min<int>(remaining, SCOPE_RING - scopePos);
                    memcpy(&scope[scopePos], &in[start], (size_t)chunk * sizeof(dsp::complex_t));
                    scopePos = (scopePos + chunk) % SCOPE_RING;
                    scopeFilled = std::min<int>(SCOPE_RING, scopeFilled + chunk);
                    start += chunk;
                    remaining -= chunk;
                }
            }
            EnvelopeBlock blk;
            blk.peakI = peakI;
            blk.peakQ = peakQ;
            blk.count = count;
            envelope[envPos] = blk;
            envPos = (envPos + 1) % ENVELOPE_BLOCKS;
            if (envFilled < ENVELOPE_BLOCKS) { envFilled++; }
        }

        std::mutex snapMtx;
        std::vector<dsp::complex_t> ring;
        int pos = 0;
        int filled = 0;
        float peakSinceRead = 0.0f;
        std::vector<dsp::complex_t> scope;
        int scopePos = 0;
        int scopeFilled = 0;
        std::vector<EnvelopeBlock> envelope;
        int envPos = 0;
        int envFilled = 0;
    };

    class IQPlotPanel {
    public:
        bool shown = false;
        int view = IQ_VIEW_SCATTER;
        int span = SPAN_SAMPLES;
        int scopeSpanIdx = SCOPE_SPAN_DEFAULT;
        int envSpanIdx = ENVELOPE_SPAN_DEFAULT;
        bool paused = false;
        IQTap tap;
        bool tapInChain = false;
        bool tapEnabled = false;

        void show() {
            if (gui::mainWindow.hasBottomWindow("iq_plot")) { return; }
            gui::mainWindow.addBottomWindow("iq_plot", [this]() { draw(); });
        }

        void hide() {
            if (!gui::mainWindow.hasBottomWindow("iq_plot")) { return; }
            gui::mainWindow.removeBottomWindow("iq_plot");
        }

        void setTapEnabled(bool enabled) {
            if (!tapInChain || enabled == tapEnabled) { return; }
            tapEnabled = enabled;
            if (!enabled) { tap.clear(); }
            sigpath::iqFrontEnd.togglePreprocessor(&tap, enabled);
        }

        void tick() {
            if (!shown) {
                hide();
                setTapEnabled(false);
                return;
            }
            show();
            setTapEnabled(true);
            // Paused, whatever the scope was keeping stays kept, through any switching
            // between views. Otherwise its samples are dropped when it stops keeping them,
            // so they are never shown later as if they were current.
            if (!(paused && haveData)) {
                bool wantScope = (view == IQ_VIEW_SCOPE && span == SPAN_SAMPLES);
                bool wasKeeping = tap.scopeCapture.load(std::memory_order_relaxed);
                tap.scopeCapture.store(wantScope, std::memory_order_relaxed);
                if (!wantScope && wasKeeping) { tap.clearScope(); }
            }
            bool freeze = paused && haveData;
            if (freeze && !tap.frozen.load(std::memory_order_relaxed)) {
                // A few blocks can land between the last picture and the freeze; take it
                // again from exactly what is held, or going back to it later would differ.
                tap.frozen.store(true, std::memory_order_relaxed);
                shownKey = -1;
            }
            else { tap.frozen.store(freeze, std::memory_order_relaxed); }

            // Paused, everything on screen stays as it was, numbers included. The scope
            // is taken again from the held samples when its span or view changes.
            if (paused && haveData) {
                int key = (view * 10000) + (span * 1000) + (scopeSpanIdx * 10) + envSpanIdx;
                if (key != shownKey) { takeScope(); }
                return;
            }

            double now = ImGui::GetTime();
            if ((now - lastRead) < 0.05) { return; }
            lastRead = now;

            float peak = 0.0f;
            int n = tap.snapshot(points, peak);
            // A peak decays slowly on screen so a single clip is still readable a moment later.
            shownPeak = std::max<float>(peak, shownPeak * 0.97f);
            if (n < 16) { return; }

            double sumI = 0.0, sumQ = 0.0, sumII = 0.0, sumQQ = 0.0;
            mags.resize(n);
            for (int i = 0; i < n; i++) {
                double re = points[i].re;
                double im = points[i].im;
                sumI += re;
                sumQ += im;
                mags[i] = (float)sqrt((re * re) + (im * im));
            }
            meanI = (float)(sumI / n);
            meanQ = (float)(sumQ / n);
            // Imbalance from the AC part of each channel, so a DC offset on one of them
            // does not read as a gain difference.
            for (int i = 0; i < n; i++) {
                double re = points[i].re - meanI;
                double im = points[i].im - meanQ;
                sumII += re * re;
                sumQQ += im * im;
            }
            double sumIQ = 0.0;
            for (int i = 0; i < n; i++) { sumIQ += (points[i].re - meanI) * (points[i].im - meanQ); }
            float rmsI = (float)sqrt(sumII / n);
            float rmsQ = (float)sqrt(sumQQ / n);
            imbalanceDb = (rmsI > 0.0f && rmsQ > 0.0f) ? 20.0f * log10f(rmsI / rmsQ) : 0.0f;
            // How far I and Q are from a quarter turn apart. Over a busy spectrum the two
            // are uncorrelated when they are exactly 90 degrees apart, and any correlation
            // left is the angle they are off by.
            double corr = (rmsI > 0.0f && rmsQ > 0.0f) ? (sumIQ / n) / ((double)rmsI * (double)rmsQ) : 0.0;
            // Averaged over a few seconds: one snapshot is a few thousand points, and the
            // estimate from that alone wanders by a degree or two either way.
            float instantDeg = (float)(asin(std::clamp<double>(corr, -1.0, 1.0)) * 180.0 / IQ_PI);
            phaseErrorDeg = phaseSeeded ? (phaseErrorDeg * 0.95f) + (instantDeg * 0.05f) : instantDeg;
            phaseSeeded = true;
            takeScope();

            // Zoom to what is actually there. Real signals sit far below full scale, so a
            // fixed full-scale plot would show a dot in the middle. Scaled to the 99th
            // percentile, so one stray spike does not shrink everything else, and snapped
            // to 1-2-5 steps so the scale does not twitch every frame.
            size_t k = (size_t)((double)n * 0.99);
            std::nth_element(mags.begin(), mags.begin() + k, mags.end());
            float want = std::max<float>(mags[k] + std::max<float>(fabsf(meanI), fabsf(meanQ)), 1e-4f) * 1.15f;
            float step = powf(10.0f, floorf(log10f(want)));
            float snapped = step;
            if (want > step * 5.0f) { snapped = step * 10.0f; }
            else if (want > step * 2.0f) { snapped = step * 5.0f; }
            else if (want > step) { snapped = step * 2.0f; }
            scale = std::min<float>(snapped, 1.0f);
            haveData = true;
        }

        void takeScope() {
            shownKey = (view * 10000) + (span * 1000) + (scopeSpanIdx * 10) + envSpanIdx;
            if (view != IQ_VIEW_SCOPE) { return; }
            if (span == SPAN_SAMPLES) { tap.scopeSnapshot(scopePoints, SCOPE_SPANS[scopeSpanIdx]); }
            else { tap.envelopeSnapshot(envelopePoints, ENVELOPE_SPANS[envSpanIdx], sigpath::iqFrontEnd.getEffectiveSamplerate()); }
        }

        // The chosen one in the theme's accent colour, so it is clear which is showing.
        bool pill(const char* label, bool active) {
            if (active) {
                ImVec4 fill = ImGui::GetStyleColorVec4(ImGuiCol_CheckMark);
                fill.w = 0.55f;
                ImGui::PushStyleColor(ImGuiCol_Button, fill);
                ImGui::PushStyleColor(ImGuiCol_ButtonHovered, fill);
            }
            bool clicked = ImGui::SmallButton(label);
            if (active) { ImGui::PopStyleColor(2); }
            return clicked;
        }

        void saveChoice(const char* key, int value) {
            core::configManager.acquire();
            core::configManager.conf[key] = value;
            core::configManager.release(true);
        }

        // Places the next button on the same line if it fits, otherwise on a new one.
        void flowNext(const char* nextLabel, float gap = -1.0f) {
            float spacing = (gap < 0.0f) ? ImGui::GetStyle().ItemSpacing.x : gap;
            float w = ImGui::CalcTextSize(nextLabel, NULL, true).x + (ImGui::GetStyle().FramePadding.x * 2.0f);
            float right = ImGui::GetItemRectMax().x + spacing + w;
            float limit = ImGui::GetWindowPos().x + ImGui::GetWindowContentRegionMax().x;
            if (right < limit) { ImGui::SameLine(0.0f, spacing); }
        }

        std::string spanText() {
            char buf[48];
            if (span == SPAN_ENVELOPE) {
                float sec = ENVELOPE_SPANS[envSpanIdx];
                snprintf(buf, sizeof buf, sec < 1.0f ? "%.1f s" : "%.0f s", sec);
                return buf;
            }
            double sr = sigpath::iqFrontEnd.getEffectiveSamplerate();
            double ms = (sr > 0.0) ? 1000.0 * SCOPE_SPANS[scopeSpanIdx] / sr : 0.0;
            if (ms <= 0.0) { snprintf(buf, sizeof buf, "%d samples", SCOPE_SPANS[scopeSpanIdx]); }
            else if (ms < 1.0) { snprintf(buf, sizeof buf, "%.2f ms", ms); }
            else if (ms < 1000.0) { snprintf(buf, sizeof buf, "%.1f ms", ms); }
            else { snprintf(buf, sizeof buf, "%.2f s", ms / 1000.0); }
            return buf;
        }

        void drawViewPills() {
            float groupGap = 10.0f * style::uiScale;
            flowNext("Scatter##_iqplot_view0", groupGap);
            if (pill("Scatter##_iqplot_view0", view == IQ_VIEW_SCATTER)) { view = IQ_VIEW_SCATTER; saveChoice("iqPlotView", view); }
            flowNext("Scope##_iqplot_view1");
            if (pill("Scope##_iqplot_view1", view == IQ_VIEW_SCOPE)) { view = IQ_VIEW_SCOPE; saveChoice("iqPlotView", view); }
            if (view == IQ_VIEW_SCOPE) {
                flowNext("Samples##_iqplot_span0", groupGap);
                if (pill("Samples##_iqplot_span0", span == SPAN_SAMPLES)) { span = SPAN_SAMPLES; saveChoice("iqPlotScopeSpan", span); }
                if (ImGui::IsItemHovered()) { style::tooltip("Consecutive samples: the waveform itself"); }
                flowNext("Envelope##_iqplot_span1");
                if (pill("Envelope##_iqplot_span1", span == SPAN_ENVELOPE)) { span = SPAN_ENVELOPE; saveChoice("iqPlotScopeSpan", span); }
                if (ImGui::IsItemHovered()) { style::tooltip("The peak of I and Q in each block, over seconds"); }

                // How much time is across the plot.
                std::string text = spanText();
                flowNext(" - ##_iqplot_less", groupGap);
                if (ImGui::SmallButton(" - ##_iqplot_less")) {
                    if (span == SPAN_SAMPLES) { scopeSpanIdx = std::max<int>(0, scopeSpanIdx - 1); saveChoice("iqPlotScopeSamples", scopeSpanIdx); }
                    else { envSpanIdx = std::max<int>(0, envSpanIdx - 1); saveChoice("iqPlotEnvelopeSpan", envSpanIdx); }
                }
                if (ImGui::IsItemHovered()) { style::tooltip("Less time across the plot"); }
                flowNext(text.c_str());
                ImGui::AlignTextToFramePadding();
                ImGui::TextUnformatted(text.c_str());
                flowNext(" + ##_iqplot_more");
                if (ImGui::SmallButton(" + ##_iqplot_more")) {
                    if (span == SPAN_SAMPLES) { scopeSpanIdx = std::min<int>(SCOPE_SPAN_COUNT - 1, scopeSpanIdx + 1); saveChoice("iqPlotScopeSamples", scopeSpanIdx); }
                    else { envSpanIdx = std::min<int>(ENVELOPE_SPAN_COUNT - 1, envSpanIdx + 1); saveChoice("iqPlotEnvelopeSpan", envSpanIdx); }
                }
                if (ImGui::IsItemHovered()) { style::tooltip("More time across the plot"); }
            }
            flowNext("Pause##_iqplot_pause", groupGap);
            if (pill(paused ? "Paused##_iqplot_pause" : "Pause##_iqplot_pause", paused)) { paused = !paused; }
            if (ImGui::IsItemHovered()) { style::tooltip(paused ? "Carry on updating" : "Hold what is on screen, numbers included"); }
        }

        // I and Q against time: the consecutive samples, or the peaks of each block.
        void drawScope(ImVec2 origin, ImVec2 avail, float headerHeight, float lineH) {
            ImVec2 plotMin(origin.x, origin.y + headerHeight);
            ImVec2 plotMax(origin.x + avail.x, origin.y + avail.y - lineH);
            if (plotMax.y - plotMin.y < 30.0f) { return; }
            ImDrawList* draw = ImGui::GetWindowDrawList();
            ImU32 gridColor = ImGui::ColorConvertFloat4ToU32(gui::themeManager.fftGridColor);
            ImU32 borderColor = ImGui::ColorConvertFloat4ToU32(gui::themeManager.fftBorderColor);
            ImU32 iColor = ImGui::GetColorU32(ImGuiCol_PlotLines);
            ImU32 qColor = ImGui::ColorConvertFloat4ToU32(gui::themeManager.fftMinHoldColor);
            ImU32 clipColor = ImGui::ColorConvertFloat4ToU32(ImVec4(1.0f, 0.35f, 0.3f, 0.8f));
            float midY = (plotMin.y + plotMax.y) / 2.0f;
            float halfH = (plotMax.y - plotMin.y) / 2.0f;
            // Samples at the scatter's scale, peaks at a scale of their own.
            float range = scale;
            auto yOf = [&](float v) { return midY - (std::clamp<float>(v / range, -1.0f, 1.0f) * halfH); };

            draw->AddRect(plotMin, plotMax, borderColor);
            draw->PushClipRect(plotMin, plotMax, true);
            draw->AddLine(ImVec2(plotMin.x, midY), ImVec2(plotMax.x, midY), gridColor);
            float w = plotMax.x - plotMin.x;
            float thick = std::max<float>(1.0f, style::uiScale);

            if (span == SPAN_SAMPLES) {
                if (scopePoints.size() >= 2) {
                    float peak = 0.0f;
                    for (const auto& pt : scopePoints) { peak = std::max<float>(peak, std::max<float>(fabsf(pt.re), fabsf(pt.im))); }
                    range = std::max<float>(scale, std::min<float>(1.0f, peak * 1.1f));
                    size_t cols = (size_t)std::max<float>(2.0f, w);
                    if (scopePoints.size() <= cols) {
                        float dx = w / (float)(scopePoints.size() - 1);
                        for (size_t i = 0; i + 1 < scopePoints.size(); i++) {
                            float x0 = plotMin.x + (dx * i), x1 = plotMin.x + (dx * (i + 1));
                            draw->AddLine(ImVec2(x0, yOf(scopePoints[i].im)), ImVec2(x1, yOf(scopePoints[i + 1].im)), qColor, thick);
                            draw->AddLine(ImVec2(x0, yOf(scopePoints[i].re)), ImVec2(x1, yOf(scopePoints[i + 1].re)), iColor, thick);
                        }
                    }
                    else {
                        // More samples than pixels: each column spans its lowest to highest,
                        // so a single clipped sample still reaches the top.
                        size_t total = scopePoints.size();
                        for (int pass = 0; pass < 2; pass++) {
                            ImU32 col = (pass == 0) ? qColor : iColor;
                            for (size_t c = 0; c < cols; c++) {
                                size_t a = c * total / cols;
                                size_t b = std::max<size_t>(a + 1, (c + 1) * total / cols);
                                // Starting from the last sample of the column before, so
                                // the columns join up into one trace.
                                size_t from = (a > 0) ? a - 1 : a;
                                float lo = 1e9f, hi = -1e9f;
                                for (size_t i = from; i < b && i < total; i++) {
                                    float v = (pass == 0) ? scopePoints[i].im : scopePoints[i].re;
                                    lo = std::min<float>(lo, v);
                                    hi = std::max<float>(hi, v);
                                }
                                float x = plotMin.x + (float)c;
                                draw->AddLine(ImVec2(x, yOf(hi)), ImVec2(x, yOf(lo) + 1.0f), col, 1.0f);
                            }
                        }
                    }
                }
            }
            else if (envelopePoints.size() >= 2) {
                float peak = 0.0f;
                for (const auto& b : envelopePoints) { peak = std::max<float>(peak, std::max<float>(b.peakI, b.peakQ)); }
                range = std::max<float>(0.01f, std::min<float>(1.0f, peak * 1.1f));
                // A minute of blocks is thousands of them; more than there are pixels, each
                // column takes the highest peak among its blocks, so a burst is not lost.
                size_t total = envelopePoints.size();
                size_t cols = std::min<size_t>(total, (size_t)std::max<float>(1.0f, w));
                float dx = w / (float)cols;
                for (size_t c = 0; c < cols; c++) {
                    size_t a = c * total / cols;
                    size_t b = std::max<size_t>(a + 1, (c + 1) * total / cols);
                    float pI = 0.0f, pQ = 0.0f;
                    for (size_t i = a; i < b && i < total; i++) {
                        pI = std::max<float>(pI, envelopePoints[i].peakI);
                        pQ = std::max<float>(pQ, envelopePoints[i].peakQ);
                    }
                    float x = plotMin.x + (dx * (float)c) + (dx / 2.0f);
                    draw->AddLine(ImVec2(x, yOf(pI)), ImVec2(x, yOf(-pI)), iColor, std::max<float>(thick, dx * 0.6f));
                    draw->AddLine(ImVec2(x - dx / 2.0f, yOf(pQ)), ImVec2(x + dx / 2.0f, yOf(pQ)), qColor, thick);
                    draw->AddLine(ImVec2(x - dx / 2.0f, yOf(-pQ)), ImVec2(x + dx / 2.0f, yOf(-pQ)), qColor, thick);
                }
            }

            // Full scale, where the receiver clips, when the scale reaches it.
            if (range >= 0.5f) {
                float dash = 4.0f * style::uiScale;
                for (float sign : { 1.0f, -1.0f }) {
                    float y = yOf(sign * 1.0f);
                    for (float x = plotMin.x; x < plotMax.x; x += dash * 2.0f) {
                        draw->AddLine(ImVec2(x, y), ImVec2(std::min<float>(x + dash, plotMax.x), y), clipColor);
                    }
                }
            }
            bool empty = (span == SPAN_SAMPLES) ? (scopePoints.size() < 2) : (envelopePoints.size() < 2);
            if (empty && paused) {
                // Switched to a view that was not keeping anything when Pause was pressed.
                const char* msg = "Nothing held for this view. Release Pause to capture.";
                float wrap = std::max<float>(40.0f, w - (12.0f * style::uiScale));
                ImVec2 ts = ImGui::CalcTextSize(msg, NULL, false, wrap);
                draw->AddText(ImGui::GetFont(), ImGui::GetFontSize(), ImVec2((plotMin.x + plotMax.x - ts.x) / 2.0f, std::max<float>(plotMin.y, midY - ts.y / 2.0f)),
                              ImGui::GetColorU32(ImGuiCol_TextDisabled), msg, NULL, wrap);
            }
            draw->PopClipRect();

            ImGui::PushFont(style::tinyFont);
            char label[96];
            double sr = sigpath::iqFrontEnd.getEffectiveSamplerate();
            if (span == SPAN_SAMPLES) {
                if (sr > 0.0) { snprintf(label, sizeof label, "I blue, Q orange  \xC2\xB1%.3g  %.2f ms across%s", range, 1000.0 * (double)scopePoints.size() / sr, paused ? "  paused" : ""); }
                else { snprintf(label, sizeof label, "I blue, Q orange  \xC2\xB1%.3g", range); }
            }
            else {
                double total = 0.0;
                for (const auto& b : envelopePoints) { total += b.count; }
                if (sr > 0.0) { snprintf(label, sizeof label, "I bars, Q ticks  \xC2\xB1%.3g  %.1f s across%s", range, total / sr, paused ? "  paused" : ""); }
                else { snprintf(label, sizeof label, "I bars, Q ticks  \xC2\xB1%.3g", range); }
            }
            draw->AddText(ImVec2(plotMin.x + (3.0f * style::uiScale), plotMin.y + style::uiScale), ImGui::GetColorU32(ImGuiCol_TextDisabled), label);
            ImGui::PopFont();
        }

        void draw() {
            ImVec2 avail = ImGui::GetContentRegionAvail();
            if (avail.x < 60.0f || avail.y < 60.0f) { return; }
            if (view == IQ_VIEW_SCOPE) {
                drawScopePanel(avail);
                return;
            }

            ImVec2 origin = ImGui::GetCursorScreenPos();
            ImGui::PushFont(style::tinyFont);
            float lineH = ImGui::GetTextLineHeightWithSpacing();
            ImGui::PopFont();

            // Header: name, the views, and the scale the outer ring stands for. First, so
            // the plot goes under however many lines it wraps onto in a narrow panel.
            if (!gui::mainWindow.sdrIsRunning() && !haveData) { ImGui::TextDisabled("IQ   radio stopped"); }
            else if (!haveData) { ImGui::TextDisabled("IQ   collecting"); }
            else { ImGui::TextUnformatted("IQ"); }
            drawViewPills();
            if (haveData) {
                char ring[48];
                snprintf(ring, sizeof ring, "outer ring %.3g%s", scale, paused ? ", paused" : "");
                ImGui::PushFont(style::tinyFont);
                flowNext(ring);
                ImGui::TextDisabled("%s", ring);
                ImGui::PopFont();
            }
            float headerHeight = ImGui::GetCursorScreenPos().y - origin.y;

            // Square plot, as big as the room allows under the header and above the
            // readout line.
            float side = std::min<float>(avail.x, avail.y - headerHeight - lineH);
            if (side < 40.0f) { return; }
            ImVec2 plotMin(origin.x + ((avail.x - side) / 2.0f), origin.y + headerHeight);
            ImVec2 plotMax(plotMin.x + side, plotMin.y + side);
            ImVec2 centre((plotMin.x + plotMax.x) / 2.0f, (plotMin.y + plotMax.y) / 2.0f);
            float radius = side / 2.0f;

            ImDrawList* draw = ImGui::GetWindowDrawList();
            ImU32 gridColor = ImGui::ColorConvertFloat4ToU32(gui::themeManager.fftGridColor);
            ImU32 borderColor = ImGui::ColorConvertFloat4ToU32(gui::themeManager.fftBorderColor);
            ImU32 textColor = ImGui::GetColorU32(ImGuiCol_Text);
            ImVec4 dotCol = ImGui::GetStyleColorVec4(ImGuiCol_PlotLines);
            dotCol.w = 0.35f;
            ImU32 dotColor = ImGui::ColorConvertFloat4ToU32(dotCol);

            draw->AddRect(plotMin, plotMax, borderColor);
            draw->PushClipRect(plotMin, plotMax, true);
            draw->AddLine(ImVec2(plotMin.x, centre.y), ImVec2(plotMax.x, centre.y), gridColor);
            draw->AddLine(ImVec2(centre.x, plotMin.y), ImVec2(centre.x, plotMax.y), gridColor);
            draw->AddCircle(centre, radius, gridColor, 64);
            draw->AddCircle(centre, radius * 0.5f, gridColor, 64);

            if (haveData) {
                float px = radius / scale;
                float dot = std::max<float>(1.0f, style::uiScale);
                for (const auto& p : points) {
                    float x = centre.x + (p.re * px);
                    float y = centre.y - (p.im * px);
                    draw->AddRectFilled(ImVec2(x, y), ImVec2(x + dot, y + dot), dotColor);
                }
                // Where the middle of the cloud actually is, if it is not in the middle.
                float cx = centre.x + (meanI * px);
                float cy = centre.y - (meanQ * px);
                float tick = 4.0f * style::uiScale;
                draw->AddLine(ImVec2(cx - tick, cy), ImVec2(cx + tick, cy), textColor);
                draw->AddLine(ImVec2(cx, cy - tick), ImVec2(cx, cy + tick), textColor);
            }
            draw->PopClipRect();

            // Readouts under the plot, as many as fit.
            if (haveData) {
                ImGui::SetCursorScreenPos(ImVec2(origin.x, plotMax.y + (2.0f * style::uiScale)));
                drawReadouts(avail.x);
            }
        }

        void drawScopePanel(ImVec2 avail) {
            ImVec2 origin = ImGui::GetCursorScreenPos();
            ImGui::PushFont(style::tinyFont);
            float lineH = ImGui::GetTextLineHeightWithSpacing();
            ImGui::PopFont();
            if (!gui::mainWindow.sdrIsRunning() && !haveData) { ImGui::TextDisabled("IQ   radio stopped"); }
            else { ImGui::TextUnformatted("IQ"); }
            drawViewPills();
            // Below however many lines the buttons took.
            float headerHeight = ImGui::GetCursorScreenPos().y - origin.y;
            if (haveData) {
                drawScope(origin, avail, headerHeight, lineH);
                ImGui::SetCursorScreenPos(ImVec2(origin.x, origin.y + avail.y - lineH));
                drawReadouts(avail.x);
            }
        }

        void drawReadouts(float width) {
            {
                ImGui::PushFont(style::tinyFont);
                bool clipping = shownPeak >= CLIP_LEVEL;
                char dc[64];
                char imb[32];
                char pk[32];
                snprintf(dc, sizeof dc, "DC I %+.2f%% Q %+.2f%%", meanI * 100.0f, meanQ * 100.0f);
                snprintf(imb, sizeof imb, "imbalance %+.2f dB", imbalanceDb);
                snprintf(pk, sizeof pk, clipping ? "CLIPPING %.0f%%" : "peak %.0f%%", shownPeak * 100.0f);
                char ph[32];
                snprintf(ph, sizeof ph, "I/Q phase %+.1f\xC2\xB0", phaseErrorDeg);
                float spacing = 10.0f * style::uiScale;
                float used = 0.0f;
                const char* parts[4] = { pk, dc, imb, ph };
                for (int i = 0; i < 4; i++) {
                    float w = ImGui::CalcTextSize(parts[i]).x;
                    if (used + w > width) { break; }
                    if (i > 0) { ImGui::SameLine(0.0f, spacing); }
                    if (i == 0 && clipping) {
                        ImGui::TextColored(ImVec4(1.0f, 0.35f, 0.3f, 1.0f), "%s", parts[i]);
                    }
                    else {
                        ImGui::TextDisabled("%s", parts[i]);
                    }
                    used += w + spacing;
                }
                ImGui::PopFont();
            }
        }

    private:
        std::vector<dsp::complex_t> points;
        std::vector<float> mags;
        double lastRead = 0.0;
        float meanI = 0.0f;
        float meanQ = 0.0f;
        float imbalanceDb = 0.0f;
        float phaseErrorDeg = 0.0f;
        bool phaseSeeded = false;
        std::vector<dsp::complex_t> scopePoints;
        std::vector<EnvelopeBlock> envelopePoints;
        float shownPeak = 0.0f;
        float scale = 1.0f;
        bool haveData = false;
        int shownKey = -1;   // the view and spans the scope was last taken for
    };

    // Made in init() and never deleted. The tap is part of the IQ front end's chain, and
    // the front end is a global in another file: as a global here, the order the two
    // were torn down in at exit would be up to the compiler, and the chain could be left
    // pointing at a block that had already gone.
    IQPlotPanel* panel = nullptr;
    EventHandler<ImGuiContext*> waterfallDrawnHandler;
}

namespace iqplot {
    void init() {
        if (panel) { return; }
        panel = new IQPlotPanel();

        core::configManager.acquire();
        if (core::configManager.conf.contains("showIQPlot")) {
            panel->shown = core::configManager.conf["showIQPlot"];
        }
        if (core::configManager.conf.contains("iqPlotView")) {
            panel->view = std::clamp<int>(core::configManager.conf["iqPlotView"], 0, 1);
        }
        if (core::configManager.conf.contains("iqPlotScopeSpan")) {
            panel->span = std::clamp<int>(core::configManager.conf["iqPlotScopeSpan"], 0, 1);
        }
        if (core::configManager.conf.contains("iqPlotScopeSamples")) {
            panel->scopeSpanIdx = std::clamp<int>(core::configManager.conf["iqPlotScopeSamples"], 0, SCOPE_SPAN_COUNT - 1);
        }
        if (core::configManager.conf.contains("iqPlotEnvelopeSpan")) {
            panel->envSpanIdx = std::clamp<int>(core::configManager.conf["iqPlotEnvelopeSpan"], 0, ENVELOPE_SPAN_COUNT - 1);
        }
        core::configManager.release();

        // Into the chain once, switched off. Enabling and disabling it after that only
        // rewires its neighbours, which the front end does while running.
        panel->tap.init(NULL);
        sigpath::iqFrontEnd.addPreprocessor(&panel->tap, false);
        panel->tapInChain = true;

        waterfallDrawnHandler.ctx = panel;
        waterfallDrawnHandler.handler = [](ImGuiContext* gctx, void* ctx) {
            ((IQPlotPanel*)ctx)->tick();
        };
        gui::mainWindow.onWaterfallDrawn.bindHandler(&waterfallDrawnHandler);
    }

    bool isShown() { return panel && panel->shown; }

    void setShown(bool shown) {
        if (!panel) { return; }
        panel->shown = shown;
        core::configManager.acquire();
        core::configManager.conf["showIQPlot"] = shown;
        core::configManager.release(true);
    }
}
