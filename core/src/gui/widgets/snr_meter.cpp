// Must come before anything that pulls in imgui_internal.h, which is where the ImVec2
// operators used below are defined.
#ifndef IMGUI_DEFINE_MATH_OPERATORS
#define IMGUI_DEFINE_MATH_OPERATORS
#endif

#include <gui/widgets/volume_meter.h>
#include <algorithm>
#include <gui/style.h>
#include <gui/gui.h>
#include <imgui/imgui_internal.h>
#include "snr_meter.h"

namespace ImGui {
    Event<SNRMeterExtPoint> onSNRMeterExtPoint;

    void SNRMeter(float val, const ImVec2& size_arg) {
        ImGuiWindow* window = GetCurrentWindow();
        ImGuiStyle& style = GImGui->Style;

        ImVec2 min = window->DC.CursorPos;
        ImVec2 size = CalcItemSize(size_arg, CalcItemWidth(), 26);
        ImRect bb(min, min + size);

        ImU32 text = ImGui::GetColorU32(ImGuiCol_Text);

        ItemSize(size, style.FramePadding.y);
        if (!ItemAdd(bb, 0)) {
            return;
        }

        val = std::clamp<float>(val, 0, 100);
        float ratio = size.x / 90;
        float it = size.x / 9;
        char buf[32];
        float drawVal = (float) val * ratio;

        window->DrawList->AddRectFilled(min + ImVec2(0, 1), min + ImVec2(roundf(drawVal), 10 * style::uiScale), ColorConvertFloat4ToU32(gui::themeManager.snrMeterColor));
        window->DrawList->AddLine(min, min + ImVec2(0, (10.0f * style::uiScale) - 1), text, style::uiScale);
        window->DrawList->AddLine(min + ImVec2(0, (10.0f * style::uiScale) - 1), min + ImVec2(size.x + 1, (10.0f * style::uiScale) - 1), text, style::uiScale);

        for (int i = 0; i < 10; i++) {
            window->DrawList->AddLine(min + ImVec2(roundf((float)i * it), (10.0f * style::uiScale) - 1), min + ImVec2(roundf((float)i * it), (15.0f * style::uiScale) - 1), text, style::uiScale);
            snprintf(buf, sizeof buf, "%d", i * 10);
            ImVec2 sz = ImGui::CalcTextSize(buf);
            window->DrawList->AddText(min + ImVec2(roundf(((float)i * it) - (sz.x / 2.0f)) + 1, 16.0f * style::uiScale), text, buf);
        }

        onSNRMeterExtPoint.emit(SNRMeterExtPoint(min + ImVec2(0, -min.y), drawVal));
    }
}