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
#include <X11/Xutil.h>

xdo_t *g_xdo = NULL;  /* non-static: also used by screenshot.c */

static int deskpal_x_error_handler(Display *display, XErrorEvent *event)
{
	/* Windows and transient drawables can disappear between recursive search,
	 * geometry, and screenshot requests. These races are normal on a live
	 * desktop and must not let Xlib's default handler terminate the MCP server. */
	if (event->error_code == BadWindow || event->error_code == BadDrawable)
		return 0;

	char message[128];
	XGetErrorText(display, event->error_code, message, sizeof(message));
	fprintf(stderr,
		"deskpal: X11 error: %s (request=%u.%u resource=0x%lx)\n",
		message, event->request_code, event->minor_code, event->resourceid);
	return 0;
}

/* ── Init / cleanup ───────────────────────────────────────────────────────── */

int x11_init(int enable_uinput)
{
	XSetErrorHandler(deskpal_x_error_handler);
	g_xdo = xdo_new(NULL);
	if (!g_xdo) return -1;

	/* Try to set up uinput for Wayland-compatible input.
	 * Get screen dimensions from X11 for the ABS axis range. */
	Display *dpy = g_xdo->xdpy;
	int sw = dpy ? DisplayWidth(dpy, DefaultScreen(dpy))  : 3840;
	int sh = dpy ? DisplayHeight(dpy, DefaultScreen(dpy)) : 2160;
	if (!enable_uinput) {
		fprintf(stderr, "deskpal: %s, using XTest input (%dx%d)\n",
			getenv("DESKPAL_HEADLESS_ACTIVE")
				? "headless display" : "uinput disabled",
			sw, sh);
	} else if (uinput_init(sw, sh) == 0) {
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
	Display *display = g_xdo->xdpy;
	XWindowAttributes attributes;
	if (!XGetWindowAttributes(display, wid, &attributes)) return -1;

	/* Title */
	Atom utf8 = XInternAtom(display, "UTF8_STRING", True);
	Atom net_wm_name = XInternAtom(display, "_NET_WM_NAME", True);
	if (utf8 != None && net_wm_name != None) {
		Atom actual_type = None;
		int actual_format = 0;
		unsigned long item_count = 0;
		unsigned long bytes_after = 0;
		unsigned char *value = NULL;
		if (XGetWindowProperty(display, wid, net_wm_name, 0,
		                       sizeof(out->title), False, utf8,
		                       &actual_type, &actual_format, &item_count,
		                       &bytes_after, &value) == Success && value) {
			size_t copy = item_count < sizeof(out->title) - 1
				? item_count : sizeof(out->title) - 1;
			memcpy(out->title, value, copy);
			out->title[copy] = '\0';
			XFree(value);
		}
	}
	if (!out->title[0]) {
		char *name = NULL;
		if (XFetchName(display, wid, &name) && name) {
			snprintf(out->title, sizeof(out->title), "%s", name);
			XFree(name);
		}
	}

	XClassHint class_hint = { 0 };
	if (XGetClassHint(display, wid, &class_hint)) {
		const char *app_class = class_hint.res_class
			? class_hint.res_class : class_hint.res_name;
		if (app_class)
			snprintf(out->app_class, sizeof(out->app_class), "%s", app_class);
		if (class_hint.res_name == class_hint.res_class) {
			if (class_hint.res_name) XFree(class_hint.res_name);
		} else {
			if (class_hint.res_name) XFree(class_hint.res_name);
			if (class_hint.res_class) XFree(class_hint.res_class);
		}
	}

	/* Geometry */
	int x = attributes.x;
	int y = attributes.y;
	Window translated_child = None;
	if (XTranslateCoordinates(display, wid, DefaultRootWindow(display), 0, 0,
	                          &x, &y, &translated_child)) {
		out->x = x;
		out->y = y;
	}
	out->width = attributes.width;
	out->height = attributes.height;
	out->viewable = attributes.map_state == IsViewable;

	/* PID */
	Atom pid_atom = XInternAtom(display, "_NET_WM_PID", True);
	if (pid_atom != None) {
		Atom actual_type = None;
		int actual_format = 0;
		unsigned long item_count = 0;
		unsigned long bytes_after = 0;
		unsigned char *value = NULL;
		if (XGetWindowProperty(display, wid, pid_atom, 0, 1, False, XA_CARDINAL,
		                       &actual_type, &actual_format, &item_count,
		                       &bytes_after, &value) == Success && value) {
			if (actual_format == 32 && item_count >= 1)
				out->pid = (long)*(unsigned long *)value;
			XFree(value);
		}
	}

	return 0;
}

/* ── Window management ────────────────────────────────────────────────────── */

static int append_window(WindowInfo *out, int max_count, int count, Window wid,
	                     const char *name_filter, int match_class)
{
	if (count >= max_count) return count;
	WindowInfo info;
	if (fill_window_info(wid, &info) != 0) return count;

	if (!info.viewable || info.width < 10 || info.height < 10 ||
	    info.title[0] == '\0')
		return count;
	if (name_filter && name_filter[0] &&
	    !strcasestr(info.title, name_filter) &&
	    !(match_class && strcasestr(info.app_class, name_filter)))
		return count;

	out[count++] = info;
	return count;
}

static int list_tree(Window root, WindowInfo *out, int max_count, int count,
	                 const char *name_filter, int match_class, int depth);

static int list_ewmh_clients(WindowInfo *out, int max_count,
	                         const char *name_filter, int match_class)
{
	Display *display = g_xdo->xdpy;
	Window root = DefaultRootWindow(display);
	Atom client_list = XInternAtom(display, "_NET_CLIENT_LIST", True);
	if (client_list == None) return -1;

	Atom actual_type = None;
	int actual_format = 0;
	unsigned long item_count = 0;
	unsigned long bytes_after = 0;
	unsigned char *data = NULL;
	int status = XGetWindowProperty(display, root, client_list, 0, ~0L, False,
	                                XA_WINDOW, &actual_type, &actual_format,
	                                &item_count, &bytes_after, &data);
	if (status != Success || actual_type != XA_WINDOW || actual_format != 32) {
		if (data) XFree(data);
		return -1;
	}

	Window *windows = (Window *)data;
	int count = 0;
	for (unsigned long i = 0; i < item_count && count < max_count; i++) {
		XWindowAttributes attributes;
		if (!XGetWindowAttributes(display, windows[i], &attributes) ||
		    attributes.map_state != IsViewable)
			continue;
		count = append_window(out, max_count, count, windows[i], name_filter,
		                      match_class);
	}
	XFree(data);
	return count;
}

static int list_tree(Window root, WindowInfo *out, int max_count, int count,
	                 const char *name_filter, int match_class, int depth)
{
	if (depth > 64 || count >= max_count) return count;
	Window returned_root = None;
	Window parent = None;
	Window *children = NULL;
	unsigned int child_count = 0;
	if (!XQueryTree(g_xdo->xdpy, root, &returned_root, &parent,
	                &children, &child_count)) {
		if (children) XFree(children);
		return count;
	}

	for (unsigned int i = 0; i < child_count && count < max_count; i++) {
		count = append_window(out, max_count, count, children[i], name_filter,
		                      match_class);
		count = list_tree(children[i], out, max_count, count, name_filter,
		                  match_class, depth + 1);
	}
	if (children) XFree(children);
	return count;
}

int x11_list_windows(WindowInfo *out, int max_count, const char *name_filter,
	                 int include_all)
{
	if (!include_all) {
		int ewmh_count = list_ewmh_clients(out, max_count, name_filter, 1);
		if (ewmh_count >= 0) return ewmh_count;
	}
	return list_tree(DefaultRootWindow(g_xdo->xdpy), out, max_count, 0,
	                 name_filter, 1, 0);
}

static unsigned long find_best_match(WindowInfo *results, int nresults,
	                                const char *name, int match_class)
{
	/* Find best match. Scoring:
	 * - Exact title match with largest area wins immediately
	 * - Otherwise prefer shorter titles (more specific partial match)
	 * - Break title-length ties by largest area */
	unsigned long best = 0;
	int best_title_len = 999999;
	int best_area = 0;
	int best_exact = 0;  /* exact title match flag */

	for (int i = 0; i < nresults; i++) {
		WindowInfo *info = &results[i];
		/* GTK/GDK processes commonly publish a 20x20 titled helper before
		 * their real managed window appears. Never let that helper satisfy a
		 * launch/find request; wait for the application or a usable dialog. */
		if (info->width <= 32 && info->height <= 32) continue;

		int title_match = strcasestr(info->title, name) != NULL;
		int class_match = match_class &&
			strcasestr(info->app_class, name) != NULL;
		if (!title_match && !class_match) continue;
		int is_exact = strcmp(info->title, name) == 0 ||
			(match_class && strcasecmp(info->app_class, name) == 0);
		int title_len = (int)strlen(info->title);
		int area = info->width * info->height;

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
			best = info->id;
			best_title_len = title_len;
			best_area = area;
			best_exact = is_exact;
		}
	}

	return best;
}

