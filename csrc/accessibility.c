/*
 * deskpal — Optional AT-SPI accessibility backend
 *
 * Copyright (c) 2026 deskpal contributors
 * SPDX-License-Identifier: MIT
 */
#include "accessibility.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <time.h>
#include <unistd.h>

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

typedef struct {
	const char *role;
	const char *name;
	const char *bus_name;
	const char *object_path;
	unsigned int process_id;
	int path[32];
	int path_len;
	int has_path;
} AccessibilitySelector;

typedef struct {
	const char *application_filter;
	const char *window_filter;
	const AccessibilitySelector *selector;
	AtspiAccessible *match;
	char application[513];
	char window[513];
	int path[32];
	int path_len;
	int match_count;
	int visited;
	int incomplete;
	int exact_scope;
	long long deadline_ms;
} AccessibilitySearch;

typedef struct {
	char *bus_name;
	char *object_path;
	unsigned int process_id;
	AtspiAccessible *object;
} AccessibilityIdentity;

typedef struct {
	char *text;
	int text_observed;
	int text_truncated;
	int state_value;
	int state_observed;
	double value;
	int value_observed;
	int selected;
	int selection_observed;
	int satisfied;
} AccessibilityVerification;

typedef struct {
	int issued;
	int reported_success;
	int outcome_unknown;
} AccessibilityMutation;

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

