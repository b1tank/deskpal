/*
 * deskpal — MCP protocol: JSON-RPC 2.0 over stdio
 *
 * Implements the Model Context Protocol server lifecycle:
 * - initialize / initialized handshake
 * - tools/list
 * - tools/call
 *
 * Copyright (c) 2026 deskpal contributors
 * SPDX-License-Identifier: MIT
 */
#include "mcp.h"
#include "sessions.h"
#include "control.h"
#include "tools.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ── Tool registry ────────────────────────────────────────────────────────── */

#define MAX_TOOLS 64
static McpTool  g_tools[MAX_TOOLS];
static int      g_tool_count = 0;
static cJSON   *g_schemas[MAX_TOOLS]; /* owned schema objects */

void mcp_register_tool(const char *name, const char *description,
                       const char *schema_json, mcp_tool_handler_t handler)
{
	if (g_tool_count >= MAX_TOOLS) return;

	McpTool *t = &g_tools[g_tool_count];
	t->name = name;
	t->description = description;
	t->handler = handler;

	cJSON *schema = cJSON_Parse(schema_json);
	if (!schema) {
		schema = cJSON_CreateObject();
		cJSON_AddStringToObject(schema, "type", "object");
	}
	g_schemas[g_tool_count] = schema;
	if (strcmp(name, "launch_isolated_app") != 0 &&
	    strcmp(name, "close_isolated_session") != 0 &&
	    strcmp(name, "accessibility_status") != 0 &&
	    strcmp(name, "get_accessibility_tree") != 0 &&
	    strcmp(name, "get_focused_element") != 0 &&
	    strcmp(name, "accessibility_action") != 0 &&
	    strcmp(name, "agent_semantic_press") != 0 &&
	    strcmp(name, "agent_semantic_set_text") != 0 &&
	    strcmp(name, "agent_semantic_set_value") != 0 &&
	    strcmp(name, "agent_semantic_select") != 0) {
		cJSON *properties = cJSON_GetObjectItem(schema, "properties");
		if (properties && cJSON_IsObject(properties) &&
		    !cJSON_GetObjectItem(properties, "sessionId")) {
			cJSON *session_schema = cJSON_CreateObject();
			cJSON_AddStringToObject(session_schema, "type", "string");
			cJSON_AddStringToObject(session_schema, "description",
				"Isolated session returned by launch_isolated_app. Omit to use the user's desktop.");
			cJSON_AddItemToObject(properties, "sessionId", session_schema);
		}
	}
	t->input_schema = schema;
	g_tool_count++;
}

const McpTool *mcp_find_tool(const char *name)
{
	for (int i = 0; i < g_tool_count; i++) {
		if (strcmp(g_tools[i].name, name) == 0)
			return &g_tools[i];
	}
	return NULL;
}

cJSON *mcp_tools_list(void)
{
	cJSON *arr = cJSON_CreateArray();
	for (int i = 0; i < g_tool_count; i++) {
		cJSON *tool = cJSON_CreateObject();
		cJSON_AddStringToObject(tool, "name", g_tools[i].name);
		cJSON_AddStringToObject(tool, "description", g_tools[i].description);
		cJSON_AddItemReferenceToObject(tool, "inputSchema",
		                               (cJSON *)g_tools[i].input_schema);
		cJSON_AddItemToArray(arr, tool);
	}
	return arr;
}

/* ── Response builders ────────────────────────────────────────────────────── */

cJSON *mcp_text_result(const char *text)
{
	cJSON *result = cJSON_CreateObject();
	cJSON *content = cJSON_CreateArray();
	cJSON *item = cJSON_CreateObject();
	cJSON_AddStringToObject(item, "type", "text");
	cJSON_AddStringToObject(item, "text", text);
	cJSON_AddItemToArray(content, item);
	cJSON_AddItemToObject(result, "content", content);
	return result;
}

cJSON *mcp_tool_error_result(const char *text)
{
	cJSON *result = mcp_text_result(text);
	cJSON_AddBoolToObject(result, "isError", 1);
	return result;
}

cJSON *mcp_image_result(const char *base64_png, const char *mime)
{
	cJSON *result = cJSON_CreateObject();
	cJSON *content = cJSON_CreateArray();
	cJSON *item = cJSON_CreateObject();
	cJSON_AddStringToObject(item, "type", "image");
	cJSON_AddStringToObject(item, "data", base64_png);
	cJSON_AddStringToObject(item, "mimeType", mime ? mime : "image/png");
	cJSON_AddItemToArray(content, item);
	cJSON_AddItemToObject(result, "content", content);
	return result;
}

