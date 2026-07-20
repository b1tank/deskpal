/*
 * deskpal — Optional AT-SPI accessibility backend
 *
 * Copyright (c) 2026 deskpal contributors
 * SPDX-License-Identifier: MIT
 */
#include "accessibility.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <time.h>

#ifdef HAVE_ATSPI
#include <atspi/atspi.h>
#endif

static int g_accessibility_available = 0;
static int g_query_error_count = 0;
static char g_last_query_error[256];

#define ACCESSIBILITY_NODE_PAYLOAD_LIMIT (2 * 1024 * 1024)

#ifdef HAVE_ATSPI

#define ACCESSIBILITY_TEXT_LIMIT 2048
#define ACCESSIBILITY_NAME_LIMIT 512
#define ACCESSIBILITY_ACTION_LIMIT 16
#define ACCESSIBILITY_ACTION_NAME_LIMIT 128
#define ACCESSIBILITY_ATTRIBUTE_LIMIT 8
#define ACCESSIBILITY_ATTRIBUTE_KEY_LIMIT 128
#define ACCESSIBILITY_ATTRIBUTE_VALUE_LIMIT 256
#define ACCESSIBILITY_QUERY_TIMEOUT_MS 2000
#define ACCESSIBILITY_CALL_TIMEOUT_MS 750
#define ACCESSIBILITY_STARTUP_TIMEOUT_MS 1000
#define ACCESSIBILITY_VISITED_LIMIT 5000

static long long g_query_deadline_ms = 0;
static int g_query_timed_out = 0;

typedef struct {
	const char *application_filter;
	const char *window_filter;
	int max_depth;
	int max_nodes;
	int include_offscreen;
	int include_text;
	int include_attributes;
	int node_count;
	int visited_nodes;
	int max_visited;
	int matched_applications;
	int matched_windows;
	int actionable_nodes;
	int truncated;
	int incomplete;
	int output_budget_exhausted;
	int node_limit_exhausted;
	long long deadline_ms;
	size_t serialized_node_bytes;
	size_t max_serialized_node_bytes;
	cJSON *applications;
} AccessibilityTraversal;

typedef struct {
	const char *application_filter;
	const char *window_filter;
	AtspiAccessible *match;
	char application[256];
	char window[512];
	int path[32];
	int path_len;
	int match_count;
	int visited;
	int incomplete;
	long long deadline_ms;
} FocusSearch;

static long long monotonic_ms(void)
{
	struct timespec ts;
	if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0) return 0;
	return (long long)ts.tv_sec * 1000LL + ts.tv_nsec / 1000000LL;
}

static void reset_query_errors(void)
{
	g_query_error_count = 0;
	g_last_query_error[0] = '\0';
	g_query_deadline_ms = 0;
	g_query_timed_out = 0;
}

#else

static void reset_query_errors(void)
{
	g_query_error_count = 0;
	g_last_query_error[0] = '\0';
}

static void end_query(void)
{
}

#endif /* HAVE_ATSPI */

#ifdef HAVE_ATSPI
static void begin_query(long long deadline_ms)
{
	g_query_deadline_ms = deadline_ms;
	g_query_timed_out = 0;
}

static void end_query(void)
{
	g_query_deadline_ms = 0;
	atspi_set_timeout(ACCESSIBILITY_CALL_TIMEOUT_MS,
		ACCESSIBILITY_STARTUP_TIMEOUT_MS);
}

static int prepare_atspi_call(void)
{
	int timeout_ms = ACCESSIBILITY_CALL_TIMEOUT_MS;
	if (g_query_deadline_ms > 0) {
		long long remaining = g_query_deadline_ms - monotonic_ms();
		if (remaining <= 0) {
			if (!g_query_timed_out) {
				g_query_timed_out = 1;
				g_query_error_count++;
				snprintf(g_last_query_error, sizeof(g_last_query_error),
					"AT-SPI query deadline exceeded");
			}
			return 0;
		}
		if (remaining < timeout_ms) timeout_ms = (int)remaining;
	}
	if (timeout_ms < 1) timeout_ms = 1;
	atspi_set_timeout(timeout_ms, timeout_ms);
	return 1;
}

static int contains_case_insensitive(const char *value, const char *filter)
{
	if (!filter || !filter[0]) return 1;
	if (!value) return 0;
	return strcasestr(value, filter) != NULL;
}

static void clear_error(GError **error)
{
	if (*error) {
		g_query_error_count++;
		snprintf(g_last_query_error, sizeof(g_last_query_error), "%s",
			(*error)->message ? (*error)->message : "AT-SPI query error");
		g_error_free(*error);
		*error = NULL;
	}
}

static char *accessible_name(AtspiAccessible *node)
{
	if (!prepare_atspi_call()) return NULL;
	GError *error = NULL;
	char *name = atspi_accessible_get_name(node, &error);
	clear_error(&error);
	return name;
}

