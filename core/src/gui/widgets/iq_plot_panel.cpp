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

    // Sits in the IQ front end's preprocessor chain. Passes every sample straight on,
    // and keeps a thinned copy for the plot. It is only enabled while the panel is
    // showing, so the chain pays for the copy only then.
    class IQTap : public dsp::Processor<dsp::complex_t, dsp::complex_t> {
        using base_type = dsp::Processor<dsp::complex_t, dsp::complex_t>;
    public:
        IQTap() { ring.resize(SNAPSHOT_POINTS); }

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

        void clear() {
            std::lock_guard<std::mutex> lck(snapMtx);
            filled = 0;
            pos = 0;
            peakSinceRead = 0.0f;
        }

    private:
        void capture(const dsp::complex_t* in, int count) {
            // The peak comes from every sample, not only the ones kept: a clip lasting a
            // single sample is exactly the kind that thinning would miss.
            float peakSq = 0.0f;
            for (int i = 0; i < count; i++) {
                float m = (in[i].re * in[i].re) + (in[i].im * in[i].im);
                if (m > peakSq) { peakSq = m; }
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
        }

        std::mutex snapMtx;
        std::vector<dsp::complex_t> ring;
        int pos = 0;
        int filled = 0;
        float peakSinceRead = 0.0f;
    };

    class IQPlotPanel {
    public:
        bool shown = false;
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
            float rmsI = (float)sqrt(sumII / n);
            float rmsQ = (float)sqrt(sumQQ / n);
            imbalanceDb = (rmsI > 0.0f && rmsQ > 0.0f) ? 20.0f * log10f(rmsI / rmsQ) : 0.0f;

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

        void draw() {
            ImVec2 avail = ImGui::GetContentRegionAvail();
            if (avail.x < 60.0f || avail.y < 60.0f) { return; }

            ImVec2 origin = ImGui::GetCursorScreenPos();
            float headerHeight = ImGui::GetTextLineHeightWithSpacing();
            ImGui::PushFont(style::tinyFont);
            float lineH = ImGui::GetTextLineHeightWithSpacing();
            ImGui::PopFont();

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

            // Header: name, and the scale the outer ring stands for.
            ImGui::SetCursorScreenPos(origin);
            if (!gui::mainWindow.sdrIsRunning() && !haveData) {
                ImGui::TextDisabled("IQ   radio stopped");
            }
            else if (!haveData) {
                ImGui::TextDisabled("IQ   collecting");
            }
            else {
                ImGui::TextUnformatted("IQ");
                ImGui::SameLine();
                ImGui::PushFont(style::tinyFont);
                ImGui::TextDisabled("outer ring %.3g of full scale", scale);
                ImGui::PopFont();
            }

            // Readouts under the plot, as many as fit.
            if (haveData) {
                ImGui::SetCursorScreenPos(ImVec2(origin.x, plotMax.y + (2.0f * style::uiScale)));
                ImGui::PushFont(style::tinyFont);
                bool clipping = shownPeak >= CLIP_LEVEL;
                char dc[64];
                char imb[32];
                char pk[32];
                snprintf(dc, sizeof dc, "DC I %+.2f%% Q %+.2f%%", meanI * 100.0f, meanQ * 100.0f);
                snprintf(imb, sizeof imb, "imbalance %+.2f dB", imbalanceDb);
                snprintf(pk, sizeof pk, clipping ? "CLIPPING %.0f%%" : "peak %.0f%%", shownPeak * 100.0f);
                float spacing = 10.0f * style::uiScale;
                float used = 0.0f;
                const char* parts[3] = { dc, imb, pk };
                for (int i = 0; i < 3; i++) {
                    float w = ImGui::CalcTextSize(parts[i]).x;
                    if (used + w > avail.x) { break; }
                    if (i > 0) { ImGui::SameLine(0.0f, spacing); }
                    if (i == 2 && clipping) {
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
        float shownPeak = 0.0f;
        float scale = 1.0f;
        bool haveData = false;
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
