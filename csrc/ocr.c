/*
 * deskpal — OCR via tesseract (dlopen for optional runtime dependency)
 *
 * We dlopen libtesseract at runtime so deskpal still works without
 * tesseract installed — OCR tools just report "not available".
 *
 * Copyright (c) 2026 deskpal contributors
 * SPDX-License-Identifier: MIT
 */
#include "ocr.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <dlfcn.h>
#include <ctype.h>

/* ── Tesseract C API types (from tesseract/capi.h) ────────────────────────── */

typedef struct TessBaseAPI TessBaseAPI;
typedef struct Pix Pix;

/* Function pointer types */
typedef TessBaseAPI *(*fn_TessBaseAPICreate)(void);
typedef int          (*fn_TessBaseAPIInit3)(TessBaseAPI *, const char *, const char *);
typedef void         (*fn_TessBaseAPISetImage)(TessBaseAPI *, const unsigned char *,
                                               int, int, int, int);
typedef int          (*fn_TessBaseAPIRecognize)(TessBaseAPI *, void *);
typedef char        *(*fn_TessBaseAPIGetUTF8Text)(TessBaseAPI *);
typedef void         (*fn_TessBaseAPIEnd)(TessBaseAPI *);
typedef void         (*fn_TessBaseAPIDelete)(TessBaseAPI *);
typedef void         (*fn_TessDeleteText)(char *);
typedef int         *(*fn_TessBaseAPIAllWordConfidences)(TessBaseAPI *);
typedef void         (*fn_TessBaseAPISetPageSegMode)(TessBaseAPI *, int);

/* Tesseract iterator types for word-level bounding boxes */
typedef struct TessResultIterator TessResultIterator;
typedef struct TessPageIterator TessPageIterator;

typedef TessResultIterator *(*fn_TessBaseAPIGetIterator)(TessBaseAPI *);
typedef int                 (*fn_TessResultIteratorNext)(TessResultIterator *, int);
typedef char               *(*fn_TessResultIteratorGetUTF8Text)(TessResultIterator *, int);
typedef float               (*fn_TessResultIteratorConfidence)(TessResultIterator *, int);
typedef int                 (*fn_TessPageIteratorBoundingBox)(TessPageIterator *,
                                                              int, int *, int *, int *, int *);
typedef void                (*fn_TessResultIteratorDelete)(TessResultIterator *);
/* TessPageIterator is obtained by casting TessResultIterator */

/* Page iterator levels */
#define RIL_WORD 3

/* Page segmentation modes */
#define PSM_AUTO 3

/* ── dlopen state ─────────────────────────────────────────────────────────── */

static void *g_tess_lib = NULL;
static int   g_ocr_available = 0;

static fn_TessBaseAPICreate             f_Create;
static fn_TessBaseAPIInit3              f_Init;
static fn_TessBaseAPISetImage           f_SetImage;
static fn_TessBaseAPIRecognize          f_Recognize;
static fn_TessBaseAPIGetUTF8Text        f_GetUTF8Text;
static fn_TessBaseAPIEnd                f_End;
static fn_TessBaseAPIDelete             f_Delete;
static fn_TessDeleteText                f_DeleteText;
static fn_TessBaseAPISetPageSegMode     f_SetPageSegMode;
static fn_TessBaseAPIGetIterator        f_GetIterator;
static fn_TessResultIteratorNext        f_IterNext;
static fn_TessResultIteratorGetUTF8Text f_IterGetText;
static fn_TessResultIteratorConfidence  f_IterConfidence;
static fn_TessPageIteratorBoundingBox   f_IterBoundingBox;
static fn_TessResultIteratorDelete      f_IterDelete;

#define LOAD_SYM(var, name) do { \
	*(void **)&(var) = dlsym(g_tess_lib, name); \
	if (!(var)) { fprintf(stderr, "deskpal: missing symbol %s\n", name); goto fail; } \
} while(0)