static char *accessible_role(AtspiAccessible *node)
{
	if (!prepare_atspi_call()) return NULL;
	GError *error = NULL;
	char *role = atspi_accessible_get_role_name(node, &error);
	clear_error(&error);
	return role;
}

static int accessible_child_count(AtspiAccessible *node)
{
	if (!prepare_atspi_call()) return 0;
	GError *error = NULL;
	int count = atspi_accessible_get_child_count(node, &error);
	if (error) {
		clear_error(&error);
		return 0;
	}
	return count > 0 ? count : 0;
}

static AtspiAccessible *accessible_child(AtspiAccessible *node, int index)
{
	if (!prepare_atspi_call()) return NULL;
	GError *error = NULL;
	AtspiAccessible *child = atspi_accessible_get_child_at_index(
		node, index, &error);
	clear_error(&error);
	return child;
}

static unsigned int accessible_process_id(AtspiAccessible *node)
{
	if (!prepare_atspi_call()) return 0;
	GError *error = NULL;
	unsigned int process_id = atspi_accessible_get_process_id(node, &error);
	clear_error(&error);
	return process_id;
}

static AtspiStateSet *accessible_state_set(AtspiAccessible *node)
{
	if (!prepare_atspi_call()) return NULL;
	return atspi_accessible_get_state_set(node);
}

static AtspiRole accessible_role_type(AtspiAccessible *node, int *known)
{
	if (known) *known = 0;
	if (!prepare_atspi_call()) return ATSPI_ROLE_INVALID;
	GError *error = NULL;
	AtspiRole role = atspi_accessible_get_role(node, &error);
	if (error) {
		clear_error(&error);
		return ATSPI_ROLE_INVALID;
	}
	if (known)
		*known = role != ATSPI_ROLE_UNKNOWN && role != ATSPI_ROLE_INVALID;
	return role;
}

static int state_contains(AtspiStateSet *states, AtspiStateType state)
{
	return states && atspi_state_set_contains(states, state);
}

static cJSON *serialize_path(const int *path, int path_len)
{
	cJSON *array = cJSON_CreateArray();
	for (int i = 0; i < path_len; i++)
		cJSON_AddItemToArray(array, cJSON_CreateNumber(path[i]));
	return array;
}

static char *bounded_copy(const char *value, size_t limit, int *truncated)
{
	if (!value) return strdup("");
	size_t length = strlen(value);
	if (truncated) *truncated = length > limit;
	size_t copy_length = length > limit ? limit : length;
	const char *invalid = NULL;
	if (!g_utf8_validate(value, (gssize)copy_length, &invalid)) {
		copy_length = (size_t)(invalid - value);
		if (truncated) *truncated = 1;
	}
	char *copy = malloc(copy_length + 1);
	if (!copy) return NULL;
	memcpy(copy, value, copy_length);
	copy[copy_length] = '\0';
	return copy;
}

static cJSON *serialize_attributes(AtspiAccessible *node, int *truncated)
{
	if (!prepare_atspi_call()) return cJSON_CreateObject();
	GError *error = NULL;
	GHashTable *attributes = atspi_accessible_get_attributes(node, &error);
	if (error || !attributes) {
		clear_error(&error);
		return cJSON_CreateObject();
	}

	cJSON *result = cJSON_CreateObject();
	GHashTableIter iter;
	gpointer key = NULL;
	gpointer value = NULL;
	int count = 0;
	g_hash_table_iter_init(&iter, attributes);
	while (g_hash_table_iter_next(&iter, &key, &value)) {
		if (count >= ACCESSIBILITY_ATTRIBUTE_LIMIT) {
			if (truncated) *truncated = 1;
			break;
		}
		if (key && value) {
			int key_truncated = 0;
			int value_truncated = 0;
			char *bounded_key = bounded_copy(key,
				ACCESSIBILITY_ATTRIBUTE_KEY_LIMIT, &key_truncated);
			char *bounded_value = bounded_copy(value,
				ACCESSIBILITY_ATTRIBUTE_VALUE_LIMIT, &value_truncated);
			if (bounded_key && bounded_value)
				cJSON_AddStringToObject(result, bounded_key, bounded_value);
			if (truncated && (key_truncated || value_truncated)) *truncated = 1;
			free(bounded_key);
			free(bounded_value);
			count++;
		}
	}
	g_hash_table_unref(attributes);
	return result;
}

