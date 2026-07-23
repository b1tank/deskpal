/*
 * deskpal — GNOME logical-cursor indicator backend
 *
 * Copyright (c) 2026 deskpal contributors
 * SPDX-License-Identifier: MIT
 */
#ifndef DESKPAL_INDICATOR_H
#define DESKPAL_INDICATOR_H

#include <stddef.h>

#define DESKPAL_INDICATOR_CURSOR_ID_LEN 41

/* Return a newly allocated GetStatus JSON string. The caller must free it. */
int indicator_get_status(char **json, char *error, size_t error_len);

/* Create an owned cursor at the target, or animate an existing owned cursor. */
int indicator_move_owned(const char *cursor_id, int x, int y,
                         const char *color, const char *label,
                         int *created, int *mutation_issued,
                         int *outcome_unknown,
                         char *error, size_t error_len);

/* Hide only a cursor owned by this Deskpal process. */
int indicator_hide_owned(const char *cursor_id, int *hidden,
                         int *mutation_issued, int *outcome_unknown,
                         char *error, size_t error_len);

/* Map a remote namespaced ID back to this process's public cursor ID. */
int indicator_logical_id_for_remote(const char *remote_id,
                                    char *cursor_id, size_t cursor_id_len);

/* Best-effort removal of all cursors owned by this process. */
void indicator_cleanup(void);

#endif /* DESKPAL_INDICATOR_H */
