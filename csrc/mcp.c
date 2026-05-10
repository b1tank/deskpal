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
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ── Tool registry ────────────────────────────────────────────────────────── */

#define MAX_TOOLS 32
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

static void send_response(const cJSON *id, cJSON *result, cJSON *error)
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
	if (json) {
		fprintf(stdout, "%s\n", json);
		fflush(stdout);
		free(json);
	}
	cJSON_Delete(resp);
}

static void send_notification(const char *method, cJSON *params)
{
	cJSON *msg = cJSON_CreateObject();
	cJSON_AddStringToObject(msg, "jsonrpc", "2.0");
	cJSON_AddStringToObject(msg, "method", method);
	if (params) {
		cJSON_AddItemToObject(msg, "params", params);
	}
	char *json = cJSON_PrintUnformatted(msg);
	if (json) {
		fprintf(stdout, "%s\n", json);
		fflush(stdout);
		free(json);
	}
	cJSON_Delete(msg);
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
	return tool->handler(arguments ? arguments : cJSON_CreateObject());
}

/* ── Main loop ────────────────────────────────────────────────────────────── */

int mcp_run(void)
{
	char *line = NULL;
	size_t line_cap = 0;
	ssize_t line_len;

	while ((line_len = getline(&line, &line_cap, stdin)) > 0) {
		/* Strip trailing newline */
		if (line_len > 0 && line[line_len - 1] == '\n')
			line[line_len - 1] = '\0';

		/* Skip empty lines */
		if (line[0] == '\0') continue;

		cJSON *req = cJSON_Parse(line);
		if (!req) {
			cJSON *err = mcp_error(-32700, "Parse error");
			send_response(NULL, NULL, err);
			continue;
		}

		const cJSON *id = cJSON_GetObjectItem(req, "id");
		const cJSON *method = cJSON_GetObjectItem(req, "method");
		const cJSON *params = cJSON_GetObjectItem(req, "params");

		if (!method || !cJSON_IsString(method)) {
			cJSON *err = mcp_error(-32600, "Invalid request: missing method");
			send_response(id, NULL, err);
			cJSON_Delete(req);
			continue;
		}

		const char *m = method->valuestring;

		if (strcmp(m, "initialize") == 0) {
			cJSON *result = handle_initialize(params);
			send_response(id, result, NULL);
		}
		else if (strcmp(m, "notifications/initialized") == 0) {
			/* No response needed for notifications */
		}
		else if (strcmp(m, "tools/list") == 0) {
			cJSON *result = handle_tools_list();
			send_response(id, result, NULL);
		}
		else if (strcmp(m, "tools/call") == 0) {
			cJSON *result = handle_tools_call(params);
			if (result) {
				send_response(id, result, NULL);
			} else {
				const cJSON *name_item = params
					? cJSON_GetObjectItem(params, "name") : NULL;
				char msg[256];
				snprintf(msg, sizeof(msg), "Unknown tool: %s",
				         name_item && cJSON_IsString(name_item)
				         ? name_item->valuestring : "(null)");
				cJSON *err = mcp_error(-32601, msg);
				send_response(id, NULL, err);
			}
		}
		else if (strncmp(m, "notifications/", 14) == 0) {
			/* Silently ignore other notifications */
		}
		else {
			cJSON *err = mcp_error(-32601, "Method not found");
			send_response(id, NULL, err);
		}

		cJSON_Delete(req);
	}

	free(line);
	return 0;
}

/* ── Tool registration entry point (called from tools.c) ─────────────────── */

/* This is a convenience wrapper used by tools.c */
void mcp_register_tool(const char *name, const char *description,
                       const char *schema_json, mcp_tool_handler_t handler);
