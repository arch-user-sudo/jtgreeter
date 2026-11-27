# jtgreeter

JTLab Greeter is a Wayland login screen that also works as a lockscreen.

## Accent and rainbow mode

`jtgreeterctl accent '#RRGGBB'` (or `jtgreeterctl '#RRGGBB'`) sets the accent
used by both the greeter and the `--lock` screen. Rainbow mode is a shared
setting in `/etc/jt/rainbow.conf`, which `jtsettings` owns and which the greeter
only reads. The only valid values are:

```text
rainbow=on
rainbow=off
```

`jtgreeterctl rainbow` prints the current value but never writes it. The files
are read at startup and polled while running. For a single launch,
`jtgreeter rainbow` (or `--rainbow`) also enables the cycle.

## Install

```sh
make
sudo make install
```

Plain `make install` installs the binaries, PAM config, and the service file for
your detected init system. It does **not** modify `/etc/inittab`.

## Enabling at boot

### OpenRC

OpenRC is supported: run the install command with the extra `greeter` target,
then switch over your login manager immediately:

```sh
sudo make install greeter
sudo rc-service <your-current-login-manager> stop
sudo rc-update del <your-current-login-manager> default
sudo rc-update add jtgreeter default
```

`make install greeter` comments out the active `tty1` getty/agetty entries in
`/etc/inittab` (saving a backup at `/etc/inittab.bak.jtgreeter`) and reloads
init right away. **Do not reboot or log out before disabling your old login
manager and enabling `jtgreeter`**, or you may be left without a login prompt
on `tty1`.

`make uninstall` restores the original `/etc/inittab` entries and removes the
backup.

### systemd

Startup of the login manager is **not done automatically for systemd**. The
installer only places `jtgreeter.service`; it does not enable it and does not
disable any display manager you are currently using.

**Do not enable the systemd service.** systemd support is still experimental
and may cause issues on your system. Leave your current display manager in
place.
