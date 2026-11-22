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

} // namespace
