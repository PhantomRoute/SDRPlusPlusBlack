#pragma once
#include <json.hpp>
#include <thread>
#include <string>
#include <mutex>
#include <memory>
#include <atomic>
#include <vector>

using nlohmann::json;

// Something in the user's settings that could not be used as it was: a file that
// was reset, a value that was replaced. Recorded so the user can be told, not just
// the log - the program carries on either way, but carrying on is not the same as
// everything being the way they left it.
struct ConfigProblem {
    std::string file;   // File name, e.g. "radio_config.json"
    std::string what;   // What happened to it, in plain words
    bool fileReset;     // The whole file went back to defaults, rather than one value
};

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
    // belongs), so a default must have the type the module stores - a null default
    // is left unchecked. Only the top level: nested objects are often the user's own
    // collections - lists, devices - and a deleted entry must stay deleted. Returns
    // whether anything changed.
    static bool fillDefaults(json& target, const json& defaults, const std::string& what = "", bool fixTypes = true);

    // Record a problem for the user to be shown. file is the settings file's name.
    static void reportProblem(const std::string& file, const std::string& what, bool fileReset = false);
    // Everything reported since the last call. Cheap to call every frame.
    static std::vector<ConfigProblem> takeProblems();

    json conf;

private:
    struct SaveJob;

    bool keepCorruptCopy();
    std::string keptNote(bool kept);

    std::string path = "";
    bool changed = false;
    std::atomic<bool> autoSaveEnabled = false;
    std::shared_ptr<SaveJob> saveJob;
    std::mutex mtx;

    friend class ConfigSaveWorker;
};
