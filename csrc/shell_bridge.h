/*
 * deskpal — bounded read-only GNOME Shell bridge client
 *
 * Copyright (c) 2026 deskpal contributors
 * SPDX-License-Identifier: MIT
 */
#ifndef DESKPAL_SHELL_BRIDGE_H
#define DESKPAL_SHELL_BRIDGE_H

#include "cJSON.h"
#include <stddef.h>

#define DESKPAL_SHELL_BRIDGE_PROTOCOL_VERSION 1
#define DESKPAL_SHELL_BRIDGE_MAX_WINDOWS 256
#define DESKPAL_SHELL_BRIDGE_RESPONSE_LIMIT (256 * 1024)

/* Parse and validate one method response. Used by the transport and fixtures. */
int shell_bridge_parse_response(const char *method, const char *json,
                                cJSON **response,
                                char *error, size_t error_len);

/* Return newly allocated validated response objects owned by the caller. */
int shell_bridge_get_capabilities(cJSON **response,
                                  char *error, size_t error_len);
int shell_bridge_list_windows(cJSON **response,
                              char *error, size_t error_len);
int shell_bridge_get_monitor_layout(cJSON **response,
                                    char *error, size_t error_len);

#endif /* DESKPAL_SHELL_BRIDGE_H */
