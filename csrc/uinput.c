/*
 * deskpal — uinput virtual mouse for Wayland-compatible input
 *
 * Creates a virtual pointer device via /dev/uinput so that mouse
 * events are delivered through the kernel input subsystem.  The
 * Wayland compositor (GNOME/mutter, KDE/KWin, …) picks them up
 * just like real hardware, bypassing XTest limitations.
 *
 * Copyright (c) 2026 deskpal contributors
 * SPDX-License-Identifier: MIT
 */
#include "uinput.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include <linux/uinput.h>

/* ── State ────────────────────────────────────────────────────────────────── */

static int  g_fd    = -1;          /* /dev/uinput fd                      */
static int  g_sw    = 0;           /* screen width  for ABS_X normalizing */
static int  g_sh    = 0;           /* screen height for ABS_Y normalizing */

/* ── Low-level helpers ────────────────────────────────────────────────────── */

static int emit(int type, int code, int value)
{
	struct input_event ev;
	memset(&ev, 0, sizeof(ev));
	ev.type  = type;
	ev.code  = code;
	ev.value = value;
	return write(g_fd, &ev, sizeof(ev)) == sizeof(ev) ? 0 : -1;
}

static int syn(void)
{
	return emit(EV_SYN, SYN_REPORT, 0);
}

/* Map a Linux BTN_* code from our 1/2/3 scheme. */
static int btn_code(int button)
{
	switch (button) {
	case 1:  return BTN_LEFT;
	case 2:  return BTN_MIDDLE;
	case 3:  return BTN_RIGHT;
	default: return BTN_LEFT;
	}
}

/* ── Public API ───────────────────────────────────────────────────────────── */

int uinput_init(int screen_width, int screen_height)
{
	if (g_fd >= 0) return 0;   /* already initialised */

	g_sw = screen_width  > 0 ? screen_width  : 3840;
	g_sh = screen_height > 0 ? screen_height : 2160;

	g_fd = open("/dev/uinput", O_WRONLY | O_NONBLOCK);
	if (g_fd < 0) {
		/* Not available — caller should fall back to XTest */
		return -1;
	}

	/* Enable event types */
	if (ioctl(g_fd, UI_SET_EVBIT,  EV_KEY) < 0) goto fail;
	if (ioctl(g_fd, UI_SET_EVBIT,  EV_ABS) < 0) goto fail;
	if (ioctl(g_fd, UI_SET_EVBIT,  EV_REL) < 0) goto fail;
	if (ioctl(g_fd, UI_SET_EVBIT,  EV_SYN) < 0) goto fail;

	/* Buttons */
	if (ioctl(g_fd, UI_SET_KEYBIT, BTN_LEFT)   < 0) goto fail;
	if (ioctl(g_fd, UI_SET_KEYBIT, BTN_MIDDLE) < 0) goto fail;
	if (ioctl(g_fd, UI_SET_KEYBIT, BTN_RIGHT)  < 0) goto fail;

	/* Keyboard keys — register the full range we may use */
	for (int k = KEY_ESC; k <= KEY_F12; k++) {
		ioctl(g_fd, UI_SET_KEYBIT, k);
	}
	/* Navigation and editing keys */
	int extra_keys[] = {
		KEY_INSERT, KEY_DELETE, KEY_HOME, KEY_END,
		KEY_PAGEUP, KEY_PAGEDOWN, KEY_UP, KEY_DOWN,
		KEY_LEFT, KEY_RIGHT, KEY_KPENTER,
		KEY_LEFTCTRL, KEY_LEFTSHIFT, KEY_LEFTALT, KEY_LEFTMETA,
		KEY_RIGHTCTRL, KEY_RIGHTSHIFT, KEY_RIGHTALT, KEY_RIGHTMETA,
		-1
	};
	for (int i = 0; extra_keys[i] >= 0; i++) {
		ioctl(g_fd, UI_SET_KEYBIT, extra_keys[i]);
	}

	/* Absolute axes for pointer positioning */
	if (ioctl(g_fd, UI_SET_ABSBIT, ABS_X) < 0) goto fail;
	if (ioctl(g_fd, UI_SET_ABSBIT, ABS_Y) < 0) goto fail;

	/* Scroll wheel */
	if (ioctl(g_fd, UI_SET_RELBIT, REL_WHEEL)  < 0) goto fail;

	/* Build the device definition */
	struct uinput_user_dev udev;
	memset(&udev, 0, sizeof(udev));
	snprintf(udev.name, UINPUT_MAX_NAME_SIZE, "deskpal-pointer");
	udev.id.bustype = BUS_USB;
	udev.id.vendor  = 0x1234;
	udev.id.product = 0xDEAD;
	udev.id.version = 1;

	/* Absolute axis ranges — match screen pixel dimensions */
	udev.absmin[ABS_X] = 0;
	udev.absmax[ABS_X] = g_sw - 1;
	udev.absmin[ABS_Y] = 0;
	udev.absmax[ABS_Y] = g_sh - 1;

	if (write(g_fd, &udev, sizeof(udev)) != sizeof(udev)) goto fail;
	if (ioctl(g_fd, UI_DEV_CREATE) < 0) goto fail;

	/* Give the compositor a moment to register the new device.
	 * Mutter/GNOME needs ~200-300 ms to recognise a new uinput
	 * pointer.  300 ms is a safe margin. */
	usleep(300000); /* 300 ms */

	return 0;

fail:
	close(g_fd);
	g_fd = -1;
	return -1;
}

