#pragma once

// A panel along the bottom of the window plotting the raw I samples against Q, as the
// receiver delivers them. The shape shows receiver trouble at a glance: a cloud sitting
// off centre is a DC offset, an ellipse instead of a circle is IQ imbalance, and a cloud
// squared off at the edges is clipping. The numbers beside it say the same in figures.
namespace iqplot {
    void init();
    bool isShown();
    void setShown(bool shown);
}
