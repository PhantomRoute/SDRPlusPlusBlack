#pragma once

// A panel along the bottom of the window showing how much of the time each part of
// the visible spectrum has been in use: the span is split into equal channels, and a
// channel counts as busy whenever its strongest point is more than a threshold above
// the noise floor. Useful for finding a quiet frequency, or for seeing how often an
// intermittent one really is on.
namespace occupancy {
    void init();
    bool isShown();
    void setShown(bool shown);
    // How far above the noise floor a channel has to reach to count as busy, in dB.
    int getThresholdDb();
    void setThresholdDb(int db);
}
