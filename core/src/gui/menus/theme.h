#pragma once
#include <string>

namespace thememenu {
    void init(std::string resDir);
    void applyTheme();
    void draw(void* ctx);
    // The theme editor is a floating window, so it is drawn from the main window rather
    // than from draw(): collapsing the Theme menu section shouldn't close it.
    void drawEditor();
    void saveStyle();
}
