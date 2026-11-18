# The mutible desktop

A machine that boots straight into mutible, full screen, as the whole
desktop: no GNOME, no KDE, no display manager. Under it is a minimal
Ubuntu 26.04 and [cage](https://github.com/cage-kiosk/cage), a Wayland
compositor that shows one app and nothing else, from Ubuntu's own archive.
mutible's terminal is the shell, its browser pane is the browser, and its
File menu has Suspend, Restart, Power Off and Log Out.

It suits cheap or old hardware that GNOME makes slow: a laptop with 4 GB of
memory, a small fanless box, a spare ThinkPad. Anything Ubuntu 26.04 runs on
with a screen the kernel drives (KMS: Intel, AMD, most others) will do; a
NVIDIA card needs its driver's KMS mode. See "Measurements" below for what it
saves, measured in virtual machines.

The setup is one script, [mutible-desktop-setup.sh](mutible-desktop-setup.sh),
which prints every change it makes, can be run again, and undoes itself.

## Installing

1. Install Ubuntu 26.04 Server and choose the minimized installation (or any
   minimal Ubuntu 26.04 without a desktop). Make your user there; it will be
   the desktop's user. Connect to a network for the install.
2. Get the script and run it with sudo, as that user:

   ```
   curl -fLO https://raw.githubusercontent.com/ible-ai/mutible/main/linux/desktop/mutible-desktop-setup.sh
   less mutible-desktop-setup.sh      # read it first
   chmod +x mutible-desktop-setup.sh
   sudo ./mutible-desktop-setup.sh
   ```

   `--dry-run` prints what it would do and changes nothing. `--user NAME`
   names another user. `--deb FILE` installs mutible from a .deb rather than
   the PPA.
3. Reboot. mutible comes up on the first console, logged in as you.

What it does, step by step (it prints the same):

- adds the mutible PPA (`ppa:echansen/minicode`, which keeps the editor's
  old name) as `/etc/apt/sources.list.d/mutible-desktop-ppa.sources`, its
  signing key inside the file;
- installs what is missing of cage, mutible, NetworkManager with
  wpasupplicant, PipeWire with WirePlumber, brightnessctl, polkitd, a font,
  git and curl, without their recommends, and records every package that
  was not there before;
- writes the session: `/usr/local/lib/mutible-desktop/session`,
  `/etc/pam.d/mutible-desktop`, `/etc/default/mutible-desktop` (yours to
  edit) and `/etc/systemd/system/mutible-desktop.service`;
- adds `/etc/udev/rules.d/90-mutible-desktop-backlight.rules` and puts you
  in the video group, so brightnessctl works without sudo;
- disables `getty@tty1.service` (the text login on the first console) and
  enables `mutible-desktop.service` in its place.

Its record is `/var/lib/mutible-desktop/state`.

## How it runs

`mutible-desktop.service` logs your user in on tty1 through PAM, which
registers a logind session on seat0, the same kind of session a login on the
console makes: you own the screen, the keyboard and mouse, and the sound
card while it is the active session. It runs
`cage -s -- mutible --foreground` through your login shell, so `~/.profile`'s
PATH (`~/.local/bin`, where language servers often are) reaches mutible,
with `MUTIBLE_DESKTOP=1`, which turns on mutible's desktop mode.

When mutible quits, cage quits, the session ends, and systemd starts it again
two seconds later. A session that keeps ending is not restarted forever:
when it ends by itself within 30 seconds of starting three times running (a
crash at start), tty1 gets a text shell instead, saying so, and `exit` there
tries the desktop again. Log Out never counts. Its output is in
`journalctl -b -u mutible-desktop`.

In desktop mode mutible:

- has no title bar: cage shows one window at a time, full screen, and has
  nothing to minimize it to. A second window (File > New Window) covers the
  first until it is closed, so one window with Open Recent (Ctrl+R) to move
  between folders works best;
- opens a fresh window on your home folder when you close its last window,
  rather than quitting (File > Quit still quits, which restarts the session);
- has Suspend…, Restart…, Power Off… and Log Out… at the end of the File
  menu. Each asks first. Restart and Power Off close mutible first, asking
  about unsaved tabs as Quit does, and a Cancel there stops them; Log Out
  only quits, and the session starts again. They go to logind on the system
  bus; when it would need a password, the status bar says so;
- uses GTK's own file chooser (`GDK_DEBUG=no-portals`), since no desktop
  portal runs.

## Keys and the way out

Everything is mutible's own keys (Ctrl+Shift+P for the palette, Ctrl+` for
the terminal; `docs/REFERENCE.md` has them all). The machine adds:

- **Ctrl+Alt+F2** to **F6**: a text console, where you can log in. cage
  passes console switching through because the session runs it with `-s`;
  **Ctrl+Alt+F1** comes back to mutible.
- **The power key**: logind powers off (its default).
- **Closing the lid**: suspends (logind's default; `HandleLidSwitch` in
  `/etc/systemd/logind.conf` changes it).

mutible's terminal is a full shell, with sudo, so nothing needs a console:
`sudo apt upgrade`, `nmtui`, `systemctl poweroff` all work there.

## Wi-Fi and the network

NetworkManager runs Wi-Fi (Ubuntu Server leaves wired networks to netplan
and systemd-networkd, as the installer set them up, and NetworkManager
leaves them alone). In mutible's terminal:

