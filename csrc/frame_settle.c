/*
 * deskpal — Capture-bound visual frame settling
 * SPDX-License-Identifier: MIT
 */
#include "frame_settle.h"

#include <stdio.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

long long frame_settle_monotonic_ms(void)
{
	struct timespec ts;
	if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0) return 0;
	return (long long)ts.tv_sec * 1000LL + ts.tv_nsec / 1000000LL;
}

static void set_error(FrameSettleResult *result, const char *message)
{
	snprintf(result->error, sizeof(result->error), "%s",
	         message && message[0] ? message : "Frame settling failed");
}

static int validate_base(const DeskpalCapture *base,
                         FrameSettleValidateTarget validate,
                         void *callback_data,
                         FrameSettleResult *result)
{
	char error[256] = {0};
	if (validate(base, callback_data, error, sizeof(error)) == 0)
		return 0;
	result->status = FRAME_SETTLE_TARGET_INVALID;
	set_error(result, error);
	return -1;
}

static int capture_frame(const DeskpalCapture *base,
                         FrameSettleCaptureTarget capture,
                         void *callback_data,
                         ScreenshotFrame *frame,
                         FrameSettleResult *result)
{
	char error[256] = {0};
	memset(frame, 0, sizeof(*frame));
	if (capture(base, callback_data, frame, error, sizeof(error)) == 0 &&
	    frame->pixels && frame->width == base->source_width &&
	    frame->height == base->source_height)
		return 0;
	screenshot_frame_clear(frame);
	result->status = FRAME_SETTLE_CAPTURE_FAILED;
	set_error(result, error[0] ? error :
	          "Captured frame dimensions differ from the retained observation");
	return -1;
}

static int wait_until(long long target_ms, long long deadline_ms,
                      FrameSettleCancelCheck cancel_check, void *cancel_data)
{
	for (;;) {
		if (cancel_check && cancel_check(cancel_data)) return 1;
		long long now = frame_settle_monotonic_ms();
		if (now >= target_ms || now >= deadline_ms) return 0;
		long long remaining = target_ms - now;
		int sleep_ms = remaining < 10 ? (int)remaining : 10;
		if (sleep_ms > 0) usleep((useconds_t)sleep_ms * 1000U);
	}
}

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
	FrameSettleResult *result)
{
	if (!result) return -1;
	memset(result, 0, sizeof(*result));
	if (!base || base->target != DESKPAL_CAPTURE_WINDOW ||
	    !base->frame_revision[0] || base->source_width <= 0 ||
	    base->source_height <= 0 || !validate || !capture ||
	    stable_ms < 1 || interval_ms < 1 || tolerance < 0 || tolerance > 255 ||
	    deadline_ms <= frame_settle_monotonic_ms()) {
		result->status = FRAME_SETTLE_TARGET_INVALID;
		set_error(result, "Frame settling request is incomplete");
		return -1;
	}
	if (validate_base(base, validate, callback_data, result) != 0) return -1;

	ScreenshotFrame anchor;
	if (capture_frame(base, capture, callback_data, &anchor, result) != 0)
		return -1;
	if (validate_base(base, validate, callback_data, result) != 0) {
		screenshot_frame_clear(&anchor);
		return -1;
	}
	result->sample_count = 1;
	if (frame_state_signature(&anchor, &result->final_signature) != 0) {
		screenshot_frame_clear(&anchor);
		result->status = FRAME_SETTLE_CAPTURE_FAILED;
		set_error(result, "Could not sign captured frame");
		return -1;
	}
	long long stable_since = frame_settle_monotonic_ms();
	long long next_sample = stable_since + interval_ms;

	for (;;) {
		if (wait_until(next_sample, deadline_ms, cancel_check, cancel_data)) {
			result->stable_for_ms = (int)(
				frame_settle_monotonic_ms() - stable_since);
			result->status = FRAME_SETTLE_CANCELLED;
			break;
		}
		long long now = frame_settle_monotonic_ms();
		if (now >= deadline_ms) {
			result->stable_for_ms = (int)(now - stable_since);
			if (validate_base(base, validate, callback_data, result) == 0)
				result->status = FRAME_SETTLE_TIMEOUT;
			break;
		}
		if (validate_base(base, validate, callback_data, result) != 0) break;

		ScreenshotFrame sample;
		if (capture_frame(base, capture, callback_data, &sample, result) != 0)
			break;
		result->sample_count++;
		if (frame_state_signature(&sample, &result->final_signature) != 0) {
			screenshot_frame_clear(&sample);
			result->status = FRAME_SETTLE_CAPTURE_FAILED;
			set_error(result, "Could not sign sampled frame");
			break;
		}
		if (validate_base(base, validate, callback_data, result) != 0) {
			screenshot_frame_clear(&sample);
			break;
		}
		now = frame_settle_monotonic_ms();
		if (now >= deadline_ms) {
			result->stable_for_ms = (int)(now - stable_since);
			result->status = FRAME_SETTLE_TIMEOUT;
			screenshot_frame_clear(&sample);
			break;
		}
		FrameStateDiff difference;
		if (frame_state_compare(&anchor, &sample, tolerance, &difference) != 0 ||
		    !difference.comparable) {
			screenshot_frame_clear(&sample);
			result->status = FRAME_SETTLE_CAPTURE_FAILED;
			set_error(result, "Sampled frames are not comparable");
			break;
		}
		if (difference.changed) {
			result->change_count++;
			result->last_change = difference;
			if (difference.changed_fraction >
			    result->largest_change.changed_fraction)
				result->largest_change = difference;
			screenshot_frame_clear(&anchor);
			anchor = sample;
			memset(&sample, 0, sizeof(sample));
			stable_since = now;
		} else {
			screenshot_frame_clear(&sample);
		}
		result->stable_for_ms = (int)(now - stable_since);
		if (result->stable_for_ms >= stable_ms) {
			if (validate_base(base, validate, callback_data, result) == 0)
				result->status = FRAME_SETTLE_SETTLED;
			break;
		}
		next_sample = now + interval_ms;
	}

	result->changed_from_capture = strcmp(
		base->frame_revision, result->final_signature.revision) != 0;
	if (frame_state_project(
	    &anchor, DESKPAL_FRAME_PROJECTION_MAX_DIMENSION,
	    DESKPAL_FRAME_PROJECTION_MAX_DIMENSION,
	    &result->final_projection) != 0 &&
	    result->status == FRAME_SETTLE_SETTLED) {
		result->status = FRAME_SETTLE_CAPTURE_FAILED;
		set_error(result, "Could not project final settled frame");
	}
	screenshot_frame_clear(&anchor);
	return result->status == FRAME_SETTLE_SETTLED ? 1 :
	       result->status == FRAME_SETTLE_TIMEOUT ||
	       result->status == FRAME_SETTLE_CANCELLED ? 0 : -1;
}

void frame_settle_result_clear(FrameSettleResult *result)
{
	if (!result) return;
	screenshot_frame_clear(&result->final_projection);
	memset(result, 0, sizeof(*result));
}