bool ocr_init(void)
{
	/* Try common library names */
	const char *libs[] = {
		"libtesseract.so.5",
		"libtesseract.so.4",
		"libtesseract.so",
		NULL
	};

	for (int i = 0; libs[i]; i++) {
		g_tess_lib = dlopen(libs[i], RTLD_LAZY);
		if (g_tess_lib) break;
	}

	if (!g_tess_lib) {
		fprintf(stderr, "deskpal: tesseract library not found (%s)\n", dlerror());
		return false;
	}

	LOAD_SYM(f_Create,       "TessBaseAPICreate");
	LOAD_SYM(f_Init,         "TessBaseAPIInit3");
	LOAD_SYM(f_SetImage,     "TessBaseAPISetImage");
	LOAD_SYM(f_Recognize,    "TessBaseAPIRecognize");
	LOAD_SYM(f_GetUTF8Text,  "TessBaseAPIGetUTF8Text");
	LOAD_SYM(f_End,          "TessBaseAPIEnd");
	LOAD_SYM(f_Delete,       "TessBaseAPIDelete");
	LOAD_SYM(f_DeleteText,   "TessDeleteText");
	LOAD_SYM(f_SetPageSegMode, "TessBaseAPISetPageSegMode");
	LOAD_SYM(f_GetIterator,    "TessBaseAPIGetIterator");
	LOAD_SYM(f_IterNext,       "TessResultIteratorNext");
	LOAD_SYM(f_IterGetText,    "TessResultIteratorGetUTF8Text");
	LOAD_SYM(f_IterConfidence, "TessResultIteratorConfidence");
	LOAD_SYM(f_IterBoundingBox, "TessPageIteratorBoundingBox");
	LOAD_SYM(f_IterDelete,     "TessResultIteratorDelete");

	g_ocr_available = 1;
	return true;

fail:
	if (g_tess_lib) { dlclose(g_tess_lib); g_tess_lib = NULL; }
	return false;
}

bool ocr_available(void)
{
	return g_ocr_available != 0;
}

void ocr_cleanup(void)
{
	if (g_tess_lib) {
		dlclose(g_tess_lib);
		g_tess_lib = NULL;
	}
	g_ocr_available = 0;
}

/* ── Internal: grow result array ──────────────────────────────────────────── */

static void result_push(OcrResult *r, const OcrBox *box)
{
	if (r->count >= r->capacity) {
		int new_cap = r->capacity ? r->capacity * 2 : 64;
		OcrBox *tmp = realloc(r->boxes, new_cap * sizeof(OcrBox));
		if (!tmp) return;
		r->boxes = tmp;
		r->capacity = new_cap;
	}
	r->boxes[r->count++] = *box;
}

/* ── OCR recognition ──────────────────────────────────────────────────────── */

OcrResult ocr_recognize_raw(const uint8_t *pixels, int width, int height,
                            int bytes_per_pixel)
{
	OcrResult result = { .boxes = NULL, .count = 0, .capacity = 0 };

	if (!g_ocr_available || !pixels) return result;

	TessBaseAPI *api = f_Create();
	if (!api) return result;

	if (f_Init(api, NULL, "eng") != 0) {
		f_Delete(api);
		return result;
	}

	f_SetPageSegMode(api, PSM_AUTO);
	f_SetImage(api, pixels, width, height, bytes_per_pixel,
	           width * bytes_per_pixel);

	if (f_Recognize(api, NULL) != 0) {
		f_End(api);
		f_Delete(api);
		return result;
	}

	/* Iterate word-level results */
	TessResultIterator *ri = f_GetIterator(api);
	if (ri) {
		do {
			char *word = f_IterGetText(ri, RIL_WORD);
			if (!word) continue;

			/* Skip empty/whitespace-only */
			const char *p = word;
			while (*p && isspace((unsigned char)*p)) p++;
			if (*p == '\0') {
				f_DeleteText(word);
				continue;
			}

			float conf = f_IterConfidence(ri, RIL_WORD);
			if (conf < 30.0f) {
				f_DeleteText(word);
				continue;
			}

			int x1, y1, x2, y2;
			/* TessPageIterator and TessResultIterator share the same
			 * pointer — cast is safe per the C API design */
			if (f_IterBoundingBox((TessPageIterator *)ri, RIL_WORD,
			                     &x1, &y1, &x2, &y2)) {
				OcrBox box;
				int len = strlen(word);
				if (len >= (int)sizeof(box.text))
					len = (int)sizeof(box.text) - 1;
				memcpy(box.text, word, len);
				box.text[len] = '\0';
				/* Trim trailing whitespace */
				while (len > 0 && isspace((unsigned char)box.text[len - 1]))
					box.text[--len] = '\0';

				box.x = x1;
				box.y = y1;
				box.width = x2 - x1;
				box.height = y2 - y1;
				box.confidence = (int)conf;
				result_push(&result, &box);
			}

			f_DeleteText(word);
		} while (f_IterNext(ri, RIL_WORD));

		f_IterDelete(ri);
	}

	f_End(api);
	f_Delete(api);
	return result;
}

