#pragma once
#include <imgui.h>
#include <gui/style.h>
#include <string>
#include <cstdio>

// Shared bits of the DSD status panels. Both demodulators report the same kind of
// thing - a signal level, whether it is synced, and how badly the voice frames are
// coming through - so they may as well look and behave the same.
namespace demod {

    // Eases a bar toward its reading so it shows a level rather than flickering once
    // per decoded frame. Hand rolled rather than std::min/std::max: the windows.h
    // macros make those unusable in this module.
    inline float approachValue(float current, float target, float perSecond) {
        float step = ImGui::GetIO().DeltaTime * perSecond;
        if (step > 1.0f) { step = 1.0f; }
        if (step < 0.0f) { step = 0.0f; }
        return current + ((target - current) * step);
    }

    // Input level as a bar rather than a bare percentage - the useful question is
    // "is it in range", which a number makes you work out. Amber below 15% because
    // that starves the slicer, red above 95% because it is close to clipping.
    inline void drawLevelBar(int percent, float& smoothed) {
        float level = (float)percent / 100.0f;
        if (level > 1.0f) { level = 1.0f; }
        if (level < 0.0f) { level = 0.0f; }
        smoothed = approachValue(smoothed, level, 10.0f);

        ImVec4 color = ImVec4(0.4f, 0.8f, 0.4f, 1.0f);
        if (percent < 15) { color = ImVec4(1.0f, 0.6f, 0.3f, 1.0f); }
        else if (percent > 95) { color = ImVec4(1.0f, 0.4f, 0.3f, 1.0f); }

        char overlay[32];
        snprintf(overlay, sizeof(overlay), "%d%%", percent);
        ImGui::LeftLabel("Level");
        ImGui::PushStyleColor(ImGuiCol_PlotHistogram, color);
        ImGui::ProgressBar(smoothed, ImVec2(ImGui::GetContentRegionAvail().x, 0), overlay);
        ImGui::PopStyleColor();
    }

    // mbelib writes one '=' per corrected bit error, then a letter if the frame was
    // an erasure (E), a tone (T), repeated because it had more than three errors (R),
    // or muted after too many repeats (M). A lengthening row of '=' carries all of
    // that and shows none of it, so read the count out as a quality bar - full and
    // green is clean - and say what the letter meant. The raw string stays on the
    // tooltip for anyone who reads mbelib output directly.
    inline void drawVoiceQualityBar(const std::string& errorbar, bool synced, float& smoothed, const std::string& idSuffix) {
        int errors = 0;
        char flag = 0;
        for (char c : errorbar) {
            if (c == '=') { errors++; }
            else if (c != ' ') { flag = c; }
        }

        const char* verdict;
        ImVec4 color;
        if (!synced) {
            verdict = "no signal";
            color = ImVec4(0.5f, 0.5f, 0.5f, 1.0f);
        } else if (flag == 'M') {
            verdict = "muted, too many bad frames";
            color = ImVec4(1.0f, 0.3f, 0.3f, 1.0f);
        } else if (flag == 'R') {
            verdict = "frame repeated";
            color = ImVec4(1.0f, 0.5f, 0.3f, 1.0f);
        } else if (flag == 'E') {
            verdict = "erasure";
            color = ImVec4(1.0f, 0.5f, 0.3f, 1.0f);
        } else if (flag == 'T') {
            verdict = "tone";
            color = ImVec4(0.4f, 0.7f, 1.0f, 1.0f);
        } else if (errors == 0) {
            verdict = "clean";
            color = ImVec4(0.3f, 1.0f, 0.3f, 1.0f);
        } else if (errors <= 4) {
            verdict = "good";
            color = ImVec4(0.5f, 1.0f, 0.4f, 1.0f);
        } else if (errors <= 10) {
            verdict = "fair";
            color = ImVec4(1.0f, 0.9f, 0.3f, 1.0f);
        } else {
            verdict = "poor";
            color = ImVec4(1.0f, 0.4f, 0.3f, 1.0f);
        }

        // Full bar is clean. The scale is a rough one: mbelib repeats a frame past
        // three errors and a voice frame carries a handful of them, so 16 stands in
        // for "every frame at the repeat threshold". Protocols pack different numbers
        // of AMBE frames per voice frame, so this is an indicator, not a measurement.
        float quality = 1.0f - ((float)errors / 16.0f);
        if (quality < 0.0f) { quality = 0.0f; }
        if (!synced) { quality = 0.0f; }
        smoothed = approachValue(smoothed, quality, 6.0f);

        char overlay[64];
        if (errors > 0) { snprintf(overlay, sizeof(overlay), "%s - %d bit errors", verdict, errors); }
        else { snprintf(overlay, sizeof(overlay), "%s", verdict); }

        ImGui::LeftLabel("Voice");
        ImGui::PushStyleColor(ImGuiCol_PlotHistogram, color);
        ImGui::ProgressBar(smoothed, ImVec2(ImGui::GetContentRegionAvail().x, 0), overlay);
        ImGui::PopStyleColor();
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip("mbelib: %s\n'=' corrected bit error, E erasure, T tone, R repeat, M muted",
                              errorbar.empty() ? "(none)" : errorbar.c_str());
        }
        (void)idSuffix;
    }

    // The sync line both panels lead with.
    inline void drawSyncLine(bool synced, const std::string& protoName) {
        ImVec4 color = synced ? ImVec4(0.3f, 1.0f, 0.3f, 1.0f) : ImVec4(1.0f, 0.4f, 0.4f, 1.0f);
        ImGui::TextColored(color, "%s  %s", synced ? "SYNC" : "no sync", protoName.c_str());
    }
}
