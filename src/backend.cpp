#include "backend.h"

#include <unistd.h>

#include <algorithm>
#include <cstdio>
#include <iostream>
#include <stdexcept>

namespace
{

constexpr auto response_type = "(bsa(usssssss))";

Result perform(const Request& request)
{
    Result result;
    try
    {
        result.action_message = change_unit(request.action, request.unit);
    }
    catch (const std::exception& error)
    {
        result.action_message = error.what();
    }
    try
    {
        result.rows = list_units(request.command_lines);
    }
    catch (const std::exception& error)
    {
        throw std::runtime_error(result.action_message +
                                 (result.action_message.empty() ? "" : "\n") +
                                 "Could not refresh units. " + error.what());
    }
    return result;
}

void send_result(bool success, const char* message, const std::vector<UnitRow>& rows)
{
    GVariantBuilder builder;
    g_variant_builder_init(&builder, G_VARIANT_TYPE("a(usssssss)"));
    for (const auto& row : rows)
    {
        g_variant_builder_add(&builder, "(usssssss)", row.pid, row.name.c_str(),
                              row.executable.c_str(), row.description.c_str(), row.startup.c_str(),
                              row.active.c_str(), row.substate.c_str(), row.command_line.c_str());
    }
    const VariantPtr reply(
        g_variant_ref_sink(g_variant_new(response_type, success, message, &builder)));
    const GStringPtr line(g_variant_print(reply.get(), true));
    std::cout << line.get() << std::endl;
}

bool valid_unit(std::string_view name)
{
    return name.size() <= 255 && (name.ends_with(".service") || name.ends_with(".socket")) &&
           std::all_of(name.begin(), name.end(),
                       [](unsigned char ch)
                       {
                           return g_ascii_isalnum(ch) || ch == ':' || ch == '_' || ch == '.' ||
                                  ch == '@' || ch == '-' || ch == '\\';
                       });
}

[[noreturn]] void throw_io_error(GError* error)
{
    const ErrorPtr owned(error);
    throw std::runtime_error(error ? error->message
                                   : "Could not communicate with the service manager helper.");
}

} // namespace

int run_backend()
{
    if (geteuid() != 0)
    {
        std::fputs("Service Panel helper requires root.\n", stderr);
        return 1;
    }

    // A private pipe carries only service-manager actions and exact unit names.
    std::cout << "READY" << std::endl;
    char buffer[260];
    while (std::cin.getline(buffer, sizeof(buffer)) && !std::cin.eof())
    {
        const std::string line(buffer, static_cast<std::size_t>(std::cin.gcount()) - 1);
        try
        {
            if (line.size() < 2 || line[0] < '0' || line[0] > '4' ||
                (line[1] != '0' && line[1] != '1'))
                throw std::runtime_error("Invalid service manager request.");
            Request request{static_cast<Action>(line[0] - '0'), line.substr(2), line[1] == '1'};
            if (request.action == Action::refresh ? !request.unit.empty()
                                                  : !valid_unit(request.unit))
                throw std::runtime_error("Invalid service or socket name.");
            auto result = perform(request);
            send_result(true, result.action_message.c_str(), result.rows);
        }
        catch (const std::exception& error)
        {
            send_result(false, error.what(), {});
        }
        if (!std::cout)
            break;
    }
    return 0;
}

Backend::~Backend()
{
    close();
}

void Backend::close()
{
    if (process_)
        g_output_stream_close(g_subprocess_get_stdin_pipe(process_.get()), nullptr, nullptr);
    output_.reset();
    process_.reset();
}

Result Backend::run(const Request& request, GCancellable* cancellation)
{
    if (geteuid() == 0)
        return perform(request);

    GError* error = nullptr;
    const bool starting = process_ == nullptr;
    if (starting)
    {
        // The GUI keeps its desktop session and only the private helper runs as root.
        process_.reset(
            g_subprocess_new(static_cast<GSubprocessFlags>(G_SUBPROCESS_FLAGS_STDIN_PIPE |
                                                           G_SUBPROCESS_FLAGS_STDOUT_PIPE |
                                                           G_SUBPROCESS_FLAGS_STDERR_SILENCE),
                             &error, "/usr/bin/pkexec", "--disable-internal-agent",
                             "/usr/bin/service-panel", "--backend", nullptr));
        if (!process_)
            throw_io_error(error);
        output_.reset(g_data_input_stream_new(g_subprocess_get_stdout_pipe(process_.get())));
    }

    gsize length = 0;
    // Read a startup reply before writing to a newly spawned helper. This also reports
    // authorization cancellation cleanly if pkexec closes its pipes before running it.
    if (starting)
    {
        const GStringPtr ready(
            g_data_input_stream_read_line(output_.get(), &length, cancellation, &error));
        if (!ready || std::string_view(ready.get(), length) != "READY")
        {
            const bool exited =
                !ready && !error && g_subprocess_wait(process_.get(), cancellation, nullptr);
            const bool cancelled = exited && g_subprocess_get_if_exited(process_.get()) &&
                                   g_subprocess_get_exit_status(process_.get()) == 126;
            close();
            if (error)
                throw_io_error(error);
            if (cancelled)
                return {{}, "Authorization cancelled.", true};
            throw std::runtime_error("Could not authorize the installed helper.");
        }
    }

    const auto line = std::to_string(static_cast<int>(request.action)) +
                      (request.command_lines ? "1" : "0") +
                      (request.action == Action::refresh ? "" : request.unit) + "\n";
    if (!g_output_stream_write_all(g_subprocess_get_stdin_pipe(process_.get()), line.data(),
                                   line.size(), nullptr, cancellation, &error))
    {
        close();
        throw_io_error(error);
    }

    const GStringPtr reply(
        g_data_input_stream_read_line(output_.get(), &length, cancellation, &error));
    if (!reply)
    {
        close();
        throw_io_error(error);
    }

    const VariantPtr parsed(g_variant_parse(G_VARIANT_TYPE(response_type), reply.get(),
                                            reply.get() + length, nullptr, &error));
    if (!parsed)
    {
        close();
        throw_io_error(error);
    }
    gboolean success = false;
    const char* message = nullptr;
    GVariant* rows = nullptr;
    g_variant_get(parsed.get(), "(b&s@a(usssssss))", &success, &message, &rows);
    const VariantPtr owned_rows(rows);
    if (!success)
        throw std::runtime_error(message);

    GVariantIter entries;
    g_variant_iter_init(&entries, owned_rows.get());
    Result result{{}, message};
    guint32 pid = 0;
    const char *name, *executable, *description, *startup, *active, *substate, *command_line;
    while (g_variant_iter_next(&entries, "(u&s&s&s&s&s&s&s)", &pid, &name, &executable,
                               &description, &startup, &active, &substate, &command_line))
    {
        result.rows.push_back(
            {name, executable, description, startup, active, substate, pid, command_line});
    }
    return result;
}
