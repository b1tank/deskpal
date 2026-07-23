/*
 * deskpal — Optional AT-SPI accessibility backend
 *
 * Copyright (c) 2026 deskpal contributors
 * SPDX-License-Identifier: MIT
 */
#ifndef DESKPAL_ACCESSIBILITY_H
#define DESKPAL_ACCESSIBILITY_H

#include "cJSON.h"

int  accessibility_init(void);
void accessibility_cleanup(void);
int  accessibility_available(void);

cJSON *accessibility_status(void);
cJSON *accessibility_tree(const char *application_filter,
                          const char *window_filter,
                          int max_depth, int max_nodes,
                          int include_offscreen, int include_text,
                          int include_attributes);
cJSON *accessibility_tree_exact(const char *application,
                                const char *window,
                                int max_depth, int max_nodes,
                                int include_offscreen, int include_text,
                                int include_attributes);
cJSON *accessibility_focused_element(const char *application_filter,
                                     const char *window_filter,
                                     int include_text);
cJSON *accessibility_action(const cJSON *params);

#endif /* DESKPAL_ACCESSIBILITY_H */