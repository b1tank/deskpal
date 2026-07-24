/*
 * deskpal — X11/XCB window management and input
 *
 * Copyright (c) 2026 deskpal contributors
 * SPDX-License-Identifier: MIT
 */
#ifndef DESKPAL_X11_H
#define DESKPAL_X11_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

/* ── Types ────────────────────────────────────────────────────────────────── */

typedef struct {
	unsigned long *window_ids;
	size_t count;
} X11StackingSnapshot;

typedef struct {
	unsigned long id;
	char          title[256];
	char          app_class[128];
	int           x, y;
	int           width, height;
	int           viewable;
	long          pid;
} WindowInfo;

/* ── Init / cleanup ───────────────────────────────────────────────────────── */

int  x11_init(int enable_uinput);
void x11_cleanup(void);

/* ── Window management ────────────────────────────────────────────────────── */

/* List visible windows. Returns count, fills array up to max_count.
 * If name_filter is non-NULL, only include windows whose title contains it.
 * include_all uses recursive X11 discovery instead of the window manager's
 * top-level client list, which is useful for dialogs and helper windows. */
int x11_list_windows(WindowInfo *out, int max_count, const char *name_filter,
	                 int include_all);

/* Find best-matching window by name. Returns window ID or 0. */
unsigned long x11_find_window(const char *name);

/* Resolve an exact, case-sensitive title across managed windows. Returns the
 * total match count and the first match; complete is false after traversal
 * errors so callers can fail closed instead of accepting a partial result. */
int x11_find_window_exact(const char *name, WindowInfo *first,
                          int *match_count, int *complete);

/* Find a managed application window by title or WM_CLASS. */
unsigned long x11_find_app(const char *name);

/* Get info for a specific window. Returns 0 on success. */
int x11_get_window_info(unsigned long wid, WindowInfo *out);

/* Focus / activate a window. */
int x11_focus_window(unsigned long wid);

/* Resize a window. */
int x11_resize_window(unsigned long wid, int width, int height);

/* Get the active window ID. */
unsigned long x11_get_active_window(void);

/* Snapshot the exact EWMH stacking order. Returns -1 when unavailable or
 * incomplete. The caller must release successful snapshots. */
int x11_get_stacking_snapshot(X11StackingSnapshot *snapshot);
void x11_free_stacking_snapshot(X11StackingSnapshot *snapshot);
int x11_stacking_snapshots_equal(const X11StackingSnapshot *left,
                                 const X11StackingSnapshot *right);

/* ── Input ────────────────────────────────────────────────────────────────── */

/* Move mouse to absolute screen coordinates. */
int x11_mouse_move(int x, int y);

/* Click at current position. button: 1=left, 2=middle, 3=right. */
int x11_click(int button, int repeat);

/* Move-and-click in one shot relative to a specific window.
 *
 * On Wayland this is the *only* reliable path: `xdo_get_window_location`
 * reports a coordinate that doesn't match the renderer's content origin
 * for Mutter-managed Electron windows (observed ~90 screen-px offset
 * with HiDPI scaling), and uinput BTN_LEFT races wl_pointer focus
 * transitions for Xwayland clients. Routing the whole sequence through
 * `xdotool --window` uses the X server's own coordinate system and
 * delivers events via XTEST, which both fixes the routing problem and
 * sidesteps the broken window origin.
 *
 * x, y are window-content-relative pixels. */
int x11_window_click(unsigned long wid, int x, int y, int button, int repeat);

/* Move the cursor to (x, y) in a window's content coordinate space.
 * Same Wayland rationale as `x11_window_click`. */
int x11_window_mouse_move(unsigned long wid, int x, int y);

/* True if running under a Wayland compositor (mutter, sway, etc.). */
int x11_is_wayland(void);

/* Type text string. delay_ms between keystrokes. */
int x11_type_text(const char *text, int delay_ms);

/* Send key combination (e.g. "ctrl+s", "Return"). */
int x11_key_press(const char *keys);

/* Drag from (x1,y1) to (x2,y2). Absolute screen coordinates.
 * steps controls smoothness (more steps = slower, smoother). */
int x11_drag(int x1, int y1, int x2, int y2, int button, int steps);

/* Press/release mouse button without moving. */
int x11_mouse_down(int button);
int x11_mouse_up(int button);

/* Scroll: button 4=up, 5=down. */
int x11_scroll(int button, int clicks);

/* ── Display info ─────────────────────────────────────────────────────────── */

/* Get display scale factor (1 or 2 for HiDPI). */
int x11_get_scale_factor(void);

#endif /* DESKPAL_X11_H */
