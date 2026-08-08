#pragma once
#include <string>

namespace thememenu {
    void init(std::string resDir);
    // userSelected is false when the theme isn't being picked but only re-applied: at
    // startup, or to drop the theme editor's live preview. The waterfall gradient and
    // the spectrum styling are settings of their own that the config remembers, so a
    // re-apply puts the config's values back instead of letting the theme file's win.
    void applyTheme(bool userSelected = true);
    void draw(void* ctx);
    // The theme editor is a floating window, so it is drawn from the main window rather
    // than from draw(): collapsing the Theme menu section shouldn't close it.
    void drawEditor();
    void saveStyle();
}