static cJSON *serialize_states(AtspiStateSet *states)
{
	cJSON *result = cJSON_CreateObject();
	cJSON_AddBoolToObject(result, "focused",
		state_contains(states, ATSPI_STATE_FOCUSED));
	cJSON_AddBoolToObject(result, "focusable",
		state_contains(states, ATSPI_STATE_FOCUSABLE));
	cJSON_AddBoolToObject(result, "showing",
		state_contains(states, ATSPI_STATE_SHOWING));
	cJSON_AddBoolToObject(result, "visible",
		state_contains(states, ATSPI_STATE_VISIBLE));
	cJSON_AddBoolToObject(result, "enabled",
		state_contains(states, ATSPI_STATE_ENABLED));
	cJSON_AddBoolToObject(result, "editable",
		state_contains(states, ATSPI_STATE_EDITABLE));
	cJSON_AddBoolToObject(result, "checked",
		state_contains(states, ATSPI_STATE_CHECKED));
	cJSON_AddBoolToObject(result, "selected",
		state_contains(states, ATSPI_STATE_SELECTED));
	return result;
}

static cJSON *serialize_bounds(AtspiAccessible *node)
{
	if (!prepare_atspi_call()) return NULL;
	AtspiComponent *component = atspi_accessible_get_component_iface(node);
	if (!component) return NULL;

	GError *error = NULL;
	AtspiRect *rect = prepare_atspi_call()
		? atspi_component_get_extents(
			component, ATSPI_COORD_TYPE_SCREEN, &error)
		: NULL;
	g_object_unref(component);
	if (error || !rect) {
		clear_error(&error);
		return NULL;
	}

	cJSON *bounds = cJSON_CreateObject();
	cJSON_AddNumberToObject(bounds, "x", rect->x);
	cJSON_AddNumberToObject(bounds, "y", rect->y);
	cJSON_AddNumberToObject(bounds, "width", rect->width);
	cJSON_AddNumberToObject(bounds, "height", rect->height);
	g_boxed_free(ATSPI_TYPE_RECT, rect);
	return bounds;
}

static cJSON *serialize_actions(AtspiAccessible *node, int *actionable,
	                            int *truncated)
{
	cJSON *actions = cJSON_CreateArray();
	if (!prepare_atspi_call()) return actions;
	AtspiAction *action = atspi_accessible_get_action_iface(node);
	if (!action) return actions;

	GError *error = NULL;
	int count = prepare_atspi_call()
		? atspi_action_get_n_actions(action, &error) : 0;
	if (error) {
		clear_error(&error);
		count = 0;
	}
	if (count > ACCESSIBILITY_ACTION_LIMIT && truncated) *truncated = 1;
	int output_count = count > ACCESSIBILITY_ACTION_LIMIT
		? ACCESSIBILITY_ACTION_LIMIT : count;
	for (int i = 0; i < output_count; i++) {
		if (!prepare_atspi_call()) break;
		char *name = atspi_action_get_action_name(action, i, &error);
		if (error) {
			clear_error(&error);
			continue;
		}
		if (name) {
			int name_truncated = 0;
			char *bounded_name = bounded_copy(name,
				ACCESSIBILITY_ACTION_NAME_LIMIT, &name_truncated);
			if (bounded_name)
				cJSON_AddItemToArray(actions, cJSON_CreateString(bounded_name));
			if (truncated && name_truncated) *truncated = 1;
			free(bounded_name);
			g_free(name);
		}
	}
	if (count > 0 && actionable) *actionable = 1;
	g_object_unref(action);
	return actions;
}

static char *read_accessible_text(AtspiAccessible *node, int *truncated)
{
	if (!prepare_atspi_call()) return NULL;
	AtspiText *text_iface = atspi_accessible_get_text_iface(node);
	if (!text_iface) return NULL;

	GError *error = NULL;
	int count = prepare_atspi_call()
		? atspi_text_get_character_count(text_iface, &error) : -1;
	if (error || count < 0) {
		clear_error(&error);
		g_object_unref(text_iface);
		return NULL;
	}
	int end = count > ACCESSIBILITY_TEXT_LIMIT
		? ACCESSIBILITY_TEXT_LIMIT : count;
	char *text = prepare_atspi_call()
		? atspi_text_get_text(text_iface, 0, end, &error) : NULL;
	g_object_unref(text_iface);
	if (error) {
		clear_error(&error);
		g_free(text);
		return NULL;
	}
	if (truncated) *truncated = count > ACCESSIBILITY_TEXT_LIMIT;
	return text;
}

