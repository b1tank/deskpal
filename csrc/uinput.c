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
