#ifndef IMGUI_DEFINE_MATH_OPERATORS
#define IMGUI_DEFINE_MATH_OPERATORS
#endif

#include <gui/widgets/occupancy_panel.h>
#include <imgui.h>
#include <imgui/imgui_internal.h>
#include <gui/gui.h>
#include <gui/style.h>
#include <gui/tuner.h>
#include <core.h>
#include <utils/event.h>
#include <utils/hrfreq.h>
#include <vector>
#include <unordered_map>
#include <algorithm>
#include <cmath>
#include <cstdint>

namespace {
    // How often the spectrum is looked at. Twenty times a second is well above how
    // fast anything worth counting comes and goes, and keeps the cost negligible.
    const double SAMPLE_PERIOD = 0.05;

    // Channels across whatever span is on screen. Fixed rather than following the
    // panel's width, so switching another panel on - which narrows this one - does not
    // change the channels and throw the counts away.
    const int CHANNELS_PER_SPAN = 128;

    // A long session panning across a wide band keeps adding channels. Past this many
    // the oldest history is not worth the memory; start again.
    const size_t MAX_TRACKED_CHANNELS = 16384;

    const int MIN_THRESHOLD_DB = 3;
    const int MAX_THRESHOLD_DB = 60;

    // The spectrum's own grid steps, so the frequency lines here fall on the same
    // frequencies as the lines on the spectrum above.
    const double GRID_STEPS[] = {
        1.0, 2.0, 2.5, 5.0, 10.0, 20.0, 25.0, 50.0, 100.0, 200.0, 250.0, 500.0,
        1e3, 2e3, 2.5e3, 5e3, 1e4, 2e4, 2.5e4, 5e4, 1e5, 2e5, 2.5e5, 5e5,
        1e6, 2e6, 2.5e6, 5e6, 1e7, 2e7, 2.5e7, 5e7
    };

    double bestGridStep(double span, int maxSteps) {
        for (double step : GRID_STEPS) {
            if (span / step < (double)maxSteps) { return step; }
        }
        return 5e7;
    }

    // Same short form as the spectrum's scale: 7.06M, 850K.
    void formatFreq(double freq, char* buf, size_t len) {
        double a = fabs(freq);
        if (a < 1e3) { snprintf(buf, len, "%.6g", freq); }
        else if (a < 1e6) { snprintf(buf, len, "%.6gK", freq / 1e3); }
        else if (a < 1e9) { snprintf(buf, len, "%.6gM", freq / 1e6); }
        else { snprintf(buf, len, "%.6gG", freq / 1e9); }
    }

    struct Channel {
        int samples = 0;
        int busy = 0;
        // The tick this channel was last looked at, so "busy now" is only believed for
        // a channel that was actually on screen a moment ago.
        uint64_t lastTick = 0;
        bool busyNow = false;
        bool everBusy = false;
        // Where the strongest point was the last time the channel was busy. A click
        // tunes here, onto the signal, rather than to the middle of the channel.
        double peakFreq = 0.0;
    };

    class OccupancyPanel {
    public:
        bool shown = false;
        int thresholdDb = 12;

        void show() {
            if (gui::mainWindow.hasBottomWindow("occupancy")) { return; }
            gui::mainWindow.addBottomWindow("occupancy", [this]() { draw(); });
        }

        void hide() {
            if (!gui::mainWindow.hasBottomWindow("occupancy")) { return; }
            gui::mainWindow.removeBottomWindow("occupancy");
        }

        void resetStats() {
            channels.clear();
            channelWidth = 0.0;
        }

