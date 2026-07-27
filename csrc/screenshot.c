/*
 * deskpal — Screenshot capture via XCB + PNG encoding
 *
 * Uses XCB GetImage for fast window capture, libpng for encoding,
 * and a custom base64 encoder.
 *
 * Copyright (c) 2026 deskpal contributors
 * SPDX-License-Identifier: MIT
 */
#include "screenshot.h"
#include "x11.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <xcb/xcb.h>
#include <xcb/shm.h>
#include <png.h>
#include <X11/Xlib.h>
#include <X11/Xlib-xcb.h>
#include <xdo.h>

/* We access the xdo display connection through x11 layer.
 * For XCB, we get the connection from the X11 Display. */

extern xdo_t *g_xdo; /* defined in x11.c — make accessible */

/* ── XCB screenshot ───────────────────────────────────────────────────────── */

int screenshot_capture_frame(unsigned long wid, ScreenshotFrame *frame)
{
	if (!frame) return -1;
	memset(frame, 0, sizeof(*frame));
	if (!g_xdo || !g_xdo->xdpy) return -1;

	Display *dpy = g_xdo->xdpy;
	xcb_connection_t *conn = XGetXCBConnection(dpy);
	if (!conn) return -1;

	xcb_window_t target;
	if (wid == 0) {
		/* Root window */
		const xcb_setup_t *setup = xcb_get_setup(conn);
		if (!setup) return -1;
		xcb_screen_iterator_t iter = xcb_setup_roots_iterator(setup);
		if (!iter.rem || !iter.data) return -1;
		target = iter.data->root;
	} else {
		target = (xcb_window_t)wid;
	}

	/* Get geometry */
	xcb_get_geometry_cookie_t geo_c = xcb_get_geometry(conn, target);
	xcb_get_geometry_reply_t *geo = xcb_get_geometry_reply(conn, geo_c, NULL);
	if (!geo) return -1;

	int w = geo->width;
	int h = geo->height;
	int depth = geo->depth;
	free(geo);

	if (w <= 0 || h <= 0 ||
	    (size_t)w > SIZE_MAX / 4 ||
	    (size_t)h > SIZE_MAX / ((size_t)w * 4))
		return -1;

	/* Get image */
	xcb_get_image_cookie_t img_c = xcb_get_image(conn, XCB_IMAGE_FORMAT_Z_PIXMAP,
	                                              target, 0, 0, w, h, ~0);
	xcb_get_image_reply_t *img = xcb_get_image_reply(conn, img_c, NULL);
	if (!img) return -1;

	int len = xcb_get_image_data_length(img);
	uint8_t *data = xcb_get_image_data(img);
	size_t expected = (size_t)w * (size_t)h * 4;
	if (len < 0 || (size_t)len != expected) {
		free(img);
		return -1;
	}

	uint8_t *pixels = malloc(expected);
	if (!pixels) {
		free(img);
		return -1;
	}
	memcpy(pixels, data, expected);
	/* Depth-24 ZPixmap stores one unused padding byte per pixel. Xvfb
	 * commonly sets it to zero, which must not become transparent PNG
	 * alpha. Preserve the fourth byte for true depth-32 ARGB windows. */
	if (depth == 24) {
		for (size_t i = 3; i < expected; i += 4) pixels[i] = 255;
	}

	free(img);
	frame->pixels = pixels;
	frame->length = expected;
	frame->width = w;
	frame->height = h;
	frame->depth = depth;
	return 0;
}

void screenshot_frame_clear(ScreenshotFrame *frame)
{
	if (!frame) return;
	free(frame->pixels);
	memset(frame, 0, sizeof(*frame));
}

/* ── PNG encoding via libpng ──────────────────────────────────────────────── */

struct png_mem_buf {
	uint8_t *data;
	uint8_t *row;
	size_t len;
	size_t cap;
};

static void png_write_cb(png_structp png, png_bytep data, png_size_t length)
{
	struct png_mem_buf *buf = (struct png_mem_buf *)png_get_io_ptr(png);
	if (!buf || length > SIZE_MAX - buf->len)
		png_error(png, "PNG output size overflow");
	size_t needed = buf->len + length;
	if (needed > buf->cap) {
		size_t new_cap = buf->cap ? buf->cap : 4096;
		while (new_cap < needed) {
			if (new_cap > SIZE_MAX / 2) {
				new_cap = needed;
				break;
			}
			new_cap *= 2;
		}
		uint8_t *tmp = realloc(buf->data, new_cap);
		if (!tmp) png_error(png, "PNG output allocation failed");
		buf->data = tmp;
		buf->cap = new_cap;
	}
	memcpy(buf->data + buf->len, data, length);
	buf->len += length;
}

