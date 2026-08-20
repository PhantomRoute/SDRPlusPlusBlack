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

    // Whether the explanatory tooltips are drawn at all. On by default: without them
    // a good deal of this program is a panel of unlabelled abbreviations. Off is for
    // people who already know it and would rather not have a box appear over the
    // spectrum every time the pointer crosses a control.
    SDRPP_EXPORT bool showTooltips;

    bool setDefaultStyle(std::string resDir);
    bool loadFonts(std::string resDir);
    void beginDisabled();
    void endDisabled();
    void testtt();

    // A tooltip for the item just drawn, unless the user has switched them off. Takes
    // printf arguments exactly like ImGui::SetTooltip, so a call site reads the same
    // as it did before.
    //
    // Everything explanatory goes through here rather than calling ImGui::SetTooltip
    // directly - one direct call and the switch only works in most of the program,
    // which is worse than not having it.
    void tooltip(const char* fmt, ...);

    // The same for the few that need more than a line of text - a separator, several
    // paragraphs, a colour. beginTooltip returns false when tooltips are off, and
    // endTooltip must only be called when it returned true.
    bool beginTooltip();
    void endTooltip();
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