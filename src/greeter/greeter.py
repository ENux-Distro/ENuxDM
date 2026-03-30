#!/usr/bin/env python3
"""ENuxDM GTK4 greeter — minimal X session login UI."""

from __future__ import annotations

import json
import os

# Plain Xorg (no compositor): GL/Vulkan GSK often paints a blank/black window.
os.environ.setdefault("GDK_BACKEND", "x11")
os.environ.setdefault("GSK_RENDERER", "cairo")
os.environ.setdefault("NO_AT_BRIDGE", "1")
import random
import socket
import subprocess
import sys
import pwd

import gi

gi.require_version("Gtk", "4.0")
gi.require_version("Gdk", "4.0")
gi.require_version("GdkPixbuf", "2.0")

from gi.repository import GLib, Gtk, Gdk, GdkPixbuf  # noqa: E402

# Cairo operator enums (pycairo-free); GTK passes a cairo.Context from gi bindings.
_CAIRO_OP_OVER = 2
_CAIRO_OP_ADD = 12


class Particle:
    __slots__ = ("x", "y", "vx", "vy", "r", "a")

    def __init__(self, x, y, vx, vy, r, a):
        self.x = x
        self.y = y
        self.vx = vx
        self.vy = vy
        self.r = r
        self.a = a


def list_session_users() -> list[str]:
    """Human users first (uid ascending so 1000 is default), root last — not alphabetical."""
    regular: list[tuple[int, str]] = []
    root_name: str | None = None
    try:
        for p in pwd.getpwall():
            sh = p.pw_shell or ""
            if "nologin" in sh or sh.endswith("/false"):
                continue
            if p.pw_uid == 0:
                root_name = p.pw_name
                continue
            if p.pw_uid < 1000:
                continue
            regular.append((p.pw_uid, p.pw_name))
    except Exception:
        pass
    regular.sort(key=lambda t: (t[0], t[1]))
    out = [name for _, name in regular]
    if root_name:
        out.append(root_name)
    return out


def keyboard_layout_badge() -> str:
    try:
        out = subprocess.check_output(
            ["setxkbmap", "-query"], text=True, timeout=2, stderr=subprocess.DEVNULL
        )
        for line in out.splitlines():
            if line.startswith("layout:"):
                lay = line.split(":", 1)[1].strip().upper()
                return lay[:5] if lay else ""
    except Exception:
        pass
    return ""


def spawn_power_command(kind: str) -> bool:
    """Run reboot or poweroff. Prefers systemctl; falls back to traditional tools. Returns True if a process was started."""
    if kind not in ("reboot", "poweroff"):
        return False
    if kind == "reboot":
        candidates = (
            ("/usr/bin/systemctl", ["systemctl", "reboot"]),
            ("/bin/systemctl", ["systemctl", "reboot"]),
            ("/sbin/reboot", ["reboot"]),
            ("/usr/sbin/reboot", ["reboot"]),
        )
    else:
        candidates = (
            ("/usr/bin/systemctl", ["systemctl", "poweroff"]),
            ("/bin/systemctl", ["systemctl", "poweroff"]),
            ("/sbin/poweroff", ["poweroff"]),
            ("/usr/sbin/poweroff", ["poweroff"]),
        )
    for path, argv in candidates:
        if not os.path.isfile(path) or not os.access(path, os.X_OK):
            continue
        try:
            subprocess.Popen(
                [path] + list(argv[1:]),
                start_new_session=True,
                stdin=subprocess.DEVNULL,
                stdout=subprocess.DEVNULL,
                stderr=subprocess.DEVNULL,
            )
            return True
        except OSError:
            continue
    return False


