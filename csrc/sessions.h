/*
 * deskpal — Isolated Xvfb session management
 *
 * Copyright (c) 2026 deskpal contributors
 * SPDX-License-Identifier: MIT
 */
#ifndef DESKPAL_SESSIONS_H
#define DESKPAL_SESSIONS_H

#include "cJSON.h"

void sessions_init(void);
void sessions_cleanup_all(void);

cJSON *sessions_forward_tool(const char *session_id, const char *tool_name,
                             const cJSON *arguments);
cJSON *tool_launch_isolated_app(const cJSON *params);
cJSON *tool_close_isolated_session(const cJSON *params);

#endif /* DESKPAL_SESSIONS_H */