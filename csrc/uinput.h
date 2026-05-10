/*
 * deskpal — uinput virtual input devices for Wayland-compatible input
 *
 * Creates two separate devices via /dev/uinput:
 *   - "deskpal-pointer"  — mouse (ABS_X/Y, buttons, scroll)
 *   - "deskpal-keyboard" — keyboard (KEY_* only, no ABS axes)
 *
 * Separating them ensures mutter/KWin correctly classifies each
 * device, and both coexist with the user's physical hardware.
 *
 * Copyright (c) 2026 deskpal contributors
 * SPDX-License-Identifier: MIT
 */
#ifndef DESKPAL_UINPUT_H
#define DESKPAL_UINPUT_H

#include <stdbool.h>

/* Open /dev/uinput and create virtual pointer + keyboard devices.
 * Returns 0 on success, -1 if uinput is unavailable.
 * Safe to call multiple times; second call is a no-op. */
int  uinput_init(int screen_width, int screen_height);

/* Destroy both virtual devices and close the fds. */
void uinput_cleanup(void);

/* True if the pointer device was created. */
bool uinput_available(void);

/* True if the keyboard device was created. */
bool uinput_kbd_available(void);

/* Move the pointer to absolute screen coordinates. */
int  uinput_mouse_move(int x, int y);

/* Click at the current pointer position. button: 1=left,2=mid,3=right. */
int  uinput_click(int button, int repeat);

/* Press / release a mouse button. */
int  uinput_mouse_down(int button);
int  uinput_mouse_up(int button);

/* Scroll: positive = down/right, negative = up/left. */
int  uinput_scroll(int amount);

/* Press a key combo in xdotool format, e.g. "Return", "ctrl+a". */
int  uinput_key_press(const char *keys);

/* Type a single ASCII character (handles shift for uppercase/symbols). */
int  uinput_type_char(char c);

#endif /* DESKPAL_UINPUT_H */