static cJSON *serialize_node(AtspiAccessible *node,
	                         const char *application,
	                         const char *window,
	                         const int *path, int path_len,
	                         int *actionable, int include_text,
	                         int include_attributes)
{
	char *raw_name = accessible_name(node);
	char *role = accessible_role(node);
	int role_known = 0;
	AtspiRole role_type = accessible_role_type(node, &role_known);
	int protected_text = !role_known || role_type == ATSPI_ROLE_PASSWORD_TEXT ||
		(role && strcasecmp(role, "password text") == 0);
	int name_truncated = 0;
	char *name = bounded_copy(protected_text ? "Protected text" : raw_name,
		ACCESSIBILITY_NAME_LIMIT,
		&name_truncated);
	g_free(raw_name);
	AtspiStateSet *states = accessible_state_set(node);

	cJSON *result = cJSON_CreateObject();
	cJSON_AddBoolToObject(result, "untrustedContent", 1);
	cJSON_AddStringToObject(result, "role", role ? role : "unknown");
	cJSON_AddStringToObject(result, "name", name ? name : "");
	cJSON_AddNumberToObject(result, "processId", accessible_process_id(node));
	cJSON_AddItemToObject(result, "path", serialize_path(path, path_len));

	cJSON *locator = cJSON_CreateObject();
	cJSON_AddStringToObject(locator, "application", application ? application : "");
	cJSON_AddStringToObject(locator, "window", window ? window : "");
	cJSON_AddStringToObject(locator, "role", role ? role : "unknown");
	cJSON_AddStringToObject(locator, "name",
		protected_text ? "" : name ? name : "");
	cJSON_AddItemToObject(locator, "path", serialize_path(path, path_len));
	cJSON_AddItemToObject(result, "locator", locator);

	cJSON_AddItemToObject(result, "states", serialize_states(states));
	int attributes_truncated = 0;
	cJSON_AddItemToObject(result, "attributes", protected_text || !include_attributes
		? cJSON_CreateObject()
		: serialize_attributes(node, &attributes_truncated));
	cJSON *bounds = serialize_bounds(node);
	if (bounds) cJSON_AddItemToObject(result, "bounds", bounds);
	int actions_truncated = 0;
	cJSON_AddItemToObject(result, "actions",
		serialize_actions(node, actionable, &actions_truncated));

	AtspiText *text_iface = prepare_atspi_call()
		? atspi_accessible_get_text_iface(node) : NULL;
	cJSON_AddBoolToObject(result, "hasText", text_iface != NULL);
	if (text_iface) g_object_unref(text_iface);
	cJSON_AddBoolToObject(result, "textProtected", protected_text);
	int text_truncated = 0;
	if (include_text && !protected_text) {
		char *text = read_accessible_text(node, &text_truncated);
		if (text) {
			cJSON_AddStringToObject(result, "text", text);
			cJSON_AddBoolToObject(result, "textTruncated", text_truncated);
			g_free(text);
		}
	}
	cJSON_AddBoolToObject(result, "metadataTruncated",
		name_truncated || attributes_truncated || actions_truncated);

	if (states) g_object_unref(states);
	free(name);
	g_free(role);
	return result;
}

static void traverse_nodes(AccessibilityTraversal *traversal,
	                       AtspiAccessible *node,
	                       const char *application,
	                       const char *window,
	                       int depth, int *path,
	                       cJSON *nodes)
{
	if (traversal->visited_nodes >= traversal->max_visited ||
	    monotonic_ms() >= traversal->deadline_ms) {
		traversal->incomplete = 1;
		return;
	}
	traversal->visited_nodes++;

	AtspiStateSet *states = accessible_state_set(node);
	int showing = state_contains(states, ATSPI_STATE_SHOWING);
	int focused = state_contains(states, ATSPI_STATE_FOCUSED);
	if (states) g_object_unref(states);

	if (traversal->include_offscreen || showing || focused || depth == 0) {
		if (traversal->node_count >= traversal->max_nodes) {
			traversal->truncated = 1;
			traversal->node_limit_exhausted = 1;
			return;
		}
		int actionable = 0;
		cJSON *serialized_node = serialize_node(node, application,
			window, path, depth, &actionable, traversal->include_text,
			traversal->include_attributes);
		char *serialized_text = cJSON_PrintUnformatted(serialized_node);
		size_t serialized_length = serialized_text
			? strlen(serialized_text) : 0;
		free(serialized_text);
		if (!serialized_length ||
		    traversal->serialized_node_bytes + serialized_length >
		    traversal->max_serialized_node_bytes) {
			cJSON_Delete(serialized_node);
			traversal->truncated = 1;
			traversal->incomplete = 1;
			traversal->output_budget_exhausted = 1;
			return;
		}
		cJSON_AddItemToArray(nodes, serialized_node);
		traversal->serialized_node_bytes += serialized_length;
		traversal->node_count++;
		traversal->actionable_nodes += actionable;
	}

	int count = accessible_child_count(node);
	if (depth >= traversal->max_depth) {
		if (count > 0) traversal->truncated = 1;
		return;
	}
	for (int i = 0; i < count; i++) {
		AtspiAccessible *child = accessible_child(node, i);
		if (!child) continue;
		path[depth] = i;
		traverse_nodes(traversal, child, application, window,
			depth + 1, path, nodes);
		g_object_unref(child);
		if (traversal->node_limit_exhausted || traversal->incomplete) {
			if (i + 1 < count) {
				traversal->truncated = 1;
			}
			break;
		}
	}
}

static int matches_filters(const char *application, const char *window,
	                       const char *application_filter,
	                       const char *window_filter)
{
	return contains_case_insensitive(application, application_filter) &&
	       contains_case_insensitive(window, window_filter);
}

