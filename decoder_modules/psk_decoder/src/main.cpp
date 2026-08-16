#include <imgui.h>
#include <config.h>
#include <core.h>
#include <gui/style.h>
#include <gui/gui.h>
#include <gui/widgets/constellation_diagram.h>
#include <signal_path/signal_path.h>
#include <module.h>
#include <utils/flog.h>
#include <utils/optionlist.h>
#include <algorithm>
#include <deque>
#include <mutex>
#include <string>
#include "psk_rx.h"
#include "varicode.h"

SDRPP_MOD_INFO{
    /* Name:            */ "psk_decoder",
    /* Description:     */ "BPSK31 and faster PSK text decoder",
    /* Author:          */ "PhantomRoute",
    /* Version:         */ 0, 1, 0,
    /* Max instances    */ -1
};

// What this decodes, and what it does not.
//
// The four BPSK speeds share one modulation and one varicode, so they are one
// decoder with a symbol rate setting. QPSK31 and QPSK63 are deliberately absent:
// they put a rate 1/2 convolutional code between the varicode and the phase changes,
// and getting either the dibit mapping or the Viterbi polynomials wrong produces a
// decoder that locks, looks healthy and prints nothing but rubbish. That is worse
// than not offering the mode, so it waits until it can be checked against a known
// signal.

#define SYMBOL_DIAG_COUNT 1024

ConfigManager config;

class PSKDecoderModule : public ModuleManager::Instance {
public:
    PSKDecoderModule(std::string name) {
        this->name = name;

        // A mistyped varicode entry decodes as the wrong letter and nothing else goes
        // wrong, which is exactly the sort of fault that survives to a release. The
        // table checks itself once, here, where the log will show it.
        std::string err = psk::validateVaricode();
        if (!err.empty()) {
            flog::error("PSK varicode table is not valid: {0}", err);
        }

        for (int i = 0; i < psk::MODE_COUNT; i++) {
            modes.define(psk::MODES[i].name, i);
        }

        config.acquire();
        if (config.conf[name].contains("mode")) {
            int m = config.conf[name]["mode"];
            if (m >= 0 && m < psk::MODE_COUNT) { modeIdx = m; }
        }
        if (config.conf[name].contains("squelch")) {
            squelch = config.conf[name]["squelch"];
        }
        if (config.conf[name].contains("bandwidthInBaud")) {
            rx.bandwidthInBaud = config.conf[name]["bandwidthInBaud"];
        }
        config.release();
        squelch = std::clamp<float>(squelch, 0.64f, 1.0f);
        rx.bandwidthInBaud = std::clamp<double>(rx.bandwidthInBaud, psk::MIN_BANDWIDTH_IN_BAUD,
                                                psk::MAX_BANDWIDTH_IN_BAUD);
        modeId = modes.valueId(modeIdx);

        // Not bandwidth-locked: the width is a control now, so the VFO has to accept
        // a range and the edges can be dragged on the waterfall like any other.
        vfo = sigpath::vfoManager.createVFO(name, ImGui::WaterfallVFO::REF_CENTER, 0,
                                            rx.bandwidthFor(modeIdx), rx.sampleRateFor(modeIdx),
                                            rx.minBandwidthFor(modeIdx), rx.maxBandwidthFor(modeIdx), false);
        // One hertz, because PSK31 is narrow enough that the usual snapping would put
        // the signal outside the filter.
        vfo->setSnapInterval(1);

        rx.init(vfo->output, modeIdx);
        rx.squelch = squelch;
        rx.onChar = [this](char c) { this->putChar(c); };
        rx.start();

        gui::menu.registerEntry(name, menuHandler, this, this);
    }

    ~PSKDecoderModule() {
        gui::menu.removeEntry(name);
        if (enabled) {
            rx.stop();
            sigpath::vfoManager.deleteVFO(vfo);
        }
    }

    void postInit() {}

    void enable() {
        double bw = gui::waterfall.getBandwidth();
        vfo = sigpath::vfoManager.createVFO(name, ImGui::WaterfallVFO::REF_CENTER,
                                            std::clamp<double>(0, -bw / 2.0, bw / 2.0),
                                            rx.bandwidthFor(modeIdx), rx.sampleRateFor(modeIdx),
                                            rx.minBandwidthFor(modeIdx), rx.maxBandwidthFor(modeIdx), false);
        vfo->setSnapInterval(1);
        rx.setInput(vfo->output);
        rx.reset();
        rx.start();
        enabled = true;
    }

    void disable() {
        rx.stop();
        sigpath::vfoManager.deleteVFO(vfo);
        enabled = false;
    }

