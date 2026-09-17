#pragma once

namespace dialogs {
    // Tells the user when their settings could not be used as they were left: a file
    // that was damaged and reset, a value that was replaced, a bookmark that had to
    // go. The program keeps running either way, and that is the point of saying so -
    // running is not the same as everything being the way it was, and someone using
    // it for anything that matters needs to know before they rely on it.
    //
    // Shows whatever ConfigManager::reportProblem has collected, as soon as there is
    // a window to show it in, and again if more turns up later. Call once per frame.
    void drawSettingsProblems();
}
