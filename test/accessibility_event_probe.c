/* Bounded integration probe for Deskpal's temporary AT-SPI listener lifecycle. */
#include "accessibility.h"
#include "semantic_change.h"
#include "semantic_state.h"
#include "x11.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef HAVE_ATSPI
#include <glib.h>
#endif

static int cancellation_requested(void *data)
{
	return data && *(int *)data;
}

static void report_ready(void *data)
{
	(void)data;
	puts("READY");
	fflush(stdout);
}

#ifdef HAVE_ATSPI
static gboolean request_cancellation(gpointer data)
{
	*(int *)data = 1;
	return G_SOURCE_REMOVE;
}
#endif

typedef struct {
	const char *application;
	const char *window;
	WindowInfo expected_window;
} ProbeContext;

static int build_snapshot(const char *application, const char *window,
                          SemanticStateSnapshot *snapshot)
{
	cJSON *tree = accessibility_tree_exact(
		application, window, 4, 120, 0, 0, 0);
	if (!tree) return -1;
	int rc = semantic_state_build(tree, snapshot);
	cJSON_Delete(tree);
	return rc;
}

static int validate_target(const DeskpalCapture *base, void *data,
                           char *error, size_t error_len)
{
	ProbeContext *context = data;
	WindowInfo current;
	if (x11_get_window_info(base->window_id, &current) == 0 &&
	    current.viewable && current.id == context->expected_window.id &&
	    current.pid == context->expected_window.pid &&
	    strcmp(current.title, context->expected_window.title) == 0 &&
	    strcmp(current.app_class, context->expected_window.app_class) == 0 &&
	    current.x == context->expected_window.x &&
	    current.y == context->expected_window.y &&
	    current.width == context->expected_window.width &&
	    current.height == context->expected_window.height)
		return 0;
	snprintf(error, error_len,
	         "Exact X11 window identity or geometry changed");
	return -1;
}

static int observe_target(const DeskpalCapture *base, void *data,
                          SemanticStateSnapshot *snapshot,
                          char *error, size_t error_len)
{
	(void)base;
	ProbeContext *context = data;
	if (build_snapshot(context->application, context->window, snapshot) == 0)
		return 0;
	snprintf(error, error_len, "Could not rebuild probe semantic snapshot");
	return -1;
}