cJSON *mcp_error(int code, const char *message)
{
	cJSON *err = cJSON_CreateObject();
	cJSON_AddNumberToObject(err, "code", code);
	cJSON_AddStringToObject(err, "message", message);
	return err;
}

/* ── JSON-RPC transport ───────────────────────────────────────────────────── */

static int send_response(const cJSON *id, cJSON *result, cJSON *error)
{
	cJSON *resp = cJSON_CreateObject();
	cJSON_AddStringToObject(resp, "jsonrpc", "2.0");

	if (id) {
		cJSON_AddItemReferenceToObject(resp, "id", (cJSON *)id);
	}

	if (error) {
		cJSON_AddItemToObject(resp, "error", error);
	} else if (result) {
		cJSON_AddItemToObject(resp, "result", result);
	} else {
		cJSON_AddItemToObject(resp, "result", cJSON_CreateObject());
	}

	char *json = cJSON_PrintUnformatted(resp);
	int failed = 0;
	if (json) {
		if (fprintf(stdout, "%s\n", json) < 0 ||
		    fflush(stdout) != 0 || ferror(stdout)) {
			failed = 1;
		}
		free(json);
	} else {
		failed = 1;
	}
	cJSON_Delete(resp);
	return failed ? -1 : 0;
}

/* ── Request handlers ─────────────────────────────────────────────────────── */

static cJSON *handle_initialize(const cJSON *params)
{
	(void)params;

	cJSON *result = cJSON_CreateObject();
	cJSON_AddStringToObject(result, "protocolVersion", "2024-11-05");

	/* Server info */
	cJSON *info = cJSON_CreateObject();
	cJSON_AddStringToObject(info, "name", "deskpal");
	cJSON_AddStringToObject(info, "version", "0.2.0");
	cJSON_AddItemToObject(result, "serverInfo", info);

	/* Capabilities */
	cJSON *caps = cJSON_CreateObject();
	cJSON *tools_cap = cJSON_CreateObject();
	cJSON_AddItemToObject(caps, "tools", tools_cap);
	cJSON_AddItemToObject(result, "capabilities", caps);

	return result;
}

static cJSON *handle_tools_list(void)
{
	cJSON *result = cJSON_CreateObject();
	cJSON *tools = mcp_tools_list();
	cJSON_AddItemToObject(result, "tools", tools);
	return result;
}

static int integer_in_range(const cJSON *arguments, const char *key,
	                        long long minimum, long long maximum)
{
	const cJSON *item = arguments ? cJSON_GetObjectItem(arguments, key) : NULL;
	if (!item) return 1;
	if (!cJSON_IsNumber(item) || !isfinite(item->valuedouble) ||
	    floor(item->valuedouble) != item->valuedouble ||
	    item->valuedouble < (double)minimum ||
	    item->valuedouble > (double)maximum)
		return 0;
	return 1;
}

static int valid_button(const cJSON *arguments)
{
	const cJSON *button = arguments
		? cJSON_GetObjectItem(arguments, "button") : NULL;
	if (!button) return 1;
	if (cJSON_IsString(button))
		return strcmp(button->valuestring, "left") == 0 ||
		       strcmp(button->valuestring, "middle") == 0 ||
		       strcmp(button->valuestring, "right") == 0;
	return integer_in_range(arguments, "button", 1, 3);
}

static int boolean_if_present(const cJSON *arguments, const char *key)
{
	const cJSON *item = arguments ? cJSON_GetObjectItem(arguments, key) : NULL;
	return !item || cJSON_IsBool(item);
}

static int bounded_string_if_present(const cJSON *arguments, const char *key,
	                                  size_t maximum)
{
	const cJSON *item = arguments ? cJSON_GetObjectItem(arguments, key) : NULL;
	return !item || (cJSON_IsString(item) && item->valuestring[0] &&
		strlen(item->valuestring) <= maximum);
}

static int valid_accessibility_path(const cJSON *path)
{
	if (!path) return 1;
	if (!cJSON_IsArray(path) || cJSON_GetArraySize(path) > 32) return 0;
	for (int i = 0; i < cJSON_GetArraySize(path); i++) {
		const cJSON *index = cJSON_GetArrayItem(path, i);
		if (!cJSON_IsNumber(index) || !isfinite(index->valuedouble) ||
		    floor(index->valuedouble) != index->valuedouble ||
		    index->valuedouble < 0 || index->valuedouble > 4096)
			return 0;
	}
	return 1;
}