        // Called once a frame from onWaterfallDrawn, which is also where the panel has
        // to be added: the main window draws its bottom panels later in the same frame.
        void tick() {
            if (!shown) {
                hide();
                resetStats();
                return;
            }
            show();

            // Nothing new arrives while the radio is stopped, so counting the last
            // spectrum over and over would make whatever it held look permanent.
            if (!gui::mainWindow.sdrIsRunning()) { return; }

            double now = ImGui::GetTime();
            if ((now - lastSample) < SAMPLE_PERIOD) { return; }
            lastSample = now;

            double span = gui::waterfall.getViewBandwidth();
            double lo = gui::waterfall.getCenterFrequency() + gui::waterfall.getViewOffset() - (span / 2.0);
            if (span <= 0.0) { return; }

            // Copy the line out and let go at once: the same lock guards the FFT update.
            int pixels = 0;
            float* fft = gui::waterfall.acquireLatestFFT(pixels);
            if (!fft) { return; }
            if (pixels < 16) {
                gui::waterfall.releaseLatestFFT();
                return;
            }
            line.assign(fft, fft + pixels);
            gui::waterfall.releaseLatestFFT();

            // Channels are fixed slices of absolute frequency, not of the screen, so
            // panning or retuning keeps the history of every channel still in view -
            // including a retune from clicking this panel. Only a zoom, which changes
            // how wide a channel is, starts again.
            double wantWidth = span / (double)CHANNELS_PER_SPAN;
            if (channelWidth <= 0.0 || fabs(wantWidth - channelWidth) > channelWidth * 1e-3 || channels.size() > MAX_TRACKED_CHANNELS) {
                channels.clear();
                channelWidth = wantWidth;
            }
            viewLo = lo;
            viewSpan = span;

            // The floor is the median of everything on screen. Most of any spectrum is
            // noise, so the middle value is the noise and not the signals - and it needs
            // no history, so it does not depend on Min hold being switched on.
            sorted = line;
            size_t mid = sorted.size() / 2;
            std::nth_element(sorted.begin(), sorted.begin() + mid, sorted.end());
            float floorDb = sorted[mid];
            // The display fills the line with -1000 until a real spectrum arrives.
            if (floorDb < -900.0f) { return; }
            lastFloorDb = floorDb;

            tickNo++;
            float busyAbove = floorDb + (float)thresholdDb;
            double pxPerHz = (double)pixels / span;
            int64_t first = (int64_t)ceil(lo / channelWidth);
            int64_t last = (int64_t)floor((lo + span) / channelWidth) - 1;
            for (int64_t idx = first; idx <= last; idx++) {
                // Only channels wholly on screen: one hanging off the edge would be
                // judged on part of its width.
                double a = (double)idx * channelWidth;
                int pa = std::clamp<int>((int)floor((a - lo) * pxPerHz), 0, pixels - 1);
                int pb = std::clamp<int>((int)ceil((a + channelWidth - lo) * pxPerHz), pa + 1, pixels);
                float peak = -1000.0f;
                int peakPx = pa;
                for (int i = pa; i < pb; i++) {
                    if (line[i] > peak) {
                        peak = line[i];
                        peakPx = i;
                    }
                }
                Channel& ch = channels[idx];
                ch.samples++;
                ch.lastTick = tickNo;
                ch.busyNow = peak > busyAbove;
                if (ch.busyNow) {
                    ch.busy++;
                    ch.everBusy = true;
                    ch.peakFreq = lo + (((double)peakPx + 0.5) / pxPerHz);
                }
            }
        }

