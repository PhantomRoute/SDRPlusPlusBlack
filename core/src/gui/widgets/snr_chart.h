#pragma once

// A panel along the bottom of the window plotting the selected VFO's SNR over the last
// minute, against dB and seconds.
//
// It used to belong to the noise_reduction_logmmse module, which is where its switch
// lived and why it only worked while that module was enabled - although it reads
// nothing from the noise reduction, only the SNR the core already measures. It is a
// view of the signal, so it lives with the other panels under Display.
namespace snrchart {
    void init();
    bool isShown();
    void setShown(bool shown);
}
