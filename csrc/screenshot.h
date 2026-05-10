/*
 * deskpal — Screenshot capture via XCB
 *
 * Copyright (c) 2026 deskpal contributors
 * SPDX-License-Identifier: MIT
 */
#ifndef DESKPAL_SCREENSHOT_H
#define DESKPAL_SCREENSHOT_H

#include <stddef.h>
#include <stdint.h>

/* Capture a window as a PNG buffer. Returns malloc'd buffer, sets *out_len.
 * If wid == 0, captures the root window (full screen).
 * Returns NULL on failure. Caller must free(). */
uint8_t *screenshot_capture_png(unsigned long wid, size_t *out_len);

/* Encode raw BGRA pixels to PNG in memory. Returns malloc'd buffer.
 * Caller must free(). */
uint8_t *screenshot_encode_png(const uint8_t *pixels, int width, int height,
                               size_t *out_len);

/* Base64-encode binary data. Returns malloc'd null-terminated string.
 * Caller must free(). */
char *screenshot_base64_encode(const uint8_t *data, size_t len);

#endif /* DESKPAL_SCREENSHOT_H */