    bool isEnabled() { return enabled; }

private:
    // Called from the DSP thread.
    void putChar(char c) {
        std::lock_guard<std::mutex> lck(textMtx);

        // Carriage return and line feed both mean a new line here, and a pair of them
        // means one new line rather than two - operators send either or both.
        if (c == '\r' || c == '\n') {
            if (!lastWasNewline) { text += '\n'; }
            lastWasNewline = true;
        }
        else {
            // Anything else unprintable is dropped rather than shown as a box. The
            // control codes in the varicode are almost always a corrupted character
            // rather than a deliberate one.
            if ((unsigned char)c >= 0x20 && (unsigned char)c < 0x7F) {
                text += c;
                lastWasNewline = false;
            }
        }

        // Keep the buffer bounded. Trimming on a line boundary means the top of the
        // window does not start mid-word.
        if (text.size() > MAX_TEXT) {
            size_t cut = text.find('\n', text.size() - KEEP_TEXT);
            text.erase(0, (cut == std::string::npos) ? (text.size() - KEEP_TEXT) : (cut + 1));
        }
        textDirty = true;
    }

    void setBandwidthHz(double hz) {
        double baud = psk::MODES[modeIdx].baud;
        rx.bandwidthInBaud = std::clamp<double>(hz / baud, psk::MIN_BANDWIDTH_IN_BAUD,
                                                psk::MAX_BANDWIDTH_IN_BAUD);
        if (enabled && vfo != NULL) { vfo->setBandwidth(rx.bandwidthFor(modeIdx)); }
        config.acquire();
        config.conf[name]["bandwidthInBaud"] = rx.bandwidthInBaud;
        config.release(true);
    }

    // The VFO edges can be dragged on the waterfall, which changes the filter without
    // going through the slider. Pick that up so the two agree and the width is still
    // there after a restart.
    void followWaterfallBandwidth() {
        if (!enabled || vfo == NULL) { return; }
        if (!vfo->getBandwidthChanged()) { return; }
        double baud = psk::MODES[modeIdx].baud;
        rx.bandwidthInBaud = std::clamp<double>(vfo->getBandwidth() / baud,
                                                psk::MIN_BANDWIDTH_IN_BAUD, psk::MAX_BANDWIDTH_IN_BAUD);
        config.acquire();
        config.conf[name]["bandwidthInBaud"] = rx.bandwidthInBaud;
        config.release(true);
    }

    void selectMode(int newIdx) {
        if (newIdx == modeIdx) { return; }
        modeIdx = newIdx;

        // The symbol rate decides both how much spectrum the signal needs and how
        // fast the baseband has to be sampled, so the VFO changes with it.
        if (enabled) {
            vfo->setBandwidthLimits(rx.minBandwidthFor(modeIdx), rx.maxBandwidthFor(modeIdx), false);
            vfo->setSampleRate(rx.sampleRateFor(modeIdx), rx.bandwidthFor(modeIdx));
            rx.setMode(modeIdx);
        }

        config.acquire();
        config.conf[name]["mode"] = modeIdx;
        config.release(true);
    }

    static void menuHandler(void* ctx) {
        PSKDecoderModule* _this = (PSKDecoderModule*)ctx;
        float menuWidth = ImGui::GetContentRegionAvail().x;

        _this->followWaterfallBandwidth();

        if (!_this->enabled) { style::beginDisabled(); }

        ImGui::LeftLabel("Mode");
        ImGui::FillWidth();
        if (ImGui::Combo(("##psk_mode_" + _this->name).c_str(), &_this->modeId, _this->modes.txt)) {
            _this->selectMode(_this->modes.value(_this->modeId));
        }
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip("All four are the same mode at different speeds. BPSK31 is what almost\n"
                              "all of the activity on HF uses; the faster ones turn up on VHF and on\n"
                              "the busier data frequencies. Pick the wrong one and nothing decodes.");
        }

