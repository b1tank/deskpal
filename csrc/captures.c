/*
 * deskpal — Bounded screenshot capture identities
 *
 * Copyright (c) 2026 deskpal contributors
 * SPDX-License-Identifier: MIT
 */
#include "captures.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/random.h>
#include <time.h>
#include <unistd.h>

#define CAPTURE_HISTORY_SIZE 16
#define CAPTURE_MAX_AGE_MS 120000

static DeskpalCapture history[CAPTURE_HISTORY_SIZE];
static unsigned int history_next;
static uint64_t session_nonce;
static uint64_t sequence;

static int64_t monotonic_ms(void)
{
	struct timespec now;
	if (clock_gettime(CLOCK_MONOTONIC, &now) != 0) return 0;
	return (int64_t)now.tv_sec * 1000 + now.tv_nsec / 1000000;
}

static void ensure_nonce(void)
{
	if (session_nonce != 0) return;
	if (getrandom(&session_nonce, sizeof(session_nonce), GRND_NONBLOCK) !=
	    (ssize_t)sizeof(session_nonce) || session_nonce == 0) {
		session_nonce = ((uint64_t)(unsigned int)getpid() << 32) ^
		                (uint64_t)monotonic_ms();
		if (session_nonce == 0) session_nonce = 1;
	}
}

static int store_capture(DeskpalCapture *next, DeskpalCapture *capture)
{
	if (!next || !capture || next->source_width <= 0 || next->source_height <= 0 ||
	    next->image_width <= 0 || next->image_height <= 0)
		return -1;
	ensure_nonce();
	int written = snprintf(next->id, sizeof(next->id), "capture-%016llx-%llu",
	                       (unsigned long long)session_nonce,
	                       (unsigned long long)++sequence);
	if (written < 0 || (size_t)written >= sizeof(next->id)) return -1;
	next->created_monotonic_ms = monotonic_ms();
	free(history[history_next].semantic_snapshot);
	history[history_next] = *next;
	history_next = (history_next + 1) % CAPTURE_HISTORY_SIZE;
	*capture = *next;
	return 0;
}

int captures_store_desktop(int source_width, int source_height,
                           int image_width, int image_height,
                           DeskpalCapture *capture)
{
	DeskpalCapture next = {
		.target = DESKPAL_CAPTURE_DESKTOP,
		.source_width = source_width,
		.source_height = source_height,
		.image_width = image_width,
		.image_height = image_height,
	};
	return store_capture(&next, capture);
}

int captures_store_window(unsigned long window_id, long process_id,
                          const char *title, const char *app_class,
                          int window_x, int window_y,
                          int window_width, int window_height,
                          int source_width, int source_height,
                          int image_width, int image_height,
                          const char *semantic_revision,
                          const SemanticWindowIdentity *semantic_window,
                          const char *semantic_snapshot,
                          int semantic_complete,
                          int semantic_max_depth,
                          int semantic_max_nodes,
                          int semantic_include_offscreen,
                          DeskpalCapture *capture)
{
	if (window_id == 0 || process_id <= 0 || !title || !title[0] ||
	    !app_class || !app_class[0] || window_width <= 0 || window_height <= 0 ||
	    !semantic_revision || !semantic_snapshot || semantic_max_depth < 1 ||
	    semantic_max_depth > 8 || semantic_max_nodes < 1 ||
	    semantic_max_nodes > 300)
		return -1;
	DeskpalCapture next = {
		.target = DESKPAL_CAPTURE_WINDOW,
		.window_id = window_id,
		.process_id = process_id,
		.window_x = window_x,
		.window_y = window_y,
		.window_width = window_width,
		.window_height = window_height,
		.source_width = source_width,
		.source_height = source_height,
		.image_width = image_width,
		.image_height = image_height,
	};
	int title_written = snprintf(next.title, sizeof(next.title), "%s", title);
	int class_written = snprintf(next.app_class, sizeof(next.app_class), "%s", app_class);
	int revision_written = snprintf(next.semantic_revision,
		sizeof(next.semantic_revision), "%s", semantic_revision);
	int bus_written = snprintf(next.semantic_window.bus_name,
		sizeof(next.semantic_window.bus_name), "%s",
		semantic_window ? semantic_window->bus_name : "");
	int path_written = snprintf(next.semantic_window.window_object_path,
		sizeof(next.semantic_window.window_object_path), "%s",
		semantic_window ? semantic_window->window_object_path : "");
	next.semantic_window.process_id = semantic_window
		? semantic_window->process_id : 0;
	char *snapshot_copy = strdup(semantic_snapshot);
	if (title_written < 0 || (size_t)title_written >= sizeof(next.title) ||
	    class_written < 0 || (size_t)class_written >= sizeof(next.app_class) ||
	    revision_written < 0 ||
	    (size_t)revision_written >= sizeof(next.semantic_revision) ||
	    bus_written < 0 ||
	    (size_t)bus_written >= sizeof(next.semantic_window.bus_name) ||
	    path_written < 0 ||
	    (size_t)path_written >=
	        sizeof(next.semantic_window.window_object_path) ||
	    !snapshot_copy) {
		free(snapshot_copy);
		return -1;
	}
	next.semantic_snapshot = snapshot_copy;
	next.semantic_complete = semantic_complete;
	next.semantic_max_depth = semantic_max_depth;
	next.semantic_max_nodes = semantic_max_nodes;
	next.semantic_include_offscreen = semantic_include_offscreen;
	if (store_capture(&next, capture) != 0) {
		free(snapshot_copy);
		return -1;
	}
	return 0;
}

int captures_lookup(const char *id, DeskpalCapture *capture)
{
	if (!id || !id[0] || !capture) return -1;
	for (unsigned int i = 0; i < CAPTURE_HISTORY_SIZE; i++) {
		if (history[i].id[0] && strcmp(history[i].id, id) == 0) {
			*capture = history[i];
			if (monotonic_ms() - history[i].created_monotonic_ms >
			    CAPTURE_MAX_AGE_MS)
				return -2;
			return 0;
		}
	}
	return -1;
}

void captures_cleanup(void)
{
	for (unsigned int i = 0; i < CAPTURE_HISTORY_SIZE; i++)
		free(history[i].semantic_snapshot);
	memset(history, 0, sizeof(history));
	history_next = 0;
	session_nonce = 0;
	sequence = 0;
}
