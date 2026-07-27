/*
 * deskpal — Privacy-safe visual frame signatures and bounded pixel diffs
 * SPDX-License-Identifier: MIT
 */
#include "frame_state.h"

#include <stdio.h>
#include <stdlib.h>
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

int frame_state_project(const ScreenshotFrame *frame,
                        int max_width, int max_height,
                        ScreenshotFrame *output)
{
	if (!valid_frame(frame) || !output || max_width < 1 || max_height < 1)
		return -1;
	memset(output, 0, sizeof(*output));
	double scale_x = frame->width > max_width
		? (double)max_width / frame->width : 1.0;
	double scale_y = frame->height > max_height
		? (double)max_height / frame->height : 1.0;
	double scale = scale_x < scale_y ? scale_x : scale_y;
	int width = (int)(frame->width * scale + 0.5);
	int height = (int)(frame->height * scale + 0.5);
	if (width < 1) width = 1;
	if (height < 1) height = 1;
	size_t length = (size_t)width * (size_t)height * 4;
	uint8_t *pixels = malloc(length);
	if (!pixels) return -1;
	for (int y = 0; y < height; y++) {
		int source_y0 = (int)((int64_t)y * frame->height / height);
		int source_y1 = (int)((int64_t)(y + 1) * frame->height / height);
		if (source_y1 <= source_y0) source_y1 = source_y0 + 1;
		for (int x = 0; x < width; x++) {
			int source_x0 = (int)((int64_t)x * frame->width / width);
			int source_x1 = (int)((int64_t)(x + 1) * frame->width / width);
			if (source_x1 <= source_x0) source_x1 = source_x0 + 1;
			uint64_t sums[4] = {0};
			uint64_t count = (uint64_t)(source_x1 - source_x0) *
			                 (uint64_t)(source_y1 - source_y0);
			for (int sy = source_y0; sy < source_y1; sy++)
				for (int sx = source_x0; sx < source_x1; sx++) {
					size_t source = ((size_t)sy * (size_t)frame->width +
					                 (size_t)sx) * 4;
					for (int channel = 0; channel < 4; channel++)
						sums[channel] += frame->pixels[source + (size_t)channel];
				}
			size_t target = ((size_t)y * (size_t)width + (size_t)x) * 4;
			for (int channel = 0; channel < 4; channel++)
				pixels[target + (size_t)channel] =
					(uint8_t)((sums[channel] + count / 2) / count);
		}
	}
	output->pixels = pixels;
	output->length = length;
	output->width = width;
	output->height = height;
	output->depth = frame->depth;
	return 0;
}

static int compare_area(const ScreenshotFrame *before,
                        const ScreenshotFrame *after,
                        int tolerance,
                        int region_x, int region_y,
                        int region_width, int region_height,
                        int invert,
                        FrameStateDiff *diff)
{
	memset(diff, 0, sizeof(*diff));
	diff->comparable = 1;
	int min_x = before->width;
	int min_y = before->height;
	int max_x = -1;
	int max_y = -1;
	for (int y = 0; y < before->height; y++) {
		for (int x = 0; x < before->width; x++) {
			int in_region = x >= region_x && y >= region_y &&
				x < region_x + region_width && y < region_y + region_height;
			if (invert ? in_region : !in_region) continue;
			diff->total_pixels++;
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

static int comparable_frames(const ScreenshotFrame *before,
                             const ScreenshotFrame *after,
                             int tolerance)
{
	return tolerance >= 0 && tolerance <= 255 && valid_frame(before) &&
	       valid_frame(after) && before->width == after->width &&
	       before->height == after->height && before->length == after->length;
}

int frame_state_compare(const ScreenshotFrame *before,
                        const ScreenshotFrame *after,
                        int tolerance,
                        FrameStateDiff *diff)
{
	if (!diff || tolerance < 0 || tolerance > 255 ||
	    !valid_frame(before) || !valid_frame(after))
		return -1;
	if (!comparable_frames(before, after, tolerance)) {
		memset(diff, 0, sizeof(*diff));
		diff->changed = 1;
		diff->total_pixels = (uint64_t)(unsigned int)before->width *
		                     (uint64_t)(unsigned int)before->height;
		return 0;
	}
	return compare_area(before, after, tolerance,
	                    0, 0, before->width, before->height, 0, diff);
}

int frame_state_compare_region(const ScreenshotFrame *before,
                               const ScreenshotFrame *after,
                               int tolerance,
                               int x, int y, int width, int height,
                               FrameStateDiff *inside,
                               FrameStateDiff *outside)
{
	if (!inside || !outside || !comparable_frames(before, after, tolerance) ||
	    x < 0 || y < 0 || width < 1 || height < 1 ||
	    x > before->width - width || y > before->height - height)
		return -1;
	compare_area(before, after, tolerance, x, y, width, height, 0, inside);
	compare_area(before, after, tolerance, x, y, width, height, 1, outside);
	return 0;
}