static void search_focused(FocusSearch *search,
	                       AtspiAccessible *node,
	                       const char *application,
	                       const char *window,
	                       int depth, int *path)
{
	if (search->visited >= ACCESSIBILITY_VISITED_LIMIT ||
	    search->match_count > 1 || monotonic_ms() >= search->deadline_ms) {
		if (search->visited >= ACCESSIBILITY_VISITED_LIMIT ||
		    monotonic_ms() >= search->deadline_ms)
			search->incomplete = 1;
		return;
	}
	search->visited++;
	AtspiStateSet *states = accessible_state_set(node);
	int focused = state_contains(states, ATSPI_STATE_FOCUSED);
	if (states) g_object_unref(states);
	if (focused && matches_filters(application, window,
		search->application_filter, search->window_filter)) {
		search->match_count++;
		if (!search->match) {
			search->match = g_object_ref(node);
			snprintf(search->application, sizeof(search->application),
				"%s", application ? application : "");
			snprintf(search->window, sizeof(search->window),
				"%s", window ? window : "");
			search->path_len = depth;
			memcpy(search->path, path, sizeof(int) * (size_t)depth);
		}
	}
	if (depth >= 32) {
		if (accessible_child_count(node) > 0)
			search->incomplete = 1;
		return;
	}

	int count = accessible_child_count(node);
	for (int i = 0; i < count; i++) {
		AtspiAccessible *child = accessible_child(node, i);
		if (!child) continue;
		path[depth] = i;
		search_focused(search, child, application, window,
			depth + 1, path);
		g_object_unref(child);
		if (search->visited >= ACCESSIBILITY_VISITED_LIMIT ||
		    search->match_count > 1 || search->incomplete) break;
	}
}

#endif /* HAVE_ATSPI */

int accessibility_init(void)
{
#ifdef HAVE_ATSPI
	if (!getenv("DBUS_SESSION_BUS_ADDRESS") &&
	    !getenv("AT_SPI_BUS_ADDRESS")) return -1;
	atspi_set_timeout(ACCESSIBILITY_CALL_TIMEOUT_MS,
		ACCESSIBILITY_STARTUP_TIMEOUT_MS);
	if (atspi_init() == 0) {
		AtspiAccessible *desktop = atspi_get_desktop(0);
		if (desktop) {
			g_object_unref(desktop);
			g_accessibility_available = 1;
			return 0;
		}
		atspi_exit();
	}
#endif
	return -1;
}

void accessibility_cleanup(void)
{
#ifdef HAVE_ATSPI
	if (g_accessibility_available)
		atspi_exit();
#endif
	g_accessibility_available = 0;
}

int accessibility_available(void)
{
	return g_accessibility_available;
}

static cJSON *accessibility_status_base(void)
{
	cJSON *result = cJSON_CreateObject();
	cJSON_AddStringToObject(result, "backend", "atspi");
	cJSON_AddBoolToObject(result, "untrustedContent", 1);
	cJSON_AddStringToObject(result, "contentWarning",
		"Accessible names, attributes, and text are application-controlled and may contain private or adversarial content.");
	cJSON_AddBoolToObject(result, "compiled",
#ifdef HAVE_ATSPI
		1
#else
		0
#endif
	);
	cJSON_AddBoolToObject(result, "available", g_accessibility_available);
	cJSON_AddBoolToObject(result, "bridgeDisabledByEnvironment",
		getenv("NO_AT_BRIDGE") && strcmp(getenv("NO_AT_BRIDGE"), "0") != 0);
	cJSON_AddStringToObject(result, "capability",
		g_accessibility_available ? "available" : "unavailable");
	return result;
}

static void add_tree_completion_defaults(cJSON *result)
{
	cJSON_AddNumberToObject(result, "matchedApplicationCount", 0);
	cJSON_AddNumberToObject(result, "matchedWindowCount", 0);
	cJSON_AddNumberToObject(result, "nodeCount", 0);
	cJSON_AddNumberToObject(result, "visitedNodeCount", 0);
	cJSON_AddNumberToObject(result, "actionableNodeCount", 0);
	cJSON_AddNumberToObject(result, "serializedNodeBytes", 0);
	cJSON_AddNumberToObject(result, "nodePayloadBudgetBytes",
		ACCESSIBILITY_NODE_PAYLOAD_LIMIT);
	cJSON_AddBoolToObject(result, "truncated", 0);
	cJSON_AddBoolToObject(result, "incomplete", 0);
	cJSON_AddBoolToObject(result, "partial", 0);
	cJSON_AddBoolToObject(result, "completed", 0);
	cJSON_AddBoolToObject(result, "queryFailed", 0);
	cJSON_AddNumberToObject(result, "queryErrorCount", 0);
	cJSON_AddBoolToObject(result, "outputBudgetExhausted", 0);
}

