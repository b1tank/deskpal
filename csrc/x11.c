/*
 * deskpal — X11/XCB window management and input
 *
 * Uses uinput virtual devices for mouse/keyboard (Wayland-compatible),
 * with libxdo fallback for right-click context menus on Xwayland.
 *
 * Copyright (c) 2026 deskpal contributors
 * SPDX-License-Identifier: MIT
 */
#include "x11.h"
#include "uinput.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>
#include <xdo.h>
#include <X11/Xlib.h>
#include <X11/Xatom.h>

xdo_t *g_xdo = NULL;  /* non-static: also used by screenshot.c */

/* ── Init / cleanup ───────────────────────────────────────────────────────── */

int x11_init(void)
{
	g_xdo = xdo_new(NULL);
	if (!g_xdo) return -1;

	/* Try to set up uinput for Wayland-compatible input.
	 * Get screen dimensions from X11 for the ABS axis range. */
	Display *dpy = g_xdo->xdpy;
	int sw = dpy ? DisplayWidth(dpy, DefaultScreen(dpy))  : 3840;
	int sh = dpy ? DisplayHeight(dpy, DefaultScreen(dpy)) : 2160;
	if (uinput_init(sw, sh) == 0) {
		fprintf(stderr, "deskpal: uinput pointer (%dx%d)%s\n",
			sw, sh,
			uinput_kbd_available() ? " + keyboard" : " (keyboard fallback: XTest)");
	} else {
		fprintf(stderr, "deskpal: uinput unavailable, using XTest fallback\n");
	}

	return 0;
}

void x11_cleanup(void)
{
	uinput_cleanup();
	if (g_xdo) {
		xdo_free(g_xdo);
		g_xdo = NULL;
	}
}

/* ── Helpers ──────────────────────────────────────────────────────────────── */

static int fill_window_info(Window wid, WindowInfo *out)
{
	memset(out, 0, sizeof(*out));
	out->id = wid;

	/* Title */
	unsigned char *name = NULL;
	int name_len = 0;
	int name_type = 0;
	if (xdo_get_window_name(g_xdo, wid, &name, &name_len, &name_type) == 0
	    && name) {
		int copy = name_len < (int)sizeof(out->title) - 1
		           ? name_len : (int)sizeof(out->title) - 1;
		memcpy(out->title, name, copy);
		out->title[copy] = '\0';
		XFree(name);
	}

	/* Geometry */
	unsigned int w = 0, h = 0;
	int x = 0, y = 0;
	/* getwindowlocation gives top-left in screen coords */
	if (xdo_get_window_location(g_xdo, wid, &x, &y, NULL) == 0) {
		out->x = x;
		out->y = y;
	}
	if (xdo_get_window_size(g_xdo, wid, &w, &h) == 0) {
		out->width = (int)w;
		out->height = (int)h;
	}

	/* PID */
	int pid = 0;
	if (xdo_get_pid_window(g_xdo, wid) > 0)
		pid = xdo_get_pid_window(g_xdo, wid);
	out->pid = pid;

	return 0;
}

/* ── Window management ────────────────────────────────────────────────────── */

int x11_list_windows(WindowInfo *out, int max_count, const char *name_filter)
{
	xdo_search_t search;
	memset(&search, 0, sizeof(search));
	search.require = SEARCH_ANY;
	search.searchmask = SEARCH_NAME;
	if (name_filter && name_filter[0]) {
		search.winname = name_filter;
	} else {
		/* Match any window that has a name */
		search.winname = "";
	}
	search.max_depth = -1;

	Window *results = NULL;
	unsigned int nresults = 0;
	if (xdo_search_windows(g_xdo, &search, &results, &nresults) != 0)
		return 0;

	int count = 0;
	for (unsigned int i = 0; i < nresults && count < max_count; i++) {
		WindowInfo info;
		fill_window_info(results[i], &info);

		/* Skip tiny windows (hidden GDK helpers, etc.) */
		if (info.width < 10 || info.height < 10) continue;
		/* Skip untitled windows */
		if (info.title[0] == '\0') continue;

		out[count++] = info;
	}

	free(results);
	return count;
}

unsigned long x11_find_window(const char *name)
{
	xdo_search_t search;
	memset(&search, 0, sizeof(search));
	search.require = SEARCH_ANY;
	search.searchmask = SEARCH_NAME;
	search.winname = name;
	search.max_depth = -1;

	Window *results = NULL;
	unsigned int nresults = 0;
	if (xdo_search_windows(g_xdo, &search, &results, &nresults) != 0 ||
	    nresults == 0) {
		free(results);
		return 0;
	}

	/* Find best match. Scoring:
	 * - Exact title match with largest area wins immediately
	 * - Otherwise prefer shorter titles (more specific partial match)
	 * - Break title-length ties by largest area */
	unsigned long best = 0;
	int best_title_len = 999999;
	int best_area = 0;
	int best_exact = 0;  /* exact title match flag */

	for (unsigned int i = 0; i < nresults; i++) {
		WindowInfo info;
		fill_window_info(results[i], &info);

		if (info.width < 10 || info.height < 10) continue;
		if (info.title[0] == '\0') continue;

		int is_exact = (strcmp(info.title, name) == 0);
		int title_len = (int)strlen(info.title);
		int area = info.width * info.height;

		/* Prefer: exact > partial, then largest area for exact,
		 * shortest title for partial, then largest area */
		int dominated = 0;
		if (best) {
			if (is_exact && !best_exact)
				dominated = 0;  /* exact beats partial */
			else if (!is_exact && best_exact)
				dominated = 1;  /* partial loses to exact */
			else if (is_exact && best_exact)
				dominated = (area <= best_area);  /* both exact: prefer larger */
			else
				dominated = (title_len > best_title_len ||
				             (title_len == best_title_len && area <= best_area));
		}

		if (!dominated) {
			best = results[i];
			best_title_len = title_len;
			best_area = area;
			best_exact = is_exact;
		}
	}

	free(results);
	return best;
}

