#include <signal_path/sink.h>
#include <utils/flog.h>
#include <imgui/imgui.h>
#include <gui/style.h>
#include <gui/icons.h>

#include <core.h>
#include <gui/gui.h>
#include <gui/dialogs/dialog_box.h>
#include "signal_path.h"

#define CONCAT(a, b) ((std::string(a) + b).c_str())

namespace {
    bool isRadioInstance(const std::string& name) {
        auto it = core::moduleManager.instances.find(name);
        return it != core::moduleManager.instances.end() && std::string(it->second.module.info->name) == "radio";
    }

    int countRadios() {
        int n = 0;
        for (auto& [name, inst] : core::moduleManager.instances) {
            if (std::string(inst.module.info->name) == "radio") { n++; }
        }
        return n;
    }

    void saveModuleInstances() {
        core::configManager.acquire();
        json instances;
        for (auto& [name, inst] : core::moduleManager.instances) {
            instances[name]["module"] = inst.module.info->name;
            instances[name]["enabled"] = inst.instance->isEnabled();
        }
        core::configManager.conf["moduleInstances"] = instances;
        core::configManager.release(true);
    }

    // A whole second radio: its own module instance, so its own VFO on the waterfall,
    // its own mode and bandwidth, and its own audio stream to send wherever it likes.
    void addRadio() {
        std::string name;
        for (int i = 2;; i++) {
            name = "Radio " + std::to_string(i);
            if (core::moduleManager.instances.find(name) == core::moduleManager.instances.end() &&
                !sigpath::vfoManager.vfoExists(name)) { break; }
        }

        // Put the new VFO beside the one in use rather than on top of it, where it
        // would hide under the old one and look as if nothing had happened.
        double viewBW = gui::waterfall.getViewBandwidth();
        double viewOffset = gui::waterfall.getViewOffset();
        double lower = viewOffset - viewBW / 2.0;
        double upper = viewOffset + viewBW / 2.0;
        double base = viewOffset;
        auto sel = gui::waterfall.vfos.find(gui::waterfall.selectedVFO);
        if (sel != gui::waterfall.vfos.end()) { base = sel->second->centerOffset; }
        double step = viewBW / 8.0;
        double offset = (base + step < upper - step) ? base + step : base - step;
        offset = std::clamp<double>(offset, lower + step / 2.0, upper - step / 2.0);

        // Its menu goes under the last radio's rather than at the very bottom, below
        // every decoder, where it would look as though it had not been added.
        int after = -1;
        for (int i = 0; i < (int)gui::menu.order.size(); i++) {
            if (isRadioInstance(gui::menu.order[i].name)) { after = i; }
        }
        bool listed = false;
        for (auto& opt : gui::menu.order) {
            if (opt.name == name) { listed = true; }
        }
        if (after >= 0 && !listed) {
            Menu::MenuOption_t opt;
            opt.name = name;
            opt.open = true;
            gui::menu.order.insert(gui::menu.order.begin() + after + 1, opt);
        }

        core::configManager.acquire();
        core::configManager.conf["vfoOffsets"][name] = offset;
        json arr = json::array();
        for (int i = 0; i < (int)gui::menu.order.size(); i++) {
            arr[i]["name"] = gui::menu.order[i].name;
            arr[i]["open"] = gui::menu.order[i].open;
        }
        core::configManager.conf["menuElements"] = arr;
        core::configManager.release(true);

        if (core::moduleManager.createInstance(name, "radio")) {
            flog::error("Could not add {}", name);
            return;
        }
        core::moduleManager.postInit(name);
        saveModuleInstances();

        if (gui::waterfall.vfos.find(name) != gui::waterfall.vfos.end()) {
            gui::waterfall.selectedVFO = name;
            gui::waterfall.selectedVFOChanged = true;
        }
    }
}

SinkManager::SinkManager() : defaultInputAudio(nullptr) {
    SinkManager::SinkProvider prov;
    prov.create = SinkManager::NullSink::create;
    registerSinkProvider("None", prov);

    sigpath::txState.bindHandler(&this->txHandler);
    txHandler.ctx = this;
    txHandler.handler = [](bool txOn, void* ctx) {
        SinkManager* _this = (SinkManager*)ctx;
        flog::info("_this->setAllMuted: {}", txOn);
        _this->setAllMuted(txOn);
    };

    defaultInputAudio.origin = "SinkManager.defaultInputAudio";

}

