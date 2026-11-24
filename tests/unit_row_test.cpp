#include "unit_row.h"

#include <gtest/gtest.h>

namespace
{

TEST(UnitRowSort, ComparesPidsNumerically)
{
    UnitRow first;
    first.pid = 9;
    UnitRow second;
    second.pid = 100;
    EXPECT_LT(compare_rows(first, second, Column::pid), 0);
    EXPECT_GT(compare_rows(second, first, Column::pid), 0);
    EXPECT_EQ(compare_rows(first, first, Column::pid), 0);
}

TEST(UnitRowSort, ComparesNamesAndStartupStates)
{
    UnitRow cockpit;
    cockpit.name = "cockpit.socket";
    cockpit.startup = "enabled";
    UnitRow docker;
    docker.name = "docker.service";
    docker.startup = "disabled";
    EXPECT_LT(compare_rows(cockpit, docker, Column::name), 0);
    EXPECT_GT(compare_rows(cockpit, docker, Column::startup), 0);
}

TEST(UnitRowSort, GroupsByActiveStateBeforeSubstate)
{
    UnitRow listening;
    listening.active = "active";
    listening.substate = "listening";
    UnitRow running = listening;
    running.substate = "running";
    UnitRow inactive;
    inactive.active = "inactive";
    inactive.substate = "dead";
    EXPECT_LT(compare_rows(listening, running, Column::status), 0);
    EXPECT_LT(compare_rows(running, inactive, Column::status), 0);
}

TEST(UnitRowSearch, MatchesNameExecutableDescriptionAndPid)
{
    UnitRow unit;
    unit.name = "example.service";
    unit.executable = "/usr/bin/worker";
    unit.description = "Réconciliation service";
    unit.pid = 12345;
    EXPECT_TRUE(matches_query(unit, casefold("EXAMPLE")));
    EXPECT_TRUE(matches_query(unit, casefold("/usr/bin/WORKER")));
    EXPECT_TRUE(matches_query(unit, casefold("RÉCONCILIATION")));
    EXPECT_TRUE(matches_query(unit, casefold("234")));
    EXPECT_FALSE(matches_query(unit, casefold("unrelated")));
}

TEST(UnitRowSearch, EmptyQueryMatchesRowsWithoutAMainPid)
{
    UnitRow socket;
    socket.name = "cockpit.socket";
    EXPECT_TRUE(matches_query(socket, casefold("")));
    EXPECT_FALSE(matches_query(socket, casefold("0")));
}

TEST(UnitRowFilter, IncludesOnlyRunningServices)
{
    UnitRow row;
    row.name = "example.service";
    row.active = "active";
    row.substate = "running";
    EXPECT_TRUE(is_running_service(row));

    row.substate = "exited";
    EXPECT_FALSE(is_running_service(row));
    row.substate = "dead";
    row.active = "inactive";
    EXPECT_FALSE(is_running_service(row));
    row.substate = "running";
    row.active = "deactivating";
    EXPECT_FALSE(is_running_service(row));
    row.active = "active";
    row.name = "example.socket";
    row.substate = "listening";
    EXPECT_FALSE(is_running_service(row));
    row.substate = "running";
    EXPECT_FALSE(is_running_service(row));
}

TEST(UnitRowFilter, IdentifiesUnitsBySystemdNamePrefix)
{
    UnitRow row;
    row.name = "systemd-journald.service";
    EXPECT_TRUE(is_systemd_unit(row));
    row.name = "systemd-journald.socket";
    EXPECT_TRUE(is_systemd_unit(row));
    row.name = "docker.service";
    row.description = "Managed by systemd";
    EXPECT_FALSE(is_systemd_unit(row));
    row.name = "dbus-org.freedesktop.systemd-helper.service";
    EXPECT_FALSE(is_systemd_unit(row));
}

TEST(ServiceUsage, MeasuresCpuAcrossAllCoresAndKeepsIdleAtZero)
{
    const ServiceSample baseline{1'000'000'000, 512, 7, 1'000'000};
    auto current = ServiceSample{4'000'000'000, 1024, 7, 3'000'000};

    EXPECT_DOUBLE_EQ(measure_usage(current, &baseline).cpu_percent.value(), 150.0);
    EXPECT_EQ(measure_usage(current, &baseline).memory_bytes, 1024);
    current.cpu_nanoseconds = baseline.cpu_nanoseconds;
    EXPECT_DOUBLE_EQ(measure_usage(current, &baseline).cpu_percent.value(), 0.0);
}

TEST(ServiceUsage, WaitsForAValidBaseline)
{
    const ServiceSample baseline{100, 512, 7, 1000};
    auto current = ServiceSample{200, 1024, 8, 2000};
    EXPECT_FALSE(measure_usage(current, nullptr).cpu_percent);
    EXPECT_EQ(measure_usage(current, nullptr).memory_bytes, 1024);
    EXPECT_FALSE(measure_usage(current, &baseline).cpu_percent);

    current.started_at = baseline.started_at;
    current.cpu_nanoseconds = 99;
    EXPECT_FALSE(measure_usage(current, &baseline).cpu_percent);
    current.cpu_nanoseconds.reset();
    EXPECT_FALSE(measure_usage(current, &baseline).cpu_percent);
    current.cpu_nanoseconds = 200;
    current.sampled_at = baseline.sampled_at;
    EXPECT_FALSE(measure_usage(current, &baseline).cpu_percent);
}

TEST(ServiceUsage, FormatsMemoryAbove1024MBInGB)
{
    constexpr std::uint64_t mb = 1024 * 1024;
    EXPECT_EQ(format_memory_usage(std::nullopt), "—");
    EXPECT_EQ(format_memory_usage(0), "0.0 MB");
    EXPECT_EQ(format_memory_usage(512 * mb), "512.0 MB");
    EXPECT_EQ(format_memory_usage(1024 * mb), "1024.0 MB");
    EXPECT_EQ(format_memory_usage(1024 * mb + 1), "1.00 GB");
    EXPECT_EQ(format_memory_usage(1536 * mb), "1.50 GB");
    EXPECT_EQ(format_cpu_usage(std::nullopt), "—");
    EXPECT_EQ(format_cpu_usage(150.25), "150.2%");
}

TEST(UnitRowSort, ComparesUsageNumericallyAndDistinguishesUnavailableFromZero)
{
    UnitRow small;
    UnitRow large;
    large.usage = {100.0, 2'000'000'000};
    small.usage = {9.0, 512'000'000};
    EXPECT_LT(compare_rows(small, large, Column::cpu), 0);
    EXPECT_LT(compare_rows(small, large, Column::memory), 0);
    small.usage = {0.0, 0};
    const UnitRow unavailable;
    EXPECT_LT(compare_rows(unavailable, small, Column::cpu), 0);
    EXPECT_LT(compare_rows(unavailable, small, Column::memory), 0);
}

} // namespace

TEST(CommandLine, PreservesWhitespaceAndEmptyArguments)
{
    using namespace std::string_view_literals;
    EXPECT_EQ(format_command_line("/usr/bin/worker\0--label\0one two\0\0"sv),
              "/usr/bin/worker --label 'one two' ''");
}

TEST(CommandLine, HandlesMissingDataAndEscapesControlCharacters)
{
    using namespace std::string_view_literals;
    EXPECT_TRUE(format_command_line(""sv).empty());
    EXPECT_EQ(format_command_line("worker\0line\nbreak"sv), "worker 'line\\x0abreak'");
}
