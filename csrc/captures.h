/*
 * deskpal — Bounded screenshot capture identities
 *
 * Copyright (c) 2026 deskpal contributors
 * SPDX-License-Identifier: MIT
 */
#ifndef DESKPAL_CAPTURES_H
#define DESKPAL_CAPTURES_H

#include <stdint.h>

#define DESKPAL_CAPTURE_ID_LEN 64
#define DESKPAL_SEMANTIC_REVISION_LEN 32

typedef enum {
	DESKPAL_CAPTURE_DESKTOP = 1,
	DESKPAL_CAPTURE_WINDOW = 2,
} DeskpalCaptureTarget;

typedef struct {
	char id[DESKPAL_CAPTURE_ID_LEN];
	DeskpalCaptureTarget target;
	unsigned long window_id;
	long process_id;
	char title[256];
	char app_class[128];
	int window_x;
	int window_y;
	int window_width;
	int window_height;
	int source_width;
	int source_height;
	int image_width;
	int image_height;
	char semantic_revision[DESKPAL_SEMANTIC_REVISION_LEN];
	const char *semantic_snapshot;
	int semantic_complete;
	int64_t created_monotonic_ms;
} DeskpalCapture;

/* Store a full-desktop capture in a bounded process-local history. */
int captures_store_desktop(int source_width, int source_height,
                           int image_width, int image_height,
                           DeskpalCapture *capture);

/* Store a stable exact-window observation and its image-to-desktop frame. */
int captures_store_window(unsigned long window_id, long process_id,
                          const char *title, const char *app_class,
                          int window_x, int window_y,
                          int window_width, int window_height,
                          int source_width, int source_height,
                          int image_width, int image_height,
                          const char *semantic_revision,
                          const char *semantic_snapshot,
                          int semantic_complete,
                          DeskpalCapture *capture);

/* Resolve an ID from this process's bounded history. Returns -2 if expired. */
int captures_lookup(const char *id, DeskpalCapture *capture);

void captures_cleanup(void);

#endif /* DESKPAL_CAPTURES_H */
