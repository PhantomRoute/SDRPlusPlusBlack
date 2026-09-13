#ifndef IMGUI_DEFINE_MATH_OPERATORS
#define IMGUI_DEFINE_MATH_OPERATORS
#endif

#include <gui/widgets/occupancy_panel.h>
#include <imgui.h>
#include <imgui/imgui_internal.h>
#include <gui/gui.h>
#include <gui/style.h>
#include <core.h>
#include <utils/event.h>
#include <utils/hrfreq.h>
#include <vector>
#include <algorithm>
#include <cmath>

namespace {
    // How often the spectrum is looked at. Twenty times a second is well above how
    // fast anything worth counting comes and goes, and keeps the cost negligible.
    const double SAMPLE_PERIOD = 0.05;

    // Channels follow the width of the panel, one bar per this many unscaled pixels,
    // so a wide panel shows finer channels rather than fatter bars.
    const float BAR_TARGET_WIDTH = 26.0f;
    const int MIN_CHANNELS = 8;
    const int MAX_CHANNELS = 96;

    const int MIN_THRESHOLD_DB = 3;
    const int MAX_THRESHOLD_DB = 60;

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
            busyCount.clear();
            busyNow.clear();
            samples = 0;
            spanWidth = 0.0;
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

            double center = gui::waterfall.getCenterFrequency() + gui::waterfall.getViewOffset();
            double width = gui::waterfall.getViewBandwidth();
            int channels = std::clamp(wantedChannels, MIN_CHANNELS, MAX_CHANNELS);

            // Copy the line out and let go at once: the same lock guards the FFT update.
            int pixels = 0;
            float* fft = gui::waterfall.acquireLatestFFT(pixels);
            if (!fft) { return; }
            if (pixels < channels) {
                gui::waterfall.releaseLatestFFT();
                return;
            }
            line.assign(fft, fft + pixels);
            gui::waterfall.releaseLatestFFT();

            // A different view is a different set of channels. Counts gathered for
            // frequencies no longer on screen would be credited to the wrong ones.
            if (center != spanCenter || width != spanWidth || channels != (int)busyCount.size()) {
                spanCenter = center;
                spanWidth = width;
                busyCount.assign(channels, 0);
                busyNow.assign(channels, false);
                samples = 0;
            }

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