static void png_flush_cb(png_structp png) { (void)png; }

uint8_t *screenshot_encode_png(const uint8_t *pixels, int width, int height,
                               size_t *out_len)
{
	if (!pixels || !out_len || width <= 0 || height <= 0 ||
	    (size_t)width > SIZE_MAX / 4 ||
	    (size_t)height > SIZE_MAX / ((size_t)width * 4))
		return NULL;
	*out_len = 0;
	size_t raw_size = (size_t)width * (size_t)height * 4;

	png_structp png = png_create_write_struct(PNG_LIBPNG_VER_STRING,
	                                          NULL, NULL, NULL);
	if (!png) return NULL;
	png_infop info = png_create_info_struct(png);
	if (!info) {
		png_destroy_write_struct(&png, NULL);
		return NULL;
	}
	struct png_mem_buf *buf = calloc(1, sizeof(*buf));
	if (!buf) {
		png_destroy_write_struct(&png, &info);
		return NULL;
	}

	if (setjmp(png_jmpbuf(png))) {
		free(buf->row);
		free(buf->data);
		free(buf);
		png_destroy_write_struct(&png, &info);
		return NULL;
	}

	buf->cap = raw_size;
	buf->data = malloc(buf->cap);
	buf->row = malloc((size_t)width * 4);
	if (!buf->data || !buf->row)
		png_error(png, "PNG working allocation failed");
	png_set_write_fn(png, buf, png_write_cb, png_flush_cb);
	png_set_IHDR(png, info, width, height, 8, PNG_COLOR_TYPE_RGBA,
	             PNG_INTERLACE_NONE, PNG_COMPRESSION_TYPE_DEFAULT,
	             PNG_FILTER_TYPE_DEFAULT);
	png_set_compression_level(png, 1);
	png_write_info(png, info);

	/* XCB gives us BGRA, PNG wants RGBA — swap B and R. */
	for (int y = 0; y < height; y++) {
		const uint8_t *src = pixels + (size_t)y * (size_t)width * 4;
		for (int x = 0; x < width; x++) {
			buf->row[x * 4 + 0] = src[x * 4 + 2];
			buf->row[x * 4 + 1] = src[x * 4 + 1];
			buf->row[x * 4 + 2] = src[x * 4 + 0];
			buf->row[x * 4 + 3] = src[x * 4 + 3];
		}
		png_write_row(png, buf->row);
	}

	free(buf->row);
	buf->row = NULL;
	png_write_end(png, NULL);
	png_destroy_write_struct(&png, &info);
	uint8_t *output = buf->data;
	*out_len = buf->len;
	free(buf);
	return output;
}

/* ── Public API ───────────────────────────────────────────────────────────── */

uint8_t *screenshot_capture_png(unsigned long wid, size_t *out_len)
{
	ScreenshotFrame frame;
	if (screenshot_capture_frame(wid, &frame) != 0) return NULL;
	uint8_t *png = screenshot_encode_png(
		frame.pixels, frame.width, frame.height, out_len);
	screenshot_frame_clear(&frame);
	return png;
}

/* ── Base64 encoder ───────────────────────────────────────────────────────── */

static const char b64_table[] =
	"ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

char *screenshot_base64_encode(const uint8_t *data, size_t len)
{
	size_t out_len = 4 * ((len + 2) / 3);
	char *out = malloc(out_len + 1);
	if (!out) return NULL;

	size_t i = 0, j = 0;
	while (i < len) {
		uint32_t a = i < len ? data[i++] : 0;
		uint32_t b = i < len ? data[i++] : 0;
		uint32_t c = i < len ? data[i++] : 0;
		uint32_t triple = (a << 16) | (b << 8) | c;

		out[j++] = b64_table[(triple >> 18) & 0x3F];
		out[j++] = b64_table[(triple >> 12) & 0x3F];
		out[j++] = b64_table[(triple >> 6) & 0x3F];
		out[j++] = b64_table[triple & 0x3F];
	}

	/* Padding */
	size_t mod = len % 3;
	if (mod == 1) {
		out[j - 1] = '=';
		out[j - 2] = '=';
	} else if (mod == 2) {
		out[j - 1] = '=';
	}

	out[j] = '\0';
	return out;
}
