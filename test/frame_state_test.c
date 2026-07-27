/* Deterministic unit coverage for visual frame signatures and diffs. */
#include "frame_state.h"

#include <stdio.h>
#include <string.h>

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
	CHECK(frame_state_compare(&first, &same, -1, &diff) == -1);
	CHECK(frame_state_signature(NULL, &first_signature) == -1);

	puts("PASS: visual frame signatures and bounded pixel diffs");
	return 0;
}