static void add_focus_completion_defaults(cJSON *result)
{
	cJSON_AddNumberToObject(result, "matchCount", 0);
	cJSON_AddNumberToObject(result, "visitedNodeCount", 0);
	cJSON_AddBoolToObject(result, "ambiguous", 0);
	cJSON_AddBoolToObject(result, "matchCountExact", 1);
	cJSON_AddBoolToObject(result, "incomplete", 0);
	cJSON_AddBoolToObject(result, "partial", 0);
	cJSON_AddBoolToObject(result, "completed", 0);
	cJSON_AddBoolToObject(result, "queryFailed", 0);
	cJSON_AddNumberToObject(result, "queryErrorCount", 0);
}

cJSON *accessibility_status(void)
{
	reset_query_errors();
	cJSON *result = accessibility_status_base();

#ifdef HAVE_ATSPI
	if (g_accessibility_available) {
		AtspiAccessible *desktop = atspi_get_desktop(0);
		int applications = 0;
		if (desktop) {
			applications = accessible_child_count(desktop);
			g_object_unref(desktop);
		}
		cJSON_AddNumberToObject(result, "applicationCount", applications);
		cJSON_AddBoolToObject(result, "coverageScanned", 0);
	}
#endif
	cJSON_AddBoolToObject(result, "partial", g_query_error_count > 0);
	cJSON_AddNumberToObject(result, "queryErrorCount", g_query_error_count);
	if (g_query_error_count > 0)
		cJSON_AddStringToObject(result, "lastError", g_last_query_error);
	return result;
}

