/*
 * deskpal — uinput virtual input devices for Wayland-compatible input
 *
 * Creates TWO separate virtual devices via /dev/uinput:
 *
 *   1. "deskpal-pointer"  — ABS_X/ABS_Y + BTN_LEFT/MID/RIGHT + REL_WHEEL
 *      Mutter classifies this as a pointer device.
 *
 *   2. "deskpal-keyboard" — KEY_* events only, no ABS axes
 *      Mutter classifies this as a keyboard device.
 *
 * Separating them means:
 *   - Keyboard events are properly delivered (mutter drops KEY events
 *     from devices with ABS axes — it thinks they're pointer devices)
 *   - Both virtual devices coexist with the user's physical mouse and
 *     keyboard — the kernel input subsystem supports multiple devices
 *   - The pointer cursor is still shared (Wayland single-seat limitation)
 *     but the user can type freely on their keyboard during automation
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

static int  g_ptr_fd = -1;        /* pointer device fd                   */
static int  g_kbd_fd = -1;        /* keyboard device fd                  */
static int  g_sw     = 0;         /* screen width  for ABS_X clamping    */
static int  g_sh     = 0;         /* screen height for ABS_Y clamping    */

/* ── Low-level helpers ────────────────────────────────────────────────────── */

static int emit_on(int fd, int type, int code, int value)
{
	struct input_event ev;
	memset(&ev, 0, sizeof(ev));
	ev.type  = type;
	ev.code  = code;
	ev.value = value;
	return write(fd, &ev, sizeof(ev)) == sizeof(ev) ? 0 : -1;
}

static int syn_on(int fd)
{
	return emit_on(fd, EV_SYN, SYN_REPORT, 0);
}

/* Convenience: emit on pointer device */
static int pemit(int type, int code, int value) { return emit_on(g_ptr_fd, type, code, value); }
static int psyn(void) { return syn_on(g_ptr_fd); }

/* Convenience: emit on keyboard device */
static int kemit(int type, int code, int value) { return emit_on(g_kbd_fd, type, code, value); }
static int ksyn(void) { return syn_on(g_kbd_fd); }

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

/* Helper: open /dev/uinput and return the fd, or -1. */
static int open_uinput(void)
{
	int fd = open("/dev/uinput", O_WRONLY | O_NONBLOCK);
	return fd;
}

int uinput_init(int screen_width, int screen_height)
{
	if (g_ptr_fd >= 0) return 0;   /* already initialised */

	g_sw = screen_width  > 0 ? screen_width  : 3840;
	g_sh = screen_height > 0 ? screen_height : 2160;

	/* ── Create pointer device ────────────────────────────────────── */
	g_ptr_fd = open_uinput();
	if (g_ptr_fd < 0) return -1;

	ioctl(g_ptr_fd, UI_SET_EVBIT,  EV_KEY);
	ioctl(g_ptr_fd, UI_SET_EVBIT,  EV_ABS);
	ioctl(g_ptr_fd, UI_SET_EVBIT,  EV_REL);
	ioctl(g_ptr_fd, UI_SET_EVBIT,  EV_SYN);

	ioctl(g_ptr_fd, UI_SET_KEYBIT, BTN_LEFT);
	ioctl(g_ptr_fd, UI_SET_KEYBIT, BTN_MIDDLE);
	ioctl(g_ptr_fd, UI_SET_KEYBIT, BTN_RIGHT);
	ioctl(g_ptr_fd, UI_SET_ABSBIT, ABS_X);
	ioctl(g_ptr_fd, UI_SET_ABSBIT, ABS_Y);
	ioctl(g_ptr_fd, UI_SET_RELBIT, REL_WHEEL);

	/* Mark as a pointer device (not a touchscreen) so compositors
	 * treat BTN_RIGHT as a context-menu trigger. */
	ioctl(g_ptr_fd, UI_SET_PROPBIT, INPUT_PROP_POINTER);

	struct uinput_user_dev pdev;
	memset(&pdev, 0, sizeof(pdev));
	snprintf(pdev.name, UINPUT_MAX_NAME_SIZE, "deskpal-pointer");
	pdev.id.bustype = BUS_USB;
	pdev.id.vendor  = 0x1234;
	pdev.id.product = 0xDE01;
	pdev.id.version = 1;
	pdev.absmin[ABS_X] = 0;
	pdev.absmax[ABS_X] = g_sw - 1;
	pdev.absmin[ABS_Y] = 0;
	pdev.absmax[ABS_Y] = g_sh - 1;

	if (write(g_ptr_fd, &pdev, sizeof(pdev)) != sizeof(pdev) ||
	    ioctl(g_ptr_fd, UI_DEV_CREATE) < 0) {
		close(g_ptr_fd);
		g_ptr_fd = -1;
		return -1;
	}

	/* ── Create keyboard device ───────────────────────────────────── */
	g_kbd_fd = open_uinput();
	if (g_kbd_fd < 0) {
		/* Pointer works, keyboard won't — still useful */
		fprintf(stderr, "deskpal: uinput keyboard failed (pointer ok)\n");
	} else {
		ioctl(g_kbd_fd, UI_SET_EVBIT, EV_KEY);
		ioctl(g_kbd_fd, UI_SET_EVBIT, EV_SYN);
		/* NO EV_ABS — this ensures mutter classifies it as keyboard */

		/* Register all keys we might need */
		for (int k = KEY_ESC; k <= KEY_F12; k++)
			ioctl(g_kbd_fd, UI_SET_KEYBIT, k);

		int extra[] = {
			KEY_INSERT, KEY_DELETE, KEY_HOME, KEY_END,
			KEY_PAGEUP, KEY_PAGEDOWN, KEY_UP, KEY_DOWN,
			KEY_LEFT, KEY_RIGHT, KEY_KPENTER,
			KEY_LEFTCTRL, KEY_LEFTSHIFT, KEY_LEFTALT, KEY_LEFTMETA,
			KEY_RIGHTCTRL, KEY_RIGHTSHIFT, KEY_RIGHTALT, KEY_RIGHTMETA,
			-1
		};
		for (int i = 0; extra[i] >= 0; i++)
			ioctl(g_kbd_fd, UI_SET_KEYBIT, extra[i]);

		struct uinput_user_dev kdev;
		memset(&kdev, 0, sizeof(kdev));
		snprintf(kdev.name, UINPUT_MAX_NAME_SIZE, "deskpal-keyboard");
		kdev.id.bustype = BUS_USB;
		kdev.id.vendor  = 0x1234;
		kdev.id.product = 0xDE02;
		kdev.id.version = 1;
		/* No abs axes */

		if (write(g_kbd_fd, &kdev, sizeof(kdev)) != sizeof(kdev) ||
		    ioctl(g_kbd_fd, UI_DEV_CREATE) < 0) {
			close(g_kbd_fd);
			g_kbd_fd = -1;
			fprintf(stderr, "deskpal: uinput keyboard create failed\n");
		}
	}

	/* Give the compositor time to register both devices */
	usleep(300000); /* 300 ms */

	return 0;
}