        // The constellation says in one glance what a row of numbers cannot: whether
        // the loops have locked. Two tight blobs left and right is a decodable BPSK
        // signal, a ring or a smear is not.
        dsp::complex_t* cbuf = _this->constDiagram.acquireBuffer();
        _this->rx.copySymbols(cbuf);
        _this->constDiagram.releaseBuffer();
        _this->constDiagram.draw();
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip("The recovered symbols. Two tight clusters on opposite sides mean the\n"
                              "decoder has locked onto a BPSK signal - they can sit at any angle, which\n"
                              "is harmless. A ring, a cross or a blur in the middle means it has not.\n"
                              "Nudge the VFO a hertz at a time until the two clusters appear.");
        }

        // What the AFC is having to take out. This is the number that says whether
        // the VFO is really on the signal, which the waterfall alone cannot show at
        // these widths, and it is worth watching: a reading that keeps climbing is a
        // drifting transmitter, and one near the edge of the filter means the signal
        // is about to fall out of it.
        double off = _this->rx.offsetHz();
        double halfFilter = _this->rx.bandwidthFor(_this->modeIdx) / 2.0;
        bool nearEdge = fabs(off) > (halfFilter * 0.8);
        bool tracking = _this->rx.afcTracking();
        ImGui::LeftLabel("Offset");
        if (!tracking) {
            // Say so rather than showing a stale number as though it were current.
            ImGui::TextDisabled("%+.1f Hz  (holding - nothing to measure)", off);
        }
        else if (nearEdge) {
            ImGui::TextColored(ImVec4(1.0f, 0.7f, 0.3f, 1.0f), "%+.1f Hz  (near the filter edge)", off);
        }
        else {
            ImGui::Text("%+.1f Hz", off);
        }
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip("How far the signal is from the middle of the filter, which the decoder\n"
                              "corrects for by itself. Tuning closer is still worth it - the further out\n"
                              "it sits, the more of the signal the filter is cutting off. Widen the\n"
                              "bandwidth below if it will not stay inside.");
        }

        float q = _this->rx.getQuality();
        ImGui::LeftLabel("Quality");
        ImGui::FillWidth();
        // 0.64 is where an unmodulated noise floor sits, so the bar starts there
        // rather than at zero - otherwise it reads as two thirds full on a dead band.
        ImGui::ProgressBar(std::clamp<float>((q - 0.64f) / 0.36f, 0.0f, 1.0f), ImVec2(0, 0));
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip("How tightly the symbols are landing where BPSK symbols should. Empty is\n"
                              "noise, full is a clean signal. The squelch below is set against this.");
        }

        // Adjustable because signals are not all the width the mode says they should
        // be: a wide or distorted one needs more room, and a crowded band needs less
        // so the station next door stays out of it.
        float bwHz = (float)_this->rx.bandwidthFor(_this->modeIdx);
        float bwMin = (float)_this->rx.minBandwidthFor(_this->modeIdx);
        float bwMax = (float)_this->rx.maxBandwidthFor(_this->modeIdx);
        ImGui::LeftLabel("Bandwidth");
        ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x - (32.0f * style::uiScale));
        if (ImGui::SliderFloat(("##psk_bw_" + _this->name).c_str(), &bwHz, bwMin, bwMax, "%.0f Hz")) {
            _this->setBandwidthHz(bwHz);
        }
        ImGui::HelpMarker("How much spectrum the decoder listens to. The signal itself is about one\n"
                          "baud wide - 31 Hz for BPSK31 - and the rest is room for it to sit off\n"
                          "centre. Widen it for a signal that is fat, distorted or drifting; narrow\n"
                          "it to keep the station next door out. You can also drag the edges of the\n"
                          "VFO on the waterfall.");

        ImGui::LeftLabel("Squelch");
        ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x - (32.0f * style::uiScale));
        if (ImGui::SliderFloat(("##psk_sql_" + _this->name).c_str(), &_this->squelch, 0.64f, 1.0f, "%.2f")) {
            _this->rx.squelch = _this->squelch;
            config.acquire();
            config.conf[_this->name]["squelch"] = _this->squelch;
            config.release(true);
        }
        ImGui::HelpMarker("Characters below this quality are thrown away instead of printed.\n"
                          "Turn it down to read a weak signal through the errors; turn it up if\n"
                          "an empty channel is printing random letters at you.");

        if (!_this->enabled) { style::endDisabled(); }

        // ---- The text itself
        ImGui::Spacing();
        {
            std::lock_guard<std::mutex> lck(_this->textMtx);
            ImGui::BeginChild(("##psk_text_" + _this->name).c_str(),
                              ImVec2(menuWidth, 200.0f * style::uiScale), true,
                              ImGuiWindowFlags_HorizontalScrollbar);
            ImGui::PushTextWrapPos(0.0f);
            ImGui::TextUnformatted(_this->text.c_str());
            ImGui::PopTextWrapPos();
            // Follow the bottom only while the reader is already there, so scrolling
            // back to read something is not undone by the next character to arrive.
            if (_this->textDirty && ImGui::GetScrollY() >= ImGui::GetScrollMaxY() - 1.0f) {
                ImGui::SetScrollHereY(1.0f);
            }
            _this->textDirty = false;
            ImGui::EndChild();
        }

        if (ImGui::Button(("Clear##psk_clear_" + _this->name).c_str(), ImVec2(menuWidth / 2.0f, 0))) {
            std::lock_guard<std::mutex> lck(_this->textMtx);
            _this->text.clear();
        }
        ImGui::SameLine();
        if (ImGui::Button(("Copy##psk_copy_" + _this->name).c_str(), ImVec2(ImGui::GetContentRegionAvail().x, 0))) {
            std::lock_guard<std::mutex> lck(_this->textMtx);
            ImGui::SetClipboardText(_this->text.c_str());
        }
    }

    static const size_t MAX_TEXT = 16384;
    static const size_t KEEP_TEXT = 12288;

    std::string name;
    bool enabled = true;

    VFOManager::VFO* vfo = NULL;
    psk::Receiver rx;
    ImGui::ConstellationDiagram constDiagram;

    OptionList<std::string, int> modes;
    int modeIdx = 0;
    int modeId = 0;
    float squelch = 0.70f;

    std::mutex textMtx;
    std::string text;
    bool textDirty = false;
    bool lastWasNewline = true;
};

MOD_EXPORT void _INIT_() {
    config.setPath(std::string(core::getRoot()) + "/psk_decoder_config.json");
    config.load(json::object());
    config.enableAutoSave();
}

MOD_EXPORT ModuleManager::Instance* _CREATE_INSTANCE_(std::string name) {
    return new PSKDecoderModule(name);
}

MOD_EXPORT void _DELETE_INSTANCE_(void* instance) {
    delete (PSKDecoderModule*)instance;
}

MOD_EXPORT void _END_() {
    config.disableAutoSave();
    config.save();
}