cJSON *accessibility_tree(const char *application_filter,
                          const char *window_filter,
                          int max_depth, int max_nodes,
						  int include_offscreen, int include_text,
						  int include_attributes)
{
	reset_query_errors();
	cJSON *result = accessibility_status_base();
	cJSON_AddNumberToObject(result, "maxDepth", max_depth);
	cJSON_AddNumberToObject(result, "maxNodes", max_nodes);
	cJSON_AddBoolToObject(result, "includeOffscreen", include_offscreen);
	cJSON_AddBoolToObject(result, "includeText", include_text);
	cJSON_AddBoolToObject(result, "includeAttributes", include_attributes);
	cJSON *applications = cJSON_CreateArray();
	cJSON_AddItemToObject(result, "applications", applications);
	add_tree_completion_defaults(result);
	if (!g_accessibility_available) return result;

#ifdef HAVE_ATSPI
	AccessibilityTraversal traversal = {
		.application_filter = application_filter,
		.window_filter = window_filter,
		.max_depth = max_depth,
		.max_nodes = max_nodes,
		.max_visited = ACCESSIBILITY_VISITED_LIMIT,
		.include_offscreen = include_offscreen,
		.include_text = include_text,
		.include_attributes = include_attributes,
		.deadline_ms = monotonic_ms() + ACCESSIBILITY_QUERY_TIMEOUT_MS,
		.max_serialized_node_bytes = ACCESSIBILITY_NODE_PAYLOAD_LIMIT,
		.applications = applications,
	};
	begin_query(traversal.deadline_ms);
	AtspiAccessible *desktop = prepare_atspi_call()
		? atspi_get_desktop(0) : NULL;
	if (!desktop) {
		cJSON_ReplaceItemInObject(result, "incomplete", cJSON_CreateBool(1));
		cJSON_ReplaceItemInObject(result, "partial", cJSON_CreateBool(1));
		cJSON_ReplaceItemInObject(result, "queryFailed", cJSON_CreateBool(1));
		cJSON_ReplaceItemInObject(result, "capability", cJSON_CreateString("error"));
		end_query();
		return result;
	}
	int app_count = accessible_child_count(desktop);
	for (int i = 0; i < app_count; i++) {
		if (traversal.visited_nodes >= traversal.max_visited ||
		    monotonic_ms() >= traversal.deadline_ms) {
			traversal.incomplete = 1;
			break;
		}
		traversal.visited_nodes++;
		AtspiAccessible *application = accessible_child(desktop, i);
		if (!application) continue;
		char *raw_app_name = accessible_name(application);
		if (!contains_case_insensitive(raw_app_name, application_filter)) {
			g_free(raw_app_name);
			g_object_unref(application);
			continue;
		}
		int app_name_truncated = 0;
		char *app_name = bounded_copy(raw_app_name, ACCESSIBILITY_NAME_LIMIT,
			&app_name_truncated);
		g_free(raw_app_name);

		cJSON *app_json = cJSON_CreateObject();
		cJSON *windows = cJSON_CreateArray();
		cJSON_AddStringToObject(app_json, "name", app_name ? app_name : "");
		cJSON_AddBoolToObject(app_json, "nameTruncated", app_name_truncated);
		cJSON_AddNumberToObject(app_json, "processId",
			accessible_process_id(application));
		cJSON_AddItemToObject(app_json, "windows", windows);
		int matched_windows = 0;
		int window_count = accessible_child_count(application);
		for (int j = 0; j < window_count; j++) {
			if (traversal.visited_nodes >= traversal.max_visited ||
			    monotonic_ms() >= traversal.deadline_ms) {
				traversal.incomplete = 1;
				break;
			}
			traversal.visited_nodes++;
			AtspiAccessible *window = accessible_child(application, j);
			if (!window) continue;
			char *raw_window_name = accessible_name(window);
			if (!contains_case_insensitive(raw_window_name, window_filter)) {
				g_free(raw_window_name);
				g_object_unref(window);
				continue;
			}
			if (traversal.node_count >= max_nodes) {
				traversal.truncated = 1;
				traversal.node_limit_exhausted = 1;
				g_free(raw_window_name);
				g_object_unref(window);
				break;
			}
			int window_name_truncated = 0;
			char *window_name = bounded_copy(raw_window_name,
				ACCESSIBILITY_NAME_LIMIT, &window_name_truncated);
			g_free(raw_window_name);

			cJSON *window_json = cJSON_CreateObject();
			cJSON *nodes = cJSON_CreateArray();
			cJSON_AddStringToObject(window_json, "name",
				window_name ? window_name : "");
			cJSON_AddBoolToObject(window_json, "nameTruncated",
				window_name_truncated);
			cJSON_AddItemToObject(window_json, "nodes", nodes);
			int path[32] = {0};
			traverse_nodes(&traversal, window, app_name, window_name,
				0, path, nodes);
			cJSON_AddItemToArray(windows, window_json);
			matched_windows++;
			traversal.matched_windows++;
			g_free(window_name);
			g_object_unref(window);
			if (traversal.incomplete || traversal.node_limit_exhausted)
				break;
		}
		if (matched_windows > 0) {
			cJSON_AddItemToArray(applications, app_json);
			traversal.matched_applications++;
		} else {
			cJSON_Delete(app_json);
		}
		free(app_name);
		g_object_unref(application);
		if (traversal.incomplete || traversal.node_limit_exhausted) break;
	}
	g_object_unref(desktop);
	cJSON_ReplaceItemInObject(result, "matchedApplicationCount",
		cJSON_CreateNumber(traversal.matched_applications));
	cJSON_ReplaceItemInObject(result, "matchedWindowCount",
		cJSON_CreateNumber(traversal.matched_windows));
	cJSON_ReplaceItemInObject(result, "nodeCount",
		cJSON_CreateNumber(traversal.node_count));
	cJSON_ReplaceItemInObject(result, "visitedNodeCount",
		cJSON_CreateNumber(traversal.visited_nodes));
	cJSON_ReplaceItemInObject(result, "actionableNodeCount",
		cJSON_CreateNumber(traversal.actionable_nodes));
	cJSON_ReplaceItemInObject(result, "serializedNodeBytes",
		cJSON_CreateNumber((double)traversal.serialized_node_bytes));
	cJSON_ReplaceItemInObject(result, "outputBudgetExhausted",
		cJSON_CreateBool(traversal.output_budget_exhausted));
	cJSON_ReplaceItemInObject(result, "truncated",
		cJSON_CreateBool(traversal.truncated));
	int incomplete = traversal.incomplete || g_query_error_count > 0;
	int partial = traversal.truncated || incomplete;
	cJSON_ReplaceItemInObject(result, "incomplete", cJSON_CreateBool(incomplete));
	cJSON_ReplaceItemInObject(result, "partial", cJSON_CreateBool(partial));
	cJSON_ReplaceItemInObject(result, "completed", cJSON_CreateBool(!partial));
	cJSON_ReplaceItemInObject(result, "queryFailed",
		cJSON_CreateBool(g_query_error_count > 0 && traversal.node_count == 0));
	cJSON_ReplaceItemInObject(result, "queryErrorCount",
		cJSON_CreateNumber(g_query_error_count));
	if (g_query_error_count > 0)
		cJSON_AddStringToObject(result, "lastError", g_last_query_error);
	cJSON_ReplaceItemInObject(result, "capability",
		cJSON_CreateString(g_query_error_count > 0 && traversal.node_count == 0
			? "error" : traversal.node_count > 0 ? "semantic" : "empty"));
#else
	(void)application_filter;
	(void)window_filter;
#endif
	end_query();
	return result;
}

