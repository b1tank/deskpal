/* Deskpal retained exact-window validation shared by capture-bound routes. */
#ifndef DESKPAL_CAPTURE_TARGET_H
#define DESKPAL_CAPTURE_TARGET_H

#include <stddef.h>

#include "captures.h"

int capture_window_still_valid(const DeskpalCapture *capture);
int capture_validate_window(const DeskpalCapture *capture, void *data,
                            char *error, size_t error_len);

#endif /* DESKPAL_CAPTURE_TARGET_H */
