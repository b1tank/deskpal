/*
 * deskpal — Bounded temporary AT-SPI semantic event listeners
 * SPDX-License-Identifier: MIT
 */
#include "semantic_events.h"

#include "accessibility.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#ifdef HAVE_ATSPI
#include <atspi/atspi.h>
#endif

#define SEMANTIC_EVENT_LIMIT 256
#define SEMANTIC_EVENT_KEY_LIMIT 64
#define SEMANTIC_EVENT_PARENT_LIMIT 32

struct SemanticEventListener {
	SemanticWindowIdentity identity;
	SemanticEventStats stats;
	long long active_deadline_ms;
	int observed_relevant_events;
#ifdef HAVE_ATSPI
	AtspiEventListener *atspi_listener;
	uint64_t keys[SEMANTIC_EVENT_KEY_LIMIT];
	int key_count;
#endif
};

long long semantic_events_monotonic_ms(void)
{
	struct timespec ts;
	if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0) return 0;
	return (long long)ts.tv_sec * 1000LL + ts.tv_nsec / 1000000LL;
}

static void set_error(char *error, size_t error_len, const char *message)
{
	if (error && error_len)
		snprintf(error, error_len, "%s", message ? message : "Semantic event error");
}

static int valid_identity(const SemanticWindowIdentity *identity)
{
	return identity && identity->process_id > 0 && identity->bus_name[0] &&
	       identity->window_object_path[0] &&
	       strnlen(identity->bus_name, sizeof(identity->bus_name)) <
	           sizeof(identity->bus_name) &&
	       strnlen(identity->window_object_path,
	               sizeof(identity->window_object_path)) <
	           sizeof(identity->window_object_path);
}

#ifdef HAVE_ATSPI

/* libatspi timeout policy is process-global. Deskpal's MCP dispatch and AT-SPI
 * use are synchronous today, so callbacks may temporarily narrow it. Keep this
 * policy isolated here; concurrent AT-SPI use requires a different design. */
#define EVENT_CALL_TIMEOUT_MS 50
#define DESKPAL_ATSPI_CALL_TIMEOUT_MS 750
#define DESKPAL_ATSPI_STARTUP_TIMEOUT_MS 1000

static void restore_atspi_timeout(void)
{
	atspi_set_timeout(DESKPAL_ATSPI_CALL_TIMEOUT_MS,
	                  DESKPAL_ATSPI_STARTUP_TIMEOUT_MS);
}

static uint64_t bounded_event_key(const AtspiEvent *event)
{
	uint64_t hash = UINT64_C(1469598103934665603);
	const char *parts[] = {
		event && event->type ? event->type : "",
		event && event->source && ATSPI_OBJECT(event->source)->path
			? ATSPI_OBJECT(event->source)->path : "",
	};
	const size_t limits[] = {128, 1024};
	for (size_t i = 0; i < sizeof(parts) / sizeof(parts[0]); i++) {
		for (size_t j = 0; parts[i][j] && j < limits[i]; j++) {
			hash ^= (unsigned char)parts[i][j];
			hash *= UINT64_C(1099511628211);
		}
		hash ^= UINT64_C(0xff);
		hash *= UINT64_C(1099511628211);
	}
	return hash;
}

static int source_in_window(AtspiAccessible *source,
                            SemanticEventListener *listener,
                            GError **error)
{
	AtspiObject *source_object = source ? ATSPI_OBJECT(source) : NULL;
	if (!source_object || !source_object->app ||
	    !source_object->app->bus_name ||
	    strcmp(source_object->app->bus_name,
	           listener->identity.bus_name) != 0)
		return 0;

	AtspiAccessible *current = g_object_ref(source);
	for (int depth = 0; current && depth <= SEMANTIC_EVENT_PARENT_LIMIT; depth++) {
		AtspiObject *object = ATSPI_OBJECT(current);
		if (object->path && strcmp(object->path,
		    listener->identity.window_object_path) == 0) {
			g_object_unref(current);
			return 1;
		}
		if (depth == SEMANTIC_EVENT_PARENT_LIMIT) break;
		long long remaining = listener->active_deadline_ms -
		                      semantic_events_monotonic_ms();
		if (remaining <= 0) {
			g_object_unref(current);
			return -1;
		}
		int timeout_ms = remaining < 10 ? (int)remaining : 10;
		if (timeout_ms < 1) timeout_ms = 1;
		atspi_set_timeout(timeout_ms, timeout_ms);
		AtspiAccessible *parent = atspi_accessible_get_parent(current, error);
		g_object_unref(current);
		current = parent;
		if (*error) {
			if (current) g_object_unref(current);
			return -1;
		}
	}
	if (current) g_object_unref(current);
	return 0;
}