void uinput_cleanup(void)
{
	if (g_kbd_fd >= 0) {
		ioctl(g_kbd_fd, UI_DEV_DESTROY);
		close(g_kbd_fd);
		g_kbd_fd = -1;
	}
	if (g_ptr_fd >= 0) {
		ioctl(g_ptr_fd, UI_DEV_DESTROY);
		close(g_ptr_fd);
		g_ptr_fd = -1;
	}
}

bool uinput_available(void)
{
	return g_ptr_fd >= 0;
}

bool uinput_kbd_available(void)
{
	return g_kbd_fd >= 0;
}

int uinput_mouse_move(int x, int y)
{
	if (g_ptr_fd < 0) return -1;

	/* Clamp to screen bounds */
	if (x < 0)      x = 0;
	if (y < 0)      y = 0;
	if (x >= g_sw)  x = g_sw - 1;
	if (y >= g_sh)  y = g_sh - 1;

	if (pemit(EV_ABS, ABS_X, x) < 0) return -1;
	if (pemit(EV_ABS, ABS_Y, y) < 0) return -1;
	return psyn();
}

int uinput_click(int button, int repeat)
{
	if (g_ptr_fd < 0) return -1;
	int code = btn_code(button);

	for (int i = 0; i < repeat; i++) {
		pemit(EV_KEY, code, 1);   /* press   */
		psyn();
		usleep(30000);            /* 30 ms   */
		pemit(EV_KEY, code, 0);   /* release */
		psyn();
		if (i < repeat - 1) usleep(50000);
	}
	return 0;
}

int uinput_mouse_down(int button)
{
	if (g_ptr_fd < 0) return -1;
	pemit(EV_KEY, btn_code(button), 1);
	return psyn();
}

int uinput_mouse_up(int button)
{
	if (g_ptr_fd < 0) return -1;
	pemit(EV_KEY, btn_code(button), 0);
	return psyn();
}

int uinput_scroll(int amount)
{
	if (g_ptr_fd < 0) return -1;

	/* Positive = scroll down, negative = scroll up */
	int dir = amount > 0 ? -1 : 1; /* REL_WHEEL: positive = up */
	int steps = amount > 0 ? amount : -amount;

	for (int i = 0; i < steps; i++) {
		pemit(EV_REL, REL_WHEEL, dir);
		psyn();
		usleep(20000);
	}
	return 0;
}

/* ── Keyboard support ─────────────────────────────────────────────────────── */

/* Linux KEY_* codes follow physical QWERTY layout, NOT alphabetical order.
 * KEY_A=30 but KEY_B=48 (not 31).  We need explicit per-character mapping. */
