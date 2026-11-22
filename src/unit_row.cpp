#include "unit_row.h"

#include <glib.h>

#include <algorithm>
#include <compare>
#include <memory>
#include <tuple>

int compare_rows(const UnitRow& left, const UnitRow& right, Column column)
{
    switch (column)
    {
    case Column::pid:
        return (left.pid > right.pid) - (left.pid < right.pid);
    case Column::name:
        return left.name.compare(right.name);
    case Column::startup:
        return left.startup.compare(right.startup);
    case Column::command_line:
        return left.command_line.compare(right.command_line);
    case Column::status:
        // Group by the unit's overall state, then by the detail shown in the same cell.
        const auto order =
            std::tie(left.active, left.substate) <=> std::tie(right.active, right.substate);
        return (order > 0) - (order < 0);
    }
    return 0;
}

std::string casefold(std::string_view text)
{
    if (text.empty())
        return {};
    const std::unique_ptr<char, decltype(&g_free)> folded(
        g_utf8_casefold(text.data(), static_cast<gssize>(text.size())), g_free);
    return folded.get();
}

bool matches_query(const UnitRow& row, std::string_view folded_query)
{
    // The view folds the search text once before applying it to every row.
    return folded_query.empty() || casefold(row.name).find(folded_query) != std::string::npos ||
           casefold(row.executable).find(folded_query) != std::string::npos ||
           casefold(row.description).find(folded_query) != std::string::npos ||
           (row.pid != 0 && std::to_string(row.pid).find(folded_query) != std::string::npos);
}

std::string format_command_line(std::string_view arguments)
{
    std::string command;
    // procfs separates arguments with NUL bytes, including empty arguments.
    while (!arguments.empty())
    {
        const auto end = arguments.find('\0');
        const auto arg = arguments.substr(0, end);
        const std::unique_ptr<char, decltype(&g_free)> valid(
            g_utf8_make_valid(arg.data(), static_cast<gssize>(arg.size())), g_free);
        std::string printable;
        for (const unsigned char ch : std::string_view(valid.get()))
        {
            if (g_ascii_iscntrl(ch))
            {
                constexpr auto hex = "0123456789abcdef";
                printable += "\\x";
                printable += hex[ch >> 4];
                printable += hex[ch & 15];
            }
            else
                printable += static_cast<char>(ch);
        }
        if (!command.empty())
            command += ' ';
        const bool plain =
            !printable.empty() &&
            std::all_of(printable.begin(), printable.end(),
                        [](unsigned char ch)
                        {
                            return g_ascii_isalnum(ch) ||
                                   std::string_view("_./:=+-@%").find(ch) != std::string_view::npos;
                        });
        if (plain)
            command += printable;
        else
        {
            const std::unique_ptr<char, decltype(&g_free)> quoted(g_shell_quote(printable.c_str()),
                                                                  g_free);
            command += quoted.get();
        }
        if (end == std::string_view::npos)
            break;
        arguments.remove_prefix(end + 1);
    }
    return command;
}
