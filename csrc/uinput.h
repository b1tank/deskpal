/*
 * deskpal — uinput virtual mouse for Wayland-compatible input
 *
 * Uses /dev/uinput to inject real input events that the Wayland
 * compositor treats as hardware input.  Falls back gracefully
 * when uinput is not accessible.
 *
 * Copyright (c) 2026 deskpal contributors
 * SPDX-License-Identifier: MIT
 */
#ifndef DESKPAL_UINPUT_H
#define DESKPAL_UINPUT_H

#include <stdbool.h>

/* Try to open /dev/uinput and create a virtual pointer.
 * Returns 0 on success, -1 if uinput is unavailable.
 * Safe to call multiple times; second call is a no-op. */
int  uinput_init(int screen_width, int screen_height);

/* Destroy the virtual device and close the fd. */
void uinput_cleanup(void);

/* True if the uinput device was created successfully. */
bool uinput_available(void);

/* Move the pointer to absolute screen coordinates. */
int  uinput_mouse_move(int x, int y);

/* Click at the current pointer position. button: 1=left,2=mid,3=right. */
int  uinput_click(int button, int repeat);

/* Press / release a mouse button. */
int  uinput_mouse_down(int button);
int  uinput_mouse_up(int button);

/* Scroll: positive = down/right, negative = up/left. */
int  uinput_scroll(int amount);

#endif /* DESKPAL_UINPUT_H */
