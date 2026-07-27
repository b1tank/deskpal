/*
 * deskpal — Bounded temporary AT-SPI semantic event listeners
 * SPDX-License-Identifier: MIT
 */
#ifndef DESKPAL_SEMANTIC_EVENTS_H
#define DESKPAL_SEMANTIC_EVENTS_H

#include <stddef.h>

#include "semantic_identity.h"

typedef struct SemanticEventListener SemanticEventListener;
typedef int (*SemanticEventCancelCheck)(void *data);

typedef enum {
	SEMANTIC_EVENT_WAKE = 1,
	SEMANTIC_EVENT_TIMEOUT = 2,
	SEMANTIC_EVENT_CANCELLED = 3,
	SEMANTIC_EVENT_ERROR = -1,
} SemanticEventWaitResult;

typedef struct {
	int registered;
	int deregistered;
	int wait_count;
	int event_count;
	int relevant_event_count;
	int irrelevant_event_count;
	int coalesced_event_count;
	int dropped_event_count;
	int overflow;
} SemanticEventStats;

long long semantic_events_monotonic_ms(void);

/* Begin one listener whose registration remains active across multiple waits.
 * The identity strings are copied. The caller must always call end on success. */
SemanticEventListener *semantic_events_begin(
	const SemanticWindowIdentity *identity,
	char *error, size_t error_len);

/* Wait until one new exact-window event, cancellation, or the absolute
 * monotonic deadline. The listener stays registered after this call. */
SemanticEventWaitResult semantic_events_wait(
	SemanticEventListener *listener,
	long long deadline_ms,
	SemanticEventCancelCheck cancel_check,
	void *cancel_data,
	char *error, size_t error_len);

/* Deregister and release the listener. On deregistration failure returns -1
 * and safely retains callback storage until process exit. */
int semantic_events_end(SemanticEventListener *listener,
                        SemanticEventStats *stats,
                        char *error, size_t error_len);

#endif /* DESKPAL_SEMANTIC_EVENTS_H */