SinkManager::Stream::Stream(dsp::stream<dsp::stereo_t>* in, EventHandler<float>* srChangeHandler, float sampleRate) {
    _in = in;
    init(srChangeHandler, sampleRate);
}

void SinkManager::Stream::init(EventHandler<float>* srChangeHandler, float sampleRate) {
    srChange.bindHandler(srChangeHandler);
    _sampleRate = sampleRate;
    if (!_in) {
        _in = &_in0;
    }
    merger.bindStream(0, _in);
    splitter.init(merger.getOutput());
    splitter.bindStream(&volumeInput);
    volumeAjust.init(&volumeInput, 1.0f, false);
    sinkOut = &volumeAjust.out;
}

void SinkManager::Stream::start() {
    if (running) {
        return;
    }

    merger.start();
    splitter.start();
    volumeAjust.start();
    sink->start();
    running = true;
}

void SinkManager::Stream::stop() {
    if (!running) {
        return;
    }
    splitter.stop();
    merger.stop();
    volumeAjust.stop();
    sink->stop();
    running = false;
}

void SinkManager::Stream::setVolume(float volume) {
    guiVolume = volume;
    volumeAjust.setVolume(volume);
}

float SinkManager::Stream::getVolume() {
    return guiVolume;
}

float SinkManager::Stream::getSampleRate() {
    return _sampleRate;
}

void SinkManager::Stream::setInput(dsp::stream<dsp::stereo_t>* in) {
    std::lock_guard<std::mutex> lck(ctrlMtx);
    merger.unbindStream(_in);
    _in = in;
    merger.bindStream(0, _in);
}

dsp::stream<dsp::stereo_t>* SinkManager::Stream::bindStream() {
    dsp::stream<dsp::stereo_t>* stream = new dsp::stream<dsp::stereo_t>;
    splitter.bindStream(stream);
    stream->origin = "SinkManager::Stream::bindStream(new)";
    return stream;
}

void SinkManager::Stream::unbindStream(dsp::stream<dsp::stereo_t>* stream) {
    splitter.unbindStream(stream);
    delete stream;
}

void SinkManager::Stream::setSampleRate(float sampleRate) {
    std::lock_guard<std::mutex> lck(ctrlMtx);
    _sampleRate = sampleRate;
    srChange.emit(sampleRate);
}

void SinkManager::registerSinkProvider(std::string name, SinkProvider provider) {
    if (providers.find(name) != providers.end()) {
        flog::error("Cannot register sink provider '{0}', this name is already taken", name);
        return;
    }

    // Add the provider to the lists
    providers[name] = provider;
    providerNames.push_back(name);

    // Recreatd the text list for the menu
    refreshProviders();

    // Update the IDs of every stream
    for (auto& [streamName, stream] : streams) {
        stream->providerId = std::distance(providerNames.begin(), std::find(providerNames.begin(), providerNames.end(), stream->providerName));
    }

    onSinkProviderRegistered.emit(name);
}

void SinkManager::unregisterSinkProvider(std::string name) {
    if (providers.find(name) == providers.end()) {
        flog::error("Cannot unregister sink provider '{0}', no such provider exists.", name);
        return;
    }

    onSinkProviderUnregister.emit(name);

    // Switch all sinks using it to a null sink
    for (auto& [streamName, stream] : streams) {
        if (providerNames[stream->providerId] != name) { continue; }
        setStreamSink(streamName, "None");
    }

    // Erase the provider from the lists
    providers.erase(name);
    providerNames.erase(std::find(providerNames.begin(), providerNames.end(), name));

    // Recreatd the text list for the menu
    refreshProviders();

    // Update the IDs of every stream
    for (auto& [streamName, stream] : streams) {
        stream->providerId = std::distance(providerNames.begin(), std::find(providerNames.begin(), providerNames.end(), stream->providerName));
    }

    onSinkProviderUnregistered.emit(name);
}

bool SinkManager::configContains(const std::string& name) const {
    core::configManager.acquire();
    bool available = core::configManager.conf["streams"].contains(name);
    core::configManager.release();
    return available;
}


void SinkManager::registerStream(std::string name, SinkManager::Stream* stream) {
    if (streams.find(name) != streams.end()) {
        flog::error("Cannot register stream '{0}', this name is already taken", name);
        return;
    }

    SinkManager::SinkProvider provider;

    provider = providers["None"];

    stream->sink = provider.create(stream, name, provider.ctx);
    stream->providerId = std::distance(providerNames.begin(), std::find(providerNames.begin(), providerNames.end(), "None"));
    stream->providerName = "None";

    streams[name] = stream;
    streamNames.push_back(name);

    // Load config
    bool available = configContains(name);
    if (available) { loadStreamConfig(name); }

    onStreamRegistered.emit(name);
}

