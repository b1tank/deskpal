/*
 * deskpal — Privacy-safe visual frame signatures and bounded pixel diffs
 * SPDX-License-Identifier: MIT
 */
#ifndef DESKPAL_FRAME_STATE_H
#define DESKPAL_FRAME_STATE_H

#include <stdint.h>

#include "frame_revision.h"
#include "screenshot.h"

typedef struct {
	char revision[DESKPAL_FRAME_REVISION_LEN];
	int width;
	int height;
	uint64_t pixel_count;
} FrameStateSignature;

typedef struct {
	int comparable;
	int changed;
	uint64_t changed_pixels;
	uint64_t total_pixels;
	double changed_fraction;
	int x;
	int y;
	int width;
	int height;
	int max_channel_delta;
} FrameStateDiff;

/* FNV-1a is an informational equality hint, not a security digest. */
int frame_state_signature(const ScreenshotFrame *frame,
                          FrameStateSignature *signature);

/* Build a bounded aspect-preserving box-average projection. Does not upscale.
 * The caller owns output through screenshot_frame_clear. */
int frame_state_project(const ScreenshotFrame *frame,
                        int max_width, int max_height,
                        ScreenshotFrame *output);

/* Compare normalized BGRA frames. A pixel changes when any channel delta is
 * greater than tolerance (0-255). Different dimensions are not comparable. */
int frame_state_compare(const ScreenshotFrame *before,
                        const ScreenshotFrame *after,
                        int tolerance,
                        FrameStateDiff *diff);

/* Compare pixels inside and outside one projection-space rectangle. */
int frame_state_compare_region(const ScreenshotFrame *before,
                               const ScreenshotFrame *after,
                               int tolerance,
                               int x, int y, int width, int height,
                               FrameStateDiff *inside,
                               FrameStateDiff *outside);

#endif /* DESKPAL_FRAME_STATE_H */
