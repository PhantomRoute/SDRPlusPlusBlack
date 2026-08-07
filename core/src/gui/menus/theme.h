#pragma once
#include <string>

namespace thememenu {
    void init(std::string resDir);
    // applyColorMap is false only at startup: the colormaps aren't loaded yet, and the
    // config already holds the gradient the user last had live, so the theme must not
    // reach in and change it out from under them.
    void applyTheme(bool applyColorMap = true);
    void draw(void* ctx);
    // The theme editor is a floating window, so it is drawn from the main window rather
    // than from draw(): collapsing the Theme menu section shouldn't close it.
    void drawEditor();
    void saveStyle();
}
