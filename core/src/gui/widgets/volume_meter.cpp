// Must come before anything that pulls in imgui_internal.h, which is where the ImVec2
// operators used below are defined.
#ifndef IMGUI_DEFINE_MATH_OPERATORS
#define IMGUI_DEFINE_MATH_OPERATORS
#endif

#include <gui/widgets/volume_meter.h>
#include <algorithm>
#include <gui/gui.h>
#include <imgui/imgui_internal.h>

namespace ImGui {
    void VolumeMeter(float avg, float peak, float val_min, float val_max, const ImVec2& size_arg) {
        ImGuiWindow* window = GetCurrentWindow();
        ImGuiStyle& style = GImGui->Style;

        avg = std::clamp<float>(avg, val_min, val_max);
        peak = std::clamp<float>(peak, val_min, val_max);

        ImVec2 min = window->DC.CursorPos;
        ImVec2 size = CalcItemSize(size_arg, CalcItemWidth(), (GImGui->FontSize / 2) + style.FramePadding.y);
        ImRect bb(min, min + size);

        float lineHeight = size.y;

        ItemSize(size, style.FramePadding.y);
        if (!ItemAdd(bb, 0)) {
            return;
        }

        float zeroDb = roundf(((-val_min) / (val_max - val_min)) * size.x);

        ImU32 bgLow = ColorConvertFloat4ToU32(gui::themeManager.volumeMeterBgLow);
        ImU32 bgHigh = ColorConvertFloat4ToU32(gui::themeManager.volumeMeterBgHigh);
        ImU32 barLow = ColorConvertFloat4ToU32(gui::themeManager.volumeMeterLow);
        ImU32 barHigh = ColorConvertFloat4ToU32(gui::themeManager.volumeMeterHigh);

        window->DrawList->AddRectFilled(min, min + ImVec2(zeroDb, lineHeight), bgLow);
        window->DrawList->AddRectFilled(min + ImVec2(zeroDb, 0), min + ImVec2(size.x, lineHeight), bgHigh);

        float end = roundf(((avg - val_min) / (val_max - val_min)) * size.x);
        float endP = roundf(((peak - val_min) / (val_max - val_min)) * size.x);

        if (avg <= 0) {
            window->DrawList->AddRectFilled(min, min + ImVec2(end, lineHeight), barLow);
        }
        else {
            window->DrawList->AddRectFilled(min, min + ImVec2(zeroDb, lineHeight), barLow);
            window->DrawList->AddRectFilled(min + ImVec2(zeroDb, 0), min + ImVec2(end, lineHeight), barHigh);
        }

        ImU32 peakCol = ColorConvertFloat4ToU32(peak <= 0 ? gui::themeManager.volumeMeterPeakLow : gui::themeManager.volumeMeterPeakHigh);
        window->DrawList->AddLine(min + ImVec2(endP, -1), min + ImVec2(endP, lineHeight - 1), peakCol);
    }
}