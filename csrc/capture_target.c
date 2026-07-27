/* Deskpal retained exact-window validation shared by capture-bound routes. */
#include "capture_target.h"

#include "x11.h"

#include <stdio.h>
#include <string.h>

int capture_window_still_valid(const DeskpalCapture *capture)
{
	if (!capture || capture->target != DESKPAL_CAPTURE_WINDOW) return 0;
	WindowInfo current;
	return x11_get_window_info(capture->window_id, &current) == 0 &&
	       current.viewable && current.pid == capture->process_id &&
	       strcmp(current.title, capture->title) == 0 &&
	       strcmp(current.app_class, capture->app_class) == 0 &&
	       current.x == capture->window_x && current.y == capture->window_y &&
	       current.width == capture->window_width &&
	       current.height == capture->window_height;
}

int capture_validate_window(const DeskpalCapture *capture, void *data,
                            char *error, size_t error_len)
{
	(void)data;
	if (capture_window_still_valid(capture)) return 0;
	snprintf(error, error_len,
	         "Captured X11 target was replaced or its geometry changed");
	return -1;
}
