#include <server.h>
#include <signal_path/source.h>
#include <utils/flog.h>
#include <signal_path/signal_path.h>
#include <core.h>
#include <http_debug_server.h>

#include <chrono>
#include <condition_variable>
#include <mutex>
#include <thread>

namespace {
    std::string currentSourceType;
    SourceManager::SourceHandler* selectedHandlerRef = nullptr;

    // How often the connected devices are re-enumerated while the source menu is
    // on screen and the radio is stopped.
    const std::chrono::milliseconds PROBE_INTERVAL(2000);

    // Enumerating USB devices costs tens of milliseconds (librtlsdr even opens
    // every dongle to read its serial), and it used to run straight from the
    // ImGui draw, which showed up as a stutter every couple of seconds. Do it on
    // a worker thread instead and only touch the module from the UI thread once
    // the device list has actually changed.
    class DeviceProbeWorker {
    public:
        DeviceProbeWorker() : workerThread(&DeviceProbeWorker::worker, this) {
        }

        // Called from the UI thread, cheap enough to call every frame. Returns
        // true exactly once after the set of connected devices changed.
        bool poll(SourceManager::SourceHandler* handler) {
            std::lock_guard<std::mutex> lock(mtx);
            if (handler != target) {
                target = handler;
                signature.clear();
                haveSignature = false;
                changed = false;
                nextProbe = std::chrono::steady_clock::time_point();
            }
            auto now = std::chrono::steady_clock::now();
            lastRequest = now;
            // Only worth waking the worker when a probe is actually due,
            // otherwise this notifies once per frame for nothing: while a probe
            // is pending the worker is already sleeping on that deadline.
            if (now >= nextProbe) { wakeup.notify_one(); }
            if (!changed) { return false; }
            changed = false;
            return true;
        }

        // Called from the UI thread before the handler's module goes away.
        void forget(SourceManager::SourceHandler* handler) {
            {
                std::lock_guard<std::mutex> lock(mtx);
                if (target == handler) {
                    target = nullptr;
                    signature.clear();
                    haveSignature = false;
                    changed = false;
                }
            }
            // Wait out a probe already in flight so the module can't be
            // destroyed while its probe handler is still running.
            //
            // Unconditionally, whoever the probe belongs to. Returning early when
            // this handler was no longer the target was wrong: the worker copies
            // the target and takes probeMtx before dropping mtx, so a probe on
            // this handler can still be running after something else has become
            // the target. Selecting a different source and then unloading the old
            // one - which is two clicks apart, and a probe takes tens of
            // milliseconds - walked straight into a destroyed module.
            std::lock_guard<std::mutex> probeLock(probeMtx);
        }

    private:
        void worker() {
            SetThreadName("device-probe");
            std::unique_lock<std::mutex> lock(mtx);
            while (true) {
                // Nothing to watch, or the UI stopped asking because the menu was
                // closed or the radio was started. Either way, stay idle: poll()
                // wakes us back up.
                auto now = std::chrono::steady_clock::now();
                if (!target || now - lastRequest > std::chrono::seconds(1)) {
                    wakeup.wait(lock);
                    continue;
                }
                if (now < nextProbe) {
                    wakeup.wait_until(lock, nextProbe);
                    continue;
                }
                nextProbe = now + PROBE_INTERVAL;

                SourceManager::SourceHandler* handler = target;
                // Taken before dropping mtx so forget() can't slip past us and
                // let the module be destroyed under the probe handler.
                probeMtx.lock();
                lock.unlock();
                std::string sig;
                try {
                    sig = handler->probeHandler(handler->ctx);
                }
                catch (...) {
                    // A device library throwing must not take the worker down.
                }
                probeMtx.unlock();
                lock.lock();

                if (target != handler) { continue; }
                if (haveSignature && sig != signature) { changed = true; }
                signature = sig;
                haveSignature = true;
            }
        }

        std::mutex mtx;
        std::mutex probeMtx;
        std::condition_variable wakeup;
        SourceManager::SourceHandler* target = nullptr;
        std::string signature;
        bool haveSignature = false;
        bool changed = false;
        std::chrono::steady_clock::time_point lastRequest;
        std::chrono::steady_clock::time_point nextProbe;
        std::thread workerThread;
    };

    DeviceProbeWorker& deviceProbeWorker() {
        // The core library outlives all modules. Keep the worker alive until
        // process exit so source modules can safely unregister themselves.
        static DeviceProbeWorker* worker = new DeviceProbeWorker();
        return *worker;
    }

    // Rate limit for sources that only provide a refreshHandler: those still run
    // on the UI thread, so they must not run every frame.
    std::chrono::steady_clock::time_point nextUnprobedRefresh;

    std::string sourceConfigEndpoint(const std::string& sourceName) {
        return "/source/" + sourceName + "/config";
    }

    void registerSourceConfigEndpoint(const std::string& sourceName, SourceManager::SourceHandler* handler) {
        // registerEndpoint appends without looking for an existing path, and this
        // runs on every selectSource() - which is every time the user picks a
        // source, and again for the current one every time any source module
        // registers or unregisters, since the menu reselects on both. The endpoint
        // list grew another copy of this path each time and never shrank.
        httpdebug::procfs::unregister(sourceConfigEndpoint(sourceName));
        httpdebug::procfs::registerEndpoint(sourceConfigEndpoint(sourceName), [sourceName]() -> std::string {
                core::configManager.acquire();
                std::string json = core::configManager.conf[sourceName].dump();
                core::configManager.release();
                return json; }, [sourceName](const std::string& val) {
                try {
                    json j = json::parse(val);
                    core::configManager.acquire();
                    core::configManager.conf[sourceName].update(j);
                    core::configManager.release(true);
                } catch (...) {} }, httpdebug::procfs::Type::String);
    }
}