def try_login(user: str, password: str) -> tuple[bool, str | None]:
    path = os.environ.get("ENUXDM_AUTH_SOCKET", "/run/enuxdm/auth.sock")
    payload = json.dumps({"cmd": "login", "user": user, "password": password})
    s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
    try:
        s.settimeout(60)
        s.connect(path)
        s.sendall(payload.encode("utf-8") + b"\n")
        buf = b""
        while True:
            chunk = s.recv(8192)
            if not chunk:
                break
            buf += chunk
            if b"\n" in buf:
                break
        data = buf.decode("utf-8", errors="replace").strip()
        r = json.loads(data)
        if r.get("ok"):
            return True, None
        return False, r.get("code") or r.get("error") or "fail"
    except Exception as e:
        return False, str(e)
    finally:
        s.close()


def default_screen_size():
    """Return (width, height) for initial window sizing on bare X11."""
    disp = Gdk.Display.get_default()
    if not disp:
        return 1920, 1080
    mon = disp.get_primary_monitor()
    if mon is None and disp.get_monitors().get_n_items() > 0:
        mon = disp.get_monitors().get_item(0)
    if mon is None:
        return 1920, 1080
    g = mon.get_geometry()
    w, h = max(g.width, 800), max(g.height, 600)
    return w, h


class EnuxdmGreeter(Gtk.Application):
    def __init__(self):
        super().__init__(application_id="linux.enuxdm.Greeter")
        self._particles: list[Particle] = []
        self._accent = os.environ.get("ENUXDM_ACCENT", "#b4b8c2")
        self._bg_path = os.environ.get("ENUXDM_BACKGROUND", "")
        self._font = os.environ.get("ENUXDM_FONT_FAMILY", "Inter")
        self._win: Gtk.ApplicationWindow | None = None
        self._card: Gtk.Box | None = None
        self._user_widget: Gtk.Widget | None = None
        self._pass_entry: Gtk.Entry | None = None
        self._err_label: Gtk.Label | None = None
        self._clock: Gtk.Label | None = None

    def do_activate(self):
        if self._win:
            self._win.present()
            return

        win = Gtk.ApplicationWindow(application=self)
        win.set_title("ENuxDM")
        win.set_decorated(False)
        win.set_resizable(True)
        win.set_name("enuxdm-window")
        self._win = win

        disp = Gdk.Display.get_default()
        gw, gh = default_screen_size()
        win.set_default_size(gw, gh)
        win.set_size_request(min(gw, 800), min(gh, 600))

        css_path = os.path.join(os.path.dirname(os.path.abspath(__file__)), "style.css")
        provider = Gtk.CssProvider()
        try:
            provider.load_from_path(css_path)
        except Exception:
            provider.load_from_data(b"")
        Gtk.StyleContext.add_provider_for_display(
            disp or Gdk.Display.get_default(), provider, Gtk.STYLE_PROVIDER_PRIORITY_USER
        )

        accent_css = f"""
        #login-button {{ background-color: {self._accent}; color: #101012; }}
        """
        accent_p = Gtk.CssProvider()
        accent_p.load_from_data(accent_css.encode())
        Gtk.StyleContext.add_provider_for_display(
            disp or Gdk.Display.get_default(), accent_p, Gtk.STYLE_PROVIDER_PRIORITY_APPLICATION
        )

        font_desc = f'"{self._font}", "DejaVu Sans", sans-serif'
        base_css = f"window * {{ font-family: {font_desc}; }}"
        font_p = Gtk.CssProvider()
        font_p.load_from_data(base_css.encode())
        Gtk.StyleContext.add_provider_for_display(
            disp or Gdk.Display.get_default(), font_p, Gtk.STYLE_PROVIDER_PRIORITY_APPLICATION + 1
        )

        overlay = Gtk.Overlay()
        win.set_child(overlay)

        da = Gtk.DrawingArea()
        da.set_hexpand(True)
        da.set_vexpand(True)
        da.set_name("bg-area")
        da.set_draw_func(self._draw_bg, None)
        overlay.set_child(da)

        card = Gtk.Box(orientation=Gtk.Orientation.VERTICAL, spacing=14)
        card.set_name("login-card")
        card.set_halign(Gtk.Align.CENTER)
        card.set_valign(Gtk.Align.CENTER)
        card.set_size_request(380, -1)
        card.set_margin_top(48)
        card.set_margin_bottom(48)
        self._card = card

        header = Gtk.Box(orientation=Gtk.Orientation.VERTICAL, spacing=4)
        header.set_halign(Gtk.Align.CENTER)
        tag = Gtk.Label(label="ENUX LINUX")
        tag.set_name("tagline")
        header.append(tag)
        card.append(header)

        users = list_session_users()
        if len(users) > 1:
            try:
                sl = Gtk.StringList()
                for u in users:
                    sl.append(u)
                dd = Gtk.DropDown(model=sl)
                dd.set_name("user-entry")
                self._user_widget = dd
                try:
                    u0 = os.environ.get("USER", "").strip()
                    pick = 0
                    if u0 and u0 != "root":
                        for i in range(sl.get_n_items()):
                            if sl.get_string(i) == u0:
                                pick = i
                                break
                    dd.set_selected(pick)
                except Exception:
                    pass
                card.append(dd)
            except Exception:
                user_entry = Gtk.Entry()
                user_entry.set_placeholder_text("Username")
                user_entry.set_name("user-entry")
                self._user_widget = user_entry
                card.append(user_entry)
        else:
            user_entry = Gtk.Entry()
            user_entry.set_placeholder_text("Username")
            user_entry.set_name("user-entry")
            self._user_widget = user_entry
            card.append(user_entry)

        pass_entry = Gtk.Entry()
        pass_entry.set_visibility(False)
        pass_entry.set_input_purpose(Gtk.InputPurpose.PASSWORD)
        pass_entry.set_placeholder_text("Password")
        pass_entry.set_name("pass-entry")
        card.append(pass_entry)
        self._pass_entry = pass_entry

        err = Gtk.Label(label="")
        err.set_name("error-label")
        err.set_xalign(0.0)
        card.append(err)
        self._err_label = err

        btn = Gtk.Button(label="Sign in")
        btn.set_name("login-button")
        btn.connect("clicked", self._on_login)
        card.append(btn)

        power_row = Gtk.Box(orientation=Gtk.Orientation.HORIZONTAL, spacing=12)
        power_row.set_name("power-row")
        power_row.set_halign(Gtk.Align.CENTER)
        b_reboot = Gtk.Button(label="Restart")
        b_reboot.set_tooltip_text("Restart the computer")
        b_reboot.connect(
            "clicked",
            lambda *_: self._confirm_power_action(
                "Restart",
                "The system will restart. Save any work elsewhere first.",
                "reboot",
                confirm_style="restart",
            ),
        )
        b_off = Gtk.Button(label="Shut down")
        b_off.set_tooltip_text("Shut down the computer")
        b_off.connect(
            "clicked",
            lambda *_: self._confirm_power_action(
                "Shut down",
                "The system will power off. Save any work elsewhere first.",
                "poweroff",
                confirm_style="shutdown",
            ),
        )
        power_row.append(b_reboot)
        power_row.append(b_off)
        card.append(power_row)

        overlay.add_overlay(card)

        clock = Gtk.Label()
        clock.set_name("clock-label")
        clock.set_halign(Gtk.Align.CENTER)
        clock.set_valign(Gtk.Align.START)
        clock.set_margin_top(28)
        overlay.add_overlay(clock)
        self._clock = clock

        host = Gtk.Label(label="")
        host.set_name("hostname-label")
        host.set_halign(Gtk.Align.START)
        host.set_valign(Gtk.Align.END)
        host.set_margin_start(24)
        host.set_margin_bottom(20)
        try:
            host.set_label(socket.gethostname())
        except Exception:
            pass
        overlay.add_overlay(host)

        lay = keyboard_layout_badge()
        if lay:
            lb = Gtk.Label(label=lay)
            lb.set_name("layout-badge")
            lb.set_halign(Gtk.Align.END)
            lb.set_valign(Gtk.Align.START)
            lb.set_margin_end(24)
            lb.set_margin_top(24)
            overlay.add_overlay(lb)

        win.connect("close-request", lambda *_: True)

        GLib.timeout_add(1000, self._tick_clock)
        GLib.timeout_add(50, self._tick_anim, da)
        self._init_particles(1920, 1080)

        self._tick_clock()
        win.fullscreen()
        win.present()
        self._grab_focus_first()

        pass_entry.connect("activate", self._on_login)

    def _confirm_power_action(
        self, title: str, body: str, power_kind: str, confirm_style: str
    ):
        dlg = Gtk.Window()
        dlg.set_name("power-confirm-dialog")
        dlg.set_transient_for(self._win)
        dlg.set_modal(True)
        dlg.set_resizable(False)
        dlg.set_title(title)
        dlg.set_default_size(420, -1)

        outer = Gtk.Box(orientation=Gtk.Orientation.VERTICAL, spacing=22)
        outer.set_margin_start(26)
        outer.set_margin_end(26)
        outer.set_margin_top(26)
        outer.set_margin_bottom(26)

        hdr = Gtk.Label()
        hdr.set_markup(f"<b>{GLib.markup_escape_text(title)}</b>")
        hdr.set_xalign(0.0)
        outer.append(hdr)

        lbl = Gtk.Label(label=body)
        lbl.set_wrap(True)
        lbl.set_max_width_chars(44)
        lbl.set_xalign(0.0)
        outer.append(lbl)

        err_lbl = Gtk.Label(label="")
        err_lbl.add_css_class("error-label")
        err_lbl.set_xalign(0.0)
        err_lbl.set_visible(False)
        outer.append(err_lbl)

        actions = Gtk.Box(orientation=Gtk.Orientation.HORIZONTAL, spacing=10)
        actions.set_name("power-confirm-actions")
        actions.set_halign(Gtk.Align.END)

        cancel = Gtk.Button(label="Cancel")
        cancel.add_css_class("power-btn-cancel")
        confirm = Gtk.Button(label=title)
        confirm.add_css_class("power-btn-confirm")
        confirm.add_css_class(confirm_style)

        def on_cancel(*_a):
            dlg.destroy()

        def on_confirm(*_a):
            if spawn_power_command(power_kind):
                dlg.destroy()
                return
            err_lbl.set_label("Could not run power command (missing systemctl/reboot?)")
            err_lbl.set_visible(True)

        cancel.connect("clicked", on_cancel)
        confirm.connect("clicked", on_confirm)
        actions.append(cancel)
        actions.append(confirm)
        outer.append(actions)

        dlg.set_child(outer)
        dlg.set_default_widget(cancel)

        def on_close(_win, *_a):
            dlg.destroy()
            return True

        dlg.connect("close-request", on_close)
        dlg.present()

    def _grab_focus_first(self):
        w = self._user_widget
        if isinstance(w, Gtk.Entry):
            w.grab_focus()
        elif isinstance(w, Gtk.DropDown):
            if self._pass_entry:
                self._pass_entry.grab_focus()

    def _username_text(self) -> str:
        w = self._user_widget
        if isinstance(w, Gtk.Entry):
            return w.get_text().strip()
        if isinstance(w, Gtk.DropDown):
            model = w.get_model()
            if not isinstance(model, Gtk.StringList):
                return ""
            i = w.get_selected()
            try:
                inv = Gtk.INVALID_LIST_POSITION  # type: ignore[attr-defined]
            except AttributeError:
                inv = 2**32 - 1
            if i == inv:
                return ""
            return model.get_string(i).strip()
        return ""

    def _init_particles(self, w: float, h: float):
        self._particles.clear()
        for _ in range(90):
            self._particles.append(
                Particle(
                    x=random.uniform(0, max(w, 100)),
                    y=random.uniform(0, max(h, 100)),
                    vx=random.uniform(-6, 6),
                    vy=random.uniform(-4, 4),
                    r=random.uniform(0.4, 1.8),
                    a=random.uniform(0.15, 0.55),
                )
            )

    def _tick_clock(self) -> bool:
        from datetime import datetime

        now = datetime.now().astimezone()
        d = now.strftime("%A, %d %B %Y")
        if self._clock:
            self._clock.set_markup(
                f"{now.strftime('%H:%M')}\n<span size='small' alpha='70%'>{d}</span>"
            )
        return True

    def _tick_anim(self, da: Gtk.DrawingArea) -> bool:
        da.queue_draw()
        return True

    def _draw_bg(self, area: Gtk.DrawingArea, cr, w: int, h: int, _data):
        if w <= 1 or h <= 1:
            return

        if not self._particles:
            self._init_particles(float(w), float(h))

        # Diagonal "mesh" without pycairo gradient types (no `import cairo` / python3-cairo).
        strips = 48
        for i in range(strips):
            u = i / max(strips - 1, 1)
            x0 = w * u
            x1 = w * (i + 1) / strips
            v = 0.035 + 0.02 * u + 0.02 * (1.0 - abs(u - 0.5))
            cr.set_source_rgb(v, v, v + 0.008)
            cr.rectangle(x0, 0, max(x1 - x0, 1), h)
            cr.fill()

        t = GLib.get_monotonic_time() / 1_000_000.0
        cr.set_operator(_CAIRO_OP_ADD)
        for i, orb in enumerate([(0.25, 0.35), (0.72, 0.62), (0.48, 0.18)]):
            ox, oy = orb
            rad = min(w, h) * (0.32 + 0.04 * i)
            cr.set_source_rgba(0.88, 0.88, 0.9, 0.08 + 0.025 * i)
            cr.arc(ox * w, oy * h, rad, 0, 6.283185307179586)
            cr.fill()
        cr.set_operator(_CAIRO_OP_OVER)

        cr.set_operator(_CAIRO_OP_ADD)
        for p in self._particles:
            p.x += p.vx * 0.02
            p.y += p.vy * 0.02
            if p.x < 0 or p.x > w:
                p.vx *= -1
            if p.y < 0 or p.y > h:
                p.vy *= -1
            p.x = max(0, min(w, p.x))
            p.y = max(0, min(h, p.y))
            cr.set_source_rgba(0.82, 0.82, 0.86, p.a * (0.35 + 0.12 * (t % 3)))
            cr.arc(p.x, p.y, p.r * 2.2, 0, 6.283185307179586)
            cr.fill()
        cr.set_operator(_CAIRO_OP_OVER)

        if self._bg_path and os.path.isfile(self._bg_path):
            try:
                pb = GdkPixbuf.Pixbuf.new_from_file_at_scale(self._bg_path, w, h, False)
                cr.save()
                Gdk.cairo_set_source_pixbuf(cr, pb, 0, 0)
                cr.paint_with_alpha(0.22)
                cr.restore()
            except Exception:
                pass

    def _on_login(self, *_):
        if not self._card or not self._pass_entry or not self._err_label:
            return
        user = self._username_text()
        pw = self._pass_entry.get_text()
        self._err_label.set_label("")
        card = self._card

        ok, _ = try_login(user, pw)
        if ok:
            card.add_css_class("fade-out")

            def finish():
                self.quit()

            GLib.timeout_add(580, finish)
            return

        self._pass_entry.set_text("")
        card.add_css_class("error-shake")
        self._err_label.set_label("Authentication failed.")

        def rm_anim():
            card.remove_css_class("error-shake")
            return False

        GLib.timeout_add(480, rm_anim)
        self._pass_entry.grab_focus()


def main():
    try:
        app = EnuxdmGreeter()
        return app.run(sys.argv)
    except Exception:
        import traceback

        traceback.print_exc(file=sys.stderr)
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
