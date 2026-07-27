/*
 * deskpal — Capture-bound semantic change orchestration
 * SPDX-License-Identifier: MIT
 */
#ifndef DESKPAL_SEMANTIC_CHANGE_H
#define DESKPAL_SEMANTIC_CHANGE_H

#include <stddef.h>

#include "semantic_events.h"
#include "semantic_state.h"

typedef int (*SemanticChangeValidateTarget)(
	const DeskpalCapture *base, void *data, char *error, size_t error_len);
typedef int (*SemanticChangeObserveTarget)(
	const DeskpalCapture *base, void *data, SemanticStateSnapshot *snapshot,
	char *error, size_t error_len);

typedef enum {
	SEMANTIC_CHANGE_CHANGED = 1,
	SEMANTIC_CHANGE_TIMEOUT = 2,
	SEMANTIC_CHANGE_CANCELLED = 3,
	SEMANTIC_CHANGE_TARGET_INVALID = -1,
	SEMANTIC_CHANGE_OBSERVATION_FAILED = -2,
	SEMANTIC_CHANGE_LISTENER_FAILED = -3,
	SEMANTIC_CHANGE_CLEANUP_FAILED = -4,
} SemanticChangeStatus;

typedef struct {
	SemanticChangeStatus status;
	SemanticEventStats events;
	SemanticStateSnapshot current;
	cJSON *diff;
	char error[256];
} SemanticChangeResult;

/* Run with an already-registered listener. This exists for deterministic
 * integration setup; the caller retains cleanup ownership. */
int semantic_change_wait_registered(
	const DeskpalCapture *base,
	SemanticEventListener *listener,
	long long deadline_ms,
	SemanticChangeValidateTarget validate,
	SemanticChangeObserveTarget observe,
	void *callback_data,
	SemanticEventCancelCheck cancel_check,
	void *cancel_data,
	SemanticChangeResult *result);

/* Register, wait, and clean up. Every wakeup is followed by exact target
 * validation and canonical re-observation. Unchanged revisions keep waiting
 * under the original absolute deadline. */
int semantic_change_wait(
	const DeskpalCapture *base,
	long long deadline_ms,
	SemanticChangeValidateTarget validate,
	SemanticChangeObserveTarget observe,
	void *callback_data,
	SemanticEventCancelCheck cancel_check,
	void *cancel_data,
	SemanticChangeResult *result);

void semantic_change_result_clear(SemanticChangeResult *result);

#endif /* DESKPAL_SEMANTIC_CHANGE_H */