unsigned long x11_find_window(const char *name)
{
	WindowInfo results[256];
	int count = list_tree(DefaultRootWindow(g_xdo->xdpy), results, 256, 0,
	                     name, 0, 0);
	return find_best_match(results, count, name, 0);
}

static void count_exact_tree(Window root, const char *name, WindowInfo *first,
                             int *match_count, int *complete, int depth)
{
	if (depth > 64) {
		*complete = 0;
		return;
	}
	Window returned_root = None;
	Window parent = None;
	Window *children = NULL;
	unsigned int child_count = 0;
	if (!XQueryTree(g_xdo->xdpy, root, &returned_root, &parent,
	                &children, &child_count)) {
		if (children) XFree(children);
		*complete = 0;
		return;
	}
	for (unsigned int i = 0; i < child_count; i++) {
		WindowInfo info;
		if (fill_window_info(children[i], &info) != 0) {
			*complete = 0;
		} else if (info.viewable && info.width >= 10 && info.height >= 10 &&
		           strcmp(info.title, name) == 0) {
			if (*match_count == 0) *first = info;
			(*match_count)++;
		}
		count_exact_tree(children[i], name, first, match_count,
		                 complete, depth + 1);
	}
	if (children) XFree(children);
}

int x11_find_window_exact(const char *name, WindowInfo *first,
                          int *match_count, int *complete)
{
	if (!name || !name[0] || !first || !match_count || !complete) return -1;
	memset(first, 0, sizeof(*first));
	*match_count = 0;
	*complete = 1;

	Display *display = g_xdo->xdpy;
	Window root = DefaultRootWindow(display);
	Atom client_list = XInternAtom(display, "_NET_CLIENT_LIST", True);
	if (client_list != None) {
		Atom actual_type = None;
		int actual_format = 0;
		unsigned long item_count = 0;
		unsigned long bytes_after = 0;
		unsigned char *data = NULL;
		int status = XGetWindowProperty(display, root, client_list, 0, ~0L,
		                                False, XA_WINDOW, &actual_type,
		                                &actual_format, &item_count,
		                                &bytes_after, &data);
		if (status == Success && actual_type == XA_WINDOW && actual_format == 32) {
			Window *windows = (Window *)data;
			for (unsigned long i = 0; i < item_count; i++) {
				WindowInfo info;
				if (fill_window_info(windows[i], &info) != 0) {
					*complete = 0;
					continue;
				}
				if (!info.viewable || info.width < 10 || info.height < 10 ||
				    strcmp(info.title, name) != 0)
					continue;
				if (*match_count == 0) *first = info;
				(*match_count)++;
			}
			XFree(data);
			return 0;
		}
		if (data) XFree(data);
	}

	count_exact_tree(root, name, first, match_count, complete, 0);
	return 0;
}