void uinput_cleanup(void)
{
	if (g_fd >= 0) {
		ioctl(g_fd, UI_DEV_DESTROY);
		close(g_fd);
		g_fd = -1;
	}
}

bool uinput_available(void)
{
	return g_fd >= 0;
}

int uinput_mouse_move(int x, int y)
{
	if (g_fd < 0) return -1;

	/* Clamp to screen bounds */
	if (x < 0)      x = 0;
	if (y < 0)      y = 0;
	if (x >= g_sw)  x = g_sw - 1;
	if (y >= g_sh)  y = g_sh - 1;

	if (emit(EV_ABS, ABS_X, x) < 0) return -1;
	if (emit(EV_ABS, ABS_Y, y) < 0) return -1;
	return syn();
}

int uinput_click(int button, int repeat)
{
	if (g_fd < 0) return -1;
	int code = btn_code(button);

	for (int i = 0; i < repeat; i++) {
		emit(EV_KEY, code, 1);   /* press   */
		syn();
		usleep(30000);           /* 30 ms   */
		emit(EV_KEY, code, 0);   /* release */
		syn();
		if (i < repeat - 1) usleep(50000); /* gap between double-clicks */
	}
	return 0;
}

int uinput_mouse_down(int button)
{
	if (g_fd < 0) return -1;
	emit(EV_KEY, btn_code(button), 1);
	return syn();
}

int uinput_mouse_up(int button)
{
	if (g_fd < 0) return -1;
	emit(EV_KEY, btn_code(button), 0);
	return syn();
}

int uinput_scroll(int amount)
{
	if (g_fd < 0) return -1;

	/* Positive = scroll down, negative = scroll up */
	int dir = amount > 0 ? -1 : 1; /* REL_WHEEL: positive = up */
	int steps = amount > 0 ? amount : -amount;

	for (int i = 0; i < steps; i++) {
		emit(EV_REL, REL_WHEEL, dir);
		syn();
		usleep(20000);
	}
	return 0;
}

/* ── Keyboard support ─────────────────────────────────────────────────────── */

/* Map an xdotool-style key name to a Linux KEY_* code.
 * Returns -1 if unknown. */
