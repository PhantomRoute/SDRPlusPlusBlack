#include <gui/menus/bandplan.h>
#include <gui/widgets/bandplan.h>
#include <gui/gui.h>
#include <core.h>
#include <gui/style.h>
#include <algorithm>

namespace bandplanmenu {
    int bandplanId;
    bool bandPlanEnabled;
    int bandPlanPos = 1; // Top, matching the default in core.cpp

    const char* bandPlanPosTxt = "Bottom\0Top\0";

    void init() {
        // todo: check if the bandplan wasn't removed
        if (bandplan::bandplanNames.size() == 0) {
            gui::waterfall.hideBandplan();
            return;
        }

        if (bandplan::bandplans.find(core::configManager.conf["bandPlan"]) != bandplan::bandplans.end()) {
            std::string name = core::configManager.conf["bandPlan"];
            bandplanId = std::distance(bandplan::bandplanNames.begin(), std::find(bandplan::bandplanNames.begin(),
                                                                                  bandplan::bandplanNames.end(), name));
            gui::waterfall.bandplan = &bandplan::bandplans[name];
        }
        else {
            gui::waterfall.bandplan = &bandplan::bandplans[bandplan::bandplanNames[0]];
        }

        bandPlanEnabled = core::configManager.conf["bandPlanEnabled"];
        bandPlanEnabled ? gui::waterfall.showBandplan() : gui::waterfall.hideBandplan();
        bandPlanPos = core::configManager.conf["bandPlanPos"];
        gui::waterfall.setBandPlanPos(bandPlanPos);
    }

    void draw(void* ctx) {
        float menuColumnWidth = ImGui::GetContentRegionAvail().x;

        // init() gives up when there are none, but this ran anyway and indexed the
        // empty list of names at the bottom.
        if (bandplan::bandplanNames.empty()) {
            ImGui::PushTextWrapPos(0.0f);
            ImGui::TextDisabled("No band plans are installed. They live in the bandplans folder of the resources directory.");
            ImGui::PopTextWrapPos();
            return;
        }
        bandplanId = std::clamp<int>(bandplanId, 0, (int)bandplan::bandplanNames.size() - 1);

        // The switch that decides whether any of this is drawn at all used to be
        // underneath the two settings it governs.
        if (ImGui::Checkbox("Show band plan on the spectrum", &bandPlanEnabled)) {
            bandPlanEnabled ? gui::waterfall.showBandplan() : gui::waterfall.hideBandplan();
            core::configManager.acquire();
            core::configManager.conf["bandPlanEnabled"] = bandPlanEnabled;
            core::configManager.release(true);
        }

        if (!bandPlanEnabled) { style::beginDisabled(); }

        // An unlabelled combo of country names, with nothing to say what picking
        // one does.
        ImGui::LeftLabel("Plan");
        ImGui::SetNextItemWidth(menuColumnWidth - ImGui::GetCursorPosX());
        if (ImGui::Combo("##_bandplan_name_", &bandplanId, bandplan::bandplanNameTxt.c_str())) {
            gui::waterfall.bandplan = &bandplan::bandplans[bandplan::bandplanNames[bandplanId]];
            core::configManager.acquire();
            core::configManager.conf["bandPlan"] = bandplan::bandplanNames[bandplanId];
            core::configManager.release(true);
        }
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
            ImGui::SetTooltip("Which country's allocations to draw along the spectrum. Pick the one\nyou are listening in; they differ between regions.");
        }

        ImGui::LeftLabel("Position");
        ImGui::SetNextItemWidth(menuColumnWidth - ImGui::GetCursorPosX());
        if (ImGui::Combo("##_bandplan_pos_", &bandPlanPos, bandPlanPosTxt)) {
            gui::waterfall.setBandPlanPos(bandPlanPos);
            core::configManager.acquire();
            core::configManager.conf["bandPlanPos"] = bandPlanPos;
            core::configManager.release(true);
        }
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
            ImGui::SetTooltip("Which edge of the spectrum the band strip is drawn along");
        }

        if (!bandPlanEnabled) { style::endDisabled(); }

        bandplan::BandPlan_t plan = bandplan::bandplans[bandplan::bandplanNames[bandplanId]];
        ImGui::TextDisabled("%s (%s), by %s", plan.countryName.c_str(), plan.countryCode.c_str(), plan.authorName.c_str());
    }
};