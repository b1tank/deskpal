/* Deskpal shared-seat compatibility pixel actions. */
#include "pixel_action.h"

#include "uinput.h"
#include "x11.h"

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

static int target_viewable(unsigned long window_id)
{
	WindowInfo info;
	return x11_get_window_info(window_id, &info) == 0 && info.viewable;
}

static int fail(char *error, size_t error_len, const char *message)
{
	snprintf(error, error_len, "%s", message);
	return -1;
}

int pixel_click_window(unsigned long window_id, int x, int y,
                       int button, int repeat,
                       char *error, size_t error_len)
{
	if (!window_id || x < 0 || y < 0 || button < 1 || button > 3 ||
	    repeat < 1 || repeat > 2)
		return fail(error, error_len, "Invalid pixel click request");
	WindowInfo info;
	if (x11_get_window_info(window_id, &info) != 0 || !info.viewable ||
	    x >= info.width || y >= info.height)
		return fail(error, error_len, "Pixel click target is unavailable");
	if (getenv("DESKPAL_HEADLESS_ACTIVE"))
		x11_focus_window(window_id);
	if (x11_is_wayland()) {
		if (x11_window_mouse_move(window_id, x, y) != 0)
			return fail(error, error_len, "Mouse move failed");
		usleep(10000);
		if (!target_viewable(window_id))
			return fail(error, error_len, "Pixel click target disappeared");
		if (x11_click(button, repeat) != 0)
			return fail(error, error_len, "Click failed");
		return 0;
	}
	if (x11_mouse_move(info.x + x, info.y + y) != 0)
		return fail(error, error_len, "Mouse move failed");
	usleep(10000);
	if (!target_viewable(window_id))
		return fail(error, error_len, "Pixel click target disappeared");
	if (button != 1 && uinput_available()) {
		char command[160];
		snprintf(command, sizeof(command),
			"xdotool mousemove --window %lu %d %d && xdotool click %d",
			window_id, x, y, button);
		for (int i = 0; i < repeat; i++)
			if (system(command) != 0)
				return fail(error, error_len, "Click failed");
		return 0;
	}
	if (x11_click(button, repeat) != 0)
		return fail(error, error_len, "Click failed");
	return 0;
}
