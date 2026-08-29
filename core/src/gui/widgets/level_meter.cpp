#include <gui/widgets/level_meter.h>
#include <gui/style.h>
#include <imgui/imgui_internal.h>
#include <algorithm>
#include <cmath>

namespace ImGui {

    LevelMeterResult LevelMeter(const char* id, float minDb, float maxDb,
                                bool haveLevel, float level, float peak, bool open,
                                float* threshold, const LevelMeterStyle& look) {
        LevelMeterResult res;
        if (threshold == NULL || !(maxDb > minDb)) { return res; }

        float width = GetContentRegionAvail().x;
        if (width < 16.0f) { return res; }
        float height = 18.0f * style::uiScale;
        float range = maxDb - minDb;

        ImVec2 pos = GetCursorScreenPos();
        InvisibleButton(id, ImVec2(width, height));

        // Anywhere on the bar, not a grab handle: the threshold is being set against
        // what the bar is showing, so the gesture is "put it here", and a handle a few
        // pixels wide is a poor target for the finger on a phone.
        if (IsItemActive()) {
            float ratio = std::clamp<float>((GetIO().MousePos.x - pos.x) / width, 0.0f, 1.0f);
            float value = std::clamp<float>(minDb + (ratio * range), minDb, maxDb);
            if (value != *threshold) {
                *threshold = value;
                res.changed = true;
            }
        }
        res.released = IsItemDeactivated();
        res.hovered = IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled);

        // style::beginDisabled() dims by pushing the standard colours rather than by
        // touching the global alpha, so the colours drawn straight from the theme have
        // to be dimmed here or the bar stays bright inside a greyed out control.
        bool disabled = (GImGui->CurrentItemFlags & ImGuiItemFlags_Disabled) != 0;
        auto col = [&](ImVec4 c) {
            if (disabled) { c.w *= 0.4f; }
            return ColorConvertFloat4ToU32(c);
        };

        ImDrawList* draw = GetWindowDrawList();
        ImVec2 boxMin = pos;
        ImVec2 boxMax = ImVec2(pos.x + width, pos.y + height);
        draw->AddRectFilled(boxMin, boxMax, GetColorU32(ImGuiCol_FrameBg), 2.0f);

        auto dbToX = [&](float db) {
            return boxMin.x + (std::clamp<float>((db - minDb) / range, 0.0f, 1.0f) * width);
        };

        if (haveLevel) {
            draw->AddRectFilled(boxMin, ImVec2(dbToX(level), boxMax.y),
                                col(open ? look.fillOpen : look.fillClosed), 2.0f);

            // Peak hold, so a short burst that opened the gate is still visible after
            // the level has fallen back.
            if (peak > level) {
                float peakX = dbToX(peak);
                draw->AddLine(ImVec2(peakX, boxMin.y + 1.0f), ImVec2(peakX, boxMax.y - 1.0f),
                              GetColorU32(ImGuiCol_Text), 1.0f);
            }
        }

        ImU32 tickColor = GetColorU32(ImGuiCol_Border);
        if (look.tickStepDb > 0.0f) {
            for (float db = minDb + look.tickStepDb; db < maxDb; db += look.tickStepDb) {
                float x = dbToX(db);
                draw->AddLine(ImVec2(x, boxMax.y - (4.0f * style::uiScale)), ImVec2(x, boxMax.y), tickColor, 1.0f);
            }
        }

        // Full height, so it can be compared with the fill at a glance, with a marker on
        // top for when the fill has covered it.
        float triggerX = dbToX(*threshold);
        ImU32 triggerColor = col(look.threshold);
        draw->AddLine(ImVec2(triggerX, boxMin.y), ImVec2(triggerX, boxMax.y), triggerColor, 2.0f * style::uiScale);
        draw->AddTriangleFilled(ImVec2(triggerX - (4.0f * style::uiScale), boxMin.y),
                                ImVec2(triggerX + (4.0f * style::uiScale), boxMin.y),
                                ImVec2(triggerX, boxMin.y + (5.0f * style::uiScale)), triggerColor);
        draw->AddRect(boxMin, boxMax, tickColor, 2.0f);

        if (haveLevel) {
            Text("%s %.1f dB", look.caption, level);
        }
        else {
            TextDisabled("%s --.- dB", look.caption);
        }
        SameLine();
        if (look.floorCaption != NULL && *threshold <= minDb) {
            TextDisabled("%s", look.floorCaption);
        }
        else {
            TextDisabled("| opens at %.1f dB", *threshold);
        }

        return res;
    }
}
