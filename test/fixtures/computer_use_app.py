#!/usr/bin/env python3
"""Deterministic Tk application used by deskpal computer-use E2E tests."""

import tkinter as tk
import ctypes
import ctypes.util
import os

TITLE = "Deskpal Computer Use Fixture"

root = tk.Tk()
root.title(TITLE)
root.geometry("720x520+40+40")
root.minsize(480, 320)


def register_ewmh_client():
    """Publish this fixture as the only managed client on bare Xvfb."""
    lib = ctypes.CDLL(ctypes.util.find_library("X11"))
    lib.XOpenDisplay.restype = ctypes.c_void_p
    lib.XDefaultRootWindow.argtypes = [ctypes.c_void_p]
    lib.XDefaultRootWindow.restype = ctypes.c_ulong
    lib.XInternAtom.argtypes = [ctypes.c_void_p, ctypes.c_char_p, ctypes.c_int]
    lib.XInternAtom.restype = ctypes.c_ulong
    lib.XChangeProperty.argtypes = [
        ctypes.c_void_p, ctypes.c_ulong, ctypes.c_ulong, ctypes.c_ulong,
        ctypes.c_int, ctypes.c_int, ctypes.c_void_p, ctypes.c_int,
    ]
    lib.XQueryTree.argtypes = [
        ctypes.c_void_p, ctypes.c_ulong,
        ctypes.POINTER(ctypes.c_ulong), ctypes.POINTER(ctypes.c_ulong),
        ctypes.POINTER(ctypes.POINTER(ctypes.c_ulong)), ctypes.POINTER(ctypes.c_uint),
    ]
    lib.XCloseDisplay.argtypes = [ctypes.c_void_p]
    display = lib.XOpenDisplay(None)
    if not display:
        return
    root_window = lib.XDefaultRootWindow(display)
    client_list = lib.XInternAtom(display, b"_NET_CLIENT_LIST", 0)
    window_atom = lib.XInternAtom(display, b"WINDOW", 0)
    inner = ctypes.c_ulong(root.winfo_id())
    returned_root = ctypes.c_ulong()
    parent = ctypes.c_ulong()
    children = ctypes.POINTER(ctypes.c_ulong)()
    child_count = ctypes.c_uint()
    lib.XQueryTree(
        display, inner.value, ctypes.byref(returned_root), ctypes.byref(parent),
        ctypes.byref(children), ctypes.byref(child_count),
    )
    client = ctypes.c_ulong(parent.value or inner.value)
    lib.XChangeProperty(
        display, root_window, client_list, window_atom, 32, 0,
        ctypes.byref(client), 1,
    )
    pid_atom = lib.XInternAtom(display, b"_NET_WM_PID", 0)
    cardinal_atom = lib.XInternAtom(display, b"CARDINAL", 0)
    process_id = ctypes.c_ulong(os.getpid())
    lib.XChangeProperty(
        display, client, pid_atom, cardinal_atom, 32, 0,
        ctypes.byref(process_id), 1,
    )
    lib.XFlush(display)
    lib.XCloseDisplay(display)


root.after(100, register_ewmh_client)

header = tk.Label(root, text="Deskpal computer use", font=("DejaVu Sans", 24))
header.pack(pady=(18, 8))

entry = tk.Entry(root, name="entry", font=("DejaVu Sans", 18), width=34)
entry.pack(pady=8)
entry.focus_set()

status = tk.Label(root, name="status", text="Status: ready", font=("DejaVu Sans", 18))
status.pack(pady=8)


def apply_text():
    status.configure(text=f"Status: {entry.get()}")


apply_button = tk.Button(
    root,
    name="apply",
    text="Apply Text",
    command=apply_text,
    font=("DejaVu Sans", 18),
)
apply_button.pack(pady=8)

scroll_frame = tk.Frame(root)
scroll_frame.pack(fill="both", expand=True, padx=24, pady=8)
scrollbar = tk.Scrollbar(scroll_frame)
scrollbar.pack(side="right", fill="y")
items = tk.Listbox(
    scroll_frame,
    name="items",
    font=("DejaVu Sans", 14),
    yscrollcommand=scrollbar.set,
)
for index in range(1, 41):
    items.insert("end", f"Item {index:02d}")
items.pack(side="left", fill="both", expand=True)
scrollbar.config(command=items.yview)

actions = tk.Frame(root)
actions.pack(pady=(4, 16))

tooltip = None
tooltip_timer = None


def show_tooltip():
    global tooltip
    tooltip = tk.Toplevel(root)
    tooltip.overrideredirect(True)
    tooltip.attributes("-topmost", True)
    x = root.winfo_rootx() + root.winfo_width() + 20
    y = root.winfo_rooty() + root.winfo_height() - 160
    tooltip.geometry(f"+{x}+{y}")
    tk.Label(
        tooltip,
        text="Deskpal tooltip detected",
        font=("DejaVu Sans", 16),
        background="#fff4b8",
        foreground="#111111",
        padx=12,
        pady=8,
        relief="solid",
        borderwidth=1,
    ).pack()


def schedule_tooltip(_event):
    global tooltip_timer
    tooltip_timer = root.after(250, show_tooltip)


def hide_tooltip(_event):
    global tooltip, tooltip_timer
    if tooltip_timer is not None:
        root.after_cancel(tooltip_timer)
        tooltip_timer = None
    if tooltip is not None:
        tooltip.destroy()
        tooltip = None


tooltip_button = tk.Button(
    actions,
    name="tooltip",
    text="Hover Target",
    font=("DejaVu Sans", 16),
)
tooltip_button.pack(side="left", padx=10)
tooltip_button.bind("<Enter>", schedule_tooltip)
tooltip_button.bind("<Leave>", hide_tooltip)

close_button = tk.Button(
    actions,
    name="close",
    text="Close Fixture",
    command=root.destroy,
    font=("DejaVu Sans", 16),
)
close_button.pack(side="left", padx=10)


transients = []


def close_transients():
    global transients
    for window in transients:
        window.destroy()
    transients = []


def open_transients(count=50):
    global transients
    close_transients()
    for index in range(count):
        window = tk.Toplevel(root)
        window.title(f"Transient {index:02d} " + "x" * 180)
        window.geometry(f"180x80+{20 + index % 10 * 5}+{20 + index % 8 * 5}")
        window.transient(root)
        tk.Label(window, text=f"Transient {index:02d}").pack(padx=8, pady=8)
        transients.append(window)
    root.update_idletasks()
    root.after(1500, close_transients)


stress_button = tk.Button(
    actions,
    name="stress",
    text="Open Stress Windows",
    command=open_transients,
    font=("DejaVu Sans", 16),
)
stress_button.pack(side="left", padx=10)


def open_race_window():
    race = tk.Toplevel(root)
    race.title("Deskpal Race Window")
    race.geometry("360x180+760+80")
    target = tk.Label(
        race,
        text="Race Target",
        font=("DejaVu Sans", 24),
        padx=30,
        pady=30,
        relief="raised",
    )
    target.pack(expand=True)
    target.bind("<Enter>", lambda _event: race.destroy())


root.bind("<Control-r>", lambda _event: open_race_window())


race_button = tk.Button(
    actions,
    name="race",
    text="Open Race Window",
    command=open_race_window,
    font=("DejaVu Sans", 16),
)
race_button.pack(side="left", padx=10)

root.mainloop()
