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
cJSON *tool_get_environment_status(const cJSON *params);
cJSON *tool_release_control(const cJSON *params);
cJSON *tool_get_app_state(const cJSON *params);
cJSON *tool_agent_cursor_status(const cJSON *params);
cJSON *tool_agent_cursor_move(const cJSON *params);
cJSON *tool_agent_cursor_hide(const cJSON *params);
cJSON *tool_agent_semantic_press(const cJSON *params);
cJSON *tool_agent_semantic_set_text(const cJSON *params);
cJSON *tool_agent_semantic_set_value(const cJSON *params);
cJSON *tool_agent_semantic_select(const cJSON *params);
cJSON *tool_agent_semantic_replace_text_range(const cJSON *params);
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

/* Tools surfaced by OTelux self-verify skill — see docs/proposed-tools.md */
cJSON *tool_get_clipboard(const cJSON *params);
cJSON *tool_set_clipboard(const cJSON *params);
cJSON *tool_hover_text(const cJSON *params);
cJSON *tool_read_file(const cJSON *params);
cJSON *tool_exec(const cJSON *params);
cJSON *tool_accessibility_status(const cJSON *params);
cJSON *tool_get_accessibility_tree(const cJSON *params);
cJSON *tool_get_focused_element(const cJSON *params);
cJSON *tool_accessibility_action(const cJSON *params);

/* Security gates set by CLI flags in main.c. Default 0 (off). */
extern int deskpal_allow_fs;
extern int deskpal_allow_exec;

#endif /* DESKPAL_TOOLS_H */
