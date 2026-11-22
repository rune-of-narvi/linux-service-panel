#pragma once

#include "unit_row.h"

#include <string>
#include <vector>

enum class Action
{
    refresh,
    start,
    stop,
    enable,
    disable
};

std::vector<UnitRow> list_units();
std::string change_unit(Action action, const std::string& name);
