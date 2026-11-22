# Service Manager

A native C++23 app for Ubuntu GNOME using GTK 4 and libadwaita. It lists
system services and sockets. It reads the list and state from the local
systemd D-Bus API and uses systemctl to carry out actions.

## Install the package on Ubuntu

Download the `.deb` for your architecture from the [latest release](https://github.com/qashoou/gnome-service-manager/releases/latest), then run
the commands below from the directory where you saved it.

```sh
sudo apt install ./gnome-service-manager_0.1.0_amd64.deb
sudo --preserve-env=WAYLAND_DISPLAY,XDG_RUNTIME_DIR gnome-service-manager
```

The first package is built on Ubuntu 24.04 for amd64.

## Build and launch on Ubuntu

From this directory, run:

```sh
sudo apt install build-essential pkg-config libgtk-4-dev libadwaita-1-dev
make
sudo --preserve-env=WAYLAND_DISPLAY,XDG_RUNTIME_DIR ./gnome-service-manager
```

Run the launch command from a terminal in your local GNOME Wayland session.
On GNOME on Xorg, use
`sudo --preserve-env=DISPLAY,XAUTHORITY ./gnome-service-manager`.

## Use

- Search by unit name or configured executable.
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
sudo apt install build-essential debhelper pkg-config libgtk-4-dev libadwaita-1-dev
dpkg-buildpackage --build=binary --unsigned-changes --unsigned-source
```

The `.deb` is written to the parent directory.

## License

[MIT](LICENSE)
