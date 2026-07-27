/* Deskpal normalized frame-buffer ownership. */
#include "screenshot.h"

#include <stdlib.h>
#include <string.h>

void screenshot_frame_clear(ScreenshotFrame *frame)
{
	if (!frame) return;
	free(frame->pixels);
	memset(frame, 0, sizeof(*frame));
}