static int valid_accessibility_selector(const cJSON *selector)
{
	if (!selector || !cJSON_IsObject(selector)) return 0;
	if (!bounded_string_if_present(selector, "role", 128)) return 0;
	const cJSON *role = cJSON_GetObjectItem(selector, "role");
	if (!role) return 0;
	if (!bounded_string_if_present(selector, "name", 512)) return 0;
	const cJSON *name = cJSON_GetObjectItem(selector, "name");
	const cJSON *path = cJSON_GetObjectItem(selector, "path");
	if (!valid_accessibility_path(path)) return 0;
	if (path) {
		const cJSON *bus_name = cJSON_GetObjectItem(selector, "busName");
		const cJSON *object_path = cJSON_GetObjectItem(selector, "objectPath");
		if (!bus_name || !cJSON_IsString(bus_name) ||
		    !bus_name->valuestring[0] || strlen(bus_name->valuestring) > 255 ||
		    !object_path || !cJSON_IsString(object_path) ||
		    !object_path->valuestring[0] ||
		    strlen(object_path->valuestring) > 1024 ||
		    !integer_in_range(selector, "processId", 1, 2147483647))
			return 0;
	}
	return name || path;
}

static int valid_accessibility_state(const char *state)
{
	static const char *states[] = {
		"focused", "checked", "selected", "enabled", "editable",
		"showing", "expanded", NULL
	};
	for (int i = 0; states[i]; i++)
		if (strcmp(state, states[i]) == 0) return 1;
	return 0;
}

