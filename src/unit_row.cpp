#include "unit_row.h"

#include "glib_ptr.h"

#include <algorithm>
#include <compare>
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
    case Column::cpu:
        return (left.usage.cpu_percent > right.usage.cpu_percent) -
               (left.usage.cpu_percent < right.usage.cpu_percent);
    case Column::memory:
        return (left.usage.memory_bytes > right.usage.memory_bytes) -
               (left.usage.memory_bytes < right.usage.memory_bytes);
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
    const GStringPtr folded(g_utf8_casefold(text.data(), static_cast<gssize>(text.size())));
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

bool is_running_service(const UnitRow& row)
{
    // systemd can keep sockets and completed one-shot services active.
    return row.name.ends_with(".service") && row.active == "active" && row.substate == "running";
}

bool is_systemd_unit(const UnitRow& row)
{
    return row.name.starts_with("systemd-");
}

ServiceUsage measure_usage(const ServiceSample& current, const ServiceSample* previous)
{
    ServiceUsage usage{std::nullopt, current.memory_bytes};
    // A fresh invocation or a reset counter needs a new baseline.
    if (previous && current.started_at == previous->started_at && current.cpu_nanoseconds &&
        previous->cpu_nanoseconds && *current.cpu_nanoseconds >= *previous->cpu_nanoseconds &&
        current.sampled_at > previous->sampled_at)
    {
        const auto elapsed_us = static_cast<double>(current.sampled_at - previous->sampled_at);
        const auto cpu_ns =
            static_cast<double>(*current.cpu_nanoseconds - *previous->cpu_nanoseconds);
        usage.cpu_percent = cpu_ns / (elapsed_us * 10.0);
    }
    return usage;
}

std::string format_cpu_usage(std::optional<double> percent)
{
    if (!percent)
        return "—";
    char text[G_ASCII_DTOSTR_BUF_SIZE];
    return std::string(g_ascii_formatd(text, sizeof(text), "%.1f", *percent)) + "%";
}

std::string format_memory_usage(std::optional<std::uint64_t> bytes)
{
    if (!bytes)
        return "—";
    constexpr std::uint64_t mb = 1024 * 1024;
    constexpr std::uint64_t gb = 1024 * mb;
    char text[G_ASCII_DTOSTR_BUF_SIZE];
    if (*bytes > gb)
    {
        return std::string(
                   g_ascii_formatd(text, sizeof(text), "%.2f", static_cast<double>(*bytes) / gb)) +
               " GB";
    }
    return std::string(
               g_ascii_formatd(text, sizeof(text), "%.1f", static_cast<double>(*bytes) / mb)) +
           " MB";
}

std::string format_command_line(std::string_view arguments)
{
    std::string command;
    // procfs separates arguments with NUL bytes, including empty arguments.
    while (!arguments.empty())
    {
        const auto end = arguments.find('\0');
        const auto arg = arguments.substr(0, end);
        const GStringPtr valid(g_utf8_make_valid(arg.data(), static_cast<gssize>(arg.size())));
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
            const GStringPtr quoted(g_shell_quote(printable.c_str()));
            command += quoted.get();
        }
        if (end == std::string_view::npos)
            break;
        arguments.remove_prefix(end + 1);
    }
    return command;
}
