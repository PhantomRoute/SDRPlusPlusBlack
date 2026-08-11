#pragma once
#include <imgui.h>
#include <string>
#include <module.h>

namespace style {
    SDRPP_EXPORT ImFont* notificationFont;
    SDRPP_EXPORT ImFont* tinyFont;
    SDRPP_EXPORT ImFont* baseFont;
    SDRPP_EXPORT ImFont* bigFont;
    SDRPP_EXPORT ImFont* mediumFont;
    SDRPP_EXPORT ImFont* hugeFont;
    SDRPP_EXPORT float uiScale;

    bool setDefaultStyle(std::string resDir);
    bool loadFonts(std::string resDir);
    void beginDisabled();
    void endDisabled();
    void testtt();
}

namespace ImGui {
    void LeftLabel(const char* text);
    void FillWidth();

    // A heading that breaks a long panel into the few things it actually covers.
    // Every menu that groups its controls drew its own copy of these two, which is
    // how they ended up looking slightly different from each other.
    void SectionHeader(const char* title);

    // A (?) after the previous item that explains it without spending a line on it.
    void HelpMarker(const char* text);
}