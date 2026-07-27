/* Deterministic unit coverage for visual frame signatures and diffs. */
#include "frame_settle.h"
#include "frame_state.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
	const uint8_t *template_pixels;
	const int *values;
	int value_count;
	int index;
	int valid;
	int cancel;
} SettleFixture;

static int validate_fixture(const DeskpalCapture *base, void *data,
                            char *error, size_t error_len)
{
	(void)base;
	SettleFixture *fixture = data;
	if (fixture->valid) return 0;
	snprintf(error, error_len, "fixture target changed");
	return -1;
}

static int capture_fixture(const DeskpalCapture *base, void *data,
                           ScreenshotFrame *frame,
                           char *error, size_t error_len)
{
	(void)error;
	(void)error_len;
	SettleFixture *fixture = data;
	frame->length = (size_t)base->source_width *
	                (size_t)base->source_height * 4;
	frame->pixels = malloc(frame->length);
	if (!frame->pixels) return -1;
	memcpy(frame->pixels, fixture->template_pixels, frame->length);
	int position = fixture->index < fixture->value_count
		? fixture->index : fixture->value_count - 1;
	frame->pixels[0] = (uint8_t)(frame->pixels[0] + fixture->values[position]);
	fixture->index++;
	frame->width = base->source_width;
	frame->height = base->source_height;
	frame->depth = 24;
	return 0;
}

static int cancel_fixture(void *data)
{
	return ((SettleFixture *)data)->cancel;
}

#define CHECK(condition) do { \
	if (!(condition)) { \
		fprintf(stderr, "frame_state_test failed at line %d\n", __LINE__); \
		return 1; \
	} \
} while (0)

int main(void)
{
	uint8_t first_pixels[16] = {
		10, 20, 30, 255, 40, 50, 60, 255,
		70, 80, 90, 255, 100, 110, 120, 255,
	};
	uint8_t same_pixels[16];
	memcpy(same_pixels, first_pixels, sizeof(first_pixels));
	ScreenshotFrame first = {
		.pixels = first_pixels,
		.length = sizeof(first_pixels),
		.width = 2,
		.height = 2,
		.depth = 24,
	};
	ScreenshotFrame same = first;
	same.pixels = same_pixels;

	FrameStateSignature first_signature;
	FrameStateSignature same_signature;
	CHECK(frame_state_signature(&first, &first_signature) == 0);
	CHECK(frame_state_signature(&same, &same_signature) == 0);
	CHECK(strcmp(first_signature.revision, same_signature.revision) == 0);
	CHECK(first_signature.pixel_count == 4);

	FrameStateDiff diff;
	CHECK(frame_state_compare(&first, &same, 0, &diff) == 0);
	CHECK(diff.comparable && !diff.changed && diff.changed_pixels == 0);
	CHECK(diff.total_pixels == 4 && diff.changed_fraction == 0.0);

	same_pixels[9] += 3;
	CHECK(frame_state_compare(&first, &same, 0, &diff) == 0);
	CHECK(diff.comparable && diff.changed && diff.changed_pixels == 1);
	CHECK(diff.x == 0 && diff.y == 1 && diff.width == 1 && diff.height == 1);
	CHECK(diff.max_channel_delta == 3);
	CHECK(diff.changed_fraction == 0.25);
	CHECK(frame_state_compare(&first, &same, 3, &diff) == 0);
	CHECK(diff.comparable && !diff.changed && diff.max_channel_delta == 3);
	FrameStateDiff inside;
	FrameStateDiff outside;
	CHECK(frame_state_compare_region(
		&first, &same, 0, 0, 1, 1, 1, &inside, &outside) == 0);
	CHECK(inside.changed && inside.changed_pixels == 1 && inside.total_pixels == 1);
	CHECK(!outside.changed && outside.total_pixels == 3);

	uint8_t small_pixels[4] = {10, 20, 30, 255};
	ScreenshotFrame small = {
		.pixels = small_pixels,
		.length = sizeof(small_pixels),
		.width = 1,
		.height = 1,
		.depth = 24,
	};
	CHECK(frame_state_compare(&first, &small, 0, &diff) == 0);
	CHECK(!diff.comparable && diff.changed);
	ScreenshotFrame projection;
	CHECK(frame_state_project(&first, 1, 1, &projection) == 0);
	CHECK(projection.width == 1 && projection.height == 1);
	CHECK(projection.pixels[0] == 55 && projection.pixels[1] == 65 &&
	      projection.pixels[2] == 75 && projection.pixels[3] == 255);
	ScreenshotFrame same_projection;
	CHECK(frame_state_project(&same, 1, 1, &same_projection) == 0);
	CHECK(frame_state_compare_region(
		&projection, &same_projection, 0, 0, 0, 1, 1,
		&inside, &outside) == 0);
	CHECK(inside.changed && outside.total_pixels == 0);
	screenshot_frame_clear(&same_projection);
	screenshot_frame_clear(&projection);
	CHECK(frame_state_compare(&first, &same, -1, &diff) == -1);
	CHECK(frame_state_signature(NULL, &first_signature) == -1);

	DeskpalCapture base = {
		.target = DESKPAL_CAPTURE_WINDOW,
		.source_width = 2,
		.source_height = 2,
	};
	snprintf(base.frame_revision, sizeof(base.frame_revision), "%s",
	         first_signature.revision);
	const int changing_values[] = {0, 0, 10, 10, 10, 10, 10};
	SettleFixture fixture = {
		.template_pixels = first_pixels,
		.values = changing_values,
		.value_count = (int)(sizeof(changing_values) /
		                    sizeof(changing_values[0])),
		.valid = 1,
	};
	FrameSettleResult settled;
	CHECK(frame_settle_wait(
		&base, frame_settle_monotonic_ms() + 500,
		30, 10, 0, validate_fixture, capture_fixture, &fixture,
		cancel_fixture, &fixture, &settled) == 1);
	CHECK(settled.status == FRAME_SETTLE_SETTLED);
	CHECK(settled.change_count == 1 && settled.changed_from_capture);
	CHECK(settled.stable_for_ms >= 30 && settled.sample_count >= 5);
	CHECK(settled.final_projection.pixels != NULL);
	frame_settle_result_clear(&settled);

	const int static_values[] = {0};
	fixture.values = static_values;
	fixture.value_count = 1;
	fixture.index = 0;
	CHECK(frame_settle_wait(
		&base, frame_settle_monotonic_ms() + 50,
		100, 20, 0, validate_fixture, capture_fixture, &fixture,
		cancel_fixture, &fixture, &settled) == 0);
	CHECK(settled.status == FRAME_SETTLE_TIMEOUT);
	frame_settle_result_clear(&settled);

	fixture.index = 0;
	fixture.cancel = 1;
	CHECK(frame_settle_wait(
		&base, frame_settle_monotonic_ms() + 100,
		30, 10, 0, validate_fixture, capture_fixture, &fixture,
		cancel_fixture, &fixture, &settled) == 0);
	CHECK(settled.status == FRAME_SETTLE_CANCELLED);
	frame_settle_result_clear(&settled);

	puts("PASS: visual frame signatures, diffs, and settling");
	return 0;
}