int main(int argc, char **argv)
{
	if (argc < 8 || argc > 9) return 2;
	char *end = NULL;
	unsigned long parsed_pid = strtoul(argv[1], &end, 10);
	if (!end || *end || parsed_pid == 0 || parsed_pid > 0xffffffffUL) return 2;
	end = NULL;
	if (!argv[2][0] || strlen(argv[2]) > 255 ||
	    !argv[3][0] || strlen(argv[3]) > 1024 ||
	    !argv[4][0] || strlen(argv[4]) > 512 ||
	    !argv[5][0] || strlen(argv[5]) > 512)
		return 2;
	long parsed_timeout = strtol(argv[6], &end, 10);
	if (!end || *end || parsed_timeout < 1 || parsed_timeout > 5000) return 2;
	end = NULL;
	unsigned long window_id = strtoul(argv[7], &end, 0);
	if (!end || *end || window_id == 0) return 2;
	int cancel = 0;
#ifdef HAVE_ATSPI
	if (argc == 9) {
		end = NULL;
		long cancel_after = strtol(argv[8], &end, 10);
		if (!end || *end || cancel_after < 1 || cancel_after > parsed_timeout)
			return 2;
		g_timeout_add((guint)cancel_after, request_cancellation, &cancel);
	}
#else
	(void)cancel;
#endif
	if (x11_init(0) != 0) return 3;
	if (accessibility_init() != 0) {
		x11_cleanup();
		return 3;
	}
	SemanticStateSnapshot before = {0};
	if (build_snapshot(argv[4], argv[5], &before) != 0) {
		accessibility_cleanup();
		x11_cleanup();
		return 4;
	}
	int identity_captured =
		before.window_identity.process_id == (unsigned int)parsed_pid &&
		strcmp(before.window_identity.bus_name, argv[2]) == 0 &&
		strcmp(before.window_identity.window_object_path, argv[3]) == 0;
	ProbeContext context = {
		.application = argv[4],
		.window = argv[5],
	};
	if (x11_get_window_info(window_id, &context.expected_window) != 0) {
		semantic_state_clear(&before);
		accessibility_cleanup();
		x11_cleanup();
		return 4;
	}
	DeskpalCapture base = {
		.target = DESKPAL_CAPTURE_WINDOW,
		.window_id = window_id,
		.process_id = context.expected_window.pid,
		.window_x = context.expected_window.x,
		.window_y = context.expected_window.y,
		.window_width = context.expected_window.width,
		.window_height = context.expected_window.height,
		.semantic_window = {
			.process_id = (unsigned int)parsed_pid,
		},
		.semantic_snapshot = before.json,
		.semantic_complete = before.complete,
		.semantic_max_depth = 4,
		.semantic_max_nodes = 120,
	};
	snprintf(base.id, sizeof(base.id), "probe-capture");
	snprintf(base.semantic_revision, sizeof(base.semantic_revision), "%s",
	         before.revision);
	snprintf(base.semantic_window.bus_name,
	         sizeof(base.semantic_window.bus_name), "%s", argv[2]);
	snprintf(base.semantic_window.window_object_path,
	         sizeof(base.semantic_window.window_object_path), "%s", argv[3]);
	char error[256] = {0};
	SemanticEventListener *listener = semantic_events_begin(
		&base.semantic_window, error, sizeof(error));
	if (!listener) {
		semantic_state_clear(&before);
		accessibility_cleanup();
		x11_cleanup();
		return 4;
	}
	report_ready(NULL);
	SemanticChangeResult change = {0};
	int rc = semantic_change_wait_registered(
		&base, listener, semantic_events_monotonic_ms() + parsed_timeout,
		validate_target, observe_target, &context,
		argc == 9 ? cancellation_requested : NULL, &cancel, &change);
	SemanticEventStats result = {0};
	if (semantic_events_end(listener, &result, error, sizeof(error)) != 0)
		rc = -1;
	int revision_changed = change.status == SEMANTIC_CHANGE_CHANGED;
	const cJSON *diff_changed = change.diff
		? cJSON_GetObjectItem(change.diff, "changed") : NULL;
	const cJSON *diff_comparable = change.diff
		? cJSON_GetObjectItem(change.diff, "comparable") : NULL;
	printf("{\"rc\":%d,\"registered\":%s,\"deregistered\":%s,"
	       "\"waitCount\":%d,\"eventCount\":%d,"
	       "\"relevantEventCount\":%d,"
	       "\"irrelevantEventCount\":%d,\"coalescedEventCount\":%d,"
	       "\"droppedEventCount\":%d,\"overflow\":%s,"
	       "\"timedOut\":%s,\"cancelled\":%s,"
	       "\"revisionChanged\":%s,\"identityCaptured\":%s,"
	       "\"diffChanged\":%s,\"diffComparable\":%s,"
	       "\"targetInvalid\":%s}\n",
	       rc, result.registered ? "true" : "false",
	       result.deregistered ? "true" : "false", result.wait_count,
	       result.event_count, result.relevant_event_count,
	       result.irrelevant_event_count,
	       result.coalesced_event_count, result.dropped_event_count,
	       result.overflow ? "true" : "false",
	       change.status == SEMANTIC_CHANGE_TIMEOUT ? "true" : "false",
	       change.status == SEMANTIC_CHANGE_CANCELLED ? "true" : "false",
	       revision_changed ? "true" : "false",
	       identity_captured ? "true" : "false",
	       cJSON_IsTrue(diff_changed) ? "true" : "false",
	       cJSON_IsTrue(diff_comparable) ? "true" : "false",
	       change.status == SEMANTIC_CHANGE_TARGET_INVALID
	           ? "true" : "false");
	semantic_change_result_clear(&change);
	semantic_state_clear(&before);
	accessibility_cleanup();
	x11_cleanup();
	return rc >= 0 ? 0 : 4;
}
