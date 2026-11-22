#pragma once

#include "systemd.h"

#include <gio/gio.h>

struct Request
{
    Action action;
    std::string unit;
    bool command_lines;
};

struct Result
{
    std::vector<UnitRow> rows;
    std::string action_message;
};

class Backend
{
  public:
    Backend() = default;
    ~Backend();
    Backend(const Backend&) = delete;
    Backend& operator=(const Backend&) = delete;

    Result run(const Request& request, GCancellable* cancellation);

  private:
    void close();
    GSubprocess* process_ = nullptr;
    GDataInputStream* output_ = nullptr;
};

int run_backend();