void SinkManager::unregisterStream(std::string name) {
    if (streams.find(name) == streams.end()) {
        flog::error("Cannot unregister stream '{0}', this stream doesn't exist", name);
        return;
    }
    onStreamUnregister.emit(name);
    SinkManager::Stream* stream = streams[name];
    stream->stop();
    delete stream->sink;
    streams.erase(name);
    streamNames.erase(std::remove(streamNames.begin(), streamNames.end(), name), streamNames.end());
    onStreamUnregistered.emit(name);
}

void SinkManager::startStream(std::string name) {
    if (streams.find(name) == streams.end()) {
        flog::error("Cannot start stream '{0}', this stream doesn't exist", name);
        return;
    }
    streams[name]->start();
}

void SinkManager::stopStream(std::string name) {
    if (streams.find(name) == streams.end()) {
        flog::error("Cannot stop stream '{0}', this stream doesn't exist", name);
        return;
    }
    streams[name]->stop();
}

float SinkManager::getStreamSampleRate(std::string name) {
    if (streams.find(name) == streams.end()) {
        flog::error("Cannot get sample rate of stream '{0}', this stream doesn't exist", name);
        return -1.0f;
    }
    return streams[name]->getSampleRate();
}

dsp::stream<dsp::stereo_t>* SinkManager::bindStream(std::string name) {
    if (streams.find(name) == streams.end()) {
        flog::error("Cannot bind to stream '{0}'. Stream doesn't exist", name);
        return NULL;
    }
    return streams[name]->bindStream();
}

void SinkManager::unbindStream(std::string name, dsp::stream<dsp::stereo_t>* stream) {
    if (streams.find(name) == streams.end()) {
        flog::error("Cannot unbind from stream '{0}'. Stream doesn't exist", name);
        return;
    }
    streams[name]->unbindStream(stream);
}

dsp::routing::Merger<dsp::stereo_t> *SinkManager::getMerger(std::string name) {
    if (streams.find(name) == streams.end()) {
        flog::error("Cannot unbind from stream '{0}'. Stream doesn't exist", name);
        return nullptr;
    }
    return streams[name]->getMerger();
}

void SinkManager::setStreamSink(std::string name, std::string providerName) {
    if (streams.find(name) == streams.end()) {
        flog::error("Cannot set sink for stream '{0}'. Stream doesn't exist", name);
        return;
    }
    Stream* stream = streams[name];
    if (providers.find(providerName) == providers.end()) {
        flog::error("Unknown sink provider '{0}'", providerName);
        return;
    }

    if (stream->running) {
        stream->sink->stop();
    }
    delete stream->sink;
    stream->providerId = std::distance(providerNames.begin(), std::find(providerNames.begin(), providerNames.end(), providerName));
    stream->providerName = providerName;
    SinkManager::SinkProvider prov = providers[providerName];
    stream->sink = prov.create(stream, name, prov.ctx);
    if (stream->running) {
        stream->sink->start();
    }
}

void SinkManager::setAllMuted(bool muted) {
    flog::info("::setAllMuted: {}", muted);
    for(auto &k : streams) {
        k.second->volumeAjust.setTempMuted(muted);
    }
}

