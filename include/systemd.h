#pragma once

#include "unit_row.h"

#include <gio/gio.h>

#include <map>
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

std::vector<UnitRow> list_units(bool command_lines);
using ServiceSamples = std::map<std::string, ServiceSample>;
ServiceSamples sample_running_services(GCancellable* cancellation);
std::string change_unit(Action action, const std::string& name);