        void draw() {
            ImVec2 avail = ImGui::GetContentRegionAvail();
            if (avail.x < 80.0f || avail.y < 60.0f) { return; }

            ImGui::PushFont(style::tinyFont);
            float labelWidth = ImGui::CalcTextSize("100%").x + (4.0f * style::uiScale);
            float axisHeight = ImGui::GetTextLineHeight() + (3.0f * style::uiScale);
            ImGui::PopFont();
            float headerHeight = ImGui::GetFrameHeightWithSpacing();

            ImVec2 origin = ImGui::GetCursorScreenPos();
            ImVec2 plotMin(origin.x + labelWidth, origin.y + headerHeight);
            ImVec2 plotMax(origin.x + avail.x, origin.y + avail.y - axisHeight);
            float plotW = plotMax.x - plotMin.x;
            if (plotW < 20.0f || plotMax.y - plotMin.y < 20.0f) { return; }

            ImDrawList* draw = ImGui::GetWindowDrawList();
            ImU32 gridColor = ImGui::ColorConvertFloat4ToU32(gui::themeManager.fftGridColor);
            ImU32 borderColor = ImGui::ColorConvertFloat4ToU32(gui::themeManager.fftBorderColor);
            ImU32 textColor = ImGui::GetColorU32(ImGuiCol_Text);
            ImVec4 lineCol = ImGui::GetStyleColorVec4(ImGuiCol_PlotHistogram);
            ImVec4 fillCol = lineCol;
            fillCol.w *= 0.35f;
            ImU32 lineColor = ImGui::ColorConvertFloat4ToU32(lineCol);
            ImU32 fillColor = ImGui::ColorConvertFloat4ToU32(fillCol);
            ImU32 nowColor = ImGui::GetColorU32(ImGuiCol_PlotHistogramHovered);
            ImU32 vfoColor = ImGui::ColorConvertFloat4ToU32(gui::themeManager.vfoSelectedLineColor);
            ImU32 hoverColor = ImGui::ColorConvertFloat4ToU32(ImVec4(1.0f, 1.0f, 1.0f, 0.08f));

            auto yOf = [&](float pct) {
                return plotMax.y - (std::clamp<float>(pct, 0.0f, 100.0f) / 100.0f) * (plotMax.y - plotMin.y);
            };
            auto xOf = [&](double freq) {
                return plotMin.x + (float)((freq - viewLo) / viewSpan) * plotW;
            };

            // Percentage grid, labelled down the left
            ImGui::PushFont(style::tinyFont);
            for (int pct = 0; pct <= 100; pct += 25) {
                float y = yOf((float)pct);
                draw->AddLine(ImVec2(plotMin.x, y), ImVec2(plotMax.x, y), gridColor, 1.0f);
                if (pct % 50 != 0) { continue; }
                char buf[8];
                snprintf(buf, sizeof buf, "%d%%", pct);
                ImVec2 sz = ImGui::CalcTextSize(buf);
                draw->AddText(ImVec2(plotMin.x - sz.x - (3.0f * style::uiScale), y - (sz.y / 2.0f)), textColor, buf);
            }

            // Frequency grid on the spectrum's own steps, labels skipped where they
            // would run into the one before.
            if (viewSpan > 0.0) {
                float labelSlot = ImGui::CalcTextSize("000.000M").x + (10.0f * style::uiScale);
                double step = bestGridStep(viewSpan, std::max<int>(2, (int)(plotW / labelSlot)));
                float lastLabelEnd = -1e9f;
                for (double f = ceil(viewLo / step) * step; f < viewLo + viewSpan; f += step) {
                    float x = roundf(xOf(f));
                    draw->AddLine(ImVec2(x, plotMin.y), ImVec2(x, plotMax.y), gridColor, 1.0f);
                    char buf[24];
                    formatFreq(f, buf, sizeof buf);
                    ImVec2 sz = ImGui::CalcTextSize(buf);
                    float lx = std::clamp<float>(x - (sz.x / 2.0f), plotMin.x, plotMax.x - sz.x);
                    if (lx < lastLabelEnd) { continue; }
                    draw->AddText(ImVec2(lx, plotMax.y + (2.0f * style::uiScale)), textColor, buf);
                    lastLabelEnd = lx + sz.x + (6.0f * style::uiScale);
                }
            }
            ImGui::PopFont();

            // The occupancy itself, as a filled step plot: one flat step per channel, so
            // it reads as a level across frequency rather than as separate stations.
            int64_t hovered = INT64_MIN;
            double hoveredA = 0.0;
            int visibleSamples = 0;
            ImVec2 mouse = ImGui::GetMousePos();
            bool mouseInPlot = ImGui::IsWindowHovered() && mouse.x >= plotMin.x && mouse.x < plotMax.x && mouse.y >= plotMin.y && mouse.y < plotMax.y;
            if (viewSpan > 0.0 && channelWidth > 0.0) {
                int64_t first = (int64_t)ceil(viewLo / channelWidth);
                int64_t last = (int64_t)floor((viewLo + viewSpan) / channelWidth) - 1;
                float stripH = std::max<float>(2.0f, 3.0f * style::uiScale);
                bool havePrev = false;
                float prevY = 0.0f;
                // What the pointer is on. A narrow signal is a spike a few pixels wide,
                // which is a poor thing to have to hit, so the pointer snaps to the
                // busiest channel within reach and only falls back to the one directly
                // under it when nothing nearby has been busy.
                float reach = 8.0f * style::uiScale;
                float bestPct = 0.0f;
                float bestDist = 1e9f;
                float hoverX0 = 0.0f;
                float hoverX1 = 0.0f;
                if (mouseInPlot) {
                    for (int64_t idx = first; idx <= last; idx++) {
                        double a = (double)idx * channelWidth;
                        float x0 = std::max<float>(xOf(a), plotMin.x);
                        float x1 = std::min<float>(xOf(a + channelWidth), plotMax.x);
                        float dist = (mouse.x < x0) ? (x0 - mouse.x) : ((mouse.x >= x1) ? (mouse.x - x1) : 0.0f);
                        if (dist > reach) { continue; }
                        auto it = channels.find(idx);
                        float pct = (it != channels.end() && it->second.samples > 0) ? (100.0f * (float)it->second.busy / (float)it->second.samples) : 0.0f;
                        bool under = (dist == 0.0f);
                        bool better = (pct > bestPct) || (pct == bestPct && dist < bestDist) || (hovered == INT64_MIN && under);
                        if (pct == 0.0f && !under && bestPct == 0.0f) { better = false; }
                        if (better) {
                            hovered = idx;
                            hoveredA = a;
                            bestPct = pct;
                            bestDist = dist;
                            hoverX0 = x0;
                            hoverX1 = x1;
                        }
                    }
                    if (hovered != INT64_MIN) {
                        draw->AddRectFilled(ImVec2(hoverX0, plotMin.y + 1.0f), ImVec2(std::max<float>(hoverX1, hoverX0 + 1.0f), plotMax.y), hoverColor);
                    }
                }

                for (int64_t idx = first; idx <= last; idx++) {
                    double a = (double)idx * channelWidth;
                    float x0 = std::max<float>(xOf(a), plotMin.x);
                    float x1 = std::min<float>(xOf(a + channelWidth), plotMax.x);
                    auto it = channels.find(idx);
                    if (it == channels.end() || it->second.samples == 0) {
                        havePrev = false;
                        continue;
                    }
                    const Channel& ch = it->second;
                    visibleSamples = std::max<int>(visibleSamples, ch.samples);
                    float pct = 100.0f * (float)ch.busy / (float)ch.samples;
                    float y = yOf(pct);
                    if (pct > 0.0f) {
                        draw->AddRectFilled(ImVec2(x0, y), ImVec2(x1, plotMax.y), fillColor);
                    }
                    if (havePrev && prevY != y) {
                        draw->AddLine(ImVec2(x0, prevY), ImVec2(x0, y), lineColor, 1.5f);
                    }
                    draw->AddLine(ImVec2(x0, y), ImVec2(x1, y), lineColor, 1.5f);
                    havePrev = true;
                    prevY = y;
                    // A strip along the bottom for channels busy at this moment, so the
                    // plot also reads as a live view and not only as a history.
                    if (ch.busyNow && ch.lastTick == tickNo) {
                        draw->AddRectFilled(ImVec2(x0, plotMax.y - stripH), ImVec2(x1, plotMax.y), nowColor);
                    }
                }
            }

            // Where the selected VFO is, so a click can be judged against it.
            auto vfoIt = gui::waterfall.vfos.find(gui::waterfall.selectedVFO);
            if (vfoIt != gui::waterfall.vfos.end() && vfoIt->second && viewSpan > 0.0) {
                double vf = gui::waterfall.getCenterFrequency() + vfoIt->second->generalOffset;
                if (vf >= viewLo && vf <= viewLo + viewSpan) {
                    float x = roundf(xOf(vf));
                    draw->AddLine(ImVec2(x, plotMin.y), ImVec2(x, plotMax.y), vfoColor, 1.0f);
                }
            }

            draw->AddRect(plotMin, plotMax, borderColor);

            // Header: what it is, what it has counted over, and what "busy" means here.
            // The detail goes in only as far as it fits before the Reset button - the
            // long form, a short form, or the name alone - rather than running under it.
            float resetWidth = ImGui::CalcTextSize("Reset").x + (ImGui::GetStyle().FramePadding.x * 2.0f);
            float textRoom = avail.x - resetWidth - ImGui::GetStyle().ItemSpacing.x;
            ImGui::SetCursorScreenPos(origin);
            if (visibleSamples == 0) {
                const char* status = gui::mainWindow.sdrIsRunning() ? "Occupancy   collecting" : "Occupancy   radio stopped";
                if (ImGui::CalcTextSize(status).x <= textRoom) { ImGui::TextDisabled("%s", status); }
                else { ImGui::TextDisabled("Occupancy"); }
            }
            else {
                int secs = (int)(visibleSamples * SAMPLE_PERIOD);
                float nameW = ImGui::CalcTextSize("Occupancy").x + ImGui::GetStyle().ItemSpacing.x;
                char longForm[128];
                char shortForm[64];
                snprintf(longForm, sizeof longForm, "over %dm %02ds, busy = %d dB above the %.0f dB floor, click to tune", secs / 60, secs % 60, thresholdDb, lastFloorDb);
                snprintf(shortForm, sizeof shortForm, "%dm%02ds  >%d dB", secs / 60, secs % 60, thresholdDb);
                ImGui::TextUnformatted("Occupancy");
                ImGui::PushFont(style::tinyFont);
                const char* detail = NULL;
                if (nameW + ImGui::CalcTextSize(longForm).x <= textRoom) { detail = longForm; }
                else if (nameW + ImGui::CalcTextSize(shortForm).x <= textRoom) { detail = shortForm; }
                if (detail) {
                    ImGui::SameLine();
                    ImGui::TextDisabled("%s", detail);
                }
                ImGui::PopFont();
            }
            ImGui::SetCursorScreenPos(ImVec2(origin.x + avail.x - resetWidth, origin.y));
            if (ImGui::SmallButton("Reset##_occupancy_reset")) {
                resetStats();
            }

            if (hovered != INT64_MIN) {
                auto it = channels.find(hovered);
                const Channel* ch = (it != channels.end()) ? &it->second : nullptr;
                double target = (ch && ch->everBusy) ? ch->peakFreq : hoveredA + (channelWidth / 2.0);
                std::string a = hrfreq::toString(hoveredA);
                std::string b = hrfreq::toString(hoveredA + channelWidth);
                std::string t = hrfreq::toString(target);
                if (ch && ch->samples > 0) {
                    float pct = 100.0f * (float)ch->busy / (float)ch->samples;
                    style::tooltip("%s - %s\n%.0f%% busy%s\nClick to tune to %s", a.c_str(), b.c_str(), pct,
                                   (ch->busyNow && ch->lastTick == tickNo) ? ", busy now" : "", t.c_str());
                }
                else {
                    style::tooltip("%s - %s\nNot counted yet\nClick to tune to %s", a.c_str(), b.c_str(), t.c_str());
                }
                if (ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
                    // As a bookmark does: the VFO moves if the frequency is on screen,
                    // otherwise the receiver retunes to bring it there.
                    tuner::tune(tuner::TUNER_MODE_NORMAL, gui::waterfall.selectedVFO, target);
                }
            }
        }

