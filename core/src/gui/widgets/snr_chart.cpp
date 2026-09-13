#ifndef IMGUI_DEFINE_MATH_OPERATORS
#define IMGUI_DEFINE_MATH_OPERATORS
#endif

#include <gui/widgets/snr_chart.h>
#include <imgui.h>
#include <imgui/imgui_internal.h>
#include <gui/gui.h>
#include <gui/style.h>
#include <core.h>
#include <utils/event.h>
#include <vector>
#include <deque>
#include <cmath>
#include <algorithm>

namespace {
    // How much of the SNR history the chart keeps and draws, and how often it samples.
    const double SNR_CHART_SPAN = 60.0;
    const double SNR_SAMPLE_PERIOD = 0.05;

    class SNRChart {
    public:
        bool shown = false;

    // ---- SNR chart
    //
    // This used to be drawn straight over the waterfall as a bare polyline running
    // *down* the screen from wherever the SNR meter happened to sit: x was the
    // meter's pixel length rather than a dB value, y was the sample index, and there
    // was no scale, no units and no time base. It now lives in a window along the
    // bottom, beside the audio waterfall, and is plotted against dB and seconds.

    struct SnrSample {
        double time;
        float snr;
    };
    std::deque<SnrSample> snrHistory;
    double lastSnrSampleTime = 0.0;
    // Non-zero while the radio is stopped: the clock the chart is drawn against, held
    // at the moment sampling stopped so the trace does not scroll away under itself.
    double chartFrozenAt = 0.0;
    // Scratch for the per-column collapse below, kept between frames so drawing the
    // chart doesn't allocate on every one.
    std::vector<float> colSum;
    std::vector<int> colCount;
    std::vector<float> colValue;
    std::vector<float> colSmooth;
    std::vector<ImVec2> tracePoints;

    void showSNRChart() {
        if (gui::mainWindow.hasBottomWindow("snr_chart")) { return; }
        gui::mainWindow.addBottomWindow("snr_chart", [this]() { drawSNRChart(); });
    }

    void hideSNRChart() {
        if (!gui::mainWindow.hasBottomWindow("snr_chart")) { return; }
        gui::mainWindow.removeBottomWindow("snr_chart");
    }

    // Called once a frame from onWaterfallDrawn, which is also where the window has
    // to be added: the host draws the bottom windows later in the same frame.
    void tickSNRChart() {
        if (!shown) {
            hideSNRChart();
            snrHistory.clear();
            return;
        }
        showSNRChart();

        // Nothing is arriving while the radio is stopped, so selectedVFOSNR sits at
        // whatever it last read. Sampling it anyway drew that one stale number across
        // the chart at twenty points a second, which looks exactly like a strong,
        // perfectly steady signal - the chart went on reporting a station that had
        // stopped being received.
        //
        // Stopping the sampling is only half of it. The horizontal axis is "seconds
        // ago", measured against the clock every frame, so with the radio stopped the
        // trace went on sliding left and off the chart and the grid marks crawled
        // with it - which still reads as a running chart even though no new reading
        // has been taken. Freezing the clock at the moment it stopped leaves the last
        // minute on screen, still, until it starts again.
        if (!gui::mainWindow.sdrIsRunning()) {
            if (chartFrozenAt <= 0.0) { chartFrozenAt = ImGui::GetTime(); }
            return;
        }
        chartFrozenAt = 0.0;

        // The core's own measurement, in dB over the noise beside the channel. The
        // chart used to be fed the SNR meter's drawn pixel length instead, which is
        // scaled by the width of the meter and so means nothing on its own.
        double now = ImGui::GetTime();
        if ((now - lastSnrSampleTime) < SNR_SAMPLE_PERIOD) { return; }
        lastSnrSampleTime = now;

        if (gui::waterfall.selectedVFO.empty()) { return; }
        float snr = gui::waterfall.selectedVFOSNR;
        if (!std::isfinite(snr)) { return; }

        snrHistory.push_back({ now, snr });
        while (!snrHistory.empty() && (now - snrHistory.front().time) > SNR_CHART_SPAN) {
            snrHistory.pop_front();
        }
    }