cJSON *accessibility_focused_element(const char *application_filter,
                                     const char *window_filter,
                                     int include_text)
{
	reset_query_errors();
	cJSON *result = accessibility_status_base();
	cJSON_AddBoolToObject(result, "includeText", include_text);
	add_focus_completion_defaults(result);
	if (!g_accessibility_available) return result;

#ifdef HAVE_ATSPI
	FocusSearch search = {
		.application_filter = application_filter,
		.window_filter = window_filter,
		.deadline_ms = monotonic_ms() + ACCESSIBILITY_QUERY_TIMEOUT_MS,
	};
	begin_query(search.deadline_ms);
	AtspiAccessible *desktop = prepare_atspi_call()
		? atspi_get_desktop(0) : NULL;
	if (!desktop) {
		cJSON_ReplaceItemInObject(result, "incomplete", cJSON_CreateBool(1));
		cJSON_ReplaceItemInObject(result, "partial", cJSON_CreateBool(1));
		cJSON_ReplaceItemInObject(result, "queryFailed", cJSON_CreateBool(1));
		cJSON_ReplaceItemInObject(result, "capability", cJSON_CreateString("error"));
		end_query();
		return result;
	}
	int app_count = accessible_child_count(desktop);
	for (int i = 0; i < app_count && !search.incomplete &&
	     search.match_count <= 1; i++) {
		if (search.visited >= ACCESSIBILITY_VISITED_LIMIT ||
		    monotonic_ms() >= search.deadline_ms) {
			search.incomplete = 1;
			break;
		}
		search.visited++;
		AtspiAccessible *application = accessible_child(desktop, i);
		if (!application) continue;
		char *app_name = accessible_name(application);
		if (!contains_case_insensitive(app_name, application_filter)) {
			g_free(app_name);
			g_object_unref(application);
			continue;
		}
		int window_count = accessible_child_count(application);
		for (int j = 0; j < window_count && !search.incomplete &&
		     search.match_count <= 1; j++) {
			if (search.visited >= ACCESSIBILITY_VISITED_LIMIT ||
			    monotonic_ms() >= search.deadline_ms) {
				search.incomplete = 1;
				break;
			}
			search.visited++;
			AtspiAccessible *window = accessible_child(application, j);
			if (!window) continue;
			char *window_name = accessible_name(window);
			if (!contains_case_insensitive(window_name, window_filter)) {
				g_free(window_name);
				g_object_unref(window);
				continue;
			}
			int path[32] = {0};
			search_focused(&search, window, app_name, window_name,
				0, path);
			g_free(window_name);
			g_object_unref(window);
		}
		g_free(app_name);
		g_object_unref(application);
	}
	g_object_unref(desktop);
	cJSON_ReplaceItemInObject(result, "matchCount",
		cJSON_CreateNumber(search.match_count));
	cJSON_ReplaceItemInObject(result, "visitedNodeCount",
		cJSON_CreateNumber(search.visited));
	cJSON_ReplaceItemInObject(result, "ambiguous",
		cJSON_CreateBool(search.match_count > 1));
	int incomplete = search.incomplete || g_query_error_count > 0;
	int ambiguous = search.match_count > 1;
	cJSON_ReplaceItemInObject(result, "matchCountExact",
		cJSON_CreateBool(!ambiguous && !incomplete));
	int partial = incomplete || ambiguous;
	cJSON_ReplaceItemInObject(result, "incomplete", cJSON_CreateBool(incomplete));
	cJSON_ReplaceItemInObject(result, "partial", cJSON_CreateBool(partial));
	cJSON_ReplaceItemInObject(result, "completed", cJSON_CreateBool(!partial));
	cJSON_ReplaceItemInObject(result, "queryFailed",
		cJSON_CreateBool(g_query_error_count > 0 && search.match_count == 0));
	cJSON_ReplaceItemInObject(result, "queryErrorCount",
		cJSON_CreateNumber(g_query_error_count));
	if (g_query_error_count > 0)
		cJSON_AddStringToObject(result, "lastError", g_last_query_error);
	if (search.match && !ambiguous && !incomplete) {
		int actionable = 0;
		cJSON_AddItemToObject(result, "element",
			serialize_node(search.match, search.application, search.window,
				search.path, search.path_len, &actionable, include_text, 0));
		g_object_unref(search.match);
		if (g_query_error_count > 0) {
			cJSON_DeleteItemFromObject(result, "element");
			cJSON_ReplaceItemInObject(result, "incomplete", cJSON_CreateBool(1));
			cJSON_ReplaceItemInObject(result, "partial", cJSON_CreateBool(1));
			cJSON_ReplaceItemInObject(result, "completed", cJSON_CreateBool(0));
			cJSON_ReplaceItemInObject(result, "queryFailed", cJSON_CreateBool(1));
			cJSON_ReplaceItemInObject(result, "capability", cJSON_CreateString("error"));
			cJSON_ReplaceItemInObject(result, "queryErrorCount",
				cJSON_CreateNumber(g_query_error_count));
			cJSON_AddStringToObject(result, "lastError", g_last_query_error);
		} else {
			cJSON_ReplaceItemInObject(result, "capability",
				cJSON_CreateString("semantic"));
		}
	} else {
		if (search.match) g_object_unref(search.match);
		cJSON_ReplaceItemInObject(result, "capability",
			cJSON_CreateString(ambiguous ? "ambiguous" :
				(g_query_error_count > 0 ? "error" : "empty")));
	}
#else
	(void)application_filter;
	(void)window_filter;
#endif
	end_query();
	return result;
}