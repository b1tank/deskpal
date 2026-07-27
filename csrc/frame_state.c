/*
 * deskpal — Privacy-safe visual frame signatures and bounded pixel diffs
 * SPDX-License-Identifier: MIT
 */
#include "frame_state.h"

#include <stdio.h>
#include <string.h>

static int valid_frame(const ScreenshotFrame *frame)
{
	if (!frame || !frame->pixels || frame->width <= 0 || frame->height <= 0)
		return 0;
	uint64_t pixels = (uint64_t)(unsigned int)frame->width *
	                  (uint64_t)(unsigned int)frame->height;
	return pixels <= SIZE_MAX / 4 && frame->length == (size_t)pixels * 4;
}

static void hash_byte(uint64_t *hash, unsigned char value)
{
	*hash ^= value;
	*hash *= UINT64_C(1099511628211);
}

int frame_state_signature(const ScreenshotFrame *frame,
                          FrameStateSignature *signature)
{
	if (!valid_frame(frame) || !signature) return -1;
	memset(signature, 0, sizeof(*signature));
	uint64_t hash = UINT64_C(1469598103934665603);
	uint32_t dimensions[] = {
		(uint32_t)frame->width,
		(uint32_t)frame->height,
	};
	for (size_t i = 0; i < sizeof(dimensions) / sizeof(dimensions[0]); i++)
		for (unsigned int shift = 0; shift < 32; shift += 8)
			hash_byte(&hash, (unsigned char)(dimensions[i] >> shift));
	for (size_t i = 0; i < frame->length; i++)
		hash_byte(&hash, frame->pixels[i]);
	snprintf(signature->revision, sizeof(signature->revision),
	         "fnv1a64-%016llx", (unsigned long long)hash);
	signature->width = frame->width;
	signature->height = frame->height;
	signature->pixel_count = (uint64_t)(unsigned int)frame->width *
	                         (uint64_t)(unsigned int)frame->height;
	return 0;
}

int frame_state_compare(const ScreenshotFrame *before,
                        const ScreenshotFrame *after,
                        int tolerance,
                        FrameStateDiff *diff)
{
	if (!diff || tolerance < 0 || tolerance > 255 ||
	    !valid_frame(before) || !valid_frame(after))
		return -1;
	memset(diff, 0, sizeof(*diff));
	diff->total_pixels = (uint64_t)(unsigned int)before->width *
	                     (uint64_t)(unsigned int)before->height;
	if (before->width != after->width || before->height != after->height ||
	    before->length != after->length) {
		diff->changed = 1;
		return 0;
	}
	diff->comparable = 1;
	int min_x = before->width;
	int min_y = before->height;
	int max_x = -1;
	int max_y = -1;
	for (int y = 0; y < before->height; y++) {
		for (int x = 0; x < before->width; x++) {
			size_t offset = ((size_t)y * (size_t)before->width +
			                 (size_t)x) * 4;
			int pixel_changed = 0;
			for (int channel = 0; channel < 4; channel++) {
				int delta = (int)before->pixels[offset + (size_t)channel] -
				            (int)after->pixels[offset + (size_t)channel];
				if (delta < 0) delta = -delta;
				if (delta > diff->max_channel_delta)
					diff->max_channel_delta = delta;
				if (delta > tolerance) pixel_changed = 1;
			}
			if (!pixel_changed) continue;
			diff->changed_pixels++;
			if (x < min_x) min_x = x;
			if (x > max_x) max_x = x;
			if (y < min_y) min_y = y;
			if (y > max_y) max_y = y;
		}
	}
	diff->changed = diff->changed_pixels > 0;
	if (diff->total_pixels)
		diff->changed_fraction =
			(double)diff->changed_pixels / (double)diff->total_pixels;
	if (diff->changed) {
		diff->x = min_x;
		diff->y = min_y;
		diff->width = max_x - min_x + 1;
		diff->height = max_y - min_y + 1;
	}
	return 0;
}