    void drawSNRChart() {
        ImVec2 avail = ImGui::GetContentRegionAvail();
        if (avail.x < 40.0f || avail.y < 30.0f) { return; }

        ImGui::PushFont(style::tinyFont);
        float labelWidth = ImGui::CalcTextSize("100").x + (4.0f * style::uiScale);
        ImGui::PopFont();
        // The readout line above the plot is drawn in the normal font, so the room
        // left for it has to be measured in that font.
        float headerHeight = ImGui::GetTextLineHeightWithSpacing();

        ImVec2 origin = ImGui::GetCursorScreenPos();
        ImVec2 plotMin(origin.x + labelWidth, origin.y + headerHeight);
        ImVec2 plotMax(origin.x + avail.x, origin.y + avail.y);
        if (plotMax.x - plotMin.x < 10.0f || plotMax.y - plotMin.y < 10.0f) { return; }

        ImDrawList* draw = ImGui::GetWindowDrawList();
        ImU32 gridColor = ImGui::ColorConvertFloat4ToU32(gui::themeManager.fftGridColor);
        ImU32 borderColor = ImGui::ColorConvertFloat4ToU32(gui::themeManager.fftBorderColor);
        ImU32 textColor = ImGui::GetColorU32(ImGuiCol_Text);
        ImVec4 traceCol = gui::themeManager.snrMeterColor;

        // A fixed scale is easier to read at a glance than one that rescales itself
        // under the trace, but a signal that runs off the top is worse, so the top
        // only ever moves up to the next 10dB mark.
        float top = 40.0f;
        float latest = 0.0f;
        float lowest = INFINITY;
        float highest = -INFINITY;
        double sum = 0.0;
        for (const auto& s : snrHistory) {
            if (s.snr > highest) { highest = s.snr; }
            if (s.snr < lowest) { lowest = s.snr; }
            sum += s.snr;
        }
        if (!snrHistory.empty()) {
            latest = snrHistory.back().snr;
            // Round the top up to the next 10dB mark, and no further than 100: a
            // single wild reading should not squash the trace into the floor.
            top = std::clamp(ceilf(highest / 10.0f) * 10.0f, 40.0f, 100.0f);
        }

        // The clock everything below is positioned against. Frozen while the radio is
        // stopped, so the chart holds the last minute instead of scrolling it away.
        double now = (chartFrozenAt > 0.0) ? chartFrozenAt : ImGui::GetTime();
        auto xOf = [&](double t) {
            double age = now - t;
            double f = 1.0 - (age / SNR_CHART_SPAN);
            return plotMin.x + (float)(std::clamp(f, 0.0, 1.0) * (plotMax.x - plotMin.x));
        };
        auto yOf = [&](float db) {
            float f = std::clamp(db / top, 0.0f, 1.0f);
            return plotMax.y - (f * (plotMax.y - plotMin.y));
        };

        // dB grid, labelled down the left
        ImGui::PushFont(style::tinyFont);
        for (float db = 0.0f; db <= top + 0.1f; db += 10.0f) {
            float y = yOf(db);
            draw->AddLine(ImVec2(plotMin.x, y), ImVec2(plotMax.x, y), gridColor, 1.0f);
            char buf[16];
            snprintf(buf, sizeof buf, "%d", (int)db);
            ImVec2 sz = ImGui::CalcTextSize(buf);
            draw->AddText(ImVec2(plotMin.x - sz.x - (3.0f * style::uiScale), y - (sz.y / 2.0f)), textColor, buf);
        }
        // 10 second marks, so the width means something
        for (double t = 10.0; t < SNR_CHART_SPAN; t += 10.0) {
            float x = xOf(now - t);
            draw->AddLine(ImVec2(x, plotMin.y), ImVec2(x, plotMax.y), gridColor, 1.0f);
        }
        ImGui::PopFont();

        draw->AddRect(plotMin, plotMax, borderColor);

        // Collapse the samples into one value per pixel column before drawing any of
        // them. A minute at 20 samples a second is 1200 points across a plot a few
        // hundred pixels wide, so drawing a segment per sample put four sub-pixel
        // wide, anti-aliased quads into every column: the partial coverage of each
        // one is what streaked the fill with vertical lines. Averaging into columns
        // is also the honest way to show more data than there are pixels for.
        int cols = (int)(plotMax.x - plotMin.x);
        if (snrHistory.size() >= 2 && cols >= 2) {
            colSum.assign(cols, 0.0f);
            colCount.assign(cols, 0);
            for (const auto& s : snrHistory) {
                double f = 1.0 - ((now - s.time) / SNR_CHART_SPAN);
                int c = (int)((f * (double)(cols - 1)) + 0.5);
                if (c < 0 || c >= cols) { continue; }
                colSum[c] += s.snr;
                colCount[c]++;
            }

            // Carry the last value across columns no sample landed in - a dropped
            // frame, or the gap left while the radio was stopped - so the trace does
            // not fall to the floor and back between two real readings.
            colValue.assign(cols, 0.0f);
            int firstCol = -1;
            int lastCol = -1;
            float carry = 0.0f;
            for (int c = 0; c < cols; c++) {
                if (colCount[c] > 0) {
                    carry = colSum[c] / (float)colCount[c];
                    if (firstCol < 0) { firstCol = c; }
                    lastCol = c;
                }
                colValue[c] = carry;
            }

            // Carrying forward is right in the middle of the trace, where it bridges
            // a dropped frame. It is wrong at the end: with the radio stopped there
            // are no samples in any column since, and carrying the last one to the
            // right hand edge drew a flat line across the rest of the minute that is
            // indistinguishable from a live, dead steady signal. The trace ends where
            // the readings do, and the gap after it stays empty.
            if (firstCol >= 0 && lastCol > firstCol) {
                // One pass of a 3 tap average over the columns. Enough to take the
                // hard edge off without flattening anything worth seeing.
                colSmooth.assign(colValue.begin(), colValue.end());
                for (int c = firstCol + 1; c < lastCol; c++) {
                    colSmooth[c] = (colValue[c - 1] + colValue[c] + colValue[c + 1]) / 3.0f;
                }

                tracePoints.clear();
                tracePoints.reserve(lastCol - firstCol + 1);
                for (int c = firstCol; c <= lastCol; c++) {
                    tracePoints.push_back(ImVec2(plotMin.x + (float)c, yOf(colSmooth[c])));
                }

                ImVec4 fillCol = traceCol;
                fillCol.w = 0.25f;
                ImU32 fill = ImGui::ColorConvertFloat4ToU32(fillCol);
                ImU32 trace = ImGui::ColorConvertFloat4ToU32(traceCol);

                // One quad per column, each a whole pixel wide. Anti-aliased filling
                // feathers half a pixel in from every edge, which on shapes this
                // narrow leaves each column edged with its own gradient and the
                // whole fill striped; the columns meet exactly without it.
                ImDrawListFlags fillFlags = draw->Flags;
                draw->Flags &= ~ImDrawListFlags_AntiAliasedFill;
                for (size_t i = 1; i < tracePoints.size(); i++) {
                    const ImVec2& a = tracePoints[i - 1];
                    const ImVec2& b = tracePoints[i];
                    draw->AddQuadFilled(a, b, ImVec2(b.x, plotMax.y), ImVec2(a.x, plotMax.y), fill);
                }
                draw->Flags = fillFlags;
                // And a single polyline over the top, which anti-aliases as one
                // continuous stroke instead of a few hundred separate ones.
                draw->AddPolyline(tracePoints.data(), (int)tracePoints.size(), trace, 0, 1.5f);
            }
        }

        // Readouts on the header line: what it is, where it is now, and what it has
        // been doing over the window.
        ImGui::SetCursorScreenPos(origin);
        if (snrHistory.empty()) {
            ImGui::TextDisabled("%s", gui::mainWindow.sdrIsRunning() ? "SNR   waiting for a VFO" : "SNR   radio stopped");
        }
        // The big number is the newest reading, which stops being a reading at all
        // once nothing is arriving. Saying so beats showing the last one as if the
        // radio were still on it.
        else if (!gui::mainWindow.sdrIsRunning()) {
            ImGui::TextDisabled("SNR   radio stopped");
            ImGui::SameLine();
            ImGui::PushFont(style::tinyFont);
            ImGui::TextDisabled("last %.0f dB", snrHistory.back().snr);
            ImGui::PopFont();
        }
        else {
            ImGui::TextColored(traceCol, "SNR %.1f dB", latest);
            ImGui::SameLine();
            ImGui::PushFont(style::tinyFont);
            ImGui::TextDisabled("min %.0f  avg %.0f  max %.0f  over %ds",
                                lowest, sum / (double)snrHistory.size(), highest, (int)SNR_CHART_SPAN);
            ImGui::PopFont();
        }
    }
    };

    SNRChart chart;
    EventHandler<ImGuiContext*> waterfallDrawnHandler;
}

namespace snrchart {
    void init() {
        core::configManager.acquire();
        if (core::configManager.conf.contains("showSNRChart")) {
            chart.shown = core::configManager.conf["showSNRChart"];
        }
        core::configManager.release();

        // Ticked once a frame from here, which is also where the panel has to be added:
        // the main window draws its bottom panels later in the same frame.
        waterfallDrawnHandler.ctx = &chart;
        waterfallDrawnHandler.handler = [](ImGuiContext* gctx, void* ctx) {
            ((SNRChart*)ctx)->tickSNRChart();
        };
        gui::mainWindow.onWaterfallDrawn.bindHandler(&waterfallDrawnHandler);
    }

    bool isShown() { return chart.shown; }

    void setShown(bool shown) {
        chart.shown = shown;
        core::configManager.acquire();
        core::configManager.conf["showSNRChart"] = shown;
        core::configManager.release(true);
    }
}
