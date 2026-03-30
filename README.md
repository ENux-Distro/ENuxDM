# ENuxDM

Display manager for ENux Linux with PAM authentication, a minimal Xorg greeter (GTK 4), and XFCE4 session launch.

## Dependencies (Debian / ENux / Ubuntu)

Build:

- `build-essential`
- `libpam0g-dev`
- `pkg-config`

Runtime:

- `xorg`, `xinit`
- `python3`, `python3-gi`, `gir1.2-gtk-4.0`, `gir1.2-gdkpixbuf-2.0` (**`python3-cairo` is not required**; the greeter does not `import cairo`)
- **D-Bus (required for XFCE):** `dbus-daemon`, `dbus-user-session`, and `dbus-x11` (provides `dbus-run-session` / `dbus-launch`)
- `xfce4-session` (or `xfce4` meta) with `startxfce4` on `PATH`
- `util-linux` (`runuser`) for Xauthority handoff

ENuxDM creates **`/run/user/<uid>`** when it is missing (no `pam_systemd` session in the greeter PAM stack). If that ever fails, it falls back to **`/tmp/enuxdm-runtime-<uid>`** so D-Bus can still open a session socket.

Optional:

- Inter or Outfit fonts (place `Inter-Regular.ttf` under `assets/fonts/` before `make install`, or install system-wide fonts)
- `xauth`, `setxkbmap` (keyboard layout badge)

The greeter has **Restart** and **Shut down** under the sign-in button. Each opens a confirmation dialog, then runs **`/usr/bin/systemctl reboot`** or **`poweroff`** when available, with fallbacks to **`/sbin/reboot`** / **`/sbin/poweroff`**. By default the greeter runs as **root**, so these normally work without polkit; a non-root greeter would need **polkit** rules or **loginctl**-based helpers.

## Build

```bash
make
sudo make install
```

`make install` installs binaries under `PREFIX` (default `/usr`), enables `enuxdm.service`, and installs PAM config to `/etc/pam.d/enuxdm`. Ensure no other display manager owns `display-manager.service`.

## Configuration

Edit `/etc/enuxdm/enuxdm.conf` (installed on first install if missing):

```ini
accent=#b4b8c2
background=/path/to/image.png
logo=/usr/share/enuxdm/assets/enux-logo.svg
font_family=Inter
```

### PAM (`/etc/pam.d/enuxdm`)

The installed stack is **pam_unix only** (no `@include common-auth`). Full desktop `common-auth` chains often add **pam_faillock**, **pam_sss**, keyring modules, etc., which can make **ENuxDM** reject valid passwords after boot while **TTY login** still works.

If login still fails, clear any faillock state and check the journal:

```bash
sudo faillock --user YOURUSER --reset
sudo journalctl -b -t enuxdm
```

To integrate LDAP/SSO, edit `/etc/pam.d/enuxdm` locally or add `@include` lines your distro expects.

## Console and logs

Xorg and greeter diagnostics are appended to `/var/log/enuxdm-session.log`. The active virtual console should stay clean during normal operation.

### Greeter shows a black screen

On a **minimal Xorg** session (no compositor), GTK 4’s default GL/Vulkan renderer often paints nothing. ENuxDM forces **`GSK_RENDERER=cairo`** in both the `xinit` environment and the greeter. If you still see black, check `/var/log/enuxdm-session.log` for Python tracebacks and confirm **`x11-xserver-utils`** is installed so `xsetroot` can set a visible root pixel during startup.

### Logout from XFCE sometimes leaves no greeter (blank or stuck X)

After **Log Out** / **Restart** / **Shut Down**, the session ends and `xinit` loops back to the greeter. If Xorg exits in a bad state, a stale **`/tmp/.X0-lock`** can remain pointing at a dead PID; the next X start then fails (“Server already active for display 0”) and the cycle breaks. ENuxDM removes that lock when the saved PID is no longer running before respawning `xinit`, and `xinitrc` runs **`xsetroot`** again after each session so the greeter has a clean root window.

### XFCE power menu (restart / shut down / suspend) does nothing

Those actions go through **polkit** and **systemd-logind**. Install a polkit agent in the user session, for example:

- Debian / ENux / Ubuntu: `policykit-1-gnome` or **`xfce-polkit`** / **`xfce4-polkit`**
- Ensure the agent autostarts with XFCE (many metapackages add it automatically).

`systemd-logind` and `dbus-user-session` should be active. ENuxDM sets **`XDG_VTNR`**, **`XDG_SEAT`**, and **`XDG_SESSION_TYPE=x11`** so the desktop session is closer to a normal VT login for policy checks.

## Security notes

- Credentials are verified over a root-owned Unix socket; a short-lived root-only file bridges the greeter and `enuxdm-launch` for a second PAM transaction (session modules).
- Replace any other active DM (`gdm3`, `lightdm`, `sddm`, …) before enabling ENuxDM.

## Manual test

From a spare VT (as root), stop getty on tty1 if needed, then:

```bash
/usr/bin/enuxdm
```

Verify login, XFCE start, logout returning to the greeter.
