#include <gui/colormaps.h>
#include <filesystem>
#include <utils/flog.h>
#include <config.h>
#include <fstream>
#include <json.hpp>
#include <cctype>
#include <stdexcept>
#include "utils/wstr.h"

using nlohmann::json;

namespace colormaps {
    std::map<std::string, Map> maps;

    void loadMap(std::string path) {
        if (!std::filesystem::is_regular_file(wstr::str2wstr(path))) {
            flog::error("Could not load {0}, file doesn't exist", path);
            return;
        }

        Map map;
        std::vector<std::string> mapTxt;

        try {
            std::ifstream file(wstr::str2wstr(path));
            json data;
            file >> data;
            file.close();
            map.name = data["name"];
            map.author = data["author"];
            mapTxt = data["map"].get<std::vector<std::string>>();
            // Checked here so the conversion below can't throw half way through.
            for (auto const& col : mapTxt) {
                if (col.size() < 7) { throw std::runtime_error("colour '" + col + "' is not #RRGGBB"); }
                for (int c = 1; c < 7; c++) {
                    if (!isxdigit((unsigned char)col[c])) { throw std::runtime_error("colour '" + col + "' is not #RRGGBB"); }
                }
            }
        }
        catch (const std::exception& e) {
            flog::error("Could not load {0}: {1}", path, e.what());
            ConfigManager::reportProblem("colour maps", "The colour map file '" + std::filesystem::path(path).filename().string() + "' is damaged and is not available.");
            return;
        }

        map.entryCount = mapTxt.size();
        map.map = new float[mapTxt.size() * 3];
        int i = 0;
        for (auto const& col : mapTxt) {
            map.map[i * 3] = std::stoi(col.substr(1, 2), NULL, 16);
            map.map[(i * 3) + 1] = std::stoi(col.substr(3, 2), NULL, 16);
            map.map[(i * 3) + 2] = std::stoi(col.substr(5, 2), NULL, 16);
            i++;
        }

        maps[map.name] = map;
    }
}