unsigned long x11_find_app(const char *name)
{
	WindowInfo results[256];
	int count = list_ewmh_clients(results, 256, name, 1);
	if (count < 0) {
		count = list_tree(DefaultRootWindow(g_xdo->xdpy), results, 256, 0,
		                  name, 1, 0);
	}
	return find_best_match(results, count, name, 1);
}

int x11_get_window_info(unsigned long wid, WindowInfo *out)
{
	return fill_window_info((Window)wid, out);
}

int x11_focus_window(unsigned long wid)
{
	if (getenv("DESKPAL_HEADLESS_ACTIVE")) {
		XRaiseWindow(g_xdo->xdpy, (Window)wid);
		int result = xdo_focus_window(g_xdo, (Window)wid);
		XSync(g_xdo->xdpy, False);
		return result;
	}

	/* Use both focus and activate for reliable keyboard delivery.
	 * xdo_focus_window sets X11 input focus (needed for XTest keys),
	 * xdo_activate_window raises the window in the stacking order. */
	int focus_result = xdo_focus_window(g_xdo, (Window)wid);
	int activate_result = xdo_activate_window(g_xdo, (Window)wid);
	XSync(g_xdo->xdpy, False);
	return focus_result == 0 || activate_result == 0 ? 0 : focus_result;
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
	/* Prefer uinput left-click ONLY on pure X11 sessions. On Wayland
	 * (mutter/Xwayland), uinput BTN_LEFT press/release races the
	 * wl_pointer focus transition for Chromium-based Xwayland clients:
	 * the cursor moves but the renderer never receives a DOM click
	 * event. xdotool click via XTEST is reliably delivered to the
	 * X window under the cursor. See docs/proposed-tools.md §8. */
	const char *wayland = getenv("WAYLAND_DISPLAY");
	if (!wayland && uinput_available() && button == 1)
		return uinput_click(button, repeat);

	/* All other paths: xdotool CLI. Originally introduced because
	 * xdo_click_window(CURRENTWINDOW) via libxdo XTest doesn't trigger
	 * GTK context menus on Xwayland; the CLI tool does. */
	for (int i = 0; i < repeat; i++) {
		char cmd[64];
		snprintf(cmd, sizeof(cmd), "xdotool click %d", button);
		if (system(cmd) != 0)
			return -1;
		if (i < repeat - 1) usleep(50000);
	}
	return 0;
}

int x11_is_wayland(void)
{
	const char *wd = getenv("WAYLAND_DISPLAY");
	return wd && *wd ? 1 : 0;
}

int x11_window_click(unsigned long wid, int x, int y, int button, int repeat)
{
	/* `xdotool mousemove --window WID X Y` translates (X, Y) through the
	 * X server using the target window's content origin, which is the
	 * coordinate space our screenshots/OCR also work in. The chained
	 * `click` runs immediately so we don't race a window move. */
	char cmd[160];
	int n = snprintf(cmd, sizeof(cmd),
		"xdotool mousemove --window %lu %d %d click --repeat %d %d",
		wid, x, y, repeat, button);
	if (n < 0 || n >= (int)sizeof(cmd))
		return -1;
	return system(cmd) == 0 ? 0 : -1;
}

int x11_window_mouse_move(unsigned long wid, int x, int y)
{
	char cmd[128];
	int n = snprintf(cmd, sizeof(cmd),
		"xdotool mousemove --window %lu %d %d", wid, x, y);
	if (n < 0 || n >= (int)sizeof(cmd))
		return -1;
	return system(cmd) == 0 ? 0 : -1;
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