void SinkManager::showVolumeSlider(std::string name, std::string prefix, float width, float btnHeight, int btnBorder, bool sameLine) {
    // TODO: Replace map with some hashmap for it to be faster
    float height = ImGui::GetTextLineHeightWithSpacing() + 2;
    float sliderHeight = height;
    if (btnHeight > 0) {
        height = btnHeight;
    }

    float ypos = ImGui::GetCursorPosY();
    float sliderOffset = 8.0f * style::uiScale;

    if (streams.find(name) == streams.end() || name == "") {
        float dummy = 0.0f;
        style::beginDisabled();
        ImGui::PushID(ImGui::GetID(("sdrpp_unmute_btn_" + name).c_str()));
        ImGui::ImageButton(icons::MUTED, ImVec2(height, height), ImVec2(0, 0), ImVec2(1, 1), btnBorder, ImVec4(0, 0, 0, 0), ImGui::GetStyleColorVec4(ImGuiCol_Text));
        ImGui::PopID();
        ImGui::SameLine();
        ImGui::SetNextItemWidth(width - height - sliderOffset);
        ImGui::SetCursorPosY(ypos + ((height - sliderHeight) / 2.0f) + btnBorder);
        ImGui::SliderFloat((prefix + name).c_str(), &dummy, 0.0f, 1.0f, "");
        style::endDisabled();
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
            style::tooltip("No audio stream yet. Start a radio to get one.");
        }
        if (sameLine) { ImGui::SetCursorPosY(ypos); }
        return;
    }

    SinkManager::Stream* stream = streams[name];

    if (stream->volumeAjust.getMuted()) {
        ImGui::PushID(ImGui::GetID(("sdrpp_unmute_btn_" + name).c_str()));
        if (ImGui::ImageButton(icons::MUTED, ImVec2(height, height), ImVec2(0, 0), ImVec2(1, 1), btnBorder, ImVec4(0, 0, 0, 0), ImGui::GetStyleColorVec4(ImGuiCol_Text))) {
            stream->volumeAjust.setMuted(false);
            core::configManager.acquire();
            saveStreamConfig(name);
            core::configManager.release(true);
        }
        ImGui::PopID();
        if (ImGui::IsItemHovered()) { style::tooltip("%s is muted. Click to unmute.", name.c_str()); }
    }
    else {
        ImGui::PushID(ImGui::GetID(("sdrpp_mute_btn_" + name).c_str()));
        if (ImGui::ImageButton(icons::UNMUTED, ImVec2(height, height), ImVec2(0, 0), ImVec2(1, 1), btnBorder, ImVec4(0, 0, 0, 0), ImGui::GetStyleColorVec4(ImGuiCol_Text))) {
            stream->volumeAjust.setMuted(true);
            core::configManager.acquire();
            saveStreamConfig(name);
            core::configManager.release(true);
        }
        ImGui::PopID();
        if (ImGui::IsItemHovered()) { style::tooltip("Mute %s", name.c_str()); }
    }

    ImGui::SameLine();

    ImGui::SetNextItemWidth(width - height - sliderOffset);
    ImGui::SetCursorPosY(ypos + ((height - sliderHeight) / 2.0f) + btnBorder);
    if (ImGui::SliderFloat((prefix + name).c_str(), &stream->guiVolume, 0.0f, 1.0f, "")) {
        stream->setVolume(stream->guiVolume);
        core::configManager.acquire();
        saveStreamConfig(name);
        core::configManager.release(true);
    }
    // The slider draws no value, so hovering is the only way to see where it is.
    if (ImGui::IsItemHovered()) { style::tooltip("%s volume: %d%%", name.c_str(), (int)((stream->guiVolume * 100.0f) + 0.5f)); }
    if (sameLine) { ImGui::SetCursorPosY(ypos); }

    this->recentStreeam = stream;
}

void SinkManager::loadStreamConfig(std::string name) {
    json conf = core::configManager.conf["streams"][name];
    SinkManager::Stream* stream = streams[name];
    std::string provName = conf["sink"];
    if (providers.find(provName) == providers.end()) {
        provName = providerNames[0];
    }
    long newProvider = std::distance(providerNames.begin(), std::find(providerNames.begin(), providerNames.end(), provName));
    if (newProvider != stream->providerId) {
        if (stream->running) {
            stream->sink->stop();
        }
        delete stream->sink;
        SinkManager::SinkProvider prov = providers[provName];
        stream->providerId = newProvider;
        stream->providerName = provName;
        stream->sink = prov.create(stream, name, prov.ctx);
        if (stream->running) {
            stream->sink->start();
        }
    }
    stream->setVolume(conf["volume"]);
    stream->volumeAjust.setMuted(conf["muted"]);
}

void SinkManager::saveStreamConfig(std::string name) {
    SinkManager::Stream* stream = streams[name];
    json conf;
    conf["sink"] = providerNames[stream->providerId];
    conf["volume"] = stream->getVolume();
    conf["muted"] = stream->volumeAjust.getMuted();
    core::configManager.conf["streams"][name] = conf;
}

// Note: acquire and release config before running this
void SinkManager::loadSinksFromConfig() {
    for (auto const& [name, stream] : streams) {
        if (!core::configManager.conf["streams"].contains(name)) { continue; }
        loadStreamConfig(name);
    }
}