OcrResult ocr_recognize(const uint8_t *png_data, size_t png_len)
{
	/* For PNG input, we need to decode first.
	 * Use a temporary file + tesseract CLI as fallback,
	 * or decode PNG to raw pixels.
	 * For now, write to tmp file and use the raw API after decoding.
	 * TODO: Use libpng to decode in-memory. */
	(void)png_data;
	(void)png_len;
	OcrResult empty = { .boxes = NULL, .count = 0, .capacity = 0 };
	return empty;
}

void ocr_result_free(OcrResult *result)
{
	free(result->boxes);
	result->boxes = NULL;
	result->count = 0;
	result->capacity = 0;
}

/* ── Text search ──────────────────────────────────────────────────────────── */

static int str_contains_ci(const char *haystack, const char *needle)
{
	return strcasestr(haystack, needle) != NULL;
}

/* Strip non-alphanumeric characters from a string for fuzzy matching */
static void strip_nonalnum(const char *src, char *dst, int dst_size)
{
	int j = 0;
	for (int i = 0; src[i] && j < dst_size - 1; i++) {
		if (isalnum((unsigned char)src[i])) {
			dst[j++] = src[i];
		}
	}
	dst[j] = '\0';
}

/* Fuzzy case-insensitive match: strip non-alnum then compare */
static int str_fuzzy_ci(const char *haystack, const char *needle)
{
	char h[256], n[256];
	strip_nonalnum(haystack, h, sizeof(h));
	strip_nonalnum(needle, n, sizeof(n));
	return strcasestr(h, n) != NULL;
}

