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

/* ── Types ────────────────────────────────────────────────────────────────── */

typedef struct {
	unsigned long id;
	char          title[256];
	int           x, y;
	int           width, height;
	long          pid;
} WindowInfo;

/* ── Init / cleanup ───────────────────────────────────────────────────────── */

int  x11_init(void);
void x11_cleanup(void);

/* ── Window management ────────────────────────────────────────────────────── */

/* List visible windows. Returns count, fills array up to max_count.
 * If name_filter is non-NULL, only include windows whose title contains it. */
int x11_list_windows(WindowInfo *out, int max_count, const char *name_filter);

/* Find best-matching window by name. Returns window ID or 0. */
unsigned long x11_find_window(const char *name);

/* Get info for a specific window. Returns 0 on success. */
int x11_get_window_info(unsigned long wid, WindowInfo *out);

/* Focus / activate a window. */
int x11_focus_window(unsigned long wid);

/* Resize a window. */
int x11_resize_window(unsigned long wid, int width, int height);

/* Get the active window ID. */
unsigned long x11_get_active_window(void);

/* ── Input ────────────────────────────────────────────────────────────────── */

/* Move mouse to absolute screen coordinates. */
int x11_mouse_move(int x, int y);

/* Click at current position. button: 1=left, 2=middle, 3=right. */
int x11_click(int button, int repeat);

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
