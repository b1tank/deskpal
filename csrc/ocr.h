/*
 * deskpal — OCR via tesseract (dlopen)
 *
 * Copyright (c) 2026 deskpal contributors
 * SPDX-License-Identifier: MIT
 */
#ifndef DESKPAL_OCR_H
#define DESKPAL_OCR_H

#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>

/* ── Types ────────────────────────────────────────────────────────────────── */

typedef struct {
	char text[128];
	int  x, y, width, height;
	int  confidence;
} OcrBox;

typedef struct {
	OcrBox *boxes;
	int     count;
	int     capacity;
} OcrResult;

/* ── API ──────────────────────────────────────────────────────────────────── */

/* Try to load tesseract via dlopen. Returns true if available. */
bool ocr_init(void);

/* Check if OCR is available (tesseract loaded). */
bool ocr_available(void);

/* Cleanup. */
void ocr_cleanup(void);

/* Run OCR on a PNG image buffer. Returns word-level bounding boxes.
 * Caller must call ocr_result_free(). */
OcrResult ocr_recognize(const uint8_t *png_data, size_t png_len);

/* Run OCR on a raw BGRA pixel buffer. */
OcrResult ocr_recognize_raw(const uint8_t *pixels, int width, int height,
                            int bytes_per_pixel);

/* Free an OCR result. */
void ocr_result_free(OcrResult *result);

/* Find all occurrences of text in OCR results.
 * Returns array of {x,y,w,h} rects. count is set. Caller must free(). */
typedef struct { int x, y, width, height; } OcrMatch;
OcrMatch *ocr_find_text(const OcrResult *result, const char *search_text,
                        int *count);

#endif /* DESKPAL_OCR_H */
