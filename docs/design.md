# GNOME Service Manager design

GNOME Service Manager is a C++23 application for inspecting and controlling
system-wide systemd services and sockets from a GTK 4 and libadwaita window.

## Components

| Source | Responsibility |
| --- | --- |
| [`src/main.cpp`](../src/main.cpp) | Creates the window, tracks selection and filters, and sends refresh and action requests without blocking GTK. |
| [`src/backend.cpp`](../src/backend.cpp) | Starts the privileged helper, sends requests over private pipes, and returns unit snapshots and action messages. |
| [`src/systemd.cpp`](../src/systemd.cpp) | Reads systemd unit files and runtime state, optionally reads process arguments, and invokes systemctl for mutations. |
| [`src/unit_row.cpp`](../src/unit_row.cpp) | Implements sorting, search, the running-only predicate, and command-line display formatting. |

## Privileges and requests

A normal launch keeps the GTK process in the user's GNOME session. The first
request starts the installed binary in `--backend` mode through `pkexec`.
The Polkit policy requests administrator authentication for an active local
session. The helper rejects a direct launch without root privileges.

The UI and helper exchange requests and responses over the child process's
standard input and output. The helper accepts refresh, start, stop, enable,
and disable, and validates the service or socket name for each action.
It passes the name to systemctl as a separate argument. An explicitly
root-launched UI uses the same operations directly.

## Data and UI flow

1. A refresh or selected action queues one request on a GLib worker. The
   main thread continues handling GTK events.
2. The backend merges installed unit files with loaded systemd units by
   name, so startup settings and live state are available in the same row.
   The optional Command Line column reads the arguments of each service's
   main PID from procfs when enabled.
3. The UI replaces its snapshot on the main thread and restores selection
   by name when that row is visible. Search and the running-only checkbox
   filter the snapshot before GTK sorts it for display.
4. The footer counts the full snapshot before filtering. A service counts
   as running when its active state is `active` and its substate is
   `running`. Socket units contribute to the total unit count.

Start and Stop change live state. Enable and Disable change startup settings.
The backend refreshes the list after an action and reports failures or
warnings in the status area.

## Tests and packaging

Run `make test` to check sorting, search, running-service filtering, and
command-line formatting. The test executable needs no graphical session or
live systemd connection. The Debian package build runs the same tests. CI also validates
the installed launcher and checks the privileged helper against systemd.
