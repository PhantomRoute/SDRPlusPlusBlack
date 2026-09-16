#pragma once
#include <string>
#include <vector>
#include <map>
#include <module.h>
#include <functional>

#define MAX_MENU_COUNT 1024

class Menu {
public:
    Menu();

    struct MenuOption_t {
        std::string name;
        bool open;
    };

    struct MenuItem_t {
        void (*drawHandler)(void* ctx);
        void* ctx;
        ModuleManager::Instance* inst;
    };

    void registerEntry(std::string name, void (*drawHandler)(void* ctx), void* ctx = NULL, ModuleManager::Instance* inst = NULL);
    void removeEntry(std::string name);
    // filter, when given, draws only the entries it accepts. Used to split the menu
    // in two on a phone; dragging to reorder is off while filtered, since the order
    // would be worked out from half the list.
    bool draw(bool updateStates, const std::function<bool(const std::string&)>& filter = nullptr);

    std::vector<MenuOption_t> order;

    bool locked = false;

private:
    bool isInOrderList(std::string name);

    
    bool menuClicked = false;
    std::string clickedMenuName = "";
    std::string draggedMenuName = "";
    int insertBefore = -1;
    std::string insertBeforeName = "";

    int draggedId = 0;
    MenuOption_t draggedOpt;

    std::map<std::string, MenuItem_t> items;

    float headerTops[MAX_MENU_COUNT];
    int optionIDs[MAX_MENU_COUNT];
};