static void receive_event(AtspiEvent *event, void *user_data)
{
	SemanticEventListener *listener = user_data;
	SemanticEventStats *stats = &listener->stats;
	if (stats->event_count >= SEMANTIC_EVENT_LIMIT) {
		stats->dropped_event_count++;
		stats->overflow = 1;
		return;
	}
	stats->event_count++;
	if (!event || !event->source || listener->active_deadline_ms <= 0) {
		stats->irrelevant_event_count++;
		return;
	}
	long long remaining = listener->active_deadline_ms -
	                      semantic_events_monotonic_ms();
	if (remaining <= 0) {
		stats->dropped_event_count++;
		return;
	}
	int timeout_ms = remaining < EVENT_CALL_TIMEOUT_MS
		? (int)remaining : EVENT_CALL_TIMEOUT_MS;
	if (timeout_ms < 1) timeout_ms = 1;
	atspi_set_timeout(timeout_ms, timeout_ms);
	GError *error = NULL;
	unsigned int process_id = atspi_accessible_get_process_id(
		event->source, &error);
	int in_window = 0;
	if (!error && process_id == listener->identity.process_id)
		in_window = source_in_window(event->source, listener, &error);
	restore_atspi_timeout();
	if (error || in_window < 0) {
		stats->dropped_event_count++;
		if (error) g_error_free(error);
		return;
	}
	if (process_id != listener->identity.process_id || !in_window) {
		stats->irrelevant_event_count++;
		return;
	}
	stats->relevant_event_count++;
	uint64_t key = bounded_event_key(event);
	for (int i = 0; i < listener->key_count; i++) {
		if (listener->keys[i] == key) {
			stats->coalesced_event_count++;
			return;
		}
	}
	if (listener->key_count < SEMANTIC_EVENT_KEY_LIMIT)
		listener->keys[listener->key_count++] = key;
	else {
		stats->dropped_event_count++;
		stats->overflow = 1;
	}
}

#endif /* HAVE_ATSPI */

SemanticEventListener *semantic_events_begin(
	const SemanticWindowIdentity *identity,
	char *error, size_t error_len)
{
	if (!valid_identity(identity)) {
		set_error(error, error_len, "Semantic event target identity is incomplete");
		return NULL;
	}
	if (!accessibility_available()) {
		set_error(error, error_len, "AT-SPI accessibility backend is unavailable");
		return NULL;
	}
#ifndef HAVE_ATSPI
	set_error(error, error_len, "AT-SPI accessibility support was not compiled");
	return NULL;
#else
	SemanticEventListener *listener = calloc(1, sizeof(*listener));
	if (!listener) {
		set_error(error, error_len, "Could not allocate semantic event listener");
		return NULL;
	}
	listener->identity = *identity;
	listener->atspi_listener = atspi_event_listener_new(
		receive_event, listener, NULL);
	if (!listener->atspi_listener) {
		set_error(error, error_len, "Could not create AT-SPI event listener");
		free(listener);
		return NULL;
	}
	GError *atspi_error = NULL;
	if (!atspi_event_listener_register(
	    listener->atspi_listener, "object", &atspi_error)) {
		set_error(error, error_len,
			atspi_error && atspi_error->message ? atspi_error->message :
			"Could not register AT-SPI event listener");
		if (atspi_error) g_error_free(atspi_error);
		g_object_unref(listener->atspi_listener);
		free(listener);
		return NULL;
	}
	listener->stats.registered = 1;
	return listener;
#endif
}

