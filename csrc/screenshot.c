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

/* Capture window pixels as BGRA. Returns malloc'd buffer. Sets w/h.
 * If wid == 0, captures root. */
static uint8_t *xcb_capture(unsigned long wid, int *out_w, int *out_h)
{
	if (!g_xdo || !g_xdo->xdpy) return NULL;

	Display *dpy = g_xdo->xdpy;
	xcb_connection_t *conn = XGetXCBConnection(dpy);
	if (!conn) return NULL;

	xcb_window_t target;
	if (wid == 0) {
		/* Root window */
		const xcb_setup_t *setup = xcb_get_setup(conn);
		xcb_screen_iterator_t iter = xcb_setup_roots_iterator(setup);
		target = iter.data->root;
	} else {
		target = (xcb_window_t)wid;
	}

	/* Get geometry */
	xcb_get_geometry_cookie_t geo_c = xcb_get_geometry(conn, target);
	xcb_get_geometry_reply_t *geo = xcb_get_geometry_reply(conn, geo_c, NULL);
	if (!geo) return NULL;

	int w = geo->width;
	int h = geo->height;
	free(geo);

	if (w <= 0 || h <= 0) return NULL;

	/* Get image */
	xcb_get_image_cookie_t img_c = xcb_get_image(conn, XCB_IMAGE_FORMAT_Z_PIXMAP,
	                                              target, 0, 0, w, h, ~0);
	xcb_get_image_reply_t *img = xcb_get_image_reply(conn, img_c, NULL);
	if (!img) return NULL;

	int len = xcb_get_image_data_length(img);
	uint8_t *data = xcb_get_image_data(img);

	uint8_t *pixels = malloc(len);
	if (pixels) {
		memcpy(pixels, data, len);
	}

	*out_w = w;
	*out_h = h;

	free(img);
	return pixels;
}

/* ── PNG encoding via libpng ──────────────────────────────────────────────── */

struct png_mem_buf {
	uint8_t *data;
	size_t   len;
	size_t   cap;
};

static void png_write_cb(png_structp png, png_bytep data, png_size_t length)
{
	struct png_mem_buf *buf = (struct png_mem_buf *)png_get_io_ptr(png);
	size_t needed = buf->len + length;
	if (needed > buf->cap) {
		size_t new_cap = buf->cap * 2;
		if (new_cap < needed) new_cap = needed;
		uint8_t *tmp = realloc(buf->data, new_cap);
		if (!tmp) return;
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
	png_structp png = png_create_write_struct(PNG_LIBPNG_VER_STRING,
	                                          NULL, NULL, NULL);
	if (!png) return NULL;

	png_infop info = png_create_info_struct(png);
	if (!info) {
		png_destroy_write_struct(&png, NULL);
		return NULL;
	}

	if (setjmp(png_jmpbuf(png))) {
		png_destroy_write_struct(&png, &info);
		return NULL;
	}

	struct png_mem_buf buf = { .data = NULL, .len = 0, .cap = 0 };
	buf.cap = width * height * 4;
	buf.data = malloc(buf.cap);
	if (!buf.data) {
		png_destroy_write_struct(&png, &info);
		return NULL;
	}

	png_set_write_fn(png, &buf, png_write_cb, png_flush_cb);

	png_set_IHDR(png, info, width, height, 8, PNG_COLOR_TYPE_RGBA,
	             PNG_INTERLACE_NONE, PNG_COMPRESSION_TYPE_DEFAULT,
	             PNG_FILTER_TYPE_DEFAULT);

	/* Use fast compression for speed */
	png_set_compression_level(png, 1);

	png_write_info(png, info);

	/* XCB gives us BGRA, PNG wants RGBA — swap B and R */
	uint8_t *row = malloc(width * 4);
	if (!row) {
		free(buf.data);
		png_destroy_write_struct(&png, &info);
		return NULL;
	}

	for (int y = 0; y < height; y++) {
		const uint8_t *src = pixels + y * width * 4;
		for (int x = 0; x < width; x++) {
			row[x * 4 + 0] = src[x * 4 + 2]; /* R <- B */
			row[x * 4 + 1] = src[x * 4 + 1]; /* G */
			row[x * 4 + 2] = src[x * 4 + 0]; /* B <- R */
			row[x * 4 + 3] = src[x * 4 + 3]; /* A */
		}
		png_write_row(png, row);
	}

	free(row);
	png_write_end(png, NULL);
	png_destroy_write_struct(&png, &info);

	*out_len = buf.len;
	return buf.data;
}

/* ── Public API ───────────────────────────────────────────────────────────── */

uint8_t *screenshot_capture_png(unsigned long wid, size_t *out_len)
{
	int w = 0, h = 0;
	uint8_t *pixels = xcb_capture(wid, &w, &h);
	if (!pixels) return NULL;

	uint8_t *png = screenshot_encode_png(pixels, w, h, out_len);
	free(pixels);
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