static int keyname_to_code(const char *name)
{
	/* Named keys (case-insensitive comparison) */
	struct { const char *name; int code; } map[] = {
		{"Return",      KEY_ENTER},
		{"KP_Enter",    KEY_KPENTER},
		{"Escape",      KEY_ESC},
		{"Tab",         KEY_TAB},
		{"BackSpace",   KEY_BACKSPACE},
		{"Delete",      KEY_DELETE},
		{"Home",        KEY_HOME},
		{"End",         KEY_END},
		{"Page_Up",     KEY_PAGEUP},
		{"Page_Down",   KEY_PAGEDOWN},
		{"Up",          KEY_UP},
		{"Down",        KEY_DOWN},
		{"Left",        KEY_LEFT},
		{"Right",       KEY_RIGHT},
		{"Insert",      KEY_INSERT},
		{"space",       KEY_SPACE},
		{"F1",          KEY_F1},
		{"F2",          KEY_F2},
		{"F3",          KEY_F3},
		{"F4",          KEY_F4},
		{"F5",          KEY_F5},
		{"F6",          KEY_F6},
		{"F7",          KEY_F7},
		{"F8",          KEY_F8},
		{"F9",          KEY_F9},
		{"F10",         KEY_F10},
		{"F11",         KEY_F11},
		{"F12",         KEY_F12},
		{"minus",       KEY_MINUS},
		{"equal",       KEY_EQUAL},
		{"plus",        KEY_EQUAL},   /* Shift+= produces + */
		{"bracketleft", KEY_LEFTBRACE},
		{"bracketright",KEY_RIGHTBRACE},
		{"backslash",   KEY_BACKSLASH},
		{"semicolon",   KEY_SEMICOLON},
		{"apostrophe",  KEY_APOSTROPHE},
		{"comma",       KEY_COMMA},
		{"period",      KEY_DOT},
		{"slash",       KEY_SLASH},
		{"grave",       KEY_GRAVE},
		{NULL, -1}
	};

	for (int i = 0; map[i].name; i++) {
		if (strcasecmp(name, map[i].name) == 0)
			return map[i].code;
	}

	/* Single letter a-z */
	if (name[0] && !name[1]) {
		char c = name[0];
		if (c >= 'a' && c <= 'z') return KEY_A + (c - 'a');
		if (c >= 'A' && c <= 'Z') return KEY_A + (c - 'A');
		if (c >= '0' && c <= '9') return KEY_0 + (c - '0');
	}

	return -1;
}

/* Map a modifier name to its KEY_* code. */
static int modifier_to_code(const char *mod)
{
	if (strcasecmp(mod, "ctrl")    == 0 ||
	    strcasecmp(mod, "control") == 0) return KEY_LEFTCTRL;
	if (strcasecmp(mod, "alt")     == 0) return KEY_LEFTALT;
	if (strcasecmp(mod, "shift")   == 0) return KEY_LEFTSHIFT;
	if (strcasecmp(mod, "super")   == 0 ||
	    strcasecmp(mod, "meta")    == 0) return KEY_LEFTMETA;
	return -1;
}

int uinput_key_press(const char *keys)
{
	if (g_fd < 0) return -1;

	/* Parse "modifier+modifier+key" format (xdotool style).
	 * Examples: "Return", "ctrl+a", "ctrl+shift+t" */
	char buf[128];
	strncpy(buf, keys, sizeof(buf) - 1);
	buf[sizeof(buf) - 1] = '\0';

	/* Split on '+' — collect modifiers, last part is the key */
	int mod_codes[8];
	int nmod = 0;
	int key_code = -1;

	char *saveptr = NULL;
	char *tok = strtok_r(buf, "+", &saveptr);

	/* Collect all tokens */
	char *tokens[16];
	int ntok = 0;
	while (tok && ntok < 16) {
		tokens[ntok++] = tok;
		tok = strtok_r(NULL, "+", &saveptr);
	}

	if (ntok == 0) return -1;

	/* Last token is the key, rest are modifiers */
	key_code = keyname_to_code(tokens[ntok - 1]);
	if (key_code < 0) return -1;

	for (int i = 0; i < ntok - 1; i++) {
		int mc = modifier_to_code(tokens[i]);
		if (mc < 0) return -1;
		mod_codes[nmod++] = mc;
	}

	/* Press modifiers */
	for (int i = 0; i < nmod; i++) {
		emit(EV_KEY, mod_codes[i], 1);
		syn();
	}

	/* Press + release key */
	emit(EV_KEY, key_code, 1);
	syn();
	usleep(20000);
	emit(EV_KEY, key_code, 0);
	syn();

	/* Release modifiers (reverse order) */
	for (int i = nmod - 1; i >= 0; i--) {
		emit(EV_KEY, mod_codes[i], 0);
		syn();
	}

	return 0;
}

