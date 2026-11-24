#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

struct ServiceUsage
{
    std::optional<double> cpu_percent;
    std::optional<std::uint64_t> memory_bytes;
};

struct ServiceSample
{
    std::optional<std::uint64_t> cpu_nanoseconds;
    std::optional<std::uint64_t> memory_bytes;
    std::uint64_t started_at = 0;
    std::uint64_t sampled_at = 0;
};

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
    ServiceUsage usage{};
};

enum class Column
{
    pid,
    name,
    startup,
    status,
    command_line,
    cpu,
    memory
};

int compare_rows(const UnitRow& left, const UnitRow& right, Column column);
std::string casefold(std::string_view text);
bool matches_query(const UnitRow& row, std::string_view folded_query);
bool is_running_service(const UnitRow& row);
bool is_systemd_unit(const UnitRow& row);
ServiceUsage measure_usage(const ServiceSample& current, const ServiceSample* previous);
std::string format_cpu_usage(std::optional<double> percent);
std::string format_memory_usage(std::optional<std::uint64_t> bytes);

std::string format_command_line(std::string_view arguments);
