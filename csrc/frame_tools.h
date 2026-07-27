/* Deskpal capture-bound visual observation MCP tools. */
#ifndef DESKPAL_FRAME_TOOLS_H
#define DESKPAL_FRAME_TOOLS_H

#include "cJSON.h"

cJSON *tool_wait_for_frame_stable(const cJSON *params);
cJSON *tool_verify_frame_change(const cJSON *params);
cJSON *tool_click_and_verify_frame_change(const cJSON *params);

#endif /* DESKPAL_FRAME_TOOLS_H */