            float busyAbove = floorDb + (float)thresholdDb;
            for (int c = 0; c < channels; c++) {
                int a = (int)(((int64_t)c * pixels) / channels);
                int b = (int)(((int64_t)(c + 1) * pixels) / channels);
                float peak = -1000.0f;
                for (int i = a; i < b; i++) {
                    if (line[i] > peak) { peak = line[i]; }
                }
                bool busy = peak > busyAbove;
                busyNow[c] = busy;
                if (busy) { busyCount[c]++; }
            }
            samples++;
        }

        void draw() {
            ImVec2 avail = ImGui::GetContentRegionAvail();
            if (avail.x < 80.0f || avail.y < 60.0f) { return; }

            // Tell the sampling how many channels this width wants. It resets the
            // counts itself when that changes.
            wantedChannels = std::clamp((int)(avail.x / (BAR_TARGET_WIDTH * style::uiScale)), MIN_CHANNELS, MAX_CHANNELS);

            ImGui::PushFont(style::tinyFont);
            float labelWidth = ImGui::CalcTextSize("100%").x + (4.0f * style::uiScale);
            float axisHeight = ImGui::GetTextLineHeight() + (3.0f * style::uiScale);
            ImGui::PopFont();
            float headerHeight = ImGui::GetFrameHeightWithSpacing();

            ImVec2 origin = ImGui::GetCursorScreenPos();
            ImVec2 plotMin(origin.x + labelWidth, origin.y + headerHeight);
            ImVec2 plotMax(origin.x + avail.x, origin.y + avail.y - axisHeight);
            if (plotMax.x - plotMin.x < 20.0f || plotMax.y - plotMin.y < 20.0f) { return; }

            ImDrawList* draw = ImGui::GetWindowDrawList();
            ImU32 gridColor = ImGui::ColorConvertFloat4ToU32(gui::themeManager.fftGridColor);
            ImU32 borderColor = ImGui::ColorConvertFloat4ToU32(gui::themeManager.fftBorderColor);
            ImU32 textColor = ImGui::GetColorU32(ImGuiCol_Text);
            ImU32 barColor = ImGui::GetColorU32(ImGuiCol_PlotHistogram);
            ImU32 barNowColor = ImGui::GetColorU32(ImGuiCol_PlotHistogramHovered);

            auto yOf = [&](float pct) {
                return plotMax.y - (std::clamp(pct, 0.0f, 100.0f) / 100.0f) * (plotMax.y - plotMin.y);
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
            ImGui::PopFont();
            draw->AddRect(plotMin, plotMax, borderColor);

            int channels = (int)busyCount.size();
            int hovered = -1;
            if (channels > 0 && samples > 0) {
                float colWidth = (plotMax.x - plotMin.x) / (float)channels;
                float gap = (colWidth >= 6.0f) ? std::max<float>(1.0f, floorf(colWidth * 0.15f)) : 0.0f;
                ImVec2 mouse = ImGui::GetMousePos();
                bool mouseInPlot = ImGui::IsWindowHovered() && mouse.x >= plotMin.x && mouse.x < plotMax.x && mouse.y >= plotMin.y && mouse.y < plotMax.y;

                for (int c = 0; c < channels; c++) {
                    float x0 = plotMin.x + (float)c * colWidth;
                    float x1 = x0 + colWidth;
                    if (mouseInPlot && mouse.x >= x0 && mouse.x < x1) {
                        hovered = c;
                        draw->AddRectFilled(ImVec2(x0, plotMin.y + 1.0f), ImVec2(x1, plotMax.y), gridColor);
                    }
                    float pct = 100.0f * (float)busyCount[c] / (float)samples;
                    if (pct <= 0.0f) { continue; }
                    // Brighter while the channel is busy right now, so the bars also
                    // read as a live view and not only as a history.
                    draw->AddRectFilled(ImVec2(x0 + gap, yOf(pct)), ImVec2(x1 - gap, plotMax.y),
                                        busyNow[c] ? barNowColor : barColor);
                }
            }

            // Frequency axis: the edges and the middle of the span being counted, each
            // only if it fits. A narrow panel gets fewer labels, not labels on top of
            // one another.
            if (spanWidth > 0.0) {
                ImGui::PushFont(style::tinyFont);
                double lo = spanCenter - (spanWidth / 2.0);
                double hi = spanCenter + (spanWidth / 2.0);
                std::string loStr = hrfreq::toString(lo);
                std::string midStr = hrfreq::toString(spanCenter);
                std::string hiStr = hrfreq::toString(hi);
                float loW = ImGui::CalcTextSize(loStr.c_str()).x;
                float midW = ImGui::CalcTextSize(midStr.c_str()).x;
                float hiW = ImGui::CalcTextSize(hiStr.c_str()).x;
                float plotW = plotMax.x - plotMin.x;
                float gap = 8.0f * style::uiScale;
                float ty = plotMax.y + (2.0f * style::uiScale);
                bool edges = (loW + hiW + gap) <= plotW;
                bool middle = edges ? ((loW + midW + hiW + (2.0f * gap)) <= plotW) : (midW <= plotW);
                if (edges) {
                    draw->AddText(ImVec2(plotMin.x, ty), textColor, loStr.c_str());
                    draw->AddText(ImVec2(plotMax.x - hiW, ty), textColor, hiStr.c_str());
                }
                if (middle) {
                    draw->AddText(ImVec2(((plotMin.x + plotMax.x) - midW) / 2.0f, ty), textColor, midStr.c_str());
                }
                ImGui::PopFont();
            }

            // Header: what it is, what it has counted over, and what "busy" means here.
            // The detail goes in only as far as it fits before the Reset button - the
            // long form, a short form, or the name alone - rather than running under it.
            float resetWidth = ImGui::CalcTextSize("Reset").x + (ImGui::GetStyle().FramePadding.x * 2.0f);
            float textRoom = avail.x - resetWidth - ImGui::GetStyle().ItemSpacing.x;
            ImGui::SetCursorScreenPos(origin);
            if (samples == 0) {
                const char* status = gui::mainWindow.sdrIsRunning() ? "Occupancy   collecting" : "Occupancy   radio stopped";
                if (ImGui::CalcTextSize(status).x <= textRoom) { ImGui::TextDisabled("%s", status); }
                else { ImGui::TextDisabled("Occupancy"); }
            }
            else {
                int secs = (int)(samples * SAMPLE_PERIOD);
                float nameW = ImGui::CalcTextSize("Occupancy").x + ImGui::GetStyle().ItemSpacing.x;
                char longForm[128];
                char shortForm[64];
                snprintf(longForm, sizeof longForm, "over %dm %02ds, busy = %d dB above the %.0f dB floor", secs / 60, secs % 60, thresholdDb, lastFloorDb);
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

            if (hovered >= 0 && hovered < channels && samples > 0) {
                double lo = spanCenter - (spanWidth / 2.0);
                double chWidth = spanWidth / (double)channels;
                std::string a = hrfreq::toString(lo + chWidth * hovered);
                std::string b = hrfreq::toString(lo + chWidth * (hovered + 1));
                float pct = 100.0f * (float)busyCount[hovered] / (float)samples;
                style::tooltip("%s - %s\n%.0f%% busy%s", a.c_str(), b.c_str(), pct, busyNow[hovered] ? ", busy now" : "");
            }
        }

        int wantedChannels = 32;

    private:
        std::vector<int> busyCount;
        std::vector<bool> busyNow;
        int samples = 0;
        double spanCenter = 0.0;
        double spanWidth = 0.0;
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