```
nmtui                              # a menu: Activate a connection
nmcli device wifi list
nmcli device wifi connect "Name" --ask
```

The network is remembered and joins again at boot. A Wi-Fi card whose
firmware the kernel lacks shows no device in `nmcli device`; installing
`linux-firmware` fixes most.

## Sound

PipeWire starts with your session. `wpctl status` lists the outputs,
`wpctl set-volume @DEFAULT_AUDIO_SINK@ 5%+` (or `5%-`, `0.5`) sets the
volume, `wpctl set-mute @DEFAULT_AUDIO_SINK@ toggle` mutes, and
`wpctl set-default ID` picks an output. The volume keys do nothing yet:
cage binds no keys of its own. A line in keys.conf can't run a shell
command either, so keep them in a terminal tab, or alias them.

## Brightness

`brightnessctl set 10%-` and `brightnessctl set 50%` in the terminal.
Ubuntu's brightnessctl asks neither logind nor udev, so the setup adds
brightnessctl's own udev rule, which lets the video group write the
backlight, and puts you in that group (from the next login on). As with
volume, the brightness keys do nothing yet.

## Suspend, restart, power off

From the File menu, or the terminal: `systemctl suspend`, `systemctl reboot`,
`systemctl poweroff`. polkit's defaults allow all three to the active local
session without a password while no other user is logged in (another user
logged in, say over SSH, makes them ask for an administrator). Closing the
lid suspends.

## Updates

mutible and everything else come from apt: `sudo apt update && sudo apt
upgrade` in the terminal, then File > Log Out… to start the new mutible.
`unattended-upgrades`, if the installer set it up, keeps security updates
coming as on any Ubuntu server.

## Undoing it

```
sudo ./mutible-desktop-setup.sh --undo
```

disables and removes the service, the session's files and the backlight
rule, takes you out of the video group if it put you there (keeping
`/etc/default/mutible-desktop` if you changed it), enables tty1's text login
again if it was enabled, removes the PPA file if the script added it, and
lists the packages the setup installed with the command that removes them;
`--remove-packages` runs that command, apt asking first. Run from mutible's
own terminal, the session keeps running until you reboot or log out;
`--now` stops it at once. After a reboot the machine is the minimal Ubuntu
it was, plus those packages until you remove them.

## Measurements

These are numbers from virtual machines, not from real hardware: QEMU 10.2
with KVM on the owner's ThinkPad, two virtual CPUs, 4 GB of memory, a
1280x800 screen drawn in software (llvmpipe, no GPU), both machines made
from the same Ubuntu 26.04.1 minimal cloud image (release 20261105). One
had this setup; the other had `ubuntu-desktop-minimal` (GNOME 50, GDM
logging in automatically, mutible started from `~/.config/autostart`, and
systemd-networkd turned off, as on a desktop install, where NetworkManager
alone runs the network). The same mutible .deb ran in both, on the home
folder, with `MUTIBLE_STARTUP_LOG=1` for its first frame's time. Memory was
read about two minutes after mutible's first frame: `free -m`, and the sum
of `Pss:` in `/proc/*/smaps_rollup` over the desktop user's processes (not
the SSH login reading them) and over every process. Two boots of each; GNOME's
first login, which also runs the "Welcome to Ubuntu" wizard, is left out of
its column (1,146 MB used with it).

| | mutible desktop (cage) | GNOME (ubuntu-desktop-minimal) |
|---|---|---|
| Memory used (`free -m`) | 485 to 507 MB | 1,010 MB |
| PSS of the session's processes | 218 to 222 MB, 11 processes | 701 MB, 73 processes |
| of which mutible | 156 to 157 MB | 111 MB |
| PSS of every process | 303 to 305 MB | 876 MB |
| Boot, `systemd-analyze` (kernel + userspace) | 6.4 to 6.5 s | 7.8 to 8.3 s |
| Kernel start to mutible's first frame | 6.9 to 7.4 s | 17.5 to 17.8 s |
| Power on to mutible's first frame | 8.9 to 9.4 s | 19.2 to 19.4 s |

About a third of the memory, and mutible on screen about ten seconds
sooner. mutible itself counts for more under cage because it is the only
program using GTK, Mesa and LLVM there, so their pages are all its own; under
GNOME, gnome-shell and the rest share them. GNOME's machine also reserves
512 MB for kdump (`crashkernel=`), which `ubuntu-desktop-minimal` brings, so
its total is 3,398 MB against 3,910. Power on is QEMU's start, so it includes
the BIOS and GRUB, about 2 s in both. A real laptop's firmware, disk and GPU
change all of these; the owner may measure on spare hardware.

## What it does not do (yet)

- No volume, brightness or media keys, and no battery, Wi-Fi or clock in
  mutible's status bar; the terminal does these for now.
- No screen locking and no screen blanking beyond the console's.
- One screen: cage shows mutible on the last screen connected (`-m last`
  in the session file changes that; it does not span screens).
- No notifications, no clipboard manager, no on-screen keyboard.
- Programs that need X run through Xwayland, which cage starts when one
  asks; programs that open windows of their own (a GUI debugger) show over
  mutible, one at a time, as cage shows dialogs.