static void add_live_identity(cJSON *locator, AtspiAccessible *node)
{
	AtspiObject *object = node ? ATSPI_OBJECT(node) : NULL;
	if (!locator || !object || !object->path || !object->app ||
	    !object->app->bus_name)
		return;
	cJSON_AddStringToObject(locator, "busName", object->app->bus_name);
	cJSON_AddStringToObject(locator, "objectPath", object->path);
	cJSON_AddNumberToObject(locator, "processId", accessible_process_id(node));
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
	cJSON_AddBoolToObject(result, "expanded",
		state_contains(states, ATSPI_STATE_EXPANDED));
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

static cJSON *serialize_value(AtspiAccessible *node)
{
	if (!prepare_atspi_call()) return NULL;
	AtspiValue *value = atspi_accessible_get_value_iface(node);
	if (!value) return NULL;
	GError *error = NULL;
	double current = atspi_value_get_current_value(value, &error);
	double minimum = error ? 0 : atspi_value_get_minimum_value(value, &error);
	double maximum = error ? 0 : atspi_value_get_maximum_value(value, &error);
	double increment = error ? 0 : atspi_value_get_minimum_increment(value, &error);
	g_object_unref(value);
	if (error || !isfinite(current) || !isfinite(minimum) ||
	    !isfinite(maximum) || !isfinite(increment)) {
		clear_error(&error);
		return NULL;
	}
	cJSON *result = cJSON_CreateObject();
	cJSON_AddNumberToObject(result, "current", current);
	cJSON_AddNumberToObject(result, "minimum", minimum);
	cJSON_AddNumberToObject(result, "maximum", maximum);
	cJSON_AddNumberToObject(result, "minimumIncrement", increment);
	return result;
}

static cJSON *serialize_selection(AtspiAccessible *node)
{
	if (!prepare_atspi_call()) return NULL;
	AtspiSelection *selection = atspi_accessible_get_selection_iface(node);
	if (!selection) return NULL;
	GError *error = NULL;
	int selected_count = atspi_selection_get_n_selected_children(selection, &error);
	int child_count = error ? -1 : accessible_child_count(node);
	if (error || selected_count < 0 || child_count < 0) {
		clear_error(&error);
		g_object_unref(selection);
		return NULL;
	}
	cJSON *result = cJSON_CreateObject();
	cJSON_AddNumberToObject(result, "childCount", child_count);
	cJSON_AddNumberToObject(result, "selectedChildCount", selected_count);
	cJSON *indices = cJSON_CreateArray();
	cJSON_AddItemToObject(result, "selectedChildIndices", indices);
	for (int i = 0; i < child_count; i++) {
		if (!prepare_atspi_call()) break;
		gboolean selected = atspi_selection_is_child_selected(
			selection, i, &error);
		if (error) {
			clear_error(&error);
			cJSON_Delete(result);
			g_object_unref(selection);
			return NULL;
		}
		if (selected) cJSON_AddItemToArray(indices, cJSON_CreateNumber(i));
	}
	g_object_unref(selection);
	return result;
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
	if (!protected_text && name && name[0])
		cJSON_AddStringToObject(locator, "name", name);
	cJSON_AddItemToObject(locator, "path", serialize_path(path, path_len));
	add_live_identity(locator, node);
	cJSON_AddItemToObject(result, "locator", locator);

	cJSON_AddItemToObject(result, "states", serialize_states(states));
	int attributes_truncated = 0;
	cJSON_AddItemToObject(result, "attributes", protected_text || !include_attributes
		? cJSON_CreateObject()
		: serialize_attributes(node, &attributes_truncated));
	cJSON *bounds = serialize_bounds(node);
	if (bounds) cJSON_AddItemToObject(result, "bounds", bounds);
	cJSON *value = serialize_value(node);
	if (value) cJSON_AddItemToObject(result, "value", value);
	cJSON *selection = serialize_selection(node);
	if (selection) cJSON_AddItemToObject(result, "selection", selection);
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

static int scope_matches(const char *value, const char *filter, int exact)
{
	if (!filter || !filter[0]) return 1;
	if (!value) return 0;
	return exact ? strcmp(value, filter) == 0 :
		contains_case_insensitive(value, filter);
}

static int parse_selector(const cJSON *object, AccessibilitySelector *selector)
{
	if (!object || !cJSON_IsObject(object) || !selector) return -1;
	memset(selector, 0, sizeof(*selector));
	const cJSON *role = cJSON_GetObjectItem(object, "role");
	const cJSON *name = cJSON_GetObjectItem(object, "name");
	const cJSON *path = cJSON_GetObjectItem(object, "path");
	const cJSON *bus_name = cJSON_GetObjectItem(object, "busName");
	const cJSON *object_path = cJSON_GetObjectItem(object, "objectPath");
	const cJSON *process_id = cJSON_GetObjectItem(object, "processId");
	if (!cJSON_IsString(role) || !role->valuestring[0]) return -1;
	selector->role = role->valuestring;
	if (name) {
		if (!cJSON_IsString(name) || !name->valuestring[0]) return -1;
		selector->name = name->valuestring;
	}
	if (path) {
		if (!cJSON_IsArray(path)) return -1;
		int count = cJSON_GetArraySize(path);
		if (count < 0 || count > 32) return -1;
		selector->has_path = 1;
		if (!cJSON_IsString(bus_name) || !bus_name->valuestring[0] ||
		    !cJSON_IsString(object_path) || !object_path->valuestring[0] ||
		    !cJSON_IsNumber(process_id) ||
		    process_id->valuedouble != process_id->valueint ||
		    process_id->valueint <= 0)
			return -1;
		selector->bus_name = bus_name->valuestring;
		selector->object_path = object_path->valuestring;
		selector->process_id = (unsigned int)process_id->valueint;
		selector->path_len = count;
		for (int i = 0; i < count; i++) {
			const cJSON *index = cJSON_GetArrayItem(path, i);
			if (!cJSON_IsNumber(index) || index->valuedouble != index->valueint ||
			    index->valueint < 0 || index->valueint > 4096)
				return -1;
			selector->path[i] = index->valueint;
		}
	}
	return selector->name || selector->has_path ? 0 : -1;
}

static int selector_matches(AtspiAccessible *node,
	                        const AccessibilitySelector *selector,
	                        const int *path, int path_len)
{
	if (selector->has_path) {
		if (selector->path_len != path_len) return 0;
		if (path_len > 0 && memcmp(selector->path, path,
		    sizeof(int) * (size_t)path_len) != 0)
			return 0;
		AtspiObject *object = ATSPI_OBJECT(node);
		if (!object || !object->path || !object->app ||
		    !object->app->bus_name ||
		    strcmp(selector->bus_name, object->app->bus_name) != 0 ||
		    strcmp(selector->object_path, object->path) != 0 ||
		    selector->process_id != accessible_process_id(node))
			return 0;
	}
	char *role = accessible_role(node);
	int role_matches = role && strcasecmp(role, selector->role) == 0;
	g_free(role);
	if (!role_matches) return 0;
	if (selector->name) {
		char *name = accessible_name(node);
		int name_matches = name && strcmp(name, selector->name) == 0;
		g_free(name);
		if (!name_matches) return 0;
	}
	return 1;
}

static void search_selector_nodes(AccessibilitySearch *search,
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
	if (selector_matches(node, search->selector, path, depth)) {
		search->match_count++;
		if (!search->match) {
			search->match = g_object_ref(node);
			snprintf(search->application, sizeof(search->application), "%s",
				application ? application : "");
			snprintf(search->window, sizeof(search->window), "%s",
				window ? window : "");
			search->path_len = depth;
			memcpy(search->path, path, sizeof(int) * (size_t)depth);
		}
	}
	if (depth >= 32) {
		if (accessible_child_count(node) > 0) search->incomplete = 1;
		return;
	}
	int count = accessible_child_count(node);
	for (int i = 0; i < count; i++) {
		if (search->visited >= ACCESSIBILITY_VISITED_LIMIT ||
		    monotonic_ms() >= search->deadline_ms) {
			search->incomplete = 1;
			break;
		}
		AtspiAccessible *child = accessible_child(node, i);
		if (!child) {
			search->incomplete = 1;
			break;
		}
		path[depth] = i;
		search_selector_nodes(search, child, application, window,
			depth + 1, path);
		g_object_unref(child);
		if (search->match_count > 1 || search->incomplete) break;
	}
}

static void resolve_selector(AccessibilitySearch *search)
{
	AtspiAccessible *desktop = prepare_atspi_call()
		? atspi_get_desktop(0) : NULL;
	if (!desktop) {
		search->incomplete = 1;
		return;
	}
	int app_count = accessible_child_count(desktop);
	for (int i = 0; i < app_count && !search->incomplete &&
	     search->match_count <= 1; i++) {
		if (search->visited >= ACCESSIBILITY_VISITED_LIMIT ||
		    monotonic_ms() >= search->deadline_ms) {
			search->incomplete = 1;
			break;
		}
		search->visited++;
		AtspiAccessible *application = accessible_child(desktop, i);
		if (!application) {
			search->incomplete = 1;
			break;
		}
		char *application_name = accessible_name(application);
		if (!application_name) {
			search->incomplete = 1;
			g_object_unref(application);
			break;
		}
		if (!scope_matches(application_name,
		    search->application_filter, search->exact_scope)) {
			g_free(application_name);
			g_object_unref(application);
			continue;
		}
		int window_count = accessible_child_count(application);
		for (int j = 0; j < window_count && !search->incomplete &&
		     search->match_count <= 1; j++) {
			if (search->visited >= ACCESSIBILITY_VISITED_LIMIT ||
			    monotonic_ms() >= search->deadline_ms) {
				search->incomplete = 1;
				break;
			}
			search->visited++;
			AtspiAccessible *window = accessible_child(application, j);
			if (!window) {
				search->incomplete = 1;
				break;
			}
			char *window_name = accessible_name(window);
			if (!window_name) {
				search->incomplete = 1;
				g_object_unref(window);
				break;
			}
			if (scope_matches(window_name,
			    search->window_filter, search->exact_scope)) {
				int path[32] = {0};
				search_selector_nodes(search, window, application_name,
					window_name, 0, path);
			}
			g_free(window_name);
			g_object_unref(window);
		}
		g_free(application_name);
		g_object_unref(application);
	}
	g_object_unref(desktop);
}

static void release_search(AccessibilitySearch *search)
{
	if (search && search->match) {
		g_object_unref(search->match);
		search->match = NULL;
	}
}

static int identity_from_search(AccessibilityIdentity *identity,
	                            const AccessibilitySearch *search)
{
	memset(identity, 0, sizeof(*identity));
	if (!search || !search->match) return -1;
	AtspiObject *object = ATSPI_OBJECT(search->match);
	if (!object || !object->path || !object->app || !object->app->bus_name)
		return -1;
	identity->bus_name = strdup(object->app->bus_name);
	identity->object_path = strdup(object->path);
	identity->process_id = accessible_process_id(search->match);
	identity->object = g_object_ref(search->match);
	if (!identity->bus_name || !identity->object_path ||
	    identity->process_id == 0 || !identity->object) {
		free(identity->bus_name);
		free(identity->object_path);
		if (identity->object) g_object_unref(identity->object);
		memset(identity, 0, sizeof(*identity));
		return -1;
	}
	return 0;
}

static int identity_matches_search(const AccessibilityIdentity *identity,
	                               const AccessibilitySearch *search)
{
	if (!identity || !identity->object || !search || !search->match)
		return 0;
	AtspiObject *object = ATSPI_OBJECT(search->match);
	return search->match == identity->object && object && object->path &&
	       object->app && object->app->bus_name &&
	       identity->process_id == accessible_process_id(search->match) &&
	       strcmp(identity->bus_name, object->app->bus_name) == 0 &&
	       strcmp(identity->object_path, object->path) == 0;
}

static void clear_identity(AccessibilityIdentity *identity)
{
	if (!identity) return;
	free(identity->bus_name);
	free(identity->object_path);
	if (identity->object) g_object_unref(identity->object);
	memset(identity, 0, sizeof(*identity));
}

static int node_is_protected(AtspiAccessible *node)
{
	int role_known = 0;
	AtspiRole role = accessible_role_type(node, &role_known);
	char *role_name = accessible_role(node);
	AtspiStateSet *states = accessible_state_set(node);
	int defunct_or_unreadable = !states ||
		state_contains(states, ATSPI_STATE_DEFUNCT);
	if (states) g_object_unref(states);
	int protected = !role_known || role == ATSPI_ROLE_PASSWORD_TEXT ||
		(role_name && strcasecmp(role_name, "password text") == 0) ||
		defunct_or_unreadable;
	g_free(role_name);
	return protected;
}

static int selector_state(const char *name, AtspiStateType *state)
{
	if (strcmp(name, "focused") == 0) *state = ATSPI_STATE_FOCUSED;
	else if (strcmp(name, "checked") == 0) *state = ATSPI_STATE_CHECKED;
	else if (strcmp(name, "selected") == 0) *state = ATSPI_STATE_SELECTED;
	else if (strcmp(name, "enabled") == 0) *state = ATSPI_STATE_ENABLED;
	else if (strcmp(name, "editable") == 0) *state = ATSPI_STATE_EDITABLE;
	else if (strcmp(name, "showing") == 0) *state = ATSPI_STATE_SHOWING;
	else if (strcmp(name, "expanded") == 0) *state = ATSPI_STATE_EXPANDED;
	else return -1;
	return 0;
}

static cJSON *search_locator(const AccessibilitySearch *search,
	                         const AccessibilitySelector *selector)
{
	cJSON *locator = cJSON_CreateObject();
	cJSON_AddStringToObject(locator, "application", search->application);
	cJSON_AddStringToObject(locator, "window", search->window);
	cJSON_AddStringToObject(locator, "role", selector->role);
	if (selector->name)
		cJSON_AddStringToObject(locator, "name", selector->name);
	cJSON_AddItemToObject(locator, "path",
		serialize_path(search->path, search->path_len));
	add_live_identity(locator, search->match);
	return locator;
}

static char *action_text(AtspiAccessible *node, int *truncated)
{
	if (node_is_protected(node)) return NULL;
	return read_accessible_text(node, truncated);
}

static AccessibilityMutation perform_set_text(AtspiAccessible *node,
	                                           const char *value)
{
	AccessibilityMutation outcome = {0};
	if (!prepare_atspi_call()) return outcome;
	AtspiEditableText *editable = atspi_accessible_get_editable_text_iface(node);
	if (!editable) return outcome;
	GError *error = NULL;
	if (prepare_atspi_call()) {
		outcome.issued = 1;
		outcome.reported_success =
			atspi_editable_text_set_text_contents(editable, value, &error);
		outcome.outcome_unknown = error != NULL;
	}
	g_object_unref(editable);
	clear_error(&error);
	return outcome;
}

static int node_value(AtspiAccessible *node, double *current,
                      double *minimum, double *maximum, double *increment)
{
	if (!prepare_atspi_call()) return -1;
	AtspiValue *value = atspi_accessible_get_value_iface(node);
	if (!value) return -1;
	GError *error = NULL;
	*current = atspi_value_get_current_value(value, &error);
	*minimum = error ? 0 : atspi_value_get_minimum_value(value, &error);
	*maximum = error ? 0 : atspi_value_get_maximum_value(value, &error);
	*increment = error ? 0 : atspi_value_get_minimum_increment(value, &error);
	g_object_unref(value);
	if (error || !isfinite(*current) || !isfinite(*minimum) ||
	    !isfinite(*maximum) || !isfinite(*increment)) {
		clear_error(&error);
		return -1;
	}
	return 0;
}

static int node_accepts_value(AtspiAccessible *node, double expected,
                              double *current, double *minimum,
                              double *maximum, double *increment)
{
	if (node_value(node, current, minimum, maximum, increment) != 0 ||
	    expected < *minimum || expected > *maximum)
		return 0;
	if (*increment > 0) {
		double steps = (expected - *minimum) / *increment;
		if (fabs(steps - round(steps)) > 1e-6) return 0;
	}
	return 1;
}

static AccessibilityMutation perform_set_value(AtspiAccessible *node,
                                                double new_value)
{
	AccessibilityMutation outcome = {0};
	if (!prepare_atspi_call()) return outcome;
	AtspiValue *value = atspi_accessible_get_value_iface(node);
	if (!value) return outcome;
	GError *error = NULL;
	if (prepare_atspi_call()) {
		outcome.issued = 1;
		outcome.reported_success = atspi_value_set_current_value(
			value, new_value, &error);
		outcome.outcome_unknown = error != NULL;
	}
	g_object_unref(value);
	clear_error(&error);
	return outcome;
}

static int node_selection(AtspiAccessible *node, int child_index,
                          int *child_count, int *selected)
{
	if (!prepare_atspi_call()) return -1;
	AtspiSelection *selection = atspi_accessible_get_selection_iface(node);
	if (!selection) return -1;
	*child_count = accessible_child_count(node);
	if (child_index < 0 || child_index >= *child_count) {
		g_object_unref(selection);
		return -1;
	}
	GError *error = NULL;
	*selected = atspi_selection_is_child_selected(
		selection, child_index, &error);
	g_object_unref(selection);
	if (error) {
		clear_error(&error);
		return -1;
	}
	return 0;
}

static AccessibilityMutation perform_select(AtspiAccessible *node,
                                             int child_index)
{
	AccessibilityMutation outcome = {0};
	if (!prepare_atspi_call()) return outcome;
	AtspiSelection *selection = atspi_accessible_get_selection_iface(node);
	if (!selection) return outcome;
	GError *error = NULL;
	if (prepare_atspi_call()) {
		outcome.issued = 1;
		outcome.reported_success = atspi_selection_select_child(
			selection, child_index, &error);
		outcome.outcome_unknown = error != NULL;
	}
	g_object_unref(selection);
	clear_error(&error);
	return outcome;
}

static AccessibilityMutation perform_focus(AtspiAccessible *node)
{
	AccessibilityMutation outcome = {0};
	if (!prepare_atspi_call()) return outcome;
	AtspiComponent *component = atspi_accessible_get_component_iface(node);
	if (!component) return outcome;
	GError *error = NULL;
	if (prepare_atspi_call()) {
		outcome.issued = 1;
		outcome.reported_success = atspi_component_grab_focus(component,
			&error);
		outcome.outcome_unknown = error != NULL;
	}
	g_object_unref(component);
	clear_error(&error);
	return outcome;
}

static int resolve_action_index_on_iface(AtspiAction *action,
	                                     const char *action_name,
	                                     int *action_matches,
	                                     int *action_index)
{
	*action_matches = 0;
	*action_index = -1;
	int errors_before = g_query_error_count;
	if (!action) return -1;
	GError *error = NULL;
	int count = prepare_atspi_call()
		? atspi_action_get_n_actions(action, &error) : 0;
	clear_error(&error);
	for (int i = 0; i < count; i++) {
		if (!prepare_atspi_call()) break;
		char *name = atspi_action_get_action_name(action, i, &error);
		clear_error(&error);
		if (name && strcmp(name, action_name) == 0) {
			(*action_matches)++;
			*action_index = i;
		}
		g_free(name);
	}
	return g_query_error_count == errors_before &&
	       *action_matches == 1 ? 0 : -1;
}

static int resolve_action_index(AtspiAccessible *node,
	                            const char *action_name,
	                            int *action_matches,
	                            int *action_index)
{
	if (!prepare_atspi_call()) return -1;
	AtspiAction *action = atspi_accessible_get_action_iface(node);
	if (!action) return -1;
	int result = resolve_action_index_on_iface(action, action_name,
		action_matches, action_index);
	g_object_unref(action);
	return result;
}

static AtspiAction *prepare_invoke(AtspiAccessible *node,
	                               const char *action_name,
	                               int *action_matches,
	                               int *action_index)
{
	*action_matches = 0;
	*action_index = -1;
	if (!prepare_atspi_call()) return NULL;
	AtspiAction *action = atspi_accessible_get_action_iface(node);
	if (!action) return NULL;
	if (resolve_action_index_on_iface(action, action_name,
	    action_matches, action_index) != 0) {
		g_object_unref(action);
		return NULL;
	}
	return action;
}

static AccessibilityMutation perform_prepared_invoke(AtspiAction *action,
	                                                   int action_index)
{
	AccessibilityMutation outcome = {0};
	if (!action || action_index < 0) return outcome;
	GError *error = NULL;
	if (prepare_atspi_call()) {
		outcome.issued = 1;
		outcome.reported_success = atspi_action_do_action(action,
			action_index, &error);
		outcome.outcome_unknown = error != NULL;
	}
	clear_error(&error);
	return outcome;
}

static int node_state(AtspiAccessible *node, AtspiStateType state,
	                  int *value)
{
	AtspiStateSet *states = accessible_state_set(node);
	if (!states) return -1;
	if (state_contains(states, ATSPI_STATE_DEFUNCT)) {
		g_object_unref(states);
		return -1;
	}
	*value = state_contains(states, state);
	g_object_unref(states);
	return 0;
}

static void clear_verification(AccessibilityVerification *verification)
{
	if (!verification) return;
	g_free(verification->text);
	memset(verification, 0, sizeof(*verification));
}

static int evaluate_verification(AtspiAccessible *node,
	                             int verify_text, const char *expected_text,
	                             int verify_state, AtspiStateType expected_state,
	                             int expected_state_value,
	                             int verify_value, double expected_value,
	                             int verify_selection, int expected_child_index,
	                             AccessibilityVerification *verification)
{
	clear_verification(verification);
	int text_satisfied = !verify_text;
	int state_satisfied = !verify_state;
	int value_satisfied = !verify_value;
	int selection_satisfied = !verify_selection;
	if (verify_text) {
		verification->text = action_text(node,
			&verification->text_truncated);
		verification->text_observed = verification->text != NULL;
		if (!verification->text_observed || verification->text_truncated)
			return -1;
		text_satisfied = strcmp(verification->text, expected_text) == 0;
	}
	if (verify_state) {
		if (node_state(node, expected_state,
		    &verification->state_value) != 0)
			return -1;
		verification->state_observed = 1;
		state_satisfied =
			verification->state_value == expected_state_value;
	}
	if (verify_value) {
		AtspiValue *value = prepare_atspi_call()
			? atspi_accessible_get_value_iface(node) : NULL;
		if (!value) return -1;
		GError *error = NULL;
		verification->value = atspi_value_get_current_value(value, &error);
		g_object_unref(value);
		if (error || !isfinite(verification->value)) {
			clear_error(&error);
			return -1;
		}
		verification->value_observed = 1;
		value_satisfied = fabs(verification->value - expected_value) <= 1e-6;
	}
	if (verify_selection) {
		int child_count = 0;
		if (node_selection(node, expected_child_index,
		                   &child_count, &verification->selected) != 0)
			return -1;
		verification->selection_observed = 1;
		selection_satisfied = verification->selected;
	}
	verification->satisfied = text_satisfied && state_satisfied &&
	                         value_satisfied && selection_satisfied;
	return 0;
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

static cJSON *accessibility_tree_scoped(const char *application_filter,
                                        const char *window_filter,
                                        int max_depth, int max_nodes,
                                        int include_offscreen, int include_text,
                                        int include_attributes,
                                        int exact_scope)
{
	reset_query_errors();
	cJSON *result = accessibility_status_base();
	cJSON_AddNumberToObject(result, "maxDepth", max_depth);
	cJSON_AddNumberToObject(result, "maxNodes", max_nodes);
	cJSON_AddBoolToObject(result, "includeOffscreen", include_offscreen);
	cJSON_AddBoolToObject(result, "includeText", include_text);
	cJSON_AddBoolToObject(result, "includeAttributes", include_attributes);
	cJSON_AddBoolToObject(result, "exactScope", exact_scope);
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
		if (!scope_matches(raw_app_name, application_filter, exact_scope)) {
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
			if (!scope_matches(raw_window_name, window_filter, exact_scope)) {
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

cJSON *accessibility_tree(const char *application_filter,
                          const char *window_filter,
                          int max_depth, int max_nodes,
                          int include_offscreen, int include_text,
                          int include_attributes)
{
	return accessibility_tree_scoped(application_filter, window_filter,
		max_depth, max_nodes, include_offscreen, include_text,
		include_attributes, 0);
}

cJSON *accessibility_tree_exact(const char *application,
                                const char *window,
                                int max_depth, int max_nodes,
                                int include_offscreen, int include_text,
                                int include_attributes)
{
	return accessibility_tree_scoped(application, window,
		max_depth, max_nodes, include_offscreen, include_text,
		include_attributes, 1);
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

cJSON *accessibility_action(const cJSON *params)
{
	reset_query_errors();
	cJSON *result = accessibility_status_base();
	cJSON_AddBoolToObject(result, "success", 0);
	cJSON_AddBoolToObject(result, "actionApplied", 0);
	cJSON_AddBoolToObject(result, "verified", 0);
	cJSON_AddBoolToObject(result, "mutationIssued", 0);
	cJSON_AddBoolToObject(result, "actionOutcomeUnknown", 0);
	cJSON_AddStringToObject(result, "actionOutcome", "not_issued");
	if (!g_accessibility_available) {
		cJSON_AddStringToObject(result, "errorCode", "backend_unavailable");
		cJSON_AddStringToObject(result, "error", "AT-SPI accessibility backend is unavailable");
		return result;
	}

#ifdef HAVE_ATSPI
	const cJSON *application_item = cJSON_GetObjectItem(params, "application");
	const char *application = application_item ? application_item->valuestring : NULL;
	const cJSON *window_item = cJSON_GetObjectItem(params, "window");
	const char *window = window_item ? window_item->valuestring : NULL;
	const char *operation = cJSON_GetObjectItem(params, "operation")->valuestring;
	const cJSON *target_object = cJSON_GetObjectItem(params, "target");
	const cJSON *verify_object = cJSON_GetObjectItem(params, "verify");
	const cJSON *timeout_item = cJSON_GetObjectItem(params, "timeoutMs");
	int timeout_ms = timeout_item ? timeout_item->valueint : 1000;
	long long started_ms = monotonic_ms();
	long long deadline_ms = started_ms + timeout_ms;
	cJSON_AddStringToObject(result, "operation", operation);
	cJSON_AddNumberToObject(result, "timeoutMs", timeout_ms);

	AccessibilitySelector target_selector;
	if (parse_selector(target_object, &target_selector) != 0) {
		cJSON_AddStringToObject(result, "errorCode", "invalid_selector");
		cJSON_AddStringToObject(result, "error", "Target selector must include role and name or path");
		return result;
	}

	AccessibilitySelector verify_selector;
	memset(&verify_selector, 0, sizeof(verify_selector));
	const char *expected_text = NULL;
	const char *expected_state_name = NULL;
	int expected_state_value = 0;
	AtspiStateType expected_state = ATSPI_STATE_INVALID;
	int verify_text = 0;
	int verify_state = 0;
	int verify_value = 0;
	double expected_value = 0;
	int verify_selection = 0;
	int expected_child_index = -1;
	AccessibilityIdentity target_identity = {0};
	AccessibilityIdentity verify_identity = {0};
	AtspiAction *prepared_action = NULL;
	int prepared_action_index = -1;
	if (strcmp(operation, "invoke") == 0) {
		if (parse_selector(verify_object, &verify_selector) != 0) {
			cJSON_AddStringToObject(result, "errorCode", "verification_required");
			cJSON_AddStringToObject(result, "error", "Invoke requires a verification selector and postcondition");
			return result;
		}
		const cJSON *text_equals = cJSON_GetObjectItem(verify_object, "textEquals");
		const cJSON *state = cJSON_GetObjectItem(verify_object, "state");
		if (text_equals) {
			expected_text = text_equals->valuestring;
			verify_text = 1;
		}
		if (state) {
			expected_state_name = state->valuestring;
			selector_state(expected_state_name, &expected_state);
			expected_state_value = cJSON_IsTrue(
				cJSON_GetObjectItem(verify_object, "stateValue"));
			verify_state = 1;
		}
	} else {
		verify_selector = target_selector;
		if (strcmp(operation, "setText") == 0) {
			expected_text = cJSON_GetObjectItem(params, "value")->valuestring;
			verify_text = 1;
		} else if (strcmp(operation, "setValue") == 0) {
			expected_value = cJSON_GetObjectItem(params, "value")->valuedouble;
			verify_value = 1;
		} else if (strcmp(operation, "select") == 0) {
			expected_child_index = cJSON_GetObjectItem(params, "value")->valueint;
			verify_selection = 1;
		} else {
			expected_state_name = "focused";
			expected_state = ATSPI_STATE_FOCUSED;
			expected_state_value = 1;
			verify_state = 1;
		}
	}

	begin_query(deadline_ms);
	AccessibilitySearch target_search = {
		.application_filter = application,
		.window_filter = window,
		.selector = &target_selector,
		.exact_scope = 1,
		.deadline_ms = deadline_ms,
	};
	resolve_selector(&target_search);
	cJSON_AddNumberToObject(result, "targetMatchCount", target_search.match_count);
	cJSON_AddBoolToObject(result, "targetMatchCountExact",
		!target_search.incomplete && g_query_error_count == 0 &&
		target_search.match_count <= 1);
	if (target_search.incomplete || g_query_error_count > 0 ||
	    target_search.match_count != 1 || !target_search.match) {
		cJSON_AddStringToObject(result, "errorCode",
			target_search.match_count > 1 ? "target_ambiguous" :
			target_search.incomplete || g_query_error_count > 0
				? "target_incomplete" : "target_not_found");
		cJSON_AddStringToObject(result, "error",
			target_search.match_count > 1 ? "Target selector matched multiple elements" :
			"Target selector did not resolve completely to one element");
		release_search(&target_search);
		goto action_done;
	}
	if (node_is_protected(target_search.match)) {
		cJSON_AddStringToObject(result, "errorCode", "protected_target");
		cJSON_AddStringToObject(result, "error", "Protected or unknown-role elements cannot be mutated");
		release_search(&target_search);
		goto action_done;
	}
	cJSON_AddItemToObject(result, "target",
		search_locator(&target_search, &target_selector));
	if (identity_from_search(&target_identity, &target_search) != 0) {
		cJSON_AddStringToObject(result, "errorCode", "target_identity_unavailable");
		cJSON_AddStringToObject(result, "error",
			"Could not bind target to one live AT-SPI object");
		release_search(&target_search);
		goto action_done;
	}

	int enabled = 0;
	int showing = 0;
	int focusable = 0;
	int editable = 0;
	double current_value = 0;
	double minimum_value = 0;
	double maximum_value = 0;
	double value_increment = 0;
	int selection_child_count = 0;
	int selection_selected = 0;
	if (node_state(target_search.match, ATSPI_STATE_ENABLED, &enabled) != 0 ||
	    node_state(target_search.match, ATSPI_STATE_SHOWING, &showing) != 0 ||
	    node_state(target_search.match, ATSPI_STATE_FOCUSABLE, &focusable) != 0 ||
	    node_state(target_search.match, ATSPI_STATE_EDITABLE, &editable) != 0) {
		cJSON_AddStringToObject(result, "errorCode", "precondition_incomplete");
		cJSON_AddStringToObject(result, "error", "Could not read target preconditions");
		release_search(&target_search);
		goto action_done;
	}
	cJSON *precondition = cJSON_CreateObject();
	cJSON_AddBoolToObject(precondition, "enabled", enabled);
	cJSON_AddBoolToObject(precondition, "showing", showing);
	cJSON_AddBoolToObject(precondition, "focusable", focusable);
	cJSON_AddBoolToObject(precondition, "editable", editable);
	if (strcmp(operation, "setValue") == 0) {
		if (node_value(target_search.match, &current_value, &minimum_value,
		               &maximum_value, &value_increment) != 0) {
			cJSON_AddStringToObject(result, "errorCode", "value_interface_unavailable");
			cJSON_AddStringToObject(result, "error",
				"Target does not expose a readable AT-SPI value interface");
			cJSON_Delete(precondition);
			release_search(&target_search);
			goto action_done;
		}
		cJSON_AddNumberToObject(precondition, "currentValue", current_value);
		cJSON_AddNumberToObject(precondition, "minimumValue", minimum_value);
		cJSON_AddNumberToObject(precondition, "maximumValue", maximum_value);
		cJSON_AddNumberToObject(precondition, "minimumIncrement", value_increment);
	}
	if (strcmp(operation, "select") == 0) {
		if (node_selection(target_search.match, expected_child_index,
		                   &selection_child_count, &selection_selected) != 0) {
			cJSON_AddStringToObject(result, "errorCode", "selection_unavailable");
			cJSON_AddStringToObject(result, "error",
				"Target does not expose the requested AT-SPI selection child");
			cJSON_Delete(precondition);
			release_search(&target_search);
			goto action_done;
		}
		cJSON_AddNumberToObject(precondition, "selectionChildCount",
		                       selection_child_count);
		cJSON_AddBoolToObject(precondition, "selected", selection_selected);
	}
	cJSON_AddItemToObject(result, "precondition", precondition);
	if (!enabled || !showing ||
	    (strcmp(operation, "focus") == 0 && !focusable) ||
	    (strcmp(operation, "setText") == 0 && !editable) ||
	    (strcmp(operation, "setValue") == 0 &&
	     (expected_value < minimum_value || expected_value > maximum_value ||
	      (value_increment > 0 && fabs((expected_value - minimum_value) /
	       value_increment - round((expected_value - minimum_value) /
	       value_increment)) > 1e-6)))) {
		cJSON_AddStringToObject(result, "errorCode", "precondition_failed");
		cJSON_AddStringToObject(result, "error", "Target is not enabled/showing or lacks the required state");
		release_search(&target_search);
		goto action_done;
	}

	AccessibilitySearch verify_pre = {
		.application_filter = application,
		.window_filter = window,
		.selector = &verify_selector,
		.exact_scope = 1,
		.deadline_ms = deadline_ms,
	};
	resolve_selector(&verify_pre);
	if (verify_pre.incomplete || g_query_error_count > 0 ||
	    verify_pre.match_count != 1 || !verify_pre.match ||
	    node_is_protected(verify_pre.match)) {
		cJSON_AddStringToObject(result, "errorCode", "verification_unresolved");
		cJSON_AddStringToObject(result, "error", "Verification selector did not resolve safely to one element");
		release_search(&verify_pre);
		release_search(&target_search);
		goto action_done;
	}
	if (identity_from_search(&verify_identity, &verify_pre) != 0) {
		cJSON_AddStringToObject(result, "errorCode", "verification_identity_unavailable");
		cJSON_AddStringToObject(result, "error",
			"Could not bind verification to one live AT-SPI object");
		release_search(&verify_pre);
		release_search(&target_search);
		clear_identity(&target_identity);
		goto action_done;
	}
	cJSON *verification = cJSON_CreateObject();
	cJSON_AddItemToObject(verification, "target",
		search_locator(&verify_pre, &verify_selector));
	if (verify_text)
		cJSON_AddStringToObject(verification, "expectedText", expected_text);
	if (verify_state) {
		cJSON_AddStringToObject(verification, "expectedState", expected_state_name);
		cJSON_AddBoolToObject(verification, "expectedStateValue", expected_state_value);
	}
	if (verify_value)
		cJSON_AddNumberToObject(verification, "expectedValue", expected_value);
	if (verify_selection)
		cJSON_AddNumberToObject(verification, "expectedSelectedChildIndex",
		                       expected_child_index);
	AccessibilityVerification before = {0};
	if (evaluate_verification(verify_pre.match,
	    verify_text, expected_text, verify_state, expected_state,
	    expected_state_value, verify_value, expected_value,
	    verify_selection, expected_child_index, &before) != 0 || g_query_error_count > 0) {
		cJSON_AddStringToObject(result, "errorCode", "verification_unreadable");
		cJSON_AddStringToObject(result, "error",
			"Verification postcondition could not be read exactly before mutation");
		clear_verification(&before);
		release_search(&verify_pre);
		release_search(&target_search);
		goto action_done;
	}
	if (before.text_observed)
		cJSON_AddBoolToObject(verification, "beforeTextObserved", 1);
	if (before.state_observed)
		cJSON_AddBoolToObject(verification, "beforeStateValue",
			before.state_value);
	if (before.value_observed)
		cJSON_AddNumberToObject(verification, "beforeValue", before.value);
	if (before.selection_observed)
		cJSON_AddBoolToObject(verification, "beforeSelected", before.selected);
	cJSON_AddBoolToObject(verification, "alreadySatisfied", before.satisfied);
	cJSON_AddItemToObject(result, "verification", verification);
	release_search(&verify_pre);
	int preflight_action_index = -1;
	int preflight_action_matches = 0;
	if (strcmp(operation, "invoke") == 0) {
		const char *action_name = cJSON_GetObjectItem(params, "action")->valuestring;
		if (resolve_action_index(target_search.match, action_name,
		    &preflight_action_matches, &preflight_action_index) != 0) {
			cJSON_AddStringToObject(result, "errorCode",
				preflight_action_matches > 1 ? "action_ambiguous" :
				"action_not_found");
			cJSON_AddStringToObject(result, "error",
				"Named action could not be resolved completely and uniquely");
			cJSON_AddStringToObject(result, "action", action_name);
			cJSON_AddNumberToObject(result, "actionMatchCount",
				preflight_action_matches);
			clear_verification(&before);
			release_search(&target_search);
			clear_identity(&target_identity);
			clear_identity(&verify_identity);
			goto action_done;
		}
	}
	if (before.satisfied) {
		cJSON_ReplaceItemInObject(result, "verified", cJSON_CreateBool(1));
		cJSON_ReplaceItemInObject(result, "success", cJSON_CreateBool(1));
		cJSON_AddNumberToObject(result, "pollCount", 0);
		cJSON_AddBoolToObject(verification, "satisfied", 1);
		if (before.text_observed)
			cJSON_AddBoolToObject(verification, "actualTextObserved", 1);
		if (before.state_observed)
			cJSON_AddBoolToObject(verification, "actualStateValue",
				before.state_value);
		if (before.value_observed)
			cJSON_AddNumberToObject(verification, "actualValue", before.value);
		if (before.selection_observed)
			cJSON_AddBoolToObject(verification, "actualSelected", before.selected);
		clear_verification(&before);
		release_search(&target_search);
		clear_identity(&target_identity);
		clear_identity(&verify_identity);
		goto action_done;
	}
	clear_verification(&before);

	AccessibilitySearch target_before_action = {
		.application_filter = application,
		.window_filter = window,
		.selector = &target_selector,
		.exact_scope = 1,
		.deadline_ms = deadline_ms,
	};
	resolve_selector(&target_before_action);
	if (target_before_action.incomplete || g_query_error_count > 0 ||
	    target_before_action.match_count != 1 ||
	    !target_before_action.match ||
	    !identity_matches_search(&target_identity, &target_before_action) ||
	    node_is_protected(target_before_action.match)) {
		cJSON_AddStringToObject(result, "errorCode", "target_changed");
		cJSON_AddStringToObject(result, "error",
			"Target identity changed or became unsafe before mutation");
		release_search(&target_before_action);
		release_search(&target_search);
		goto action_done;
	}
	release_search(&target_search);
	target_search = target_before_action;

	enabled = showing = focusable = editable = 0;
	int value_precondition_valid = strcmp(operation, "setValue") != 0 ||
		node_accepts_value(target_search.match, expected_value,
		                   &current_value, &minimum_value, &maximum_value,
		                   &value_increment);
	int selection_precondition_valid = strcmp(operation, "select") != 0 ||
		node_selection(target_search.match, expected_child_index,
		               &selection_child_count, &selection_selected) == 0;
	if (node_state(target_search.match, ATSPI_STATE_ENABLED, &enabled) != 0 ||
	    node_state(target_search.match, ATSPI_STATE_SHOWING, &showing) != 0 ||
	    node_state(target_search.match, ATSPI_STATE_FOCUSABLE, &focusable) != 0 ||
	    node_state(target_search.match, ATSPI_STATE_EDITABLE, &editable) != 0 ||
	    !enabled || !showing ||
	    (strcmp(operation, "focus") == 0 && !focusable) ||
	    (strcmp(operation, "setText") == 0 && !editable) ||
	    !value_precondition_valid || !selection_precondition_valid) {
		cJSON_AddStringToObject(result, "errorCode", "precondition_changed");
		cJSON_AddStringToObject(result, "error",
			"Target preconditions changed before mutation");
		release_search(&target_search);
		goto action_done;
	}

	AccessibilitySearch verify_before_action = {
		.application_filter = application,
		.window_filter = window,
		.selector = &verify_selector,
		.exact_scope = 1,
		.deadline_ms = deadline_ms,
	};
	resolve_selector(&verify_before_action);
	if (verify_before_action.incomplete || g_query_error_count > 0 ||
	    verify_before_action.match_count != 1 ||
	    !verify_before_action.match ||
	    !identity_matches_search(&verify_identity, &verify_before_action) ||
	    node_is_protected(verify_before_action.match)) {
		cJSON_AddStringToObject(result, "errorCode", "verification_changed");
		cJSON_AddStringToObject(result, "error",
			"Verification identity changed or became unreadable before mutation");
		release_search(&verify_before_action);
		release_search(&target_search);
		goto action_done;
	}
	AccessibilityVerification immediate = {0};
	if (evaluate_verification(verify_before_action.match,
	    verify_text, expected_text, verify_state, expected_state,
	    expected_state_value, verify_value, expected_value,
	    verify_selection, expected_child_index, &immediate) != 0 || g_query_error_count > 0) {
		cJSON_AddStringToObject(result, "errorCode", "verification_unreadable");
		cJSON_AddStringToObject(result, "error",
			"Verification postcondition could not be read immediately before mutation");
		clear_verification(&immediate);
		release_search(&verify_before_action);
		release_search(&target_search);
		goto action_done;
	}
	release_search(&verify_before_action);
	if (immediate.satisfied) {
		cJSON_ReplaceItemInObject(result, "verified", cJSON_CreateBool(1));
		cJSON_ReplaceItemInObject(result, "success", cJSON_CreateBool(1));
		cJSON_AddNumberToObject(result, "pollCount", 0);
		cJSON_ReplaceItemInObject(verification, "alreadySatisfied",
			cJSON_CreateBool(1));
		cJSON_AddBoolToObject(verification, "satisfied", 1);
		if (immediate.text_observed)
			cJSON_AddBoolToObject(verification, "actualTextObserved", 1);
		if (immediate.state_observed)
			cJSON_AddBoolToObject(verification, "actualStateValue",
				immediate.state_value);
		if (immediate.value_observed)
			cJSON_AddNumberToObject(verification, "actualValue", immediate.value);
		if (immediate.selection_observed)
			cJSON_AddBoolToObject(verification, "actualSelected", immediate.selected);
		clear_verification(&immediate);
		release_search(&target_search);
		goto action_done;
	}
	clear_verification(&immediate);
	int verification_was_readable = 1;

	AccessibilitySearch target_dispatch = {
		.application_filter = application,
		.window_filter = window,
		.selector = &target_selector,
		.exact_scope = 1,
		.deadline_ms = deadline_ms,
	};
	resolve_selector(&target_dispatch);
	if (target_dispatch.incomplete || g_query_error_count > 0 ||
	    target_dispatch.match_count != 1 || !target_dispatch.match ||
	    !identity_matches_search(&target_identity, &target_dispatch) ||
	    node_is_protected(target_dispatch.match)) {
		cJSON_AddStringToObject(result, "errorCode", "target_changed");
		cJSON_AddStringToObject(result, "error",
			"Target identity changed or became unsafe immediately before mutation");
		release_search(&target_dispatch);
		release_search(&target_search);
		goto action_done;
	}
	enabled = showing = focusable = editable = 0;
	value_precondition_valid = strcmp(operation, "setValue") != 0 ||
		node_accepts_value(target_dispatch.match, expected_value,
		                   &current_value, &minimum_value, &maximum_value,
		                   &value_increment);
	selection_precondition_valid = strcmp(operation, "select") != 0 ||
		node_selection(target_dispatch.match, expected_child_index,
		               &selection_child_count, &selection_selected) == 0;
	if (node_state(target_dispatch.match, ATSPI_STATE_ENABLED, &enabled) != 0 ||
	    node_state(target_dispatch.match, ATSPI_STATE_SHOWING, &showing) != 0 ||
	    node_state(target_dispatch.match, ATSPI_STATE_FOCUSABLE, &focusable) != 0 ||
	    node_state(target_dispatch.match, ATSPI_STATE_EDITABLE, &editable) != 0 ||
	    !enabled || !showing ||
	    (strcmp(operation, "focus") == 0 && !focusable) ||
	    (strcmp(operation, "setText") == 0 && !editable) ||
	    !value_precondition_valid || !selection_precondition_valid) {
		cJSON_AddStringToObject(result, "errorCode", "precondition_changed");
		cJSON_AddStringToObject(result, "error",
			"Target preconditions changed immediately before mutation");
		release_search(&target_dispatch);
		release_search(&target_search);
		goto action_done;
	}
	release_search(&target_search);
	target_search = target_dispatch;

	int final_action_matches = 0;
	const char *action_name = NULL;
	if (strcmp(operation, "invoke") == 0) {
		action_name = cJSON_GetObjectItem(params, "action")->valuestring;
		prepared_action = prepare_invoke(target_search.match, action_name,
			&final_action_matches, &prepared_action_index);
		cJSON_AddStringToObject(result, "action", action_name);
		cJSON_AddNumberToObject(result, "actionMatchCount", final_action_matches);
		if (!prepared_action) {
			cJSON_AddStringToObject(result, "errorCode",
				final_action_matches > 1 ? "action_ambiguous" :
				"action_not_found");
			cJSON_AddStringToObject(result, "error",
				"Named action changed or could not be resolved completely before mutation");
			release_search(&target_search);
			goto action_done;
		}
	}

	AccessibilitySearch verify_dispatch = {
		.application_filter = application,
		.window_filter = window,
		.selector = &verify_selector,
		.exact_scope = 1,
		.deadline_ms = deadline_ms,
	};
	resolve_selector(&verify_dispatch);
	if (verify_dispatch.incomplete || g_query_error_count > 0 ||
	    verify_dispatch.match_count != 1 || !verify_dispatch.match ||
	    !identity_matches_search(&verify_identity, &verify_dispatch) ||
	    node_is_protected(verify_dispatch.match)) {
		cJSON_AddStringToObject(result, "errorCode", "verification_changed");
		cJSON_AddStringToObject(result, "error",
			"Verification identity changed or became unreadable immediately before mutation");
		release_search(&verify_dispatch);
		release_search(&target_search);
		goto action_done;
	}
	AccessibilityVerification dispatch_verification = {0};
	if (evaluate_verification(verify_dispatch.match,
	    verify_text, expected_text, verify_state, expected_state,
	    expected_state_value, verify_value, expected_value,
	    verify_selection, expected_child_index, &dispatch_verification) != 0 ||
	    g_query_error_count > 0) {
		cJSON_AddStringToObject(result, "errorCode", "verification_unreadable");
		cJSON_AddStringToObject(result, "error",
			"Verification postcondition could not be read at dispatch");
		clear_verification(&dispatch_verification);
		release_search(&verify_dispatch);
		release_search(&target_search);
		goto action_done;
	}
	release_search(&verify_dispatch);
	if (dispatch_verification.satisfied) {
		cJSON_ReplaceItemInObject(result, "verified", cJSON_CreateBool(1));
		cJSON_ReplaceItemInObject(result, "success", cJSON_CreateBool(1));
		cJSON_AddNumberToObject(result, "pollCount", 0);
		cJSON_ReplaceItemInObject(verification, "alreadySatisfied",
			cJSON_CreateBool(1));
		cJSON_AddBoolToObject(verification, "satisfied", 1);
		if (dispatch_verification.text_observed)
			cJSON_AddBoolToObject(verification, "actualTextObserved", 1);
		if (dispatch_verification.state_observed)
			cJSON_AddBoolToObject(verification, "actualStateValue",
				dispatch_verification.state_value);
		if (dispatch_verification.value_observed)
			cJSON_AddNumberToObject(verification, "actualValue",
			                        dispatch_verification.value);
		if (dispatch_verification.selection_observed)
			cJSON_AddBoolToObject(verification, "actualSelected",
			                      dispatch_verification.selected);
		clear_verification(&dispatch_verification);
		release_search(&target_search);
		goto action_done;
	}
	clear_verification(&dispatch_verification);

	AccessibilityMutation mutation = {0};
	if (strcmp(operation, "setText") == 0) {
		mutation = perform_set_text(target_search.match, expected_text);
	} else if (strcmp(operation, "setValue") == 0) {
		mutation = perform_set_value(target_search.match, expected_value);
	} else if (strcmp(operation, "select") == 0) {
		mutation = perform_select(target_search.match, expected_child_index);
	} else if (strcmp(operation, "focus") == 0) {
		mutation = perform_focus(target_search.match);
	} else {
		mutation = perform_prepared_invoke(prepared_action,
			prepared_action_index);
	}
	release_search(&target_search);
	cJSON_ReplaceItemInObject(result, "mutationIssued",
		cJSON_CreateBool(mutation.issued));
	cJSON_ReplaceItemInObject(result, "actionApplied",
		cJSON_CreateBool(mutation.reported_success));
	cJSON_ReplaceItemInObject(result, "actionOutcomeUnknown",
		cJSON_CreateBool(mutation.outcome_unknown));
	cJSON_ReplaceItemInObject(result, "actionOutcome", cJSON_CreateString(
		!mutation.issued ? "not_issued" :
		mutation.outcome_unknown ? "unknown" :
		mutation.reported_success ? "reported_applied" : "reported_failed"));
	if (!mutation.issued) {
		if (!cJSON_GetObjectItem(result, "errorCode")) {
			cJSON_AddStringToObject(result, "errorCode", "action_not_issued");
			cJSON_AddStringToObject(result, "error",
				"AT-SPI mutation could not be issued before the deadline");
		}
		goto action_done;
	}

	int polls = 0;
	int verified = 0;
	int verification_incomplete = 0;
	AccessibilityVerification observed = {0};
	int verification_error_base = g_query_error_count;
	while (monotonic_ms() < deadline_ms) {
		AccessibilitySearch post = {
			.application_filter = application,
			.window_filter = window,
			.selector = &verify_selector,
			.exact_scope = 1,
			.deadline_ms = deadline_ms,
		};
		resolve_selector(&post);
		polls++;
		if (post.match_count > 1 ||
		    (post.match_count == 1 &&
		     (!identity_matches_search(&verify_identity, &post) ||
		      node_is_protected(post.match)))) {
			verification_incomplete = 1;
			release_search(&post);
			break;
		}
		int new_query_error = g_query_error_count > verification_error_base;
		if (post.incomplete && !new_query_error) {
			if (monotonic_ms() < deadline_ms ||
			    !verification_was_readable)
				verification_incomplete = 1;
			release_search(&post);
			break;
		}
		if (new_query_error) {
			verification_error_base = g_query_error_count;
			release_search(&post);
			long long retry_remaining = deadline_ms - monotonic_ms();
			if (retry_remaining <= 0) {
				if (!verification_was_readable)
					verification_incomplete = 1;
				break;
			}
			int retry_sleep = retry_remaining < 10
				? (int)retry_remaining : 10;
			if (retry_sleep > 0)
				usleep((useconds_t)retry_sleep * 1000U);
			continue;
		}
		if (post.match_count == 1 && post.match) {
			if (evaluate_verification(post.match,
			    verify_text, expected_text, verify_state, expected_state,
			    expected_state_value, verify_value, expected_value,
			    verify_selection, expected_child_index, &observed) != 0) {
				verification_incomplete = 1;
				release_search(&post);
				break;
			}
			verified = observed.satisfied;
			verification_was_readable = 1;
		}
		release_search(&post);
		if (verified) break;
		long long remaining_ms = deadline_ms - monotonic_ms();
		if (remaining_ms <= 0) break;
		int sleep_ms = remaining_ms < 10 ? (int)remaining_ms : 10;
		if (sleep_ms > 0) usleep((useconds_t)sleep_ms * 1000U);
	}
	if (observed.text_observed)
		cJSON_AddBoolToObject(verification, "actualTextObserved", 1);
	if (observed.state_observed)
		cJSON_AddBoolToObject(verification, "actualStateValue",
			observed.state_value);
	if (observed.value_observed)
		cJSON_AddNumberToObject(verification, "actualValue", observed.value);
	if (observed.selection_observed)
		cJSON_AddBoolToObject(verification, "actualSelected", observed.selected);
	if (!verified && g_query_timed_out &&
	    verification_was_readable)
		verification_incomplete = 0;
	cJSON_AddBoolToObject(verification, "satisfied", verified);
	cJSON_AddNumberToObject(result, "pollCount", polls);
	cJSON_ReplaceItemInObject(result, "verified", cJSON_CreateBool(verified));
	cJSON_ReplaceItemInObject(result, "success", cJSON_CreateBool(verified));
	if (!verified) {
		cJSON_AddStringToObject(result, "errorCode",
			mutation.outcome_unknown ? "action_outcome_unknown" :
			verification_incomplete ? "verification_incomplete" :
			"verification_failed");
		cJSON_AddStringToObject(result, "error",
			mutation.outcome_unknown
				? "Mutation outcome is unknown and the postcondition was not verified; do not retry blindly"
				: "Action was issued but the postcondition was not verified before timeout");
	}
	clear_verification(&observed);

action_done:
	if (prepared_action) g_object_unref(prepared_action);
	clear_identity(&target_identity);
	clear_identity(&verify_identity);
	cJSON_AddNumberToObject(result, "queryErrorCount", g_query_error_count);
	if (g_query_error_count > 0)
		cJSON_AddStringToObject(result, "lastError", g_last_query_error);
	cJSON_AddNumberToObject(result, "elapsedMs", monotonic_ms() - started_ms);
	end_query();
#else
	(void)params;
	cJSON_AddStringToObject(result, "errorCode", "backend_unavailable");
	cJSON_AddStringToObject(result, "error", "AT-SPI accessibility support was not compiled");
#endif
	return result;
}