#include "unit_row.h"

#include <glib.h>

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
