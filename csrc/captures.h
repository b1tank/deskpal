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

typedef struct {
	char id[DESKPAL_CAPTURE_ID_LEN];
	int source_width;
	int source_height;
	int image_width;
	int image_height;
	int64_t created_monotonic_ms;
} DeskpalCapture;

/* Store a full-desktop capture in a bounded process-local history. */
int captures_store_desktop(int source_width, int source_height,
                           int image_width, int image_height,
                           DeskpalCapture *capture);

/* Resolve an ID from this process's bounded history. Returns -2 if expired. */
int captures_lookup(const char *id, DeskpalCapture *capture);

void captures_cleanup(void);

#endif /* DESKPAL_CAPTURES_H */
