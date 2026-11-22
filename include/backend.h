#pragma once

#include "glib_ptr.h"
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
    ObjectPtr<GSubprocess> process_;
    ObjectPtr<GDataInputStream> output_;
};

int run_backend();