static cJSON *validate_tool_arguments(const char *tool_name,
	                                  const cJSON *arguments)
{
	if (arguments && !cJSON_IsObject(arguments))
		return mcp_tool_error_result("arguments must be an object");

	if (strcmp(tool_name, "click") == 0 ||
	    strcmp(tool_name, "mouse_move") == 0 ||
	    strcmp(tool_name, "mouse_down") == 0) {
		if (!integer_in_range(arguments, "x", -32768, 32768) ||
		    !integer_in_range(arguments, "y", -32768, 32768))
			return mcp_tool_error_result("x/y must be integral pixels between -32768 and 32768");
	}
	if ((strcmp(tool_name, "click") == 0 ||
	     strcmp(tool_name, "click_text") == 0 ||
	     strcmp(tool_name, "mouse_down") == 0 ||
	     strcmp(tool_name, "mouse_up") == 0 ||
	    strcmp(tool_name, "drag") == 0) && !valid_button(arguments))
		return mcp_tool_error_result(
			"button must be left/middle/right or an integer between 1 and 3");
	if (strcmp(tool_name, "drag") == 0) {
		for (int i = 0; i < 4; i++) {
			const char *keys[] = { "fromX", "fromY", "toX", "toY" };
			if (!integer_in_range(arguments, keys[i], -32768, 32768))
				return mcp_tool_error_result("drag coordinates must be integral pixels between -32768 and 32768");
		}
		if (!integer_in_range(arguments, "steps", 1, 500))
			return mcp_tool_error_result("steps must be an integer between 1 and 500");
	}
	if (strcmp(tool_name, "scroll") == 0 &&
	    !integer_in_range(arguments, "clicks", 1, 100))
		return mcp_tool_error_result("clicks must be an integer between 1 and 100");
	if (strcmp(tool_name, "resize_window") == 0 &&
	    (!integer_in_range(arguments, "width", 1, 16384) ||
	     !integer_in_range(arguments, "height", 1, 16384)))
		return mcp_tool_error_result("width/height must be integers between 1 and 16384");
	if ((strcmp(tool_name, "launch_app") == 0 ||
	     strcmp(tool_name, "launch_isolated_app") == 0 ||
	     strcmp(tool_name, "wait_for_window") == 0) &&
	    !integer_in_range(arguments, "timeout", 1, 120))
		return mcp_tool_error_result("timeout must be an integer between 1 and 120 seconds");
	if (strcmp(tool_name, "exec") == 0 &&
	    !integer_in_range(arguments, "timeoutMs", 1, 60000))
		return mcp_tool_error_result("timeoutMs must be an integer between 1 and 60000");
	if (strcmp(tool_name, "hover_text") == 0 &&
	    !integer_in_range(arguments, "settleMs", 0, 10000))
		return mcp_tool_error_result("settleMs must be an integer between 0 and 10000");
	if (strcmp(tool_name, "click_text") == 0) {
		if (!integer_in_range(arguments, "occurrence", 1, 1000))
			return mcp_tool_error_result("occurrence must be an integer between 1 and 1000");
		const cJSON *offset = arguments
			? cJSON_GetObjectItem(arguments, "offset") : NULL;
		if (offset && (!cJSON_IsObject(offset) ||
		    !integer_in_range(offset, "x", -4096, 4096) ||
		    !integer_in_range(offset, "y", -4096, 4096)))
			return mcp_tool_error_result("offset x/y must be integral pixels between -4096 and 4096");
	}
	if ((strcmp(tool_name, "screenshot") == 0 ||
	     strcmp(tool_name, "get_app_state") == 0) &&
	    (!integer_in_range(arguments, "maxWidth", 0, 8192) ||
	     !integer_in_range(arguments, "maxHeight", 0, 8192)))
		return mcp_tool_error_result("maxWidth/maxHeight must be integers between 0 and 8192");
	if (strcmp(tool_name, "get_app_state") == 0) {
		if (!integer_in_range(arguments, "semanticMaxDepth", 1, 8) ||
		    !integer_in_range(arguments, "semanticMaxNodes", 1, 300))
			return mcp_tool_error_result(
				"semanticMaxDepth must be an integer between 1 and 8 and semanticMaxNodes between 1 and 300");
		if (!bounded_string_if_present(arguments, "windowId", 64) ||
		    !bounded_string_if_present(arguments, "windowName", 255))
			return mcp_tool_error_result(
				"windowId/windowName must be non-empty bounded strings");
		if (!boolean_if_present(arguments, "includeText") ||
		    !boolean_if_present(arguments, "includeAttributes"))
			return mcp_tool_error_result(
				"includeText/includeAttributes must be booleans");
	}
	if (strcmp(tool_name, "read_screen_text") == 0) {
		const cJSON *region = arguments
			? cJSON_GetObjectItem(arguments, "region") : NULL;
		if (region && (!cJSON_IsObject(region) ||
		    !integer_in_range(region, "x", 0, 32768) ||
		    !integer_in_range(region, "y", 0, 32768) ||
		    !integer_in_range(region, "width", 1, 16384) ||
		    !integer_in_range(region, "height", 1, 16384)))
			return mcp_tool_error_result(
				"region x/y must be non-negative integral pixels and width/height between 1 and 16384");
	}
	if (strcmp(tool_name, "read_file") == 0 &&
	    !integer_in_range(arguments, "maxBytes", 1, 16 * 1024 * 1024))
		return mcp_tool_error_result("maxBytes must be an integer between 1 and 16777216");
	if (strcmp(tool_name, "get_accessibility_tree") == 0 &&
	    (!integer_in_range(arguments, "maxDepth", 1, 16) ||
	     !integer_in_range(arguments, "maxNodes", 1, 1000)))
		return mcp_tool_error_result(
			"maxDepth must be an integer between 1 and 16 and maxNodes between 1 and 1000");
	if (strcmp(tool_name, "get_accessibility_tree") == 0 ||
	    strcmp(tool_name, "get_focused_element") == 0 ||
	    strcmp(tool_name, "accessibility_action") == 0) {
		const char *keys[] = { "application", "window" };
		for (int i = 0; i < 2; i++) {
			if (!bounded_string_if_present(arguments, keys[i], 512))
				return mcp_tool_error_result(
					"application/window filters must be non-empty strings of at most 512 bytes");
		}
		if (!boolean_if_present(arguments, "includeText") ||
		    !boolean_if_present(arguments, "includeOffscreen") ||
		    !boolean_if_present(arguments, "includeAttributes"))
			return mcp_tool_error_result(
				"includeText/includeOffscreen/includeAttributes must be booleans when provided");
	}
	if (strcmp(tool_name, "accessibility_action") == 0) {
		const cJSON *application = arguments
			? cJSON_GetObjectItem(arguments, "application") : NULL;
		const cJSON *window = arguments
			? cJSON_GetObjectItem(arguments, "window") : NULL;
		if (!application && !window)
			return mcp_tool_error_result(
				"accessibility_action requires application or window scope");
		const cJSON *target = arguments
			? cJSON_GetObjectItem(arguments, "target") : NULL;
		if (!valid_accessibility_selector(target))
			return mcp_tool_error_result(
				"target requires role and a non-empty name or bounded integer path");
		const cJSON *operation = arguments
			? cJSON_GetObjectItem(arguments, "operation") : NULL;
		if (!operation || !cJSON_IsString(operation) ||
		    (strcmp(operation->valuestring, "setText") != 0 &&
		     strcmp(operation->valuestring, "setValue") != 0 &&
		     strcmp(operation->valuestring, "select") != 0 &&
		     strcmp(operation->valuestring, "focus") != 0 &&
		     strcmp(operation->valuestring, "invoke") != 0))
			return mcp_tool_error_result(
				"operation must be setText, setValue, select, focus, or invoke");
		if (!integer_in_range(arguments, "timeoutMs", 1, 5000))
			return mcp_tool_error_result(
				"timeoutMs must be an integer between 1 and 5000");
		const cJSON *verify = cJSON_GetObjectItem(arguments, "verify");
		if (strcmp(operation->valuestring, "setText") == 0) {
			const cJSON *value = cJSON_GetObjectItem(arguments, "value");
			if (!value || !cJSON_IsString(value) ||
			    strlen(value->valuestring) > 2048)
				return mcp_tool_error_result(
					"setText requires value with at most 2048 bytes");
			if (verify)
				return mcp_tool_error_result(
					"setText uses automatic text verification and does not accept verify");
		} else if (strcmp(operation->valuestring, "setValue") == 0) {
			const cJSON *value = cJSON_GetObjectItem(arguments, "value");
			if (!value || !cJSON_IsNumber(value) || !isfinite(value->valuedouble))
				return mcp_tool_error_result(
					"setValue requires one finite numeric value");
			if (verify)
				return mcp_tool_error_result(
					"setValue uses automatic numeric verification and does not accept verify");
		} else if (strcmp(operation->valuestring, "select") == 0) {
			const cJSON *value = cJSON_GetObjectItem(arguments, "value");
			if (!value || !cJSON_IsNumber(value) ||
			    !isfinite(value->valuedouble) ||
			    floor(value->valuedouble) != value->valuedouble ||
			    value->valuedouble < 0 || value->valuedouble > 4096)
				return mcp_tool_error_result(
					"select requires one child index between 0 and 4096");
			if (verify)
				return mcp_tool_error_result(
					"select uses automatic selected-child verification and does not accept verify");
		} else if (strcmp(operation->valuestring, "focus") == 0) {
			if (verify)
				return mcp_tool_error_result(
					"focus uses automatic focused-state verification and does not accept verify");
		} else {
			if (!bounded_string_if_present(arguments, "action", 128) ||
			    !cJSON_GetObjectItem(arguments, "action"))
				return mcp_tool_error_result(
					"invoke requires a named action with at most 128 bytes");
			if (!valid_accessibility_selector(verify))
				return mcp_tool_error_result(
					"invoke requires a verification selector");
			const cJSON *text_equals = cJSON_GetObjectItem(verify, "textEquals");
			const cJSON *state = cJSON_GetObjectItem(verify, "state");
			const cJSON *state_value = cJSON_GetObjectItem(verify, "stateValue");
			if (!text_equals && !state)
				return mcp_tool_error_result(
					"verify requires textEquals and/or state");
			if (text_equals && (!cJSON_IsString(text_equals) ||
			    strlen(text_equals->valuestring) > 2048))
				return mcp_tool_error_result(
					"verify.textEquals must be a string of at most 2048 bytes");
			if (state_value && !state)
				return mcp_tool_error_result(
					"verify.stateValue is only valid with verify.state");
			if (state) {
				if (!cJSON_IsString(state) ||
				    !valid_accessibility_state(state->valuestring) ||
				    !state_value || !cJSON_IsBool(state_value))
					return mcp_tool_error_result(
						"verify.state requires a supported state and boolean stateValue");
			}
		}
	}
	if (strcmp(tool_name, "agent_semantic_press") == 0) {
		if (!bounded_string_if_present(arguments, "captureId", 63) ||
		    !cJSON_GetObjectItem(arguments, "captureId") ||
		    !bounded_string_if_present(arguments, "action", 128) ||
		    !cJSON_GetObjectItem(arguments, "action") ||
		    !bounded_string_if_present(arguments, "cursorId", 40) ||
		    !bounded_string_if_present(arguments, "color", 7) ||
		    !bounded_string_if_present(arguments, "label", 48) ||
		    !integer_in_range(arguments, "timeoutMs", 1, 5000))
			return mcp_tool_error_result(
				"agent_semantic_press has invalid capture, action, cursor style, or timeout fields");
		const cJSON *target = cJSON_GetObjectItem(arguments, "target");
		if (!valid_accessibility_selector(target) ||
		    !cJSON_GetObjectItem(target, "path"))
			return mcp_tool_error_result(
				"target requires role, path, busName, objectPath, and processId");
		const cJSON *verify = cJSON_GetObjectItem(arguments, "verify");
		if (!valid_accessibility_selector(verify))
			return mcp_tool_error_result(
				"verify requires role and a non-empty name or complete path");
		const cJSON *text_equals = cJSON_GetObjectItem(verify, "textEquals");
		const cJSON *state = cJSON_GetObjectItem(verify, "state");
		const cJSON *state_value = cJSON_GetObjectItem(verify, "stateValue");
		if (!text_equals && !state)
			return mcp_tool_error_result(
				"verify requires textEquals and/or state");
		if (text_equals && (!cJSON_IsString(text_equals) ||
		    strlen(text_equals->valuestring) > 2048))
			return mcp_tool_error_result(
				"verify.textEquals must be a string of at most 2048 bytes");
		if (state && (!cJSON_IsString(state) ||
		    !valid_accessibility_state(state->valuestring) ||
		    !state_value || !cJSON_IsBool(state_value)))
			return mcp_tool_error_result(
				"verify.state requires a supported state and boolean stateValue");
		if (state_value && !state)
			return mcp_tool_error_result(
				"verify.stateValue is only valid with verify.state");
	}
	if (strcmp(tool_name, "agent_semantic_set_text") == 0) {
		if (!bounded_string_if_present(arguments, "captureId", 63) ||
		    !cJSON_GetObjectItem(arguments, "captureId") ||
		    !bounded_string_if_present(arguments, "cursorId", 40) ||
		    !bounded_string_if_present(arguments, "color", 7) ||
		    !bounded_string_if_present(arguments, "label", 48) ||
		    !integer_in_range(arguments, "timeoutMs", 1, 5000))
			return mcp_tool_error_result(
				"agent_semantic_set_text has invalid capture, cursor style, or timeout fields");
		const cJSON *target = cJSON_GetObjectItem(arguments, "target");
		if (!valid_accessibility_selector(target) ||
		    !cJSON_GetObjectItem(target, "path"))
			return mcp_tool_error_result(
				"target requires role, path, busName, objectPath, and processId");
		const cJSON *value = cJSON_GetObjectItem(arguments, "value");
		if (!value || !cJSON_IsString(value) || strlen(value->valuestring) > 2048)
			return mcp_tool_error_result(
				"value must be a string of at most 2048 bytes");
	}
	if (strcmp(tool_name, "agent_semantic_set_value") == 0) {
		if (!bounded_string_if_present(arguments, "captureId", 63) ||
		    !cJSON_GetObjectItem(arguments, "captureId") ||
		    !bounded_string_if_present(arguments, "cursorId", 40) ||
		    !bounded_string_if_present(arguments, "color", 7) ||
		    !bounded_string_if_present(arguments, "label", 48) ||
		    !integer_in_range(arguments, "timeoutMs", 1, 5000))
			return mcp_tool_error_result(
				"agent_semantic_set_value has invalid capture, cursor style, or timeout fields");
		const cJSON *target = cJSON_GetObjectItem(arguments, "target");
		if (!valid_accessibility_selector(target) ||
		    !cJSON_GetObjectItem(target, "path"))
			return mcp_tool_error_result(
				"target requires role, path, busName, objectPath, and processId");
		const cJSON *value = cJSON_GetObjectItem(arguments, "value");
		if (!value || !cJSON_IsNumber(value) || !isfinite(value->valuedouble))
			return mcp_tool_error_result("value must be one finite number");
	}
	if (strcmp(tool_name, "agent_semantic_select") == 0) {
		if (!bounded_string_if_present(arguments, "captureId", 63) ||
		    !cJSON_GetObjectItem(arguments, "captureId") ||
		    !bounded_string_if_present(arguments, "cursorId", 40) ||
		    !bounded_string_if_present(arguments, "color", 7) ||
		    !bounded_string_if_present(arguments, "label", 48) ||
		    !integer_in_range(arguments, "timeoutMs", 1, 5000))
			return mcp_tool_error_result(
				"agent_semantic_select has invalid capture, cursor style, or timeout fields");
		const cJSON *target = cJSON_GetObjectItem(arguments, "target");
		if (!valid_accessibility_selector(target) ||
		    !cJSON_GetObjectItem(target, "path"))
			return mcp_tool_error_result(
				"target requires role, path, busName, objectPath, and processId");
		if (!integer_in_range(arguments, "value", 0, 4096) ||
		    !cJSON_GetObjectItem(arguments, "value"))
			return mcp_tool_error_result(
				"value must be one direct child index between 0 and 4096");
	}
	if (strcmp(tool_name, "type_text") == 0) {
		if (!integer_in_range(arguments, "delay", 0, 1000))
			return mcp_tool_error_result("delay must be an integer between 0 and 1000 ms");
		const cJSON *text = arguments ? cJSON_GetObjectItem(arguments, "text") : NULL;
		long long delay = 12;
		const cJSON *delay_item = arguments
			? cJSON_GetObjectItem(arguments, "delay") : NULL;
		if (delay_item) delay = (long long)delay_item->valuedouble;
		if (text && cJSON_IsString(text) &&
		    (strlen(text->valuestring) > 65536 ||
		     (long long)strlen(text->valuestring) * delay > 60000))
			return mcp_tool_error_result("typed text is limited to 65536 characters and 60 seconds");
	}
	return NULL;
}

