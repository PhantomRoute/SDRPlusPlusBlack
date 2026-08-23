#pragma once

#include <memory>
#include <string>
#include <vector>
#include <imgui.h>
#include <dsp/types.h>

struct SubWaterfall {

    struct SubWaterfallPrivate;

    std::vector<float> peaks;

    std::shared_ptr<SubWaterfallPrivate> pvt;

    SubWaterfall(int sampleRate, int wfrange, const std::string &);
    ~SubWaterfall();
    void init();
    void draw();
    void setFreqVisible(bool visible);
    // The spectrum/waterfall split, in widget pixels, so it can be remembered across
    // runs the way the main waterfall's is. Out of range values are clamped to what
    // the current size allows, so a stale one from a taller strip does no harm.
    int getSplit() const;
    void setSplit(int height);
    // Height of the waterfall widget itself, which is the strip's height less the
    // padding and the resize grip above it. The split is stored as a share of this
    // so it keeps its proportions when the strip is resized.
    int getWidgetHeight() const;
    // True once, after the user finishes dragging the split.
    bool takeSplitMoved();
    void addAudioSamples(dsp::stereo_t * samples, int count, int sampleRate);

};