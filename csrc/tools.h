/*
 * deskpal — Tool dispatch
 *
 * Copyright (c) 2026 deskpal contributors
 * SPDX-License-Identifier: MIT
 */
#ifndef DESKPAL_TOOLS_H
#define DESKPAL_TOOLS_H

#include "cJSON.h"

/* Register all tool handlers with the MCP layer. */
void tools_register_all(void);

/* ── Individual tool handlers ─────────────────────────────────────────────── */

cJSON *tool_screenshot(const cJSON *params);
cJSON *tool_list_windows(const cJSON *params);
cJSON *tool_find_window(const cJSON *params);
cJSON *tool_focus_window(const cJSON *params);
cJSON *tool_click(const cJSON *params);
cJSON *tool_click_text(const cJSON *params);
cJSON *tool_read_screen_text(const cJSON *params);
cJSON *tool_launch_app(const cJSON *params);
cJSON *tool_type_text(const cJSON *params);
cJSON *tool_key_press(const cJSON *params);
cJSON *tool_get_window_geometry(const cJSON *params);
cJSON *tool_resize_window(const cJSON *params);
cJSON *tool_wait_for_window(const cJSON *params);
cJSON *tool_mouse_move(const cJSON *params);
cJSON *tool_scroll(const cJSON *params);
cJSON *tool_drag(const cJSON *params);
cJSON *tool_mouse_down(const cJSON *params);
cJSON *tool_mouse_up(const cJSON *params);

#endif /* DESKPAL_TOOLS_H */
