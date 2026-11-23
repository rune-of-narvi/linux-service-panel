# Service Panel

[![Ubuntu 24.04 package CI](https://github.com/rune-of-narvi/linux-service-panel/actions/workflows/deb.yml/badge.svg?branch=main&event=push)](https://github.com/rune-of-narvi/linux-service-panel/actions/workflows/deb.yml)
[![Ubuntu 26.04 build and test CI](https://github.com/rune-of-narvi/linux-service-panel/actions/workflows/ubuntu-26.yml/badge.svg?branch=main&event=push)](https://github.com/rune-of-narvi/linux-service-panel/actions/workflows/ubuntu-26.yml)
[![Latest release](https://img.shields.io/github/v/release/rune-of-narvi/linux-service-panel?label=release&logo=github)](https://github.com/rune-of-narvi/linux-service-panel/releases/latest)
[![MIT license](https://img.shields.io/badge/license-MIT-blue)](LICENSE)

[![Arch Linux packaging: not supported](https://img.shields.io/badge/Arch_Linux-not_supported-yellow?logo=archlinux)](https://github.com/rune-of-narvi/linux-service-panel/issues/1)
[![Debian packaging: not supported](https://img.shields.io/badge/Debian-not_supported-yellow?logo=debian)](https://github.com/rune-of-narvi/linux-service-panel/issues/2)
[![Fedora packaging: not supported](https://img.shields.io/badge/Fedora-not_supported-yellow?logo=fedora)](https://github.com/rune-of-narvi/linux-service-panel/issues/3)

View and manage systemd services and sockets on Linux. Service Panel uses
C++23, GTK 4, and libadwaita. It reads unit state from the local systemd
D-Bus API and uses systemctl to carry out actions.

## Install the package on Ubuntu

[Download service-panel-latest.deb](https://github.com/rune-of-narvi/linux-service-panel/releases/latest/download/service-panel-latest.deb)
for amd64. [Numbered releases](https://github.com/rune-of-narvi/linux-service-panel/releases)
include versioned packages, checksums, and release notes.

Run this command from the download directory.

```sh
sudo apt install ./service-panel-latest.deb
```

Open **Service Panel** from Ubuntu's app grid or run
`service-panel` from a terminal in your GNOME session. Authenticate
when prompted. To pin it, right-click its icon in the app grid or dock and
choose **Add to Favorites**.

The amd64 release package is built and tested on Ubuntu 24.04. The Ubuntu
26.04 workflow builds the source and runs the unit tests.

## Upgrade from the old package

Close the old app, then install the new `.deb` using the install command
above. APT replaces the old `gnome-service-manager` package. If you pinned
the old launcher, pin **Service Panel** from the app grid after upgrading.
Update any scripts that use the old executable to call `service-panel`.

## Uninstall

```sh
sudo apt remove service-panel
```

## Build and launch on Ubuntu

From this directory, run:

```sh
sudo apt install build-essential pkg-config libgtk-4-dev libadwaita-1-dev
make
sudo --preserve-env=WAYLAND_DISPLAY,XDG_RUNTIME_DIR ./service-panel
```

Run the development build from a terminal in your local GNOME Wayland session.
Install the Debian package to test the app launcher and elevation prompt.
On GNOME on Xorg, use
`sudo --preserve-env=DISPLAY,XAUTHORITY ./service-panel`.

## Use

- Search by unit name, configured executable, main PID, or description.
- Check **Show running services only** at the bottom to filter the list to
  services that systemd reports as running. The checkbox also works with search.
- The footer shows the total number of units and the number of running
  services. Both counts use the full list, even when filters are active.
- Use the **Command Line** button to show the live arguments of each service’s
  main process. The column is hidden by default and is blank for sockets
  and services without a readable command line.
- Click any column header to sort. Click it again to reverse the order.
- Unit descriptions appear below the name and executable when available.
- Select a row to start, stop, enable, or disable the unit.
- Start and Stop change the current run state. Enable and Disable change
  startup links. To stop socket activation immediately, stop the socket too.
- Use the refresh button to see changes made outside the app. A service row
  shows its systemd main PID and the first configured ExecStart executable
  when available. Socket rows have no PID or executable of their own.
- The list shows concrete units. Uninstantiated templates do not appear.


## Build a Debian package

From the source directory on Ubuntu, run:

```sh
sudo apt install build-essential debhelper pkg-config libgtk-4-dev libadwaita-1-dev libgtest-dev
dpkg-buildpackage --build=binary --unsigned-changes --unsigned-source
```

The `.deb` is written to the parent directory.

## Unit tests and formatting

Install `libgtest-dev` alongside the build dependencies and run `make test`.
The tests run without sudo, a graphical session, or a systemd connection.
Debian package builds run the same suite.

Run `clang-format -i include/*.h src/*.cpp tests/*.cpp` to apply the project's
formatting rules.

See the [design document](docs/design.md) for the application architecture and request flow.

## Releases

To publish a release, add an entry to `debian/changelog` with the changes
since the previous release and the next patch or minor version. A minor
release resets the patch number to zero. A successful build on `main`
publishes the package, checksum, and notes from that entry on GitHub.

## License

[MIT](LICENSE)
