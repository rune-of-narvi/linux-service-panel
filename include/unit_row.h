#pragma once

#include <string>
#include <string_view>

struct UnitRow
{
    std::string name;
    std::string executable;
    std::string description;
    std::string startup;
    std::string active;
    std::string substate;
    // A zero PID means systemd reported no main process for this unit.
    unsigned int pid = 0;
};

enum class Column
{
    pid,
    name,
    startup,
    status
};

int compare_rows(const UnitRow& left, const UnitRow& right, Column column);
std::string casefold(std::string_view text);
bool matches_query(const UnitRow& row, std::string_view folded_query);
