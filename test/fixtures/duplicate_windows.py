#!/usr/bin/env python3
"""Publish two managed X11 clients with the same exact title."""

import ctypes
import ctypes.util
import os
import tkinter as tk

TITLE = "Deskpal Duplicate App State Fixture"

root = tk.Tk()
root.title(TITLE)
root.geometry("360x220+80+80")
second = tk.Toplevel(root)
second.title(TITLE)
second.geometry("360x220+480+80")
tk.Label(root, text="first duplicate").pack(padx=40, pady=40)
tk.Label(second, text="second duplicate").pack(padx=40, pady=40)


def publish_clients():
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

    def client_window(widget):
        inner = ctypes.c_ulong(widget.winfo_id())
        returned_root = ctypes.c_ulong()
        parent = ctypes.c_ulong()
        children = ctypes.POINTER(ctypes.c_ulong)()
        child_count = ctypes.c_uint()
        lib.XQueryTree(
            display, inner.value, ctypes.byref(returned_root), ctypes.byref(parent),
            ctypes.byref(children), ctypes.byref(child_count),
        )
        return parent.value or inner.value

    clients = (ctypes.c_ulong * 2)(client_window(root), client_window(second))
    root_window = lib.XDefaultRootWindow(display)
    client_list = lib.XInternAtom(display, b"_NET_CLIENT_LIST", 0)
    window_atom = lib.XInternAtom(display, b"WINDOW", 0)
    pid_atom = lib.XInternAtom(display, b"_NET_WM_PID", 0)
    cardinal_atom = lib.XInternAtom(display, b"CARDINAL", 0)
    process_id = ctypes.c_ulong(os.getpid())
    lib.XChangeProperty(
        display, root_window, client_list, window_atom, 32, 0, clients, 2,
    )
    for client in clients:
        lib.XChangeProperty(
            display, client, pid_atom, cardinal_atom, 32, 0,
            ctypes.byref(process_id), 1,
        )
    lib.XFlush(display)
    lib.XCloseDisplay(display)


root.after(100, publish_clients)
root.mainloop()
