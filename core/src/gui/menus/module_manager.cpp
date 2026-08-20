#include <gui/menus/module_manager.h>
#include <imgui.h>
#include <core.h>
#include <string.h>
#include <gui/style.h>
#include <gui/dialogs/dialog_box.h>

namespace module_manager_menu {
    char modName[1024];
    std::vector<std::string> modTypes;
    std::string toBeRemoved;
    std::string modTypesTxt;
    std::string errorMessage;
    int modTypeId;
    bool confirmOpened = false;
    bool errorOpen = false;

    void init() {
        modName[0] = 0;

        modTypes.clear();
        modTypesTxt = "";
        for (auto& [name, mod] : core::moduleManager.modules) {
            modTypes.push_back(name);
            modTypesTxt += name;
            modTypesTxt += '\0';
        }
        modTypeId = 0;
    }

    // Why the + button is greyed out, or an empty string when it is not. The button
    // used to just be disabled with nothing said about it, leaving you to guess.
    static std::string addBlockedReason() {
        if (modTypes.empty()) { return "No modules are loaded."; }
        if (strlen(modName) == 0) { return "Type a name for the new instance first."; }
        if (core::moduleManager.instances.find(modName) != core::moduleManager.instances.end()) {
            return std::string("There is already an instance called \"") + modName + "\".";
        }
        std::string type = modTypes[modTypeId];
        int maxCount = core::moduleManager.modules[type].info->maxInstances;
        if (maxCount > 0 && core::moduleManager.countModuleInstances(type) >= maxCount) {
            return type + " allows " + std::to_string(maxCount) + (maxCount == 1 ? " instance." : " instances.");
        }
        return "";
    }

    void draw(void* ctx) {
        bool modified = false;

        // Calculate delete button size and cell size
        ImVec2 cellpad = ImGui::GetStyle().CellPadding;
        float lheight = ImGui::GetTextLineHeight();
        float cellWidth = lheight - (2.0f * cellpad.y);
        float hdiff = cellpad.x - cellpad.y;
        ImVec2 btnSize = ImVec2(lheight, lheight - 1);

        ImGui::TextDisabled("Running module instances");
        if (ImGui::IsItemHovered()) {
            style::tooltip("One row per copy of a module that is loaded: a radio, a recorder, a\n"
                              "decoder, a source. Unticking one shuts it down and takes its section\n"
                              "out of the menu without deleting its settings. Changes here are kept\n"
                              "across restarts.");
        }

        if (ImGui::BeginTable("Module Manager Table", 4, ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY, ImVec2(0, 200.0f * style::uiScale))) {
            ImGui::TableSetupColumn("On", ImGuiTableColumnFlags_WidthFixed, ImGui::GetFrameHeight());
            ImGui::TableSetupColumn("Name");
            ImGui::TableSetupColumn("Type");
            ImGui::TableSetupColumn("", ImGuiTableColumnFlags_WidthFixed, cellWidth);
            ImGui::TableSetupScrollFreeze(4, 1);
            ImGui::TableHeadersRow();

            for (auto& [name, inst] : core::moduleManager.instances) {
                ImGui::TableNextRow();

                ImGui::TableSetColumnIndex(0);
                bool instEnabled = inst.instance->isEnabled();
                if (ImGui::Checkbox(("##module_mgr_ena_" + name).c_str(), &instEnabled)) {
                    if (instEnabled) {
                        core::moduleManager.enableInstance(name);
                    }
                    else {
                        core::moduleManager.disableInstance(name);
                    }
                    modified = true;
                }

                ImGui::TableSetColumnIndex(1);
                if (instEnabled) {
                    ImGui::TextUnformatted(name.c_str());
                }
                else {
                    ImGui::TextDisabled("%s", name.c_str());
                }

                ImGui::TableSetColumnIndex(2);
                if (instEnabled) {
                    ImGui::TextUnformatted(inst.module.info->name);
                }
                else {
                    ImGui::TextDisabled("%s", inst.module.info->name);
                }

                ImGui::TableSetColumnIndex(3);
                // This button used to be blank, with the character "_" drawn on top
                // of it at a hand-picked offset to stand in for an icon.
                ImVec2 cpos = ImGui::GetCursorPos();
                ImGui::SetCursorPos(ImVec2(cpos.x - hdiff, cpos.y + 1));
                if (ImGui::Button(("x##module_mgr_" + name).c_str(), btnSize)) {
                    toBeRemoved = name;
                    confirmOpened = true;
                }
                if (ImGui::IsItemHovered()) { style::tooltip("Delete \"%s\" and its settings", name.c_str()); }
            }
            ImGui::EndTable();
        }

        if (ImGui::GenericDialog("module_mgr_confirm_", confirmOpened, GENERIC_DIALOG_BUTTONS_YES_NO, []() {
                ImGui::Text("Deleting \"%s\". Are you sure?", toBeRemoved.c_str());
            }) == GENERIC_DIALOG_BUTTON_YES) {
            core::moduleManager.deleteInstance(toBeRemoved);
            modified = true;
        }

        ImGui::GenericDialog("module_mgr_error_", errorOpen, GENERIC_DIALOG_BUTTONS_OK, []() {
            ImGui::TextUnformatted(errorMessage.c_str());
        });

        // Add module row with slightly different settings
        std::string addBlocked = addBlockedReason();
        if (ImGui::BeginTable("Module Manager Add Table", 3)) {
            ImGui::TableSetupColumn("Name");
            ImGui::TableSetupColumn("Type");
            ImGui::TableSetupColumn("", ImGuiTableColumnFlags_WidthFixed, cellWidth + cellpad.x);

            ImGui::TableNextRow();

            ImGui::TableSetColumnIndex(0);
            ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x + cellpad.x);
            ImGui::InputTextWithHint("##module_mod_name", "New instance name", modName, 1000);

            ImGui::TableSetColumnIndex(1);
            ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x + cellpad.x);
            ImGui::Combo("##module_mgr_type", &modTypeId, modTypesTxt.c_str());

            ImGui::TableSetColumnIndex(2);
            if (!addBlocked.empty()) { style::beginDisabled(); }
            if (ImGui::Button("+##module_mgr_add_btn", ImVec2(btnSize.x, 0))) {
                if (!core::moduleManager.createInstance(modName, modTypes[modTypeId])) {
                    core::moduleManager.postInit(modName);
                    modName[0] = 0;
                    modified = true;
                }
                else {
                    errorMessage = "Could not create an instance of " + modTypes[modTypeId] + ".\nThe log window has the reason.";
                    errorOpen = true;
                }
            }
            if (!addBlocked.empty()) { style::endDisabled(); }
            if (addBlocked.empty() && ImGui::IsItemHovered()) { style::tooltip("Add this instance"); }
            ImGui::EndTable();
        }

        if (!addBlocked.empty()) {
            ImGui::PushTextWrapPos(0.0f);
            ImGui::TextDisabled("%s", addBlocked.c_str());
            ImGui::PopTextWrapPos();
        }

        if (modified) {
            // Update enabled and disabled modules
            core::configManager.acquire();
            json instances;
            for (auto [_name, inst] : core::moduleManager.instances) {
                instances[_name]["module"] = inst.module.info->name;
                instances[_name]["enabled"] = inst.instance->isEnabled();
            }
            core::configManager.conf["moduleInstances"] = instances;
            core::configManager.release(true);
        }
    }
}
