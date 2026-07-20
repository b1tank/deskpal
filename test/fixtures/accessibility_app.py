#!/usr/bin/python3
"""GTK fixture for deterministic AT-SPI read-only tool coverage."""

import gi
import os
import time

gi.require_version("Gtk", "3.0")
gi.require_version("Gdk", "3.0")
gi.require_version("Atk", "1.0")
from gi.repository import Atk, Gdk, GLib, Gtk

MODE = os.environ.get("DESKPAL_A11Y_FIXTURE_MODE", "normal")
TITLE = {
    "boundary": "Deskpal Accessibility Boundary Fixture",
    "deep": "Deskpal Accessibility Deep Fixture",
    "ambiguous": "Deskpal Accessibility Ambiguous Fixture",
    "stalled": "Deskpal Accessibility Stalled Fixture",
    "unknown": "Deskpal Accessibility Unknown Fixture",
}.get(MODE, "Deskpal Accessibility Fixture")


if MODE == "stalled":
    class SlowEntryAccessible(Gtk.EntryAccessible):
        def do_get_extents(self, coord_type):
            time.sleep(2)
            return super().do_get_extents(coord_type)


    class SlowEntry(Gtk.Entry):
        pass


    SlowEntry.set_accessible_type(SlowEntryAccessible)
elif MODE == "unknown":
    class UnknownRoleAccessible(Gtk.EntryAccessible):
        def do_get_role(self):
            return Atk.Role.UNKNOWN


    class UnknownRoleEntry(Gtk.Entry):
        pass


    UnknownRoleEntry.set_accessible_type(UnknownRoleAccessible)


window = Gtk.Window(title=TITLE)
window.set_default_size(520, 300)
window.set_border_width(24)
window.connect("destroy", Gtk.main_quit)

layout = Gtk.Box(orientation=Gtk.Orientation.VERTICAL, spacing=14)
window.add(layout)

heading = Gtk.Label(label="Semantic controls")
heading.set_xalign(0)
layout.pack_start(heading, False, False, 0)

entry = Gtk.Entry()
entry.set_placeholder_text("Enter a semantic value")
entry.get_accessible().set_name("Validation message")
layout.pack_start(entry, False, False, 0)

password = Gtk.Entry()
password.set_visibility(False)
password.set_text("semantic-secret")
password.get_accessible().set_name("Validation password")
layout.pack_start(password, False, False, 0)

status = Gtk.Label(label="Status: ready")
status.set_xalign(0)
status.get_accessible().set_name("Fixture status")
layout.pack_start(status, False, False, 0)

button = Gtk.Button(label="Apply Message")
button.get_accessible().set_name("Apply validation message")
layout.pack_start(button, False, False, 0)

checkbox = Gtk.CheckButton(label="Approved")
checkbox.get_accessible().set_name("Approval checkbox")
layout.pack_start(checkbox, False, False, 0)

hidden = Gtk.Box(orientation=Gtk.Orientation.VERTICAL)
for index in range(200):
    hidden.pack_start(Gtk.Label(label=f"Hidden node {index:03d}"), False, False, 0)
layout.pack_start(hidden, False, False, 0)
hidden.hide()

deep_target = None
if MODE in ("boundary", "deep"):
    parent = layout
    nested_count = 30 if MODE == "boundary" else 33
    for _ in range(nested_count):
        nested = Gtk.Box(orientation=Gtk.Orientation.VERTICAL)
        parent.pack_start(nested, False, False, 0)
        parent = nested
    deep_target = Gtk.Entry()
    deep_target.get_accessible().set_name("Deep focus target")
    parent.pack_start(deep_target, False, False, 0)
elif MODE == "stalled":
    slow_entry = SlowEntry()
    slow_entry.set_text("slow")
    slow_entry.get_accessible().set_name("Slow semantic node")
    layout.pack_start(slow_entry, False, False, 0)
elif MODE == "unknown":
    unknown_entry = UnknownRoleEntry()
    unknown_entry.set_text("unknown-role-secret")
    unknown_entry.get_accessible().set_name("Unknown role secret name")
    layout.pack_start(unknown_entry, False, False, 0)


def apply_message(_button):
    status.set_text("Status: " + entry.get_text())


button.connect("clicked", apply_message)
entry.connect("activate", apply_message)
window.show_all()
hidden.hide()


def focus_entry():
    window.present()
    (deep_target if deep_target is not None else entry).grab_focus()
    if window.get_window() is not None:
        window.get_window().focus(Gdk.CURRENT_TIME)
    return False


GLib.timeout_add(100, focus_entry)
Gtk.main()
