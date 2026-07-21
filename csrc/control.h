/*
 * deskpal — visible-desktop control arbitration
 *
 * Copyright (c) 2026 deskpal contributors
 * SPDX-License-Identifier: MIT
 */
#ifndef DESKPAL_CONTROL_H
#define DESKPAL_CONTROL_H

#include <stddef.h>

int control_tool_requires_lock(const char *tool_name);
int control_acquire(char *error, size_t error_len);
int control_export_to_fd(int target_fd, char *error, size_t error_len);
int control_adopt_fd(int fd, char *error, size_t error_len);
void control_cleanup(void);

#endif /* DESKPAL_CONTROL_H */