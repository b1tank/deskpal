/*
 * deskpal — Capture-bound semantic change orchestration
 * SPDX-License-Identifier: MIT
 */
#include "semantic_change.h"

#include <stdio.h>
#include <string.h>

static void set_result_error(SemanticChangeResult *result, const char *message)
{
	snprintf(result->error, sizeof(result->error), "%s",
	         message && message[0] ? message : "Semantic change wait failed");
}

static int same_semantic_window(const SemanticWindowIdentity *left,
                                const SemanticWindowIdentity *right)
{
	return left->process_id == right->process_id &&
	       strcmp(left->bus_name, right->bus_name) == 0 &&
	       strcmp(left->window_object_path, right->window_object_path) == 0;
}

static int valid_request(const DeskpalCapture *base, long long deadline_ms,
                         SemanticChangeValidateTarget validate,
                         SemanticChangeObserveTarget observe)
{
	return base && base->target == DESKPAL_CAPTURE_WINDOW &&
	       base->semantic_snapshot && base->semantic_window.process_id &&
	       base->semantic_window.bus_name[0] &&
	       base->semantic_window.window_object_path[0] && validate && observe &&
	       deadline_ms > semantic_events_monotonic_ms();
}

static int validate_base(const DeskpalCapture *base,
                         SemanticChangeValidateTarget validate,
                         void *callback_data,
                         SemanticChangeResult *result)
{
	char error[256] = {0};
	if (validate(base, callback_data, error, sizeof(error)) == 0)
		return 0;
	result->status = SEMANTIC_CHANGE_TARGET_INVALID;
	set_result_error(result, error);
	return -1;
}

int semantic_change_wait_registered(
	const DeskpalCapture *base,
	SemanticEventListener *listener,
	long long deadline_ms,
	SemanticChangeValidateTarget validate,
	SemanticChangeObserveTarget observe,
	void *callback_data,
	SemanticEventCancelCheck cancel_check,
	void *cancel_data,
	SemanticChangeResult *result)
{
	if (!result) return -1;
	memset(result, 0, sizeof(*result));
	if (!listener || !valid_request(base, deadline_ms, validate, observe)) {
		result->status = SEMANTIC_CHANGE_TARGET_INVALID;
		set_result_error(result, "Semantic change request is incomplete");
		return -1;
	}

	if (validate_base(base, validate, callback_data, result) != 0)
		return -1;
	char callback_error[256] = {0};

	for (;;) {
		SemanticEventWaitResult wait = semantic_events_wait(
			listener, deadline_ms, cancel_check, cancel_data,
			result->error, sizeof(result->error));
		if (wait == SEMANTIC_EVENT_TIMEOUT) {
			if (validate_base(base, validate, callback_data, result) == 0)
				result->status = SEMANTIC_CHANGE_TIMEOUT;
			break;
		}
		if (wait == SEMANTIC_EVENT_CANCELLED) {
			result->status = SEMANTIC_CHANGE_CANCELLED;
			break;
		}
		if (wait != SEMANTIC_EVENT_WAKE) {
			result->status = SEMANTIC_CHANGE_LISTENER_FAILED;
			break;
		}

		if (validate_base(base, validate, callback_data, result) != 0)
			break;

		semantic_state_clear(&result->current);
		callback_error[0] = '\0';
		if (observe(base, callback_data, &result->current,
		            callback_error, sizeof(callback_error)) != 0) {
			result->status = SEMANTIC_CHANGE_OBSERVATION_FAILED;
			set_result_error(result, callback_error);
			break;
		}
		if (!same_semantic_window(&base->semantic_window,
		                          &result->current.window_identity)) {
			result->status = SEMANTIC_CHANGE_TARGET_INVALID;
			set_result_error(result,
				"Accessible window identity changed during semantic wait");
			break;
		}

		if (validate_base(base, validate, callback_data, result) != 0)
			break;
		if (strcmp(base->semantic_revision,
		           result->current.revision) == 0)
			continue;

		result->diff = semantic_state_diff(base, &result->current);
		if (!result->diff) {
			result->status = SEMANTIC_CHANGE_OBSERVATION_FAILED;
			set_result_error(result, "Could not build semantic change diff");
			break;
		}
		cJSON_AddStringToObject(result->diff, "baseRevision",
		                      base->semantic_revision);
		cJSON_AddStringToObject(result->diff, "currentRevision",
		                      result->current.revision);
		result->status = SEMANTIC_CHANGE_CHANGED;
		break;
	}
	return result->status == SEMANTIC_CHANGE_CHANGED ? 1 :
	       result->status == SEMANTIC_CHANGE_TIMEOUT ||
	       result->status == SEMANTIC_CHANGE_CANCELLED ? 0 : -1;
}

int semantic_change_wait(
	const DeskpalCapture *base,
	long long deadline_ms,
	SemanticChangeValidateTarget validate,
	SemanticChangeObserveTarget observe,
	void *callback_data,
	SemanticEventCancelCheck cancel_check,
	void *cancel_data,
	SemanticChangeResult *result)
{
	if (!result) return -1;
	memset(result, 0, sizeof(*result));
	if (!valid_request(base, deadline_ms, validate, observe)) {
		result->status = SEMANTIC_CHANGE_TARGET_INVALID;
		set_result_error(result, "Semantic change base capture is incomplete");
		return -1;
	}
	SemanticEventListener *listener = semantic_events_begin(
		&base->semantic_window, result->error, sizeof(result->error));
	if (!listener) {
		result->status = SEMANTIC_CHANGE_LISTENER_FAILED;
		return -1;
	}
	int rc = semantic_change_wait_registered(
		base, listener, deadline_ms, validate, observe, callback_data,
		cancel_check, cancel_data, result);
	char cleanup_error[256] = {0};
	if (semantic_events_end(listener, &result->events,
	                        cleanup_error, sizeof(cleanup_error)) != 0) {
		result->status = SEMANTIC_CHANGE_CLEANUP_FAILED;
		set_result_error(result, cleanup_error);
		return -1;
	}
	return rc;
}

void semantic_change_result_clear(SemanticChangeResult *result)
{
	if (!result) return;
	semantic_state_clear(&result->current);
	cJSON_Delete(result->diff);
	memset(result, 0, sizeof(*result));
}