int x11_get_window_info(unsigned long wid, WindowInfo *out)
{
	return fill_window_info((Window)wid, out);
}

int x11_focus_window(unsigned long wid)
{
	/* Use both focus and activate for reliable keyboard delivery.
	 * xdo_focus_window sets X11 input focus (needed for XTest keys),
	 * xdo_activate_window raises the window in the stacking order. */
	xdo_focus_window(g_xdo, (Window)wid);
	return xdo_activate_window(g_xdo, (Window)wid);
}

int x11_resize_window(unsigned long wid, int width, int height)
{
	return xdo_set_window_size(g_xdo, (Window)wid, width, height, 0);
}

unsigned long x11_get_active_window(void)
{
	Window wid = 0;
	xdo_get_active_window(g_xdo, &wid);
	return wid;
}

/* ── Input ────────────────────────────────────────────────────────────────── */

int x11_mouse_move(int x, int y)
{
	if (uinput_available()) {
		uinput_mouse_move(x, y);
		/* Also sync the X11 cursor position so xdotool-based
		 * operations (e.g. right-click fallback) work correctly. */
		xdo_move_mouse(g_xdo, x, y, 0);
		return 0;
	}
	return xdo_move_mouse(g_xdo, x, y, 0);
}

int x11_click(int button, int repeat)
{
	if (uinput_available() && button == 1)
		return uinput_click(button, repeat);
	/* Right/middle click: use xdotool CLI rather than libxdo because
	 * xdo_click_window(CURRENTWINDOW) via XTest doesn't trigger GTK
	 * context menus on Xwayland. The CLI xdotool click works. */
	for (int i = 0; i < repeat; i++) {
		char cmd[64];
		snprintf(cmd, sizeof(cmd), "xdotool click %d", button);
		if (system(cmd) != 0)
			return -1;
		if (i < repeat - 1) usleep(50000);
	}
	return 0;
}

int x11_type_text(const char *text, int delay_ms)
{
	if (uinput_kbd_available()) {
		for (int i = 0; text[i]; i++) {
			uinput_type_char(text[i]);
			if (delay_ms > 0) usleep(delay_ms * 1000);
		}
		return 0;
	}
	return xdo_enter_text_window(g_xdo, CURRENTWINDOW, text,
	                             delay_ms * 1000);
}

int x11_key_press(const char *keys)
{
	if (uinput_kbd_available())
		return uinput_key_press(keys);
	return xdo_send_keysequence_window(g_xdo, CURRENTWINDOW, keys, 0);
}

int x11_scroll(int button, int clicks)
{
	if (uinput_available()) {
		/* button 4=up, 5=down → amount: positive=down */
		int amount = (button == 4) ? -clicks : clicks;
		return uinput_scroll(amount);
	}
	for (int i = 0; i < clicks; i++) {
		if (xdo_click_window(g_xdo, CURRENTWINDOW, button) != 0)
			return -1;
	}
	return 0;
}

int x11_drag(int x1, int y1, int x2, int y2, int button, int steps)
{
	if (steps < 1) steps = 10;

	/* Move to start */
	x11_mouse_move(x1, y1);
	usleep(50000);

	/* Press */
	x11_mouse_down(button);
	usleep(50000);

	/* Interpolate movement */
	for (int i = 1; i <= steps; i++) {
		int cx = x1 + (x2 - x1) * i / steps;
		int cy = y1 + (y2 - y1) * i / steps;
		x11_mouse_move(cx, cy);
		usleep(10000); /* 10ms per step */
	}

	usleep(50000);

	/* Release */
	x11_mouse_up(button);
	return 0;
}

int x11_mouse_down(int button)
{
	if (uinput_available())
		return uinput_mouse_down(button);
	return xdo_mouse_down(g_xdo, CURRENTWINDOW, button);
}

int x11_mouse_up(int button)
{
	if (uinput_available())
		return uinput_mouse_up(button);
	return xdo_mouse_up(g_xdo, CURRENTWINDOW, button);
}

/* ── Display info ─────────────────────────────────────────────────────────── */

int x11_get_scale_factor(void)
{
	/* Check GDK_SCALE env first */
	const char *gdk_scale = getenv("GDK_SCALE");
	if (gdk_scale) {
		int s = atoi(gdk_scale);
		if (s > 0) return s;
	}

	/* Try to detect from Xft.dpi resource */
	Display *dpy = g_xdo ? g_xdo->xdpy : NULL;
	if (dpy) {
		char *rms = XResourceManagerString(dpy);
		if (rms) {
			char *dpi_str = strstr(rms, "Xft.dpi:");
			if (dpi_str) {
				int dpi = atoi(dpi_str + 8);
				if (dpi >= 192) return 2;
				if (dpi >= 144) return 2;
			}
		}
	}

	return 1;
}
