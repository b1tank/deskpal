#!/usr/bin/python3
"""GTK fixture for deterministic AT-SPI inspection and action coverage."""

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
    "mutation": "Deskpal Accessibility Mutation Fixture",
    "interleave": "Deskpal Accessibility Interleave Fixture",
    "defunct": "Deskpal Accessibility Defunct Fixture",
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
elif MODE == "mutation":
    class SlowMutationAccessible(Gtk.EntryAccessible):
        def do_set_text_contents(self, value):
            widget = self.get_widget()
            if widget is not None:
                widget.set_text(value)
            time.sleep(2)
            return True


    class SlowMutationEntry(Gtk.Entry):
        pass


    SlowMutationEntry.set_accessible_type(SlowMutationAccessible)
elif MODE == "interleave":
    interleave_read_count = [0]
    interleave_replace = [None]

    class InterleaveVerificationAccessible(Gtk.EntryAccessible):
        def do_get_text(self, start_offset, end_offset):
            interleave_read_count[0] += 1
            if interleave_read_count[0] == 2 and interleave_replace[0] is not None:
                interleave_replace[0]()
            return super().do_get_text(start_offset, end_offset)


    class InterleaveVerificationEntry(Gtk.Entry):
        pass


    InterleaveVerificationEntry.set_accessible_type(
        InterleaveVerificationAccessible
    )
elif MODE == "defunct":
    class DefunctEntryAccessible(Gtk.EntryAccessible):
        def do_ref_state_set(self):
            states = Atk.StateSet.new()
            states.add_state(Atk.StateType.DEFUNCT)
            return states


    class DefunctEntry(Gtk.Entry):
        pass


    DefunctEntry.set_accessible_type(DefunctEntryAccessible)


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
entry_holder = [entry]

password = Gtk.Entry()
password.set_visibility(False)
password.set_text("semantic-secret")
password.get_accessible().set_name("Validation password")
layout.pack_start(password, False, False, 0)

status = Gtk.Label(label="Status: ready")
status.set_xalign(0)
status.get_accessible().set_name("Fixture status")
layout.pack_start(status, False, False, 0)

apply_count = [0]
count_label = Gtk.Label(label="Apply count: 0")
count_label.set_xalign(0)
count_label.get_accessible().set_name("Apply count")
layout.pack_start(count_label, False, False, 0)

generation = Gtk.Label(label="Entry generation: 1")
generation.set_xalign(0)
generation.get_accessible().set_name("Entry generation")
layout.pack_start(generation, False, False, 0)

replace_entry = Gtk.Button(label="Replace Entry")
replace_entry.get_accessible().set_name("Replace validation field")
layout.pack_start(replace_entry, False, False, 0)

long_verification = Gtk.Label(label="x" * 2049)
long_verification.get_accessible().set_name("Long verification")
long_verification.set_no_show_all(True)
long_verification.hide()
layout.pack_start(long_verification, False, False, 0)

button = Gtk.Button(label="Apply Message")
button.get_accessible().set_name("Apply validation message")
layout.pack_start(button, False, False, 0)

checkbox = Gtk.CheckButton(label="Approved")
checkbox.get_accessible().set_name("Approval checkbox")
layout.pack_start(checkbox, False, False, 0)

for label in ("Duplicate A", "Duplicate B"):
    duplicate = Gtk.Button(label=label)
    duplicate.get_accessible().set_name("Duplicate action")
    layout.pack_start(duplicate, False, False, 0)

hidden = Gtk.Box(orientation=Gtk.Orientation.VERTICAL)
hidden.set_no_show_all(True)
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
elif MODE == "mutation":
    slow_mutation = SlowMutationEntry()
    slow_mutation.set_text("before")
    slow_mutation.get_accessible().set_name("Slow mutation field")
    layout.pack_start(slow_mutation, False, False, 0)
elif MODE == "interleave":
    interleave_target = [Gtk.Button(label="Interleave Target")]
    interleave_target[0].get_accessible().set_name("Interleave target")
    layout.pack_start(interleave_target[0], False, False, 0)

    interleave_verification = InterleaveVerificationEntry()
    interleave_verification.set_text("pending")
    interleave_verification.get_accessible().set_name(
        "Interleave verification"
    )
    layout.pack_start(interleave_verification, False, False, 0)

    interleave_count = Gtk.Label(label="Interleave count: 0")
    interleave_count.get_accessible().set_name("Interleave count")
    layout.pack_start(interleave_count, False, False, 0)

    def original_interleave(_button):
        interleave_count.set_text("Interleave count: original")
        interleave_verification.set_text("original clicked")

    def replacement_interleave(_button):
        interleave_count.set_text("Interleave count: replacement")
        interleave_verification.set_text("replacement clicked")

    interleave_target[0].connect("clicked", original_interleave)

    def replace_interleave_target():
        old = interleave_target[0]
        index = layout.child_get_property(old, "position")
        layout.remove(old)
        replacement = Gtk.Button(label="Interleave Target Replacement")
        replacement.get_accessible().set_name("Interleave target")
        replacement.connect("clicked", replacement_interleave)
        layout.pack_start(replacement, False, False, 0)
        layout.reorder_child(replacement, index)
        replacement.show()
        interleave_target[0] = replacement
        interleave_replace[0] = None

    interleave_replace[0] = replace_interleave_target
elif MODE == "defunct":
    defunct_count = Gtk.Label(label="Defunct count: 0")
    defunct_count.get_accessible().set_name("Defunct count")
    layout.pack_start(defunct_count, False, False, 0)

    defunct_target = Gtk.Button(label="Defunct Target")
    defunct_target.get_accessible().set_name("Defunct target")
    defunct_target.connect(
        "clicked", lambda _button: defunct_count.set_text("Defunct count: 1")
    )
    layout.pack_start(defunct_target, False, False, 0)

    defunct_verifier = DefunctEntry()
    defunct_verifier.set_text("defunct")
    defunct_verifier.get_accessible().set_name("Defunct verifier")
    layout.pack_start(defunct_verifier, False, False, 0)


def apply_message(_button):
    apply_count[0] += 1
    status.set_text("Status: " + entry_holder[0].get_text())
    count_label.set_text(f"Apply count: {apply_count[0]}")


def replace_validation_entry(_button):
    old_entry = entry_holder[0]
    layout.remove(old_entry)
    replacement = Gtk.Entry()
    replacement.set_placeholder_text("Replacement semantic value")
    replacement.get_accessible().set_name("Replacement message")
    replacement.connect("activate", apply_message)
    layout.pack_start(replacement, False, False, 0)
    layout.reorder_child(replacement, 1)
    replacement.show()
    entry_holder[0] = replacement
    generation.set_text("Entry generation: 2")
    replace_entry.set_sensitive(False)


button.connect("clicked", apply_message)
entry.connect("activate", apply_message)
replace_entry.connect("clicked", replace_validation_entry)
window.show_all()
hidden.hide()
long_verification.hide()


def focus_entry():
    window.present()
    (deep_target if deep_target is not None else entry_holder[0]).grab_focus()
    if window.get_window() is not None:
        window.get_window().focus(Gdk.CURRENT_TIME)
    return False


GLib.timeout_add(100, focus_entry)
Gtk.main()