SourceManager::SourceManager() {
    nullSource.origin = "source.nullsource";

    httpdebug::procfs::registerEndpoint("/source/type", []() -> std::string { return currentSourceType; }, [](const std::string& val) {
            currentSourceType = val;
            httpdebug::requestSourceChange(val); }, httpdebug::procfs::Type::String);

    httpdebug::procfs::registerEndpoint("/source/type:options", []() -> std::string {
            auto names = sigpath::sourceManager.getSourceNames();
            std::string json = "[";
            for (size_t i = 0; i < names.size(); i++) {
                if (i > 0) json += ",";
                std::string escaped;
                for (char c : names[i]) {
                    if (c == '"') escaped += "\\\"";
                    else if (c == '\\') escaped += "\\\\";
                    else escaped += c;
                }
                json += "\"" + escaped + "\"";
            }
            json += "]";
            return json; }, nullptr, httpdebug::procfs::Type::String);
}

void SourceManager::registerSource(std::string name, SourceHandler* handler) {
    if (sources.find(name) != sources.end()) {
        flog::error("Tried to register new source with existing name: {0}", name);
        return;
    }
    sources[name] = handler;
    onSourceRegistered.emit(name);
}

void SourceManager::unregisterSource(std::string name) {
    if (sources.find(name) == sources.end()) {
        flog::error("Tried to unregister non existent source: {0}", name);
        return;
    }
    onSourceUnregister.emit(name);
    // Blocks until any probe in flight has returned, so the module can't be
    // destroyed while the worker is inside its probe handler.
    if (sources[name]->probeHandler != NULL) {
        deviceProbeWorker().forget(sources[name]);
    }
    // The module is going away, so its endpoint has to go with it. Left behind, it
    // stayed in the listing describing a source that is no longer loaded.
    httpdebug::procfs::unregister(sourceConfigEndpoint(name));
    if (name == selectedName) {
        if (selectedHandler != NULL) {
            // Through the handler, not sources[selectedName]. They are the same
            // pointer whenever the two are in step, but map::operator[] on a name
            // that had gone stale would insert a null handler and dereference it,
            // turning a bookkeeping slip into a crash at the worst moment.
            selectedHandler->deselectHandler(selectedHandler->ctx);
        }
        sigpath::iqFrontEnd.setInput(&nullSource);
        selectedHandler = NULL;
        // Leaving these behind made the manager report a source that no longer
        // exists, both to the debug endpoint and to the next selectSource().
        selectedName.clear();
        currentSourceType.clear();
        selectedHandlerRef = nullptr;
    }
    sources.erase(name);
    onSourceUnregistered.emit(name);
}

std::vector<std::string> SourceManager::getSourceNames() {
    std::vector<std::string> names;
    for (auto const& [name, src] : sources) { names.push_back(name); }
    return names;
}

void SourceManager::selectSource(std::string name) {
    if (sources.find(name) == sources.end()) {
        flog::error("Tried to select non existent source: {0}", name);
        return;
    }
    if (selectedHandler != NULL) {
        // See unregisterSource: the handler already is the selected source, and
        // going back through the map by name only adds a way to get it wrong.
        selectedHandler->deselectHandler(selectedHandler->ctx);
    }
    selectedHandler = sources[name];
    selectedHandler->selectHandler(selectedHandler->ctx);
    selectedName = name;
    currentSourceType = name;
    selectedHandlerRef = selectedHandler;
    if (core::args["server"].b()) {
        server::setInput(selectedHandler->stream);
    }
    else {
        sigpath::iqFrontEnd.setInput(selectedHandler->stream);
    }
    // Set server input here
    registerSourceConfigEndpoint(name, selectedHandler);
    onSourceSelected.emit(name);
}

void SourceManager::showSelectedMenu() {
    if (selectedHandler == NULL || selectedHandler->menuHandler == NULL) {
        return;
    }
    selectedHandler->menuHandler(selectedHandler->ctx);
}

void SourceManager::pollDeviceChanges() {
    if (selectedHandler == NULL || selectedHandler->refreshHandler == NULL) {
        return;
    }

    if (selectedHandler->probeHandler != NULL) {
        if (!deviceProbeWorker().poll(selectedHandler)) { return; }
    }
    else {
        auto now = std::chrono::steady_clock::now();
        if (now < nextUnprobedRefresh) { return; }
        nextUnprobedRefresh = now + PROBE_INTERVAL;
    }

    selectedHandler->refreshHandler(selectedHandler->ctx);
}

bool SourceManager::start() {
    if (selectedHandler == NULL) {
        return false;
    }
    selectedHandler->startHandler(selectedHandler->ctx);
    if (selectedHandler->runningHandler == NULL) {
        return true;
    }
    return selectedHandler->runningHandler(selectedHandler->ctx);
}

void SourceManager::stop() {
    if (selectedHandler == NULL) {
        return;
    }
    selectedHandler->stopHandler(selectedHandler->ctx);
}

void SourceManager::tune(double freq) {
    if (selectedHandler == NULL) {
        return;
    }
    // TODO: No need to always retune the hardware in Panadapter mode
    selectedHandler->tuneHandler(abs(((tuneMode == TuningMode::NORMAL) ? freq : ifFreq) + tuneOffset), selectedHandler->ctx);
    onRetune.emit(freq);
    currentFreq = freq;
    onTuneChanged.emit(freq);
}

void SourceManager::setTuningOffset(double offset) {
    tuneOffset = offset;
    tune(currentFreq);
}

void SourceManager::setTuningMode(TuningMode mode) {
    tuneMode = mode;
    tune(currentFreq);
}

void SourceManager::setPanadapterIF(double freq) {
    ifFreq = freq;
    tune(currentFreq);
}