    private:
        std::unordered_map<int64_t, Channel> channels;
        double channelWidth = 0.0;
        double viewLo = 0.0;
        double viewSpan = 0.0;
        uint64_t tickNo = 0;
        float lastFloorDb = 0.0f;
        double lastSample = 0.0;
        std::vector<float> line;
        std::vector<float> sorted;
    };

    OccupancyPanel panel;
    EventHandler<ImGuiContext*> waterfallDrawnHandler;
}

namespace occupancy {
    void init() {
        core::configManager.acquire();
        if (core::configManager.conf.contains("showOccupancy")) {
            panel.shown = core::configManager.conf["showOccupancy"];
        }
        if (core::configManager.conf.contains("occupancyThresholdDb")) {
            int saved = core::configManager.conf["occupancyThresholdDb"];
            panel.thresholdDb = std::clamp(saved, MIN_THRESHOLD_DB, MAX_THRESHOLD_DB);
        }
        core::configManager.release();

        waterfallDrawnHandler.ctx = &panel;
        waterfallDrawnHandler.handler = [](ImGuiContext* gctx, void* ctx) {
            ((OccupancyPanel*)ctx)->tick();
        };
        gui::mainWindow.onWaterfallDrawn.bindHandler(&waterfallDrawnHandler);
    }

    bool isShown() { return panel.shown; }

    void setShown(bool shown) {
        panel.shown = shown;
        core::configManager.acquire();
        core::configManager.conf["showOccupancy"] = shown;
        core::configManager.release(true);
    }

    int getThresholdDb() { return panel.thresholdDb; }

    void setThresholdDb(int db) {
        db = std::clamp(db, MIN_THRESHOLD_DB, MAX_THRESHOLD_DB);
        if (db == panel.thresholdDb) { return; }
        panel.thresholdDb = db;
        // A different threshold is a different question; the counts so far answered
        // the old one.
        panel.resetStats();
        core::configManager.acquire();
        core::configManager.conf["occupancyThresholdDb"] = db;
        core::configManager.release(true);
    }
}
