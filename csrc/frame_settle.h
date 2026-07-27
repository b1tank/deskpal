/*
 * deskpal — Capture-bound visual frame settling
 * SPDX-License-Identifier: MIT
 */
#ifndef DESKPAL_FRAME_SETTLE_H
#define DESKPAL_FRAME_SETTLE_H

#include <stddef.h>

#include "captures.h"
#include "frame_state.h"

typedef int (*FrameSettleValidateTarget)(
	const DeskpalCapture *base, void *data, char *error, size_t error_len);
typedef int (*FrameSettleCaptureTarget)(
	const DeskpalCapture *base, void *data, ScreenshotFrame *frame,
	char *error, size_t error_len);
typedef int (*FrameSettleCancelCheck)(void *data);

typedef enum {
	FRAME_SETTLE_SETTLED = 1,
	FRAME_SETTLE_TIMEOUT = 2,
	FRAME_SETTLE_CANCELLED = 3,
	FRAME_SETTLE_TARGET_INVALID = -1,
	FRAME_SETTLE_CAPTURE_FAILED = -2,
} FrameSettleStatus;

typedef struct {
	FrameSettleStatus status;
	FrameStateSignature final_signature;
	ScreenshotFrame final_projection;
	FrameStateDiff last_change;
	FrameStateDiff largest_change;
	int sample_count;
	int change_count;
	int stable_for_ms;
	int changed_from_capture;
	char error[256];
} FrameSettleResult;

long long frame_settle_monotonic_ms(void);

/* Sample under one absolute deadline. Stability compares every sample against
 * the anchor captured when the current stable period began, preventing slow
 * per-sample drift from being classified as stable. */
int frame_settle_wait(
	const DeskpalCapture *base,
	long long deadline_ms,
	int stable_ms,
	int interval_ms,
	int tolerance,
	FrameSettleValidateTarget validate,
	FrameSettleCaptureTarget capture,
	void *callback_data,
	FrameSettleCancelCheck cancel_check,
	void *cancel_data,
	FrameSettleResult *result);

void frame_settle_result_clear(FrameSettleResult *result);

#endif /* DESKPAL_FRAME_SETTLE_H */