OcrMatch *ocr_find_text(const OcrResult *result, const char *search_text,
                        int *count)
{
	*count = 0;
	if (!result || result->count == 0 || !search_text) return NULL;

	/* Check if search is single word or multi-word */
	const char *space = strchr(search_text, ' ');

	OcrMatch *matches = NULL;
	int cap = 0;

	if (!space) {
		/* Single word search */
		for (int i = 0; i < result->count; i++) {
			if (str_contains_ci(result->boxes[i].text, search_text)) {
				if (*count >= cap) {
					cap = cap ? cap * 2 : 8;
					OcrMatch *tmp = realloc(matches, cap * sizeof(OcrMatch));
					if (!tmp) break;
					matches = tmp;
				}
				matches[*count] = (OcrMatch){
					.x = result->boxes[i].x,
					.y = result->boxes[i].y,
					.width = result->boxes[i].width,
					.height = result->boxes[i].height,
				};
				(*count)++;
			}
		}
	} else {
		/* Multi-word: tokenize and find consecutive boxes */
		/* Count words */
		char *dup = strdup(search_text);
		if (!dup) return NULL;

		char *words[32];
		int nwords = 0;
		char *tok = strtok(dup, " \t");
		while (tok && nwords < 32) {
			words[nwords++] = tok;
			tok = strtok(NULL, " \t");
		}

		for (int i = 0; i <= result->count - nwords; i++) {
			if (!str_contains_ci(result->boxes[i].text, words[0]))
				continue;

			/* Found first word — find remaining words by scanning
			 * forward for boxes on the same line, to the right.
			 * This handles cases where dedup inserts extra boxes
			 * between the actual word sequence (e.g. "ShowDependencies"
			 * sitting between "Show" and "Dependencies"). */
			int match = 1;
			int last_idx = i;
			for (int j = 1; j < nwords; j++) {
				int found = 0;
				const OcrBox *prev = &result->boxes[last_idx];
				int prev_cy = prev->y + prev->height / 2;
				for (int k = last_idx + 1; k < result->count; k++) {
					const OcrBox *cur = &result->boxes[k];
					int cur_cy = cur->y + cur->height / 2;
					/* Must be on same line (within 20px) and to the right */
					if (abs(cur_cy - prev_cy) > 20) {
						/* Past this line — stop searching */
						if (cur_cy - prev_cy > 20) break;
						continue;
					}
					if (cur->x < prev->x) continue;
					if (str_contains_ci(cur->text, words[j])) {
						last_idx = k;
						found = 1;
						break;
					}
				}
				if (!found) { match = 0; break; }
			}

			if (match) {
				const OcrBox *first = &result->boxes[i];
				const OcrBox *last = &result->boxes[last_idx];
				int x = first->x;
				int y = first->y < last->y ? first->y : last->y;
				int x2 = last->x + last->width;
				int y2_a = first->y + first->height;
				int y2_b = last->y + last->height;
				int y2 = y2_a > y2_b ? y2_a : y2_b;

				if (*count >= cap) {
					cap = cap ? cap * 2 : 8;
					OcrMatch *tmp = realloc(matches, cap * sizeof(OcrMatch));
					if (!tmp) break;
					matches = tmp;
				}
				matches[*count] = (OcrMatch){
					.x = x, .y = y,
					.width = x2 - x, .height = y2 - y,
				};
				(*count)++;
			}
		}

		free(dup);
	}

	/* Fuzzy fallback: if no exact matches found, retry with non-alnum
	 * characters stripped (handles OCR misreading "% CPU" as "eke" etc.) */
	if (*count == 0) {
		/* Build stripped version of search text */
		char stripped_search[256];
		strip_nonalnum(search_text, stripped_search, sizeof(stripped_search));
		if (stripped_search[0] == '\0') return matches;

		if (!strchr(search_text, ' ')) {
			/* Single word fuzzy */
			for (int i = 0; i < result->count; i++) {
				if (str_fuzzy_ci(result->boxes[i].text, stripped_search)) {
					if (*count >= cap) {
						cap = cap ? cap * 2 : 8;
						OcrMatch *tmp = realloc(matches, cap * sizeof(OcrMatch));
						if (!tmp) break;
						matches = tmp;
					}
					matches[*count] = (OcrMatch){
						.x = result->boxes[i].x,
						.y = result->boxes[i].y,
						.width = result->boxes[i].width,
						.height = result->boxes[i].height,
					};
					(*count)++;
				}
			}
		} else {
			/* Multi-word fuzzy: concatenate all search words (stripped),
			 * then search for the concatenation in adjacent boxes */
			char *dup = strdup(search_text);
			if (!dup) return matches;

			char *words[32];
			int nwords = 0;
			char *tok = strtok(dup, " \t");
			while (tok && nwords < 32) {
				words[nwords++] = tok;
				tok = strtok(NULL, " \t");
			}

			/* Try matching consecutive boxes with fuzzy compare */
			for (int i = 0; i <= result->count - nwords; i++) {
				int match = 1;
				for (int j = 0; j < nwords; j++) {
					if (!str_fuzzy_ci(result->boxes[i + j].text, words[j])) {
						match = 0;
						break;
					}
				}
				if (match) {
					const OcrBox *first = &result->boxes[i];
					const OcrBox *last = &result->boxes[i + nwords - 1];
					int x = first->x;
					int y = first->y < last->y ? first->y : last->y;
					int x2 = last->x + last->width;
					int y2_a = first->y + first->height;
					int y2_b = last->y + last->height;
					int y2 = y2_a > y2_b ? y2_a : y2_b;

					if (*count >= cap) {
						cap = cap ? cap * 2 : 8;
						OcrMatch *tmp = realloc(matches, cap * sizeof(OcrMatch));
						if (!tmp) break;
						matches = tmp;
					}
					matches[*count] = (OcrMatch){
						.x = x, .y = y,
						.width = x2 - x, .height = y2 - y,
					};
					(*count)++;
				}
			}

			free(dup);
		}
	}

	return matches;
}