int uinput_type_char(char c)
{
	if (g_fd < 0) return -1;

	int key = -1;
	int need_shift = 0;

	if (c >= 'a' && c <= 'z')      { key = KEY_A + (c - 'a'); }
	else if (c >= 'A' && c <= 'Z') { key = KEY_A + (c - 'A'); need_shift = 1; }
	else if (c >= '0' && c <= '9') { key = KEY_0 + (c - '0'); }
	else if (c == ' ')  key = KEY_SPACE;
	else if (c == '\n') key = KEY_ENTER;
	else if (c == '\t') key = KEY_TAB;
	else if (c == '-')  key = KEY_MINUS;
	else if (c == '=')  key = KEY_EQUAL;
	else if (c == '[')  key = KEY_LEFTBRACE;
	else if (c == ']')  key = KEY_RIGHTBRACE;
	else if (c == '\\') key = KEY_BACKSLASH;
	else if (c == ';')  key = KEY_SEMICOLON;
	else if (c == '\'') key = KEY_APOSTROPHE;
	else if (c == ',')  key = KEY_COMMA;
	else if (c == '.')  key = KEY_DOT;
	else if (c == '/')  key = KEY_SLASH;
	else if (c == '`')  key = KEY_GRAVE;
	/* Shifted symbols */
	else if (c == '!')  { key = KEY_1;          need_shift = 1; }
	else if (c == '@')  { key = KEY_2;          need_shift = 1; }
	else if (c == '#')  { key = KEY_3;          need_shift = 1; }
	else if (c == '$')  { key = KEY_4;          need_shift = 1; }
	else if (c == '%')  { key = KEY_5;          need_shift = 1; }
	else if (c == '^')  { key = KEY_6;          need_shift = 1; }
	else if (c == '&')  { key = KEY_7;          need_shift = 1; }
	else if (c == '*')  { key = KEY_8;          need_shift = 1; }
	else if (c == '(')  { key = KEY_9;          need_shift = 1; }
	else if (c == ')')  { key = KEY_0;          need_shift = 1; }
	else if (c == '_')  { key = KEY_MINUS;      need_shift = 1; }
	else if (c == '+')  { key = KEY_EQUAL;      need_shift = 1; }
	else if (c == '{')  { key = KEY_LEFTBRACE;  need_shift = 1; }
	else if (c == '}')  { key = KEY_RIGHTBRACE; need_shift = 1; }
	else if (c == '|')  { key = KEY_BACKSLASH;  need_shift = 1; }
	else if (c == ':')  { key = KEY_SEMICOLON;  need_shift = 1; }
	else if (c == '"')  { key = KEY_APOSTROPHE; need_shift = 1; }
	else if (c == '<')  { key = KEY_COMMA;      need_shift = 1; }
	else if (c == '>')  { key = KEY_DOT;        need_shift = 1; }
	else if (c == '?')  { key = KEY_SLASH;      need_shift = 1; }
	else if (c == '~')  { key = KEY_GRAVE;      need_shift = 1; }
	else return -1; /* unsupported character */

	if (need_shift) {
		emit(EV_KEY, KEY_LEFTSHIFT, 1);
		syn();
	}

	emit(EV_KEY, key, 1);
	syn();
	usleep(10000);
	emit(EV_KEY, key, 0);
	syn();

	if (need_shift) {
		emit(EV_KEY, KEY_LEFTSHIFT, 0);
		syn();
	}

	return 0;
}
