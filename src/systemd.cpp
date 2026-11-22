#include "systemd.h"

#include "glib_ptr.h"

#include <gio/gio.h>

#include <map>
#include <stdexcept>
#include <utility>

namespace
{

constexpr auto destination = "org.freedesktop.systemd1";
constexpr auto manager_path = "/org/freedesktop/systemd1";
constexpr auto manager_interface = "org.freedesktop.systemd1.Manager";

void check_error(GError* error)
{
    if (error)
    {
        const ErrorPtr owner(error);
        g_dbus_error_strip_remote_error(error);
        throw std::runtime_error(error->message);
    }
}

VariantPtr call(GDBusConnection* bus, const char* path, const char* interface, const char* method,
                GVariant* parameters = nullptr)
{
    GError* error = nullptr;
    VariantPtr result(g_dbus_connection_call_sync(bus, destination, path, interface, method,
                                                  parameters, nullptr, G_DBUS_CALL_FLAGS_NONE, 5000,
                                                  nullptr, &error));
    check_error(error);
    return result;
}

std::string string_at(GVariant* tuple, gsize index)
{
    const VariantPtr child(g_variant_get_child_value(tuple, index));
    return g_variant_get_string(child.get(), nullptr);
}

const char* const unit_patterns[] = {"*.service", "*.socket", nullptr};

struct UnitEntry
{
    UnitRow row;
    std::string path;
};

std::string unit_file_name(const std::string& path)
{
    const auto slash = path.rfind('/');
    return slash == std::string::npos ? path : path.substr(slash + 1);
}

VariantPtr get_properties(GDBusConnection* bus, const std::string& path, const char* interface)
{
    auto reply = call(bus, path.c_str(), "org.freedesktop.DBus.Properties", "GetAll",
                      g_variant_new("(s)", interface));
    return VariantPtr(g_variant_get_child_value(reply.get(), 0));
}

std::string property_string(GVariant* properties, const char* key)
{
    const char* value = nullptr;
    return g_variant_lookup(properties, key, "&s", &value) ? value : "";
}

void read_unit_details(GDBusConnection* bus, UnitEntry& entry)
{
    if (entry.row.startup.starts_with("masked"))
        return;

    auto& row = entry.row;
    try
    {
        if (entry.path.empty())
        {
            // LoadUnit makes metadata available for a unit file without starting it.
            auto reply = call(bus, manager_path, manager_interface, "LoadUnit",
                              g_variant_new("(s)", row.name.c_str()));
            entry.path = string_at(reply.get(), 0);
            auto props = get_properties(bus, entry.path, "org.freedesktop.systemd1.Unit");
            row.active = property_string(props.get(), "ActiveState");
            row.substate = property_string(props.get(), "SubState");
            row.description = property_string(props.get(), "Description");
            // systemd can use the unit ID as a fallback when no description is configured.
            if (row.description == property_string(props.get(), "Id"))
                row.description.clear();
        }
        if (!row.name.ends_with(".service"))
            return;

        auto props = get_properties(bus, entry.path, "org.freedesktop.systemd1.Service");
        guint32 pid = 0;
        g_variant_lookup(props.get(), "MainPID", "u", &pid);
        row.pid = pid;

        const VariantPtr commands(g_variant_lookup_value(props.get(), "ExecStart", nullptr));
        if (commands && g_variant_n_children(commands.get()) != 0)
        {
            const VariantPtr command(g_variant_get_child_value(commands.get(), 0));
            row.executable = string_at(command.get(), 0);
        }
    }
    catch (const std::runtime_error&)
    {
        // A unit can disappear, fail to load, or have incomplete metadata.
        // Preserve the listing and leave details unavailable for that row.
        if (row.active.empty())
            row.active = "unavailable";
    }
}

} // namespace

