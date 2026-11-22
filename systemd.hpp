#pragma once

#include <string>
#include <vector>

struct UnitRow {
    std::string name;
    std::string executable;
    std::string startup;
    std::string active;
    std::string substate;
    unsigned int pid = 0;
};

enum class Action { refresh, start, stop, enable, disable };

std::vector<UnitRow> list_units();
std::string change_unit(Action action, const std::string& name);