static int char_to_keycode(char c)
{
	switch (c) {
	/* Letters — QWERTY physical order */
	case 'a': return KEY_A;  case 'b': return KEY_B;  case 'c': return KEY_C;
	case 'd': return KEY_D;  case 'e': return KEY_E;  case 'f': return KEY_F;
	case 'g': return KEY_G;  case 'h': return KEY_H;  case 'i': return KEY_I;
	case 'j': return KEY_J;  case 'k': return KEY_K;  case 'l': return KEY_L;
	case 'm': return KEY_M;  case 'n': return KEY_N;  case 'o': return KEY_O;
	case 'p': return KEY_P;  case 'q': return KEY_Q;  case 'r': return KEY_R;
	case 's': return KEY_S;  case 't': return KEY_T;  case 'u': return KEY_U;
	case 'v': return KEY_V;  case 'w': return KEY_W;  case 'x': return KEY_X;
	case 'y': return KEY_Y;  case 'z': return KEY_Z;
	/* Digits */
	case '0': return KEY_0;  case '1': return KEY_1;  case '2': return KEY_2;
	case '3': return KEY_3;  case '4': return KEY_4;  case '5': return KEY_5;
	case '6': return KEY_6;  case '7': return KEY_7;  case '8': return KEY_8;
	case '9': return KEY_9;
	/* Punctuation */
	case '-': return KEY_MINUS;       case '=': return KEY_EQUAL;
	case '[': return KEY_LEFTBRACE;   case ']': return KEY_RIGHTBRACE;
	case '\\': return KEY_BACKSLASH;  case ';': return KEY_SEMICOLON;
	case '\'': return KEY_APOSTROPHE; case '`': return KEY_GRAVE;
	case ',': return KEY_COMMA;       case '.': return KEY_DOT;
	case '/': return KEY_SLASH;       case ' ': return KEY_SPACE;
	default:  return -1;
	}
}

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

	/* Single character — use char_to_keycode */
	if (name[0] && !name[1]) {
		char c = name[0];
		if (c >= 'A' && c <= 'Z') c = c - 'A' + 'a'; /* lowercase */
		return char_to_keycode(c);
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
	if (g_kbd_fd < 0) return -1;

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
		kemit(EV_KEY, mod_codes[i], 1);
		ksyn();
	}

	/* Press + release key */
	kemit(EV_KEY, key_code, 1);
	ksyn();
	usleep(20000);
	kemit(EV_KEY, key_code, 0);
	ksyn();

	/* Release modifiers (reverse order) */
	for (int i = nmod - 1; i >= 0; i--) {
		kemit(EV_KEY, mod_codes[i], 0);
		ksyn();
	}

	return 0;
}

int uinput_type_char(char c)
{
	if (g_kbd_fd < 0) return -1;

	int key = -1;
	int need_shift = 0;

	/* Lowercase letters and digits — direct lookup */
	if (c >= 'a' && c <= 'z') {
		key = char_to_keycode(c);
	} else if (c >= 'A' && c <= 'Z') {
		key = char_to_keycode(c - 'A' + 'a');
		need_shift = 1;
	} else if ((c >= '0' && c <= '9') || c == '-' || c == '=' ||
	           c == '[' || c == ']' || c == '\\' || c == ';' ||
	           c == '\'' || c == '`' || c == ',' || c == '.' ||
	           c == '/' || c == ' ') {
		key = char_to_keycode(c);
	}
	/* Special whitespace */
	else if (c == '\n') key = KEY_ENTER;
	else if (c == '\t') key = KEY_TAB;
	/* Shifted symbols */
	else {
		struct { char ch; int code; } shifted[] = {
			{'!', KEY_1},  {'@', KEY_2},  {'#', KEY_3},  {'$', KEY_4},
			{'%', KEY_5},  {'^', KEY_6},  {'&', KEY_7},  {'*', KEY_8},
			{'(', KEY_9},  {')', KEY_0},  {'_', KEY_MINUS}, {'+', KEY_EQUAL},
			{'{', KEY_LEFTBRACE},  {'}', KEY_RIGHTBRACE},
			{'|', KEY_BACKSLASH},  {':', KEY_SEMICOLON},
			{'"', KEY_APOSTROPHE}, {'~', KEY_GRAVE},
			{'<', KEY_COMMA},      {'>', KEY_DOT},
			{'?', KEY_SLASH},
			{0, -1}
		};
		for (int i = 0; shifted[i].ch; i++) {
			if (c == shifted[i].ch) {
				key = shifted[i].code;
				need_shift = 1;
				break;
			}
		}
	}

	if (key < 0) return -1;

	if (need_shift) {
		kemit(EV_KEY, KEY_LEFTSHIFT, 1);
		ksyn();
	}

	kemit(EV_KEY, key, 1);
	ksyn();
	usleep(10000);
	kemit(EV_KEY, key, 0);
	ksyn();

	if (need_shift) {
		kemit(EV_KEY, KEY_LEFTSHIFT, 0);
		ksyn();
	}

	return 0;
}
