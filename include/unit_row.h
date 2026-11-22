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
    std::string command_line;
};

enum class Column
{
    pid,
    name,
    startup,
    status,
    command_line
};

int compare_rows(const UnitRow& left, const UnitRow& right, Column column);
std::string casefold(std::string_view text);
bool matches_query(const UnitRow& row, std::string_view folded_query);
bool is_running_service(const UnitRow& row);

std::string format_command_line(std::string_view arguments);