SemanticEventWaitResult semantic_events_wait(
	SemanticEventListener *listener,
	long long deadline_ms,
	SemanticEventCancelCheck cancel_check,
	void *cancel_data,
	char *error, size_t error_len)
{
	if (!listener || !listener->stats.registered ||
	    deadline_ms <= semantic_events_monotonic_ms()) {
		set_error(error, error_len, "Semantic event wait deadline is invalid");
		return SEMANTIC_EVENT_ERROR;
	}
#ifndef HAVE_ATSPI
	(void)cancel_check;
	(void)cancel_data;
	set_error(error, error_len, "AT-SPI accessibility support was not compiled");
	return SEMANTIC_EVENT_ERROR;
#else
	listener->stats.wait_count++;
	listener->active_deadline_ms = deadline_ms;
	if (cancel_check && cancel_check(cancel_data)) {
		listener->active_deadline_ms = 0;
		return SEMANTIC_EVENT_CANCELLED;
	}
	int baseline = listener->observed_relevant_events;
	while (listener->stats.relevant_event_count == baseline) {
		if (listener->stats.overflow) {
			listener->active_deadline_ms = 0;
			set_error(error, error_len,
			          "Semantic event accounting overflowed its safety bound");
			return SEMANTIC_EVENT_ERROR;
		}
		if (cancel_check && cancel_check(cancel_data)) {
			listener->active_deadline_ms = 0;
			return SEMANTIC_EVENT_CANCELLED;
		}
		long long remaining = deadline_ms - semantic_events_monotonic_ms();
		if (remaining <= 0) {
			listener->active_deadline_ms = 0;
			return SEMANTIC_EVENT_TIMEOUT;
		}
		int dispatched = g_main_context_iteration(NULL, FALSE);
		if (!dispatched) {
			int sleep_ms = remaining < 2 ? (int)remaining : 2;
			if (sleep_ms > 0) usleep((useconds_t)sleep_ms * 1000U);
		}
	}
	listener->observed_relevant_events =
		listener->stats.relevant_event_count;
	/* Keep the deadline active while the caller re-observes. Events dispatched
	 * by synchronous AT-SPI work are then retained for the next wait rather
	 * than falling into a wake/re-observe gap. */
	return SEMANTIC_EVENT_WAKE;
#endif
}

int semantic_events_end(SemanticEventListener *listener,
                        SemanticEventStats *stats,
                        char *error, size_t error_len)
{
	if (!listener) return 0;
	int rc = 0;
#ifndef HAVE_ATSPI
	(void)error;
	(void)error_len;
#else
	listener->active_deadline_ms = semantic_events_monotonic_ms() + 10;
	for (int drained = 0; drained < 4 &&
	     g_main_context_iteration(NULL, FALSE); drained++) {
		/* Drain a small bounded batch queued before listener removal. */
	}
	listener->active_deadline_ms = 0;
	GError *atspi_error = NULL;
	if (listener->stats.registered && !atspi_event_listener_deregister(
	    listener->atspi_listener, "object", &atspi_error)) {
		set_error(error, error_len,
			atspi_error && atspi_error->message ? atspi_error->message :
			"Could not deregister AT-SPI event listener");
		rc = -1;
	} else if (listener->stats.registered) {
		listener->stats.deregistered = 1;
	}
	if (atspi_error) g_error_free(atspi_error);
	if (rc == 0 && listener->atspi_listener)
		g_object_unref(listener->atspi_listener);
#endif
	if (stats) *stats = listener->stats;
	if (rc == 0) free(listener);
	/* On deregistration failure the registry may still call back with this
	 * user_data. Retain the listener until process exit rather than risking a
	 * use-after-free. The public wait route must report cleanup failure. */
	return rc;
}