static cJSON *handle_tools_call(const cJSON *params)
{
	const cJSON *name_item = cJSON_GetObjectItem(params, "name");
	if (!name_item || !cJSON_IsString(name_item)) {
		return NULL; /* will send error */
	}

	const McpTool *tool = mcp_find_tool(name_item->valuestring);
	if (!tool) {
		return NULL;
	}

	const cJSON *arguments = cJSON_GetObjectItem(params, "arguments");
	const char *tool_name = name_item->valuestring;
	cJSON *argument_error = validate_tool_arguments(tool_name, arguments);
	if (argument_error) return argument_error;
	if ((strcmp(tool_name, "exec") == 0 ||
	     strcmp(tool_name, "launch_app") == 0 ||
	     strcmp(tool_name, "launch_isolated_app") == 0) &&
	    !deskpal_allow_exec) {
		return mcp_tool_error_result(
			"Command execution is disabled. Start deskpal with --allow-exec to enable exec and app launch tools.");
	}

	const cJSON *window_id = arguments
		? cJSON_GetObjectItem(arguments, "windowId") : NULL;
	const cJSON *window_name = arguments
		? cJSON_GetObjectItem(arguments, "windowName") : NULL;
	if (window_id && window_name) {
		return mcp_tool_error_result(
			"Specify only one of windowId or windowName");
	}
	if ((window_id && (!cJSON_IsString(window_id) || !window_id->valuestring[0])) ||
	    (window_name && (!cJSON_IsString(window_name) || !window_name->valuestring[0]))) {
		return mcp_tool_error_result(
			"windowId/windowName must be a non-empty string when provided");
	}
	const cJSON *full_screen = arguments
		? cJSON_GetObjectItem(arguments, "fullScreen") : NULL;
	if (full_screen && cJSON_IsTrue(full_screen) && (window_id || window_name)) {
		return mcp_tool_error_result(
			"fullScreen cannot be combined with windowId or windowName");
	}
	const cJSON *session_id = arguments
		? cJSON_GetObjectItem(arguments, "sessionId") : NULL;
	int session_routable =
		strcmp(tool_name, "launch_isolated_app") != 0 &&
		strcmp(tool_name, "close_isolated_session") != 0 &&
		strcmp(tool_name, "accessibility_status") != 0 &&
		strcmp(tool_name, "get_accessibility_tree") != 0 &&
		strcmp(tool_name, "get_focused_element") != 0 &&
		strcmp(tool_name, "accessibility_action") != 0 &&
		strcmp(tool_name, "agent_semantic_press") != 0 &&
		strcmp(tool_name, "agent_semantic_set_text") != 0 &&
		strcmp(tool_name, "agent_semantic_set_value") != 0 &&
		strcmp(tool_name, "agent_semantic_select") != 0;
	if (session_id && !session_routable &&
	    (strcmp(tool_name, "accessibility_status") == 0 ||
	     strcmp(tool_name, "get_accessibility_tree") == 0 ||
	     strcmp(tool_name, "get_focused_element") == 0 ||
	     strcmp(tool_name, "accessibility_action") == 0 ||
	     strcmp(tool_name, "agent_semantic_press") == 0 ||
	     strcmp(tool_name, "agent_semantic_set_text") == 0 ||
	     strcmp(tool_name, "agent_semantic_set_value") == 0 ||
	     strcmp(tool_name, "agent_semantic_select") == 0))
		return mcp_tool_error_result(
			"Accessibility tools inspect the visible desktop only; sessionId is not supported");
	if (session_id && session_routable &&
	    (!cJSON_IsString(session_id) || !session_id->valuestring[0])) {
		return mcp_tool_error_result(
			"sessionId must be a non-empty string. Omit it only when the tool should target the user's desktop.");
	}
	if (session_id && session_routable) {
		if (strcmp(tool_name, "exec") == 0 ||
		    strcmp(tool_name, "launch_app") == 0) {
			char error[320];
			if (control_acquire(error, sizeof(error)) != 0)
				return mcp_tool_error_result(error);
		}
		return sessions_forward_tool(session_id->valuestring, tool_name, arguments);
	}
	if (control_tool_requires_lock(tool_name)) {
		char error[320];
		if (control_acquire(error, sizeof(error)) != 0)
			return mcp_tool_error_result(error);
	}
	static const cJSON empty_arguments = { .type = cJSON_Object };
	return tool->handler(arguments ? arguments : &empty_arguments);
}

