/*
 * deskpal — Playwright for the desktop
 * MCP protocol layer: JSON-RPC 2.0 over stdio
 *
 * Copyright (c) 2026 deskpal contributors
 * SPDX-License-Identifier: MIT
 */
#ifndef DESKPAL_MCP_H
#define DESKPAL_MCP_H

#include "cJSON.h"

/* ── MCP server lifecycle ─────────────────────────────────────────────────── */

/* Run the MCP stdio loop. Blocks until stdin closes. */
int mcp_run(void);

/* Pump bounded transport input while a synchronous handler is running. Returns
 * true when its JSON-RPC request was cancelled or the client disconnected. */
int mcp_request_cancelled(void);

/* ── Response helpers ─────────────────────────────────────────────────────── */

/* Build a text content response for a tool call. Caller owns result. */
cJSON *mcp_text_result(const char *text);

/* Build a failed tool response with isError=true. Caller owns result. */
cJSON *mcp_tool_error_result(const char *text);

/* Build an image content response (base64 PNG). Caller owns result. */
cJSON *mcp_image_result(const char *base64_png, const char *mime);

/* Build a JSON-RPC error response. Caller owns result. */
cJSON *mcp_error(int code, const char *message);

/* ── Tool registration ────────────────────────────────────────────────────── */

/* Handler function type: receives params object, returns result object.
 * The result must be a JSON object with a "content" array. */
typedef cJSON *(*mcp_tool_handler_t)(const cJSON *params);

typedef struct {
	const char          *name;
	const char          *description;
	const cJSON         *input_schema;   /* JSON Schema object (static) */
	mcp_tool_handler_t   handler;
} McpTool;

/* Register all tools. Called during init. */
void mcp_register_tools(void);

/* Look up a tool by name. Returns NULL if not found. */
const McpTool *mcp_find_tool(const char *name);

/* Get the tools list as a JSON array (for capabilities response). */
cJSON *mcp_tools_list(void);

#endif /* DESKPAL_MCP_H */
