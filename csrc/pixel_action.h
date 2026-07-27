/* Deskpal shared-seat compatibility pixel actions. */
#ifndef DESKPAL_PIXEL_ACTION_H
#define DESKPAL_PIXEL_ACTION_H

#include <stddef.h>

int pixel_click_window(unsigned long window_id, int x, int y,
                       int button, int repeat,
                       char *error, size_t error_len);

#endif /* DESKPAL_PIXEL_ACTION_H */