/* ── Main loop ────────────────────────────────────────────────────────────── */

int mcp_run(void)
{
	char *line = NULL;
	size_t line_cap = 0;
	ssize_t line_len;
	int transport_failed = 0;

	while ((line_len = getline(&line, &line_cap, stdin)) > 0) {
		/* Strip trailing newline */
		if (line_len > 0 && line[line_len - 1] == '\n')
			line[line_len - 1] = '\0';

		/* Skip empty lines */
		if (line[0] == '\0') continue;

		cJSON *req = cJSON_Parse(line);
		if (!req) {
			cJSON *err = mcp_error(-32700, "Parse error");
			if (send_response(NULL, NULL, err) != 0) {
				transport_failed = 1;
				break;
			}
			continue;
		}

		const cJSON *id = cJSON_GetObjectItem(req, "id");
		const cJSON *method = cJSON_GetObjectItem(req, "method");
		const cJSON *params = cJSON_GetObjectItem(req, "params");

		if (!method || !cJSON_IsString(method)) {
			cJSON *err = mcp_error(-32600, "Invalid request: missing method");
			int send_failed = send_response(id, NULL, err) != 0;
			cJSON_Delete(req);
			if (send_failed) {
				transport_failed = 1;
				break;
			}
			continue;
		}

		const char *m = method->valuestring;
		int send_failed = 0;

		if (strcmp(m, "initialize") == 0) {
			cJSON *result = handle_initialize(params);
			send_failed = send_response(id, result, NULL) != 0;
		}
		else if (strcmp(m, "notifications/initialized") == 0) {
			/* No response needed for notifications */
		}
		else if (strcmp(m, "tools/list") == 0) {
			cJSON *result = handle_tools_list();
			send_failed = send_response(id, result, NULL) != 0;
		}
		else if (strcmp(m, "tools/call") == 0) {
			cJSON *result = handle_tools_call(params);
			if (result) {
				send_failed = send_response(id, result, NULL) != 0;
			} else {
				const cJSON *name_item = params
					? cJSON_GetObjectItem(params, "name") : NULL;
				char msg[256];
				snprintf(msg, sizeof(msg), "Unknown tool: %s",
				         name_item && cJSON_IsString(name_item)
				         ? name_item->valuestring : "(null)");
				cJSON *err = mcp_error(-32601, msg);
				send_failed = send_response(id, NULL, err) != 0;
			}
		}
		else if (strncmp(m, "notifications/", 14) == 0) {
			/* Silently ignore other notifications */
		}
		else {
			cJSON *err = mcp_error(-32601, "Method not found");
			send_failed = send_response(id, NULL, err) != 0;
		}

		cJSON_Delete(req);
		if (send_failed) {
			transport_failed = 1;
			break;
		}
	}

	free(line);
	return transport_failed ? -1 : 0;
}

/* ── Tool registration entry point (called from tools.c) ─────────────────── */

/* This is a convenience wrapper used by tools.c */
void mcp_register_tool(const char *name, const char *description,
                       const char *schema_json, mcp_tool_handler_t handler);