void SinkManager::showMenu() {
    float menuWidth = ImGui::GetContentRegionAvail().x;
    int count = 0;
    int maxCount = streams.size();

    std::string provStr = "";
    for (auto const& name : providerNames) {
        provStr += name;
        provStr += '\0';
    }

    std::vector<std::function<void()>> postActions;
    for (auto const& [name, stream] : streams) {
        ImGui::SetCursorPosX((menuWidth / 2.0f) - (ImGui::CalcTextSize(name.c_str()).x / 2.0f));
        auto [primaryName, index] = getSecondaryStreamIndex(name);
        ImGui::Text("%s", (primaryName + (index <= 0 ? "" : " (output " + std::to_string(index+1) + ")")).c_str());

        ImGui::SetNextItemWidth(menuWidth);
        if (ImGui::Combo(CONCAT("##_sdrpp_sink_select_", name), &stream->providerId, provStr.c_str())) {
            setStreamSink(name, providerNames[stream->providerId]);
            core::configManager.acquire();
            saveStreamConfig(name);
            core::configManager.release(true);
        }
        // An unlabelled list of sink names, with nothing to say it decides where
        // this stream's audio ends up.
        if (ImGui::IsItemHovered()) {
            style::tooltip("Where the audio from %s goes: a sound card, the network, or nowhere.", name.c_str());
        }

        stream->sink->menuHandler();

        auto v1 = ImGui::GetContentRegionAvail();
        showVolumeSlider(name, "##_sdrpp_sink_menu_vol_", menuWidth);
        auto v2 = ImGui::GetContentRegionAvail();

        // Extra outputs of one radio used to be what the add button made. New ones are
        // whole radios now, but a config that still has the old kind keeps them, and
        // they can still be taken away here.
        if (SinkManager::isSecondaryStream(name)) {
            if (ImGui::Button(CONCAT("Remove this output##_sdrpp_sink_rmout_", name), ImVec2(menuWidth, 0))) {
                auto name0 = name;
                postActions.emplace_back([=]{onRemoveSubstream.emit(name0);});
            }
            if (ImGui::IsItemHovered()) {
                style::tooltip("An extra copy of %s's audio. Removing it leaves the radio as it is.", primaryName.c_str());
            }
        }
        else if (isRadioInstance(name) && countRadios() > 1) {
            if (ImGui::Button(CONCAT("Remove " + name + "##_sdrpp_sink_rmradio_", name), ImVec2(menuWidth, 0))) {
                radioToRemove = name;
                confirmRemoveRadio = true;
            }
            if (ImGui::IsItemHovered()) {
                style::tooltip("Take %s off the waterfall and forget its settings.", name.c_str());
            }
        }


        count++;
        if (count < maxCount) {
            ImGui::Spacing();
            ImGui::Separator();
        }
        ImGui::Spacing();
    }

    if (core::moduleManager.modules.find("radio") != core::moduleManager.modules.end()) {
        ImGui::Spacing();
        if (ImGui::Button("Add another radio##_sdrpp_sink_addradio", ImVec2(menuWidth, 0))) {
            // Not now: this is drawn from inside the loop over the menu entries, and
            // a new radio adds an entry of its own to the list being walked.
            gui::mainWindow.addMainThreadTask([]{ addRadio(); });
        }
        if (ImGui::IsItemHovered()) {
            style::tooltip("A second receiver with its own VFO on the waterfall, its own mode and\nbandwidth, and its own audio output. Listen to two frequencies at once\nwithin what the SDR is receiving.");
        }
    }

    if (ImGui::GenericDialog("sink_mgr_remove_radio_", confirmRemoveRadio, GENERIC_DIALOG_BUTTONS_YES_NO, [this]() {
            ImGui::Text("Remove \"%s\" and its settings?", radioToRemove.c_str());
        }) == GENERIC_DIALOG_BUTTON_YES) {
        auto name0 = radioToRemove;
        gui::mainWindow.addMainThreadTask([=]{
            if (!isRadioInstance(name0) || countRadios() <= 1) { return; }
            core::moduleManager.deleteInstance(name0);
            saveModuleInstances();
        });
    }

    for(auto &_: postActions) _();  // to optionally modify the streams list after loop over it.
}

std::vector<std::string> SinkManager::getStreamNames() {
    return streamNames;
}

void SinkManager::refreshProviders() {
    providerNamesTxt.clear();
    for (auto& provName : providerNames) {
        providerNamesTxt += provName;
        providerNamesTxt += '\0';
    }
}