std::vector<UnitRow> list_units(bool command_lines)
{
    GError* error = nullptr;
    const ObjectPtr<GDBusConnection> bus(g_bus_get_sync(G_BUS_TYPE_SYSTEM, nullptr, &error));
    check_error(error);

    // Unit files supply startup state even for units that systemd hasn't loaded.
    auto files_reply =
        call(bus.get(), manager_path, manager_interface, "ListUnitFilesByPatterns",
             g_variant_new("(@as^as)", g_variant_new_strv(nullptr, 0), unit_patterns));
    const VariantPtr files(g_variant_get_child_value(files_reply.get(), 0));
    std::map<std::string, UnitEntry> units;
    for (gsize i = 0; i < g_variant_n_children(files.get()); ++i)
    {
        const VariantPtr file(g_variant_get_child_value(files.get(), i));
        const auto name = unit_file_name(string_at(file.get(), 0));
        if (name.ends_with("@.service") || name.ends_with("@.socket"))
            continue;
        auto& row = units[name].row;
        row.name = name;
        row.startup = string_at(file.get(), 1);
    }

    // Merge the live state by unit name. Loaded units may have no matching file.
    auto loaded_reply =
        call(bus.get(), manager_path, manager_interface, "ListUnitsByPatterns",
             g_variant_new("(@as^as)", g_variant_new_strv(nullptr, 0), unit_patterns));
    const VariantPtr loaded(g_variant_get_child_value(loaded_reply.get(), 0));
    for (gsize i = 0; i < g_variant_n_children(loaded.get()); ++i)
    {
        const VariantPtr unit(g_variant_get_child_value(loaded.get(), i));
        const auto name = string_at(unit.get(), 0);
        auto& entry = units[name];
        entry.row.name = name;
        auto description = string_at(unit.get(), 1);
        if (description != name)
            entry.row.description = std::move(description);
        entry.row.active = string_at(unit.get(), 3);
        entry.row.substate = string_at(unit.get(), 4);
        entry.path = string_at(unit.get(), 6);
        if (entry.row.startup.empty())
        {
            try
            {
                auto reply = call(bus.get(), manager_path, manager_interface, "GetUnitFileState",
                                  g_variant_new("(s)", name.c_str()));
                entry.row.startup = string_at(reply.get(), 0);
            }
            catch (const std::runtime_error&)
            {
                entry.row.startup = "unknown";
            }
        }
    }

    std::vector<UnitRow> rows;
    rows.reserve(units.size());
    for (auto& [name, entry] : units)
    {
        read_unit_details(bus.get(), entry);
        if (entry.row.active.empty())
            entry.row.active = "inactive";
        if (command_lines && entry.row.pid != 0)
        {
            // A service may exit between the MainPID lookup and reading procfs.
            const auto path = "/proc/" + std::to_string(entry.row.pid) + "/cmdline";
            gchar* contents = nullptr;
            gsize size = 0;
            if (g_file_get_contents(path.c_str(), &contents, &size, nullptr))
            {
                const GStringPtr owned(contents);
                entry.row.command_line = format_command_line({owned.get(), size});
            }
        }
        rows.push_back(std::move(entry.row));
    }
    return rows;
}

std::string change_unit(Action action, const std::string& name)
{
    const char* verb = nullptr;
    switch (action)
    {
    case Action::refresh:
        return {};
    case Action::start:
        verb = "start";
        break;
    case Action::stop:
        verb = "stop";
        break;
    case Action::enable:
        verb = "enable";
        break;
    case Action::disable:
        verb = "disable";
        break;
    }

    // systemctl waits for start/stop jobs and reloads units after enable/disable.
    const char* const args[] = {"/usr/bin/systemctl", "--system", "--no-pager",
                                "--no-ask-password",  verb,       "--",
                                name.c_str(),         nullptr};
    GError* error = nullptr;
    const ObjectPtr<GSubprocess> process(
        g_subprocess_newv(args,
                          static_cast<GSubprocessFlags>(G_SUBPROCESS_FLAGS_STDOUT_SILENCE |
                                                        G_SUBPROCESS_FLAGS_STDERR_PIPE),
                          &error));
    check_error(error);

    gchar* output = nullptr;
    g_subprocess_communicate_utf8(process.get(), nullptr, nullptr, nullptr, &output, &error);
    const GStringPtr stderr_text(output);
    check_error(error);
    if (!g_subprocess_get_successful(process.get()))
    {
        throw std::runtime_error(output && *output ? output : "systemctl failed");
    }
    return output && *output ? g_strstrip(output) : "";
}
