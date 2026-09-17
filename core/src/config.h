#pragma once
#include <json.hpp>
#include <thread>
#include <string>
#include <mutex>
#include <memory>
#include <atomic>

using nlohmann::json;

class ConfigManager {
public:
    ConfigManager();
    ~ConfigManager();
    void setPath(std::string file);
    void load(json def, bool lock = true);
    void save(bool lock = true);
    void enableAutoSave();
    void disableAutoSave();
    void acquire();
    void release(bool modified = false);

    // Adds each key of defaults that target lacks, so a config written by an older
    // version, or edited by hand, can't stop a module at startup. With fixTypes it
    // also replaces a value whose type is not the default's (text where a number
    // belongs); only for defaults known to match what the module stores, which not
    // every module's file defaults do. Only the top level: nested objects are often
    // the user's own collections - lists, devices - and a deleted entry must stay
    // deleted. Returns whether anything changed.
    static bool fillDefaults(json& target, const json& defaults, const std::string& what = "", bool fixTypes = true);

    json conf;

private:
    struct SaveJob;

    void keepCorruptCopy();

    std::string path = "";
    bool changed = false;
    std::atomic<bool> autoSaveEnabled = false;
    std::shared_ptr<SaveJob> saveJob;
    std::mutex mtx;

    friend class ConfigSaveWorker;
};
