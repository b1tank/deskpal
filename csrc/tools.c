/*
 * deskpal — Tool implementations
 *
 * Each tool_xxx function receives a cJSON params object and returns
 * a cJSON result object (with "content" array).
 *
 * Copyright (c) 2026 deskpal contributors
 * SPDX-License-Identifier: MIT
 */
#include "tools.h"
#include "mcp.h"
#include "x11.h"
#include "screenshot.h"
#include "ocr.h"
#include "uinput.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <signal.h>
#include <sys/wait.h>
#include <sys/time.h>     /* setitimer for tool_exec deadline */

/* ── Helpers ──────────────────────────────────────────────────────────────── */

static const char *json_str(const cJSON *obj, const char *key, const char *def)
{
	const cJSON *item = cJSON_GetObjectItem(obj, key);
	if (item && cJSON_IsString(item)) return item->valuestring;
	return def;
}

static int json_int(const cJSON *obj, const char *key, int def)
{
	const cJSON *item = cJSON_GetObjectItem(obj, key);
	if (item && cJSON_IsNumber(item)) return item->valueint;
	return def;
}

/* json_double available if needed in future tools
static double json_double(const cJSON *obj, const char *key, double def)
{
	const cJSON *item = cJSON_GetObjectItem(obj, key);
	if (item && cJSON_IsNumber(item)) return item->valuedouble;
	return def;
}
*/

static int json_bool(const cJSON *obj, const char *key, int def)
{
	const cJSON *item = cJSON_GetObjectItem(obj, key);
	if (!item) return def;
	if (cJSON_IsBool(item)) return cJSON_IsTrue(item);
	return def;
}

/* Parse button value: accepts number (1,2,3) or string ("left","right","middle"). */
static int json_button(const cJSON *obj, const char *key, int def)
{
	const cJSON *item = cJSON_GetObjectItem(obj, key);
	if (!item) return def;
	if (cJSON_IsNumber(item)) return item->valueint;
	if (cJSON_IsString(item) && item->valuestring) {
		if (strcmp(item->valuestring, "right") == 0) return 3;
		if (strcmp(item->valuestring, "middle") == 0) return 2;
		if (strcmp(item->valuestring, "left") == 0) return 1;
	}
	return def;
}

/* Resolve windowId or windowName to a window ID. Returns 0 if not found. */
static unsigned long resolve_window(const cJSON *params)
{
	const char *wid_str = json_str(params, "windowId", NULL);
	const char *wname = json_str(params, "windowName", NULL);

	if (wid_str) return strtoul(wid_str, NULL, 0);
	if (wname) return x11_find_window(wname);
	return 0;
}

static void usleep_ms(int ms)
{
	usleep(ms * 1000);
}

/* ── screenshot ──────────────────────────────────────────────────────────── */

cJSON *tool_screenshot(const cJSON *params)
{
	int full_screen = json_bool(params, "fullScreen", 0);
	unsigned long wid = resolve_window(params);

	unsigned long target = 0; /* 0 = root for full_screen */
	if (!full_screen) {
		target = wid ? wid : x11_get_active_window();
	}

	size_t png_len = 0;
	uint8_t *png = screenshot_capture_png(target, &png_len);

	if (!png && target != 0) {
		/* XCB GetImage fails for some windows (e.g. transient dialogs on
		 * Xwayland) — fall back to ImageMagick import */
		char tmp[64];
		snprintf(tmp, sizeof(tmp), "/tmp/deskpal_ss_%d.png", getpid());
		char cmd[256];
		snprintf(cmd, sizeof(cmd),
			"import -window 0x%lx png:\"%s\" 2>/dev/null",
			target, tmp);
		system(cmd);
		FILE *f = fopen(tmp, "rb");
		if (f) {
			fseek(f, 0, SEEK_END);
			png_len = ftell(f);
			fseek(f, 0, SEEK_SET);
			png = malloc(png_len);
			if (png) fread(png, 1, png_len, f);
			fclose(f);
			unlink(tmp);
		}
	}

	if (!png && full_screen) {
		/* XCB root capture fails on Xwayland — use gnome-screenshot/grim */
		char tmp[64];
		snprintf(tmp, sizeof(tmp), "/tmp/deskpal_ss_%d.png", getpid());
		char cmd[256];
		snprintf(cmd, sizeof(cmd),
			"gnome-screenshot -f \"%s\" 2>/dev/null"
			" || grim \"%s\" 2>/dev/null", tmp, tmp);
		system(cmd);
		FILE *f = fopen(tmp, "rb");
		if (f) {
			fseek(f, 0, SEEK_END);
			png_len = ftell(f);
			fseek(f, 0, SEEK_SET);
			png = malloc(png_len);
			if (png) fread(png, 1, png_len, f);
			fclose(f);
			unlink(tmp);
		}
	}

	if (!png) {
		return mcp_text_result("Screenshot failed: could not capture window");
	}

	char *b64 = screenshot_base64_encode(png, png_len);
	free(png);

	if (!b64) {
		return mcp_text_result("Screenshot failed: base64 encoding error");
	}

	cJSON *result = mcp_image_result(b64, "image/png");
	free(b64);
	return result;
}

/* ── list_windows ────────────────────────────────────────────────────────── */

cJSON *tool_list_windows(const cJSON *params)
{
	const char *name_filter = json_str(params, "name", NULL);

	WindowInfo windows[50];
	int count = x11_list_windows(windows, 50, name_filter);

	int scale = x11_get_scale_factor();

	char buf[8192];
	int pos = 0;

	for (int i = 0; i < count && pos < (int)sizeof(buf) - 256; i++) {
		pos += snprintf(buf + pos, sizeof(buf) - pos,
			"[%lu] \"%s\" pid=%ld\n"
			"  Position: %d,%d (screen: 0)\n"
			"  Geometry: %dx%d\n\n",
			windows[i].id, windows[i].title, windows[i].pid,
			windows[i].x, windows[i].y,
			windows[i].width, windows[i].height);
	}

	if (count > 0) {
		pos += snprintf(buf + pos, sizeof(buf) - pos,
			"Display scale: %dx", scale);
	} else {
		snprintf(buf, sizeof(buf), "No visible windows found");
	}

	return mcp_text_result(buf);
}

/* ── find_window ─────────────────────────────────────────────────────────── */

cJSON *tool_find_window(const cJSON *params)
{
	const char *name = json_str(params, "name", NULL);
	if (!name) name = json_str(params, "windowName", "");
	unsigned long wid = x11_find_window(name);
	if (!wid) {
		char msg[256];
		snprintf(msg, sizeof(msg), "No window found matching \"%s\"", name);
		return mcp_text_result(msg);
	}

	WindowInfo info;
	x11_get_window_info(wid, &info);

	char buf[512];
	snprintf(buf, sizeof(buf),
		"[%lu] \"%s\"\n  Position: %d,%d\n  Size: %dx%d\n  PID: %ld",
		info.id, info.title, info.x, info.y, info.width, info.height, info.pid);
	return mcp_text_result(buf);
}

/* ── focus_window ────────────────────────────────────────────────────────── */

cJSON *tool_focus_window(const cJSON *params)
{
	unsigned long wid = resolve_window(params);
	if (!wid) return mcp_text_result("Window not found");

	x11_focus_window(wid);

	char buf[64];
	snprintf(buf, sizeof(buf), "Focused window %lu", wid);
	return mcp_text_result(buf);
}

/* ── click ───────────────────────────────────────────────────────────────── */

cJSON *tool_click(const cJSON *params)
{
	unsigned long wid = resolve_window(params);
	int x = json_int(params, "x", 0);
	int y = json_int(params, "y", 0);
	int button = json_button(params, "button", 1);
	int dbl = json_bool(params, "doubleClick", 0);

	unsigned long target = wid ? wid : x11_get_active_window();
	WindowInfo info;
	if (x11_get_window_info(target, &info) != 0) {
		return mcp_text_result("Could not get window position");
	}

	int abs_x = info.x + x;
	int abs_y = info.y + y;

	if (x11_is_wayland()) {
		/* On Wayland we cannot trust info.x/info.y from
		 * xdo_get_window_location to match the renderer's content
		 * origin (observed ~90 screen-px offset on mutter+HiDPI).
		 * Route the whole motion+press through xdotool --window,
		 * which uses the X server's coordinate system. */
		x11_window_click(target, x, y, button, dbl ? 2 : 1);
	} else {
		/* Move cursor to click position — with uinput this also
		 * gives the window compositor-level focus automatically. */
		x11_mouse_move(abs_x, abs_y);
		usleep_ms(10);

		if (button != 1 && uinput_available()) {
			/* uinput ABS right-click doesn't trigger GTK context
			 * menus; xdotool does. Keep this path for parity. */
			char cmd[128];
			snprintf(cmd, sizeof(cmd),
				"xdotool mousemove --window %lu %d %d && xdotool click %d",
				target, x, y, button);
			if (dbl) {
				char cmd2[280];
				snprintf(cmd2, sizeof(cmd2), "%s && %s", cmd, cmd);
				system(cmd2);
			} else {
				system(cmd);
			}
		} else {
			x11_click(button, dbl ? 2 : 1);
		}
	}

	char buf[128];
	snprintf(buf, sizeof(buf),
		"Clicked (%d, %d) in window %lu [abs: %d, %d]",
		x, y, target, abs_x, abs_y);
	return mcp_text_result(buf);
}

/* ── OCR helper: screenshot → tesseract → OcrResult ──────────────────────── */

/* Run OCR on a screenshot of the given window (0 = full screen).
 * Returns the number of word boxes found. Caller must ocr_result_free(). */
static int ocr_screenshot(unsigned long wid, OcrResult *out)
{
	out->boxes = NULL;
	out->count = 0;
	out->capacity = 0;

	char tmp_png[64];
	snprintf(tmp_png, sizeof(tmp_png), "/tmp/deskpal_ocr_%d.png", getpid());

	size_t png_len = 0;
	uint8_t *png_data = screenshot_capture_png(wid, &png_len);

	if (!png_data && wid == 0) {
		/* XCB root capture fails on Xwayland — fall back to external tools */
		char cmd[256];
		snprintf(cmd, sizeof(cmd),
			"gnome-screenshot -f \"%s\" 2>/dev/null"
			" || grim \"%s\" 2>/dev/null",
			tmp_png, tmp_png);
		system(cmd);
		/* Check if the file was created */
		FILE *f = fopen(tmp_png, "rb");
		if (f) { fclose(f); goto do_ocr; }
		return 0;
	}

	if (!png_data) return 0;

	{
		FILE *f = fopen(tmp_png, "wb");
		if (!f) { free(png_data); return 0; }
		fwrite(png_data, 1, png_len, f);
		fclose(f);
		free(png_data);
	}

do_ocr:;

	/* Preprocess: scale up 2x + invert for dark themes.
	 * Larger text gives tesseract much better accuracy. */
	char preproc_png[80];
	snprintf(preproc_png, sizeof(preproc_png), "/tmp/deskpal_ocr_pre_%d.png", getpid());
	char preproc_cmd[512];

	/* Scale up the original image 2x for better OCR accuracy */
	char scaled_png[80];
	snprintf(scaled_png, sizeof(scaled_png), "/tmp/deskpal_ocr_2x_%d.png", getpid());
	snprintf(preproc_cmd, sizeof(preproc_cmd),
		"convert \"%s\" -resize 200%% \"%s\" 2>/dev/null",
		tmp_png, scaled_png);
	system(preproc_cmd);
	/* Use scaled image for OCR, fall back to original if scaling fails */
	{
		FILE *ft = fopen(scaled_png, "rb");
		if (ft) {
			fclose(ft);
			unlink(tmp_png);
			/* Rename is fine since same filesystem */
			rename(scaled_png, tmp_png);
		}
	}

	snprintf(preproc_cmd, sizeof(preproc_cmd),
		"convert \"%s\" -negate -colorspace Gray -sharpen 0x1 \"%s\" 2>/dev/null",
		tmp_png, preproc_png);
	system(preproc_cmd);

	/* Run tesseract on both original and inverted */
	char cmd[256];
	char tsv_base[64], tsv_base2[64];
	snprintf(tsv_base, sizeof(tsv_base), "/tmp/deskpal_tsv_%d", getpid());
	snprintf(tsv_base2, sizeof(tsv_base2), "/tmp/deskpal_tsv2_%d", getpid());

	snprintf(cmd, sizeof(cmd),
		"tesseract \"%s\" \"%s\" -l eng --psm 3 tsv 2>/dev/null", tmp_png, tsv_base);
	int rc = system(cmd);

	snprintf(cmd, sizeof(cmd),
		"tesseract \"%s\" \"%s\" -l eng --psm 3 tsv 2>/dev/null", preproc_png, tsv_base2);
	int rc2 = system(cmd);
	unlink(tmp_png);
	unlink(preproc_png);

	if (rc != 0 && rc2 != 0) return 0;

	char tsv_file[80], tsv_file2[80];
	snprintf(tsv_file, sizeof(tsv_file), "%s.tsv", tsv_base);
	snprintf(tsv_file2, sizeof(tsv_file2), "%s.tsv", tsv_base2);

	const char *tsv_files[] = { tsv_file, tsv_file2, NULL };
	for (int fi = 0; tsv_files[fi]; fi++) {
		FILE *ft = fopen(tsv_files[fi], "r");
		if (!ft) continue;

		char line[512];
		int line_no = 0;
		while (fgets(line, sizeof(line), ft)) {
			line_no++;
			if (line_no == 1) continue;

			char *cols[12];
			int ncols = 0;
			char *p = line;
			while (ncols < 12 && *p) {
				cols[ncols++] = p;
				char *tab = strchr(p, '\t');
				if (tab) { *tab = '\0'; p = tab + 1; }
				else {
					size_t len = strlen(p);
					if (len > 0 && p[len - 1] == '\n') p[len - 1] = '\0';
					break;
				}
			}
			if (ncols < 12) continue;
			int conf = atoi(cols[10]);
			char *word = cols[11];
			if (conf < 30 || !word[0] || (word[0] == ' ' && word[1] == '\0'))
				continue;

			OcrBox box;
			int len = strlen(word);
			if (len >= (int)sizeof(box.text)) len = (int)sizeof(box.text) - 1;
			memcpy(box.text, word, len);
			box.text[len] = '\0';
			while (len > 0 && (box.text[len-1] == ' ' || box.text[len-1] == '\n' ||
			       box.text[len-1] == '\r'))
				box.text[--len] = '\0';
			if (len == 0) continue;

			box.x = atoi(cols[6]) / 2;  /* halve: image was scaled 2x */
			box.y = atoi(cols[7]) / 2;
			box.width = atoi(cols[8]) / 2;
			box.height = atoi(cols[9]) / 2;
			box.confidence = conf;

			if (out->count >= out->capacity) {
				int new_cap = out->capacity ? out->capacity * 2 : 64;
				OcrBox *tmp = realloc(out->boxes, new_cap * sizeof(OcrBox));
				if (!tmp) break;
				out->boxes = tmp;
				out->capacity = new_cap;
			}
			out->boxes[out->count++] = box;
		}
		fclose(ft);
	}
	unlink(tsv_file);
	unlink(tsv_file2);

	/* Deduplicate overlapping boxes from normal + inverted runs */
	for (int i = 0; i < out->count; i++) {
		for (int j = i + 1; j < out->count; j++) {
			OcrBox *a = &out->boxes[i];
			OcrBox *b = &out->boxes[j];
			if (strcasecmp(a->text, b->text) == 0 &&
			    abs(a->x - b->x) < 20 && abs(a->y - b->y) < 20) {
				if (b->confidence > a->confidence) *a = *b;
				memmove(b, b + 1, (out->count - j - 1) * sizeof(OcrBox));
				out->count--;
				j--;
			}
		}
	}

	/* Sort boxes by position (y first, then x) so multi-word matching
	 * works correctly after merging normal + inverted OCR results */
	for (int i = 0; i < out->count - 1; i++) {
		for (int j = i + 1; j < out->count; j++) {
			OcrBox *a = &out->boxes[i];
			OcrBox *b = &out->boxes[j];
			/* Same line: within 15px vertically */
			int ay = a->y + a->height / 2;
			int by = b->y + b->height / 2;
			int swap = 0;
			if (abs(ay - by) <= 15) {
				/* Same line — sort by x */
				if (b->x < a->x) swap = 1;
			} else if (by < ay) {
				swap = 1;
			}
			if (swap) {
				OcrBox tmp = *a;
				*a = *b;
				*b = tmp;
			}
		}
	}

	return out->count;
}

/* ── click_text ──────────────────────────────────────────────────────────── */

cJSON *tool_click_text(const cJSON *params)
{
	if (!ocr_available()) {
		return mcp_text_result(
			"OCR not available. Install: sudo apt install tesseract-ocr");
	}

	const char *text = json_str(params, "text", "");
	int occurrence = json_int(params, "occurrence", 1);
	int button = json_button(params, "button", 1);

	unsigned long wid = resolve_window(params);
	unsigned long target = wid ? wid : x11_get_active_window();

	WindowInfo info;
	if (x11_get_window_info(target, &info) != 0) {
		return mcp_text_result("Could not get window info");
	}

	/* Get offset from params */
	const cJSON *offset = cJSON_GetObjectItem(params, "offset");
	int off_x = offset ? json_int(offset, "x", 0) : 0;
	int off_y = offset ? json_int(offset, "y", 0) : 0;

	/* ── Pass 1: OCR on the target window ──────────────────────────────── */
	OcrResult ocr_result;
	ocr_screenshot(target, &ocr_result);

	int match_count = 0;
	OcrMatch *matches = NULL;

	if (ocr_result.count > 0) {
		matches = ocr_find_text(&ocr_result, text, &match_count);
	}

	if (match_count > 0) {
		int idx = occurrence - 1;
		if (idx < 0) idx = 0;
		if (idx >= match_count) idx = match_count - 1;
		OcrMatch match = matches[idx];
		free(matches);

		int click_x = match.x + match.width / 2 + off_x;
		int click_y = match.y + match.height / 2 + off_y;
		int abs_x = info.x + click_x;
		int abs_y = info.y + click_y;

		if (x11_is_wayland()) {
			x11_window_click(target, click_x, click_y, button, 1);
		} else {
			x11_mouse_move(abs_x, abs_y);
			usleep_ms(10);
			x11_click(button, 1);
		}

		char buf[256];
		snprintf(buf, sizeof(buf),
			"Clicked \"%s\" (occurrence %d/%d) at (%d, %d) in window %lu\n"
			"Text bbox: %d,%d %dx%d",
			text, idx + 1, match_count, click_x, click_y, target,
			match.x, match.y, match.width, match.height);
		ocr_result_free(&ocr_result);
		return mcp_text_result(buf);
	}

	/* ── Pass 2: text not in window — capture area around window ─────── */
	/* Popup menus, context menus, dropdowns render as separate windows.
	 * Crop a region around the target window to focus OCR on relevant area. */
	ocr_result_free(&ocr_result);

	/* Take a full-screen screenshot, then crop to window region + margin */
	char ss_png[64], crop_png[64];
	snprintf(ss_png, sizeof(ss_png), "/tmp/deskpal_ss_%d.png", getpid());
	snprintf(crop_png, sizeof(crop_png), "/tmp/deskpal_crop_%d.png", getpid());

	{
		char cmd[256];
		snprintf(cmd, sizeof(cmd),
			"gnome-screenshot -f \"%s\" 2>/dev/null"
			" || grim \"%s\" 2>/dev/null", ss_png, ss_png);
		system(cmd);
	}

	/* Crop to window area + 200px margin on each side */
	int margin = 200;
	int crop_x = info.x > margin ? info.x - margin : 0;
	int crop_y = info.y > margin ? info.y - margin : 0;
	int crop_w = info.width + 2 * margin;
	int crop_h = info.height + 2 * margin;

	{
		char cmd[512];
		snprintf(cmd, sizeof(cmd),
			"convert \"%s\" -crop %dx%d+%d+%d +repage -resize 200%% \"%s\" 2>/dev/null",
			ss_png, crop_w, crop_h, crop_x, crop_y, crop_png);
		system(cmd);
	}
	unlink(ss_png);

	/* OCR the cropped region — rename to deskpal_ocr_ so ocr_screenshot-like
	 * preprocessing works. We do it manually here. */
	OcrResult screen_ocr = { .boxes = NULL, .count = 0, .capacity = 0 };
	{
		/* Invert for dark theme */
		char inv_png[80];
		snprintf(inv_png, sizeof(inv_png), "/tmp/deskpal_crop_inv_%d.png", getpid());
		char cmd[512];
		snprintf(cmd, sizeof(cmd),
			"convert \"%s\" -negate -colorspace Gray -sharpen 0x1 \"%s\" 2>/dev/null",
			crop_png, inv_png);
		system(cmd);

		/* Run tesseract on both */
		char tsv1[64], tsv2[64];
		snprintf(tsv1, sizeof(tsv1), "/tmp/deskpal_ctsv_%d", getpid());
		snprintf(tsv2, sizeof(tsv2), "/tmp/deskpal_ctsv2_%d", getpid());

		snprintf(cmd, sizeof(cmd),
			"tesseract \"%s\" \"%s\" -l eng --psm 3 tsv 2>/dev/null",
			crop_png, tsv1);
		system(cmd);
		snprintf(cmd, sizeof(cmd),
			"tesseract \"%s\" \"%s\" -l eng --psm 3 tsv 2>/dev/null",
			inv_png, tsv2);
		system(cmd);
		unlink(crop_png);
		unlink(inv_png);

		char tsv1f[80], tsv2f[80];
		snprintf(tsv1f, sizeof(tsv1f), "%s.tsv", tsv1);
		snprintf(tsv2f, sizeof(tsv2f), "%s.tsv", tsv2);

		const char *files[] = { tsv1f, tsv2f, NULL };
		for (int fi = 0; files[fi]; fi++) {
			FILE *ft = fopen(files[fi], "r");
			if (!ft) continue;
			char line[512];
			int line_no = 0;
			while (fgets(line, sizeof(line), ft)) {
				line_no++;
				if (line_no == 1) continue;
				char *cols[12];
				int ncols = 0;
				char *p = line;
				while (ncols < 12 && *p) {
					cols[ncols++] = p;
					char *tab = strchr(p, '\t');
					if (tab) { *tab = '\0'; p = tab + 1; }
					else { size_t l = strlen(p); if (l > 0 && p[l-1]=='\n') p[l-1]='\0'; break; }
				}
				if (ncols < 12) continue;
				int conf = atoi(cols[10]);
				char *word = cols[11];
				if (conf < 30 || !word[0] || (word[0]==' ' && !word[1])) continue;
				OcrBox box;
				int len = strlen(word);
				if (len >= (int)sizeof(box.text)) len = (int)sizeof(box.text) - 1;
				memcpy(box.text, word, len);
				box.text[len] = '\0';
				while (len > 0 && (box.text[len-1]==' '||box.text[len-1]=='\n'||box.text[len-1]=='\r'))
					box.text[--len] = '\0';
				if (!len) continue;
				box.x = atoi(cols[6]) / 2;  /* halve: image was scaled 2x */
				box.y = atoi(cols[7]) / 2;
				box.width = atoi(cols[8]) / 2;
				box.height = atoi(cols[9]) / 2;
				box.confidence = conf;
				if (screen_ocr.count >= screen_ocr.capacity) {
					int nc = screen_ocr.capacity ? screen_ocr.capacity * 2 : 64;
					OcrBox *tmp = realloc(screen_ocr.boxes, nc * sizeof(OcrBox));
					if (!tmp) break;
					screen_ocr.boxes = tmp;
					screen_ocr.capacity = nc;
				}
				screen_ocr.boxes[screen_ocr.count++] = box;
			}
			fclose(ft);
		}
		unlink(tsv1f);
		unlink(tsv2f);

		/* Dedup */
		for (int i = 0; i < screen_ocr.count; i++) {
			for (int j = i + 1; j < screen_ocr.count; j++) {
				OcrBox *a = &screen_ocr.boxes[i];
				OcrBox *b = &screen_ocr.boxes[j];
				if (strcasecmp(a->text, b->text) == 0 &&
				    abs(a->x - b->x) < 20 && abs(a->y - b->y) < 20) {
					if (b->confidence > a->confidence) *a = *b;
					memmove(b, b + 1, (screen_ocr.count - j - 1) * sizeof(OcrBox));
					screen_ocr.count--;
					j--;
				}
			}
		}
		/* Sort by position */
		for (int i = 0; i < screen_ocr.count - 1; i++) {
			for (int j = i + 1; j < screen_ocr.count; j++) {
				OcrBox *a = &screen_ocr.boxes[i];
				OcrBox *b = &screen_ocr.boxes[j];
				int ay = a->y + a->height / 2, by = b->y + b->height / 2;
				int swap = 0;
				if (abs(ay - by) <= 15) { if (b->x < a->x) swap = 1; }
				else if (by < ay) swap = 1;
				if (swap) { OcrBox tmp = *a; *a = *b; *b = tmp; }
			}
		}
	}

	match_count = 0;
	matches = NULL;
	if (screen_ocr.count > 0) {
		matches = ocr_find_text(&screen_ocr, text, &match_count);
	}

	if (match_count > 0) {
		int idx = occurrence - 1;
		if (idx < 0) idx = 0;
		if (idx >= match_count) idx = match_count - 1;
		OcrMatch match = matches[idx];
		free(matches);

		/* Cropped coords → absolute screen coords */
		int abs_x = crop_x + match.x + match.width / 2 + off_x;
		int abs_y = crop_y + match.y + match.height / 2 + off_y;

		x11_mouse_move(abs_x, abs_y);
		usleep_ms(10);
		x11_click(button, 1);

		char buf[256];
		snprintf(buf, sizeof(buf),
			"Clicked \"%s\" (occurrence %d/%d) at screen (%d, %d) [popup/menu]\n"
			"Text bbox: %d,%d %dx%d",
			text, idx + 1, match_count, abs_x, abs_y,
			match.x, match.y, match.width, match.height);
		ocr_result_free(&screen_ocr);
		return mcp_text_result(buf);
	}

	/* Not found anywhere */
	char visible[2048];
	int vpos = 0;
	vpos += snprintf(visible + vpos, sizeof(visible) - vpos,
		"Text \"%s\" not found on screen.\n\nVisible text: ", text);
	for (int i = 0; i < screen_ocr.count && vpos < (int)sizeof(visible) - 64; i++) {
		if (i > 0) vpos += snprintf(visible + vpos, sizeof(visible) - vpos, ", ");
		vpos += snprintf(visible + vpos, sizeof(visible) - vpos,
			"%s", screen_ocr.boxes[i].text);
	}
	ocr_result_free(&screen_ocr);
	return mcp_text_result(visible);
}
cJSON *tool_read_screen_text(const cJSON *params)
{
	if (!ocr_available()) {
		return mcp_text_result(
			"OCR not available. Install: sudo apt install tesseract-ocr");
	}

	unsigned long wid = resolve_window(params);
	unsigned long target = wid ? wid : x11_get_active_window();

	/* Screenshot */
	size_t png_len = 0;
	uint8_t *png_data = screenshot_capture_png(target, &png_len);
	if (!png_data) {
		return mcp_text_result("Screenshot failed");
	}

	/* Check for region crop */
	const cJSON *region = cJSON_GetObjectItem(params, "region");
	int has_region = region && cJSON_IsObject(region);

	/* Write to temp file for tesseract */
	char tmp_png[64];
	snprintf(tmp_png, sizeof(tmp_png), "/tmp/deskpal_ocr_read_%d.png", getpid());

	if (has_region) {
		/* Write full screenshot, then crop with ImageMagick */
		char full_png[64];
		snprintf(full_png, sizeof(full_png), "/tmp/deskpal_ocr_full_%d.png", getpid());
		FILE *f = fopen(full_png, "wb");
		if (f) { fwrite(png_data, 1, png_len, f); fclose(f); }
		free(png_data);

		int rx = json_int(region, "x", 0);
		int ry = json_int(region, "y", 0);
		int rw = json_int(region, "width", 100);
		int rh = json_int(region, "height", 100);

		char cmd[256];
		snprintf(cmd, sizeof(cmd),
			"convert \"%s\" -crop %dx%d+%d+%d +repage \"%s\" 2>/dev/null",
			full_png, rw, rh, rx, ry, tmp_png);
		system(cmd);
		unlink(full_png);
	} else {
		FILE *f = fopen(tmp_png, "wb");
		if (f) { fwrite(png_data, 1, png_len, f); fclose(f); }
		free(png_data);
	}

	/* Run tesseract */
	char tsv_base[64];
	snprintf(tsv_base, sizeof(tsv_base), "/tmp/deskpal_tsv_read_%d", getpid());
	char cmd[256];
	snprintf(cmd, sizeof(cmd),
		"tesseract \"%s\" \"%s\" -l eng --psm 3 tsv 2>/dev/null", tmp_png, tsv_base);
	system(cmd);
	unlink(tmp_png);

	/* Parse TSV */
	char tsv_file[80];
	snprintf(tsv_file, sizeof(tsv_file), "%s.tsv", tsv_base);
	FILE *f = fopen(tsv_file, "r");
	if (!f) {
		return mcp_text_result("OCR failed to produce results");
	}

	/* Read all words with positions */
	char output[8192];
	int opos = 0;
	char line[512];
	int line_no = 0;
	int word_count = 0;
	int last_line_num = -1;

	while (fgets(line, sizeof(line), f) && opos < (int)sizeof(output) - 256) {
		line_no++;
		if (line_no == 1) continue;

		char *cols[12];
		int ncols = 0;
		char *p = line;
		while (ncols < 12 && *p) {
			cols[ncols++] = p;
			char *tab = strchr(p, '\t');
			if (tab) { *tab = '\0'; p = tab + 1; }
			else {
				size_t len = strlen(p);
				if (len > 0 && p[len-1] == '\n') p[len-1] = '\0';
				break;
			}
		}
		if (ncols < 12) continue;

		int conf = atoi(cols[10]);
		char *word = cols[11];
		if (conf < 30 || !word[0]) continue;

		/* Trim */
		size_t wlen = strlen(word);
		while (wlen > 0 && (word[wlen-1] == ' ' || word[wlen-1] == '\n'))
			word[--wlen] = '\0';
		if (wlen == 0) continue;

		int cur_line = atoi(cols[4]);
		int top = atoi(cols[7]);

		if (cur_line != last_line_num) {
			if (word_count > 0) opos += snprintf(output + opos, sizeof(output) - opos, "\n");
			opos += snprintf(output + opos, sizeof(output) - opos, "[y=%d] ", top);
			last_line_num = cur_line;
		} else {
			opos += snprintf(output + opos, sizeof(output) - opos, " ");
		}
		opos += snprintf(output + opos, sizeof(output) - opos, "%s", word);
		word_count++;
	}
	fclose(f);
	unlink(tsv_file);

	if (word_count == 0) {
		return mcp_text_result("No text detected in the specified area.");
	}

	char header[64];
	snprintf(header, sizeof(header), "OCR results (%d words):\n\n", word_count);

	char *result = malloc(strlen(header) + strlen(output) + 1);
	if (!result) return mcp_text_result("Out of memory");
	strcpy(result, header);
	strcat(result, output);

	cJSON *ret = mcp_text_result(result);
	free(result);
	return ret;
}

/* ── launch_app ──────────────────────────────────────────────────────────── */

cJSON *tool_launch_app(const cJSON *params)
{
	const char *command = json_str(params, "command", "");
	int kill_existing = json_bool(params, "killExisting", 1);
	int timeout = json_int(params, "timeout", 10);
	const char *wait_title = json_str(params, "waitForWindow", NULL);

	/* Extract basename */
	const char *basename = strrchr(command, '/');
	basename = basename ? basename + 1 : command;

	/* Kill existing */
	if (kill_existing) {
		char kill_cmd[256];
		snprintf(kill_cmd, sizeof(kill_cmd), "pkill -f \"%s\" 2>/dev/null", basename);
		system(kill_cmd);
		usleep_ms(500);
	}

	/* Build command with env */
	char full_cmd[1024];
	const cJSON *env = cJSON_GetObjectItem(params, "env");
	int pos = 0;
	pos += snprintf(full_cmd + pos, sizeof(full_cmd) - pos,
		"DISPLAY=:0 DBUS_SESSION_BUS_ADDRESS=\"\" ");
	if (env && cJSON_IsObject(env)) {
		cJSON *item = NULL;
		cJSON_ArrayForEach(item, env) {
			if (cJSON_IsString(item)) {
				pos += snprintf(full_cmd + pos, sizeof(full_cmd) - pos,
					"%s=%s ", item->string, item->valuestring);
			}
		}
	}

	/* Build args */
	const cJSON *args = cJSON_GetObjectItem(params, "args");
	pos += snprintf(full_cmd + pos, sizeof(full_cmd) - pos, "%s", command);
	if (args && cJSON_IsArray(args)) {
		cJSON *arg = NULL;
		cJSON_ArrayForEach(arg, args) {
			if (cJSON_IsString(arg)) {
				pos += snprintf(full_cmd + pos, sizeof(full_cmd) - pos,
					" %s", arg->valuestring);
			}
		}
	}
	pos += snprintf(full_cmd + pos, sizeof(full_cmd) - pos, " &>/dev/null &");

	/* Launch */
	system(full_cmd);

	/* Wait for window */
	const char *search_title = wait_title ? wait_title : basename;
	int deadline_ms = timeout * 1000;
	int elapsed = 0;

	while (elapsed < deadline_ms) {
		usleep_ms(500);
		elapsed += 500;
		unsigned long wid = x11_find_window(search_title);
		if (wid) {
			WindowInfo info;
			x11_get_window_info(wid, &info);
			char buf[512];
			snprintf(buf, sizeof(buf),
				"Launched \"%s\"\n[%lu] \"%s\"\n  Position: %d,%d\n  "
				"Size: %dx%d\n  PID: %ld",
				command, info.id, info.title,
				info.x, info.y, info.width, info.height, info.pid);
			return mcp_text_result(buf);
		}
	}

	char buf[256];
	snprintf(buf, sizeof(buf),
		"Launched \"%s\" but no window matching \"%s\" appeared after %ds",
		command, search_title, timeout);
	return mcp_text_result(buf);
}

/* ── type_text ───────────────────────────────────────────────────────────── */

cJSON *tool_type_text(const cJSON *params)
{
	const char *text = json_str(params, "text", "");
	int delay = json_int(params, "delay", 12);

	unsigned long wid = resolve_window(params);
	if (wid) {
		x11_focus_window(wid);
		usleep_ms(50);
	}

	x11_type_text(text, delay);

	char buf[64];
	snprintf(buf, sizeof(buf), "Typed %d characters", (int)strlen(text));
	return mcp_text_result(buf);
}

/* ── key_press ───────────────────────────────────────────────────────────── */

cJSON *tool_key_press(const cJSON *params)
{
	const char *keys = json_str(params, "keys", "");

	unsigned long wid = resolve_window(params);
	if (wid) {
		x11_focus_window(wid);
		usleep_ms(50);
	}

	x11_key_press(keys);

	char buf[128];
	snprintf(buf, sizeof(buf), "Pressed: %s", keys);
	return mcp_text_result(buf);
}

/* ── get_window_geometry ─────────────────────────────────────────────────── */

cJSON *tool_get_window_geometry(const cJSON *params)
{
	unsigned long wid = resolve_window(params);
	if (!wid) wid = x11_get_active_window();

	WindowInfo info;
	if (x11_get_window_info(wid, &info) != 0) {
		return mcp_text_result("Window not found");
	}

	int scale = x11_get_scale_factor();

	char buf[512];
	snprintf(buf, sizeof(buf),
		"Window: [%lu] \"%s\"\n"
		"Position: %d,%d\n"
		"Size: %dx%d\n"
		"Scale: %dx\n"
		"PID: %ld",
		info.id, info.title, info.x, info.y,
		info.width, info.height, scale, info.pid);
	return mcp_text_result(buf);
}

/* ── resize_window ───────────────────────────────────────────────────────── */

cJSON *tool_resize_window(const cJSON *params)
{
	unsigned long wid = resolve_window(params);
	if (!wid) wid = x11_get_active_window();

	int width = json_int(params, "width", 800);
	int height = json_int(params, "height", 600);

	x11_resize_window(wid, width, height);

	char buf[64];
	snprintf(buf, sizeof(buf), "Resized window %lu to %dx%d", wid, width, height);
	return mcp_text_result(buf);
}

/* ── wait_for_window ─────────────────────────────────────────────────────── */

cJSON *tool_wait_for_window(const cJSON *params)
{
	const char *name = json_str(params, "name", "");
	int timeout = json_int(params, "timeout", 10);

	int deadline_ms = timeout * 1000;
	int elapsed = 0;

	while (elapsed < deadline_ms) {
		unsigned long wid = x11_find_window(name);
		if (wid) {
			WindowInfo info;
			x11_get_window_info(wid, &info);
			char buf[512];
			snprintf(buf, sizeof(buf),
				"Window appeared: [%lu] \"%s\" (%dx%d)",
				info.id, info.title, info.width, info.height);
			return mcp_text_result(buf);
		}
		usleep_ms(500);
		elapsed += 500;
	}

	char buf[128];
	snprintf(buf, sizeof(buf),
		"Timeout: no window matching \"%s\" after %ds", name, timeout);
	return mcp_text_result(buf);
}

/* ── mouse_move ──────────────────────────────────────────────────────────── */

cJSON *tool_mouse_move(const cJSON *params)
{
	int x = json_int(params, "x", 0);
	int y = json_int(params, "y", 0);

	unsigned long wid = resolve_window(params);
	if (wid) {
		WindowInfo info;
		if (x11_get_window_info(wid, &info) == 0) {
			int abs_x = info.x + x;
			int abs_y = info.y + y;
			if (x11_is_wayland()) {
				/* info.x/y unreliable on mutter — see
				 * x11_window_click commentary. */
				x11_window_mouse_move(wid, x, y);
			} else {
				x11_mouse_move(abs_x, abs_y);
			}
			char buf[128];
			snprintf(buf, sizeof(buf),
				"Mouse moved to (%d, %d) in window [abs: %d, %d]",
				x, y, abs_x, abs_y);
			return mcp_text_result(buf);
		}
	}

	x11_mouse_move(x, y);
	char buf[64];
	snprintf(buf, sizeof(buf), "Mouse moved to absolute (%d, %d)", x, y);
	return mcp_text_result(buf);
}

/* ── scroll ──────────────────────────────────────────────────────────────── */

cJSON *tool_scroll(const cJSON *params)
{
	const char *dir = json_str(params, "direction", "down");
	int clicks = json_int(params, "clicks", 3);

	unsigned long wid = resolve_window(params);
	if (wid) {
		x11_focus_window(wid);
		usleep_ms(50);
	}

	int button = (strcmp(dir, "up") == 0) ? 4 : 5;
	x11_scroll(button, clicks);

	char buf[64];
	snprintf(buf, sizeof(buf), "Scrolled %s %d clicks", dir, clicks);
	return mcp_text_result(buf);
}

/* ── drag ────────────────────────────────────────────────────────────────── */

cJSON *tool_drag(const cJSON *params)
{
	int from_x = json_int(params, "fromX", 0);
	int from_y = json_int(params, "fromY", 0);
	int to_x = json_int(params, "toX", 0);
	int to_y = json_int(params, "toY", 0);
	int button = json_button(params, "button", 1);
	int steps = json_int(params, "steps", 10);

	unsigned long wid = resolve_window(params);
	if (wid) {
		x11_focus_window(wid);
		usleep_ms(100);
	}

	unsigned long target = wid ? wid : x11_get_active_window();
	WindowInfo info;
	if (x11_get_window_info(target, &info) != 0) {
		return mcp_text_result("Could not get window position");
	}

	int abs_from_x = info.x + from_x;
	int abs_from_y = info.y + from_y;
	int abs_to_x = info.x + to_x;
	int abs_to_y = info.y + to_y;

	x11_drag(abs_from_x, abs_from_y, abs_to_x, abs_to_y, button, steps);

	char buf[192];
	snprintf(buf, sizeof(buf),
		"Dragged from (%d, %d) to (%d, %d) in window %lu [abs: (%d,%d)->(%d,%d)]",
		from_x, from_y, to_x, to_y, target,
		abs_from_x, abs_from_y, abs_to_x, abs_to_y);
	return mcp_text_result(buf);
}

/* ── mouse_down / mouse_up ───────────────────────────────────────────────── */

cJSON *tool_mouse_down(const cJSON *params)
{
	int button = json_button(params, "button", 1);
	int x = json_int(params, "x", -1);
	int y = json_int(params, "y", -1);

	unsigned long wid = resolve_window(params);
	if (wid && x >= 0 && y >= 0) {
		WindowInfo info;
		if (x11_get_window_info(wid, &info) == 0) {
			x11_mouse_move(info.x + x, info.y + y);
			usleep_ms(10);
		}
	} else if (x >= 0 && y >= 0) {
		x11_mouse_move(x, y);
		usleep_ms(10);
	}

	x11_mouse_down(button);

	char buf[64];
	snprintf(buf, sizeof(buf), "Mouse button %d pressed", button);
	return mcp_text_result(buf);
}

cJSON *tool_mouse_up(const cJSON *params)
{
	int button = json_button(params, "button", 1);
	x11_mouse_up(button);

	char buf[64];
	snprintf(buf, sizeof(buf), "Mouse button %d released", button);
	return mcp_text_result(buf);
}

/* ── Clipboard ────────────────────────────────────────────────────────────
 * Surfaced by OTelux self-verify §2.2 (verify "click to copy" wrote to
 * the OS clipboard).  Implementation shells out to whatever clipboard
 * helper is on PATH — wl-paste (Wayland), xclip (X11), xsel (X11
 * fallback). Returns empty string + ok=true if no clipboard owner is
 * set, never blocks.
 *
 * Note: native XCB selection requests would be more elegant but require
 * an event loop and ~200 LOC. The shell-out path is portable across
 * X11/Wayland sessions and matches the style of screenshot fallbacks
 * elsewhere in this file. */

/* Detect best clipboard helper. cmd_buf must be PATH_LEN long.
 * mode is 'r' for read, 'w' for write. Returns 0 on success, -1 if no
 * helper is available. */
static int build_clipboard_cmd(char *cmd_buf, size_t cmd_len, char mode,
                                const char *selection)
{
	int is_primary = selection && strcmp(selection, "primary") == 0;

	/* Probe helpers in order: wl-clipboard (Wayland), xclip (X11), xsel. */
	if (access("/usr/bin/wl-paste", X_OK) == 0 && getenv("WAYLAND_DISPLAY")) {
		if (mode == 'r') {
			snprintf(cmd_buf, cmd_len,
				"wl-paste --no-newline%s 2>/dev/null",
				is_primary ? " --primary" : "");
		} else {
			snprintf(cmd_buf, cmd_len,
				"wl-copy%s",
				is_primary ? " --primary" : "");
		}
		return 0;
	}
	if (access("/usr/bin/xclip", X_OK) == 0) {
		const char *sel = is_primary ? "primary" : "clipboard";
		if (mode == 'r') {
			snprintf(cmd_buf, cmd_len,
				"xclip -selection %s -o 2>/dev/null", sel);
		} else {
			snprintf(cmd_buf, cmd_len,
				"xclip -selection %s -i", sel);
		}
		return 0;
	}
	if (access("/usr/bin/xsel", X_OK) == 0) {
		const char *flag = is_primary ? "-p" : "-b";
		if (mode == 'r') {
			snprintf(cmd_buf, cmd_len, "xsel %s -o 2>/dev/null", flag);
		} else {
			snprintf(cmd_buf, cmd_len, "xsel %s -i", flag);
		}
		return 0;
	}
	return -1;
}

cJSON *tool_get_clipboard(const cJSON *params)
{
	const char *selection = json_str(params, "selection", "clipboard");

	char cmd[256];
	if (build_clipboard_cmd(cmd, sizeof(cmd), 'r', selection) != 0) {
		return mcp_text_result(
			"No clipboard helper available. Install one of: "
			"wl-clipboard (Wayland), xclip, xsel.");
	}

	FILE *p = popen(cmd, "r");
	if (!p) return mcp_text_result("Failed to spawn clipboard reader");

	/* Cap at 1 MiB — clipboard contents shouldn't be huge for our
	 * automation use cases (URLs, JSON, short strings). */
	const size_t cap = 1024 * 1024;
	char *buf = malloc(cap + 1);
	if (!buf) { pclose(p); return mcp_text_result("Out of memory"); }
	size_t total = 0;
	size_t n;
	while ((n = fread(buf + total, 1, cap - total, p)) > 0) {
		total += n;
		if (total >= cap) break;
	}
	buf[total] = '\0';
	pclose(p);

	/* Strip a single trailing newline — xclip/xsel append one, wl-paste
	 * with --no-newline does not. */
	while (total > 0 && (buf[total - 1] == '\n' || buf[total - 1] == '\r')) {
		buf[--total] = '\0';
	}

	cJSON *ret = mcp_text_result(buf[0] ? buf : "(clipboard empty)");
	free(buf);
	return ret;
}

cJSON *tool_set_clipboard(const cJSON *params)
{
	const char *text = json_str(params, "text", "");
	const char *selection = json_str(params, "selection", "clipboard");

	char cmd[256];
	if (build_clipboard_cmd(cmd, sizeof(cmd), 'w', selection) != 0) {
		return mcp_text_result(
			"No clipboard helper available. Install one of: "
			"wl-clipboard (Wayland), xclip, xsel.");
	}

	FILE *p = popen(cmd, "w");
	if (!p) return mcp_text_result("Failed to spawn clipboard writer");
	size_t tlen = strlen(text);
	if (tlen > 0) fwrite(text, 1, tlen, p);
	int rc = pclose(p);

	char buf[128];
	snprintf(buf, sizeof(buf),
		"Wrote %zu bytes to %s selection%s",
		tlen, selection,
		rc == 0 ? "" : " (helper exited non-zero)");
	return mcp_text_result(buf);
}

/* ── hover_text ─────────────────────────────────────────────────────────────
 * Surfaced by OTelux self-verify §2.1 (status-dot tooltip). Hovers over
 * an OCR-located word, waits for the tooltip to render, OCRs again, and
 * returns the text that became visible. Returning the diff (vs. dumping
 * the whole screen) tells the caller exactly what the tooltip says. */

/* Return true if box `b` already appears in `before` — same text and
 * its centre lies within `b`'s footprint of an existing box. The position
 * tolerance is generous because OCR jitters bbox edges by a few pixels
 * between consecutive captures even with no visual change. */
static int box_was_present_before(const OcrBox *b, const OcrResult *before)
{
	int cx = b->x + b->width / 2;
	int cy = b->y + b->height / 2;
	for (int i = 0; i < before->count; i++) {
		const OcrBox *o = &before->boxes[i];
		if (strcmp(o->text, b->text) != 0) continue;
		int ocx = o->x + o->width / 2;
		int ocy = o->y + o->height / 2;
		/* Allow ±20 px drift in either axis. */
		if (abs(ocx - cx) <= 20 && abs(ocy - cy) <= 20) return 1;
	}
	return 0;
}

cJSON *tool_hover_text(const cJSON *params)
{
	if (!ocr_available()) {
		return mcp_text_result(
			"OCR not available. Install: sudo apt install tesseract-ocr");
	}

	const char *text = json_str(params, "text", "");
	int settle_ms = json_int(params, "settleMs", 800);
	if (!text[0]) return mcp_text_result("Missing 'text' parameter");

	unsigned long wid = resolve_window(params);
	unsigned long target = wid ? wid : x11_get_active_window();

	WindowInfo info;
	if (x11_get_window_info(target, &info) != 0) {
		return mcp_text_result("Could not get window info");
	}

	/* Locate the target text in the window. */
	OcrResult before = { 0 };
	ocr_screenshot(target, &before);
	if (before.count == 0) {
		ocr_result_free(&before);
		return mcp_text_result("OCR returned no words for the target window");
	}

	int n_matches = 0;
	OcrMatch *matches = ocr_find_text(&before, text, &n_matches);
	if (n_matches == 0) {
		free(matches);
		ocr_result_free(&before);
		char buf[160];
		snprintf(buf, sizeof(buf), "Text \"%s\" not found on screen", text);
		return mcp_text_result(buf);
	}
	OcrMatch hit = matches[0];
	free(matches);

	int rel_x = hit.x + hit.width / 2;
	int rel_y = hit.y + hit.height / 2;
	int hover_x = info.x + rel_x;
	int hover_y = info.y + rel_y;
	if (x11_is_wayland()) {
		x11_window_mouse_move(target, rel_x, rel_y);
	} else {
		x11_mouse_move(hover_x, hover_y);
	}
	usleep_ms(settle_ms);

	/* After-snapshot: tooltip may render outside the target window (it
	 * is its own override-redirect window). Capture the full screen. */
	OcrResult after = { 0 };
	ocr_screenshot(0, &after);

	/* Build a textual diff. */
	char body[4096];
	int bpos = 0;
	int new_words = 0;
	for (int i = 0; i < after.count && bpos < (int)sizeof(body) - 64; i++) {
		const OcrBox *b = &after.boxes[i];
		if (box_was_present_before(b, &before)) continue;
		if (b->confidence < 40) continue;
		bpos += snprintf(body + bpos, sizeof(body) - bpos,
			"  %s (x=%d y=%d)\n", b->text, b->x, b->y);
		new_words++;
	}

	ocr_result_free(&before);
	ocr_result_free(&after);

	char header[256];
	if (new_words == 0) {
		snprintf(header, sizeof(header),
			"Hovered \"%s\" at (%d,%d); no tooltip detected after %d ms.",
			text, hover_x, hover_y, settle_ms);
		return mcp_text_result(header);
	}
	snprintf(header, sizeof(header),
		"Hovered \"%s\" at (%d,%d); %d new word(s) became visible after %d ms:\n",
		text, hover_x, hover_y, new_words, settle_ms);

	char *result = malloc(strlen(header) + strlen(body) + 1);
	if (!result) return mcp_text_result("Out of memory");
	strcpy(result, header);
	strcat(result, body);
	cJSON *ret = mcp_text_result(result);
	free(result);
	return ret;
}

/* ── read_file ─────────────────────────────────────────────────────────────
 * Surfaced by OTelux self-verify §5 (settings persistence). Lets agents
 * verify on-disk state without dropping out of deskpal. Gated behind
 * --allow-fs because reading arbitrary files materially expands the
 * blast radius of this MCP server. */

cJSON *tool_read_file(const cJSON *params)
{
	if (!deskpal_allow_fs) {
		return mcp_text_result(
			"read_file is disabled. Start deskpal with --allow-fs to enable it.");
	}

	const char *path = json_str(params, "path", "");
	const char *encoding = json_str(params, "encoding", "utf8");
	int max_bytes = json_int(params, "maxBytes", 1024 * 1024);
	if (max_bytes <= 0 || max_bytes > 16 * 1024 * 1024) {
		max_bytes = 1024 * 1024;
	}
	if (!path[0]) return mcp_text_result("Missing 'path' parameter");

	/* Reject obviously sensitive paths even with --allow-fs. The operator
	 * opted into the tool, not into surfacing their SSH keys. */
	static const char *deny_prefixes[] = {
		"/etc/shadow", "/etc/sudoers",
		"/root/", "/proc/self/maps", NULL
	};
	for (int i = 0; deny_prefixes[i]; i++) {
		if (strncmp(path, deny_prefixes[i], strlen(deny_prefixes[i])) == 0) {
			char buf[256];
			snprintf(buf, sizeof(buf),
				"read_file: refusing to read sensitive path %s", path);
			return mcp_text_result(buf);
		}
	}

	FILE *f = fopen(path, "rb");
	if (!f) {
		char buf[512];
		snprintf(buf, sizeof(buf), "read_file: cannot open %s", path);
		return mcp_text_result(buf);
	}

	char *buf = malloc((size_t)max_bytes + 1);
	if (!buf) { fclose(f); return mcp_text_result("Out of memory"); }
	size_t n = fread(buf, 1, (size_t)max_bytes, f);
	int truncated = 0;
	/* Probe one more byte to detect truncation. */
	if (n == (size_t)max_bytes) {
		char probe;
		if (fread(&probe, 1, 1, f) == 1) truncated = 1;
	}
	fclose(f);
	buf[n] = '\0';

	if (strcmp(encoding, "base64") == 0) {
		/* Tools don't need base64 today; the requesting agent should
		 * pass utf8. Surface a clear error rather than half-implement. */
		free(buf);
		return mcp_text_result(
			"read_file: encoding=\"base64\" is not implemented yet; "
			"use \"utf8\" for text files.");
	}

	char header[256];
	snprintf(header, sizeof(header),
		"%s (%zu bytes%s):\n",
		path, n, truncated ? ", TRUNCATED" : "");
	char *result = malloc(strlen(header) + n + 1);
	if (!result) { free(buf); return mcp_text_result("Out of memory"); }
	strcpy(result, header);
	memcpy(result + strlen(header), buf, n + 1);
	free(buf);
	cJSON *ret = mcp_text_result(result);
	free(result);
	return ret;
}

/* ── exec ──────────────────────────────────────────────────────────────────
 * Surfaced by OTelux self-verify §1.1 (port check), §6/§10 (curl),
 * §11/§12 (`ss`). Lets agents run short shell commands inside the
 * deskpal session instead of splitting between two tool surfaces.
 *
 * Gated behind --allow-exec. Implemented via popen + a SIGALRM-based
 * deadline so a hung child doesn't wedge the MCP server. We only
 * capture stdout — stderr is merged in via the shell `2>&1` rather
 * than via pipe(2)+select(2) to keep the implementation small. */

static volatile pid_t g_exec_child_pid = 0;
static volatile int   g_exec_timed_out = 0;

static void exec_alarm_handler(int sig)
{
	(void)sig;
	if (g_exec_child_pid > 0) {
		g_exec_timed_out = 1;
		/* Killing the shell kills its descendants in most cases; for
		 * stubborn children the agent will hit it again on the next
		 * call. */
		kill(g_exec_child_pid, SIGTERM);
	}
}

cJSON *tool_exec(const cJSON *params)
{
	if (!deskpal_allow_exec) {
		return mcp_text_result(
			"exec is disabled. Start deskpal with --allow-exec to enable it.");
	}

	const char *command = json_str(params, "command", "");
	int timeout_ms = json_int(params, "timeoutMs", 5000);
	if (timeout_ms <= 0) timeout_ms = 5000;
	if (timeout_ms > 60000) timeout_ms = 60000;
	if (!command[0]) return mcp_text_result("Missing 'command' parameter");

	/* Build "command 2>&1" so we capture both streams in one pipe. */
	char *full = malloc(strlen(command) + 8);
	if (!full) return mcp_text_result("Out of memory");
	sprintf(full, "%s 2>&1", command);

	/* popen() forks a shell — that's the child PID we want to alarm. We
	 * can't get popen's PID directly, so use posix_spawn-style fork/exec
	 * manually with /bin/sh. */
	int pipefd[2];
	if (pipe(pipefd) != 0) { free(full); return mcp_text_result("pipe() failed"); }
	pid_t pid = fork();
	if (pid < 0) {
		free(full);
		close(pipefd[0]); close(pipefd[1]);
		return mcp_text_result("fork() failed");
	}
	if (pid == 0) {
		/* Child */
		close(pipefd[0]);
		dup2(pipefd[1], STDOUT_FILENO);
		dup2(pipefd[1], STDERR_FILENO);
		close(pipefd[1]);
		execl("/bin/sh", "sh", "-c", full, (char *)NULL);
		_exit(127);
	}
	close(pipefd[1]);
	free(full);

	g_exec_child_pid = pid;
	g_exec_timed_out = 0;
	struct sigaction sa = { 0 }, old_sa;
	sa.sa_handler = exec_alarm_handler;
	sigaction(SIGALRM, &sa, &old_sa);
	/* setitimer for millisecond resolution. */
	struct itimerval itv = { 0 };
	itv.it_value.tv_sec  = timeout_ms / 1000;
	itv.it_value.tv_usec = (timeout_ms % 1000) * 1000;
	setitimer(ITIMER_REAL, &itv, NULL);

	/* Read output. */
	const size_t cap = 64 * 1024;
	char *out = malloc(cap + 1);
	if (!out) {
		close(pipefd[0]);
		waitpid(pid, NULL, 0);
		setitimer(ITIMER_REAL, &(struct itimerval){{0,0},{0,0}}, NULL);
		sigaction(SIGALRM, &old_sa, NULL);
		g_exec_child_pid = 0;
		return mcp_text_result("Out of memory");
	}
	size_t total = 0;
	ssize_t rn;
	while (total < cap &&
	       (rn = read(pipefd[0], out + total, cap - total)) > 0) {
		total += (size_t)rn;
	}
	out[total] = '\0';
	close(pipefd[0]);

	int status = 0;
	waitpid(pid, &status, 0);

	/* Clear the timer + restore handler. */
	setitimer(ITIMER_REAL, &(struct itimerval){{0,0},{0,0}}, NULL);
	sigaction(SIGALRM, &old_sa, NULL);
	int timed_out = g_exec_timed_out;
	g_exec_child_pid = 0;

	int exit_code = -1;
	if (WIFEXITED(status))        exit_code = WEXITSTATUS(status);
	else if (WIFSIGNALED(status)) exit_code = 128 + WTERMSIG(status);

	char header[256];
	snprintf(header, sizeof(header),
		"$ %s\nexit=%d%s%s\n--- output (%zu bytes) ---\n",
		command, exit_code,
		timed_out ? " (timed out)" : "",
		total >= cap ? " (output truncated)" : "",
		total);

	char *result = malloc(strlen(header) + total + 1);
	if (!result) { free(out); return mcp_text_result("Out of memory"); }
	strcpy(result, header);
	memcpy(result + strlen(header), out, total + 1);
	free(out);
	cJSON *ret = mcp_text_result(result);
	free(result);
	return ret;
}

/* ── Tool registration ────────────────────────────────────────────────────── */

/* Declaration of the register function from mcp.c */
extern void mcp_register_tool(const char *name, const char *description,
                               const char *schema_json, mcp_tool_handler_t handler);

void tools_register_all(void)
{
	mcp_register_tool("screenshot",
		"Capture a screenshot of a window or the entire screen. Returns base64 PNG.",
		"{"
		"  \"type\": \"object\","
		"  \"properties\": {"
		"    \"windowId\": {\"type\": \"string\", \"description\": \"X11 window ID\"},"
		"    \"windowName\": {\"type\": \"string\", \"description\": \"Window title substring\"},"
		"    \"fullScreen\": {\"type\": \"boolean\", \"description\": \"Capture entire screen\", \"default\": false}"
		"  }"
		"}",
		tool_screenshot);

	mcp_register_tool("list_windows",
		"List all visible windows with IDs, titles, geometry, PID, and display scale.",
		"{"
		"  \"type\": \"object\","
		"  \"properties\": {"
		"    \"name\": {\"type\": \"string\", \"description\": \"Optional title filter\"}"
		"  }"
		"}",
		tool_list_windows);

	mcp_register_tool("find_window",
		"Find a window by title substring. Filters tiny windows, prefers exact match.",
		"{"
		"  \"type\": \"object\","
		"  \"properties\": {"
		"    \"name\": {\"type\": \"string\", \"description\": \"Window title to search (alias: windowName)\"}"
		"  },"
		"  \"required\": [\"name\"]"
		"}",
		tool_find_window);

	mcp_register_tool("focus_window",
		"Bring a window to the front and give it focus.",
		"{"
		"  \"type\": \"object\","
		"  \"properties\": {"
		"    \"windowId\": {\"type\": \"string\"},"
		"    \"windowName\": {\"type\": \"string\"}"
		"  }"
		"}",
		tool_focus_window);

	mcp_register_tool("click",
		"Click at pixel position relative to window top-left.",
		"{"
		"  \"type\": \"object\","
		"  \"properties\": {"
		"    \"windowId\": {\"type\": \"string\"},"
		"    \"windowName\": {\"type\": \"string\"},"
		"    \"x\": {\"type\": \"number\", \"description\": \"X relative to window\"},"
		"    \"y\": {\"type\": \"number\", \"description\": \"Y relative to window\"},"
"    \"button\": {\"description\": \"1 or 'left', 2 or 'middle', 3 or 'right'\", \"default\": 1},"
		"    \"doubleClick\": {\"type\": \"boolean\", \"default\": false}"
		"  },"
		"  \"required\": [\"x\", \"y\"]"
		"}",
		tool_click);

	mcp_register_tool("click_text",
		"Find visible text on screen using OCR and click its center. "
		"Most reliable way to click buttons, tabs, menu items — no coordinate guessing.",
		"{"
		"  \"type\": \"object\","
		"  \"properties\": {"
		"    \"text\": {\"type\": \"string\", \"description\": \"Text to find and click (case-insensitive)\"},"
		"    \"windowId\": {\"type\": \"string\"},"
		"    \"windowName\": {\"type\": \"string\"},"
		"    \"occurrence\": {\"type\": \"number\", \"default\": 1, \"description\": \"Which occurrence (1-based)\"},"
		"    \"button\": {\"description\": \"1=left, 2=middle, 3=right (or string)\", \"default\": 1},"
		"    \"offset\": {\"type\": \"object\", \"properties\": {\"x\": {\"type\": \"number\"}, \"y\": {\"type\": \"number\"}}}"
		"  },"
		"  \"required\": [\"text\"]"
		"}",
		tool_click_text);

	mcp_register_tool("read_screen_text",
		"Read all visible text from a window using OCR. Returns text with positions.",
		"{"
		"  \"type\": \"object\","
		"  \"properties\": {"
		"    \"windowId\": {\"type\": \"string\"},"
		"    \"windowName\": {\"type\": \"string\"},"
		"    \"region\": {\"type\": \"object\", \"properties\": {"
		"      \"x\": {\"type\": \"number\"}, \"y\": {\"type\": \"number\"},"
		"      \"width\": {\"type\": \"number\"}, \"height\": {\"type\": \"number\"}"
		"    }}"
		"  }"
		"}",
		tool_read_screen_text);

	mcp_register_tool("launch_app",
		"Launch a desktop application, handling GApplication D-Bus delegation. "
		"Kills existing instance, launches fresh, waits for window.",
		"{"
		"  \"type\": \"object\","
		"  \"properties\": {"
		"    \"command\": {\"type\": \"string\", \"description\": \"Command to launch\"},"
		"    \"args\": {\"type\": \"array\", \"items\": {\"type\": \"string\"}},"
		"    \"waitForWindow\": {\"type\": \"string\"},"
		"    \"timeout\": {\"type\": \"number\", \"default\": 10},"
		"    \"killExisting\": {\"type\": \"boolean\", \"default\": true},"
		"    \"env\": {\"type\": \"object\"}"
		"  },"
		"  \"required\": [\"command\"]"
		"}",
		tool_launch_app);

	mcp_register_tool("type_text",
		"Type text into the focused window.",
		"{"
		"  \"type\": \"object\","
		"  \"properties\": {"
		"    \"text\": {\"type\": \"string\"},"
		"    \"windowId\": {\"type\": \"string\"},"
		"    \"windowName\": {\"type\": \"string\"},"
		"    \"delay\": {\"type\": \"number\", \"default\": 12}"
		"  },"
		"  \"required\": [\"text\"]"
		"}",
		tool_type_text);

	mcp_register_tool("key_press",
		"Send keyboard shortcuts (e.g. 'Return', 'ctrl+s', 'alt+F4').",
		"{"
		"  \"type\": \"object\","
		"  \"properties\": {"
		"    \"keys\": {\"type\": \"string\"},"
		"    \"windowId\": {\"type\": \"string\"},"
		"    \"windowName\": {\"type\": \"string\"}"
		"  },"
		"  \"required\": [\"keys\"]"
		"}",
		tool_key_press);

	mcp_register_tool("get_window_geometry",
		"Get position, size, and display scale factor of a window.",
		"{"
		"  \"type\": \"object\","
		"  \"properties\": {"
		"    \"windowId\": {\"type\": \"string\"},"
		"    \"windowName\": {\"type\": \"string\"}"
		"  }"
		"}",
		tool_get_window_geometry);

	mcp_register_tool("resize_window",
		"Resize a window to specified dimensions.",
		"{"
		"  \"type\": \"object\","
		"  \"properties\": {"
		"    \"width\": {\"type\": \"number\"},"
		"    \"height\": {\"type\": \"number\"},"
		"    \"windowId\": {\"type\": \"string\"},"
		"    \"windowName\": {\"type\": \"string\"}"
		"  },"
		"  \"required\": [\"width\", \"height\"]"
		"}",
		tool_resize_window);

	mcp_register_tool("wait_for_window",
		"Wait for a window with given title to appear.",
		"{"
		"  \"type\": \"object\","
		"  \"properties\": {"
		"    \"name\": {\"type\": \"string\"},"
		"    \"timeout\": {\"type\": \"number\", \"default\": 10}"
		"  },"
		"  \"required\": [\"name\"]"
		"}",
		tool_wait_for_window);

	mcp_register_tool("mouse_move",
		"Move mouse to position relative to a window.",
		"{"
		"  \"type\": \"object\","
		"  \"properties\": {"
		"    \"x\": {\"type\": \"number\"},"
		"    \"y\": {\"type\": \"number\"},"
		"    \"windowId\": {\"type\": \"string\"},"
		"    \"windowName\": {\"type\": \"string\"}"
		"  },"
		"  \"required\": [\"x\", \"y\"]"
		"}",
		tool_mouse_move);

	mcp_register_tool("scroll",
		"Scroll up or down in a window.",
		"{"
		"  \"type\": \"object\","
		"  \"properties\": {"
		"    \"direction\": {\"type\": \"string\", \"enum\": [\"up\", \"down\"]},"
		"    \"clicks\": {\"type\": \"number\", \"default\": 3},"
		"    \"windowId\": {\"type\": \"string\"},"
		"    \"windowName\": {\"type\": \"string\"}"
		"  },"
		"  \"required\": [\"direction\"]"
		"}",
		tool_scroll);

	mcp_register_tool("drag",
		"Click-and-drag from one point to another within a window. "
		"Coordinates are relative to window top-left. "
		"Use for sliders, selections, resizing panels, moving objects.",
		"{"
		"  \"type\": \"object\","
		"  \"properties\": {"
		"    \"fromX\": {\"type\": \"number\", \"description\": \"Start X relative to window\"},"
		"    \"fromY\": {\"type\": \"number\", \"description\": \"Start Y relative to window\"},"
		"    \"toX\": {\"type\": \"number\", \"description\": \"End X relative to window\"},"
		"    \"toY\": {\"type\": \"number\", \"description\": \"End Y relative to window\"},"
		"    \"windowId\": {\"type\": \"string\"},"
		"    \"windowName\": {\"type\": \"string\"},"
		"    \"button\": {\"description\": \"1=left, 2=middle, 3=right (or string)\", \"default\": 1},"
		"    \"steps\": {\"type\": \"number\", \"default\": 10, \"description\": \"Smoothness (more=slower)\"}"
		"  },"
		"  \"required\": [\"fromX\", \"fromY\", \"toX\", \"toY\"]"
		"}",
		tool_drag);

	mcp_register_tool("mouse_down",
		"Press and hold a mouse button. Use with mouse_move + mouse_up for complex gestures.",
		"{"
		"  \"type\": \"object\","
		"  \"properties\": {"
		"    \"button\": {\"description\": \"1=left, 2=middle, 3=right (or string)\", \"default\": 1},"
		"    \"x\": {\"type\": \"number\", \"description\": \"Optional X to move to first\"},"
		"    \"y\": {\"type\": \"number\", \"description\": \"Optional Y to move to first\"},"
		"    \"windowId\": {\"type\": \"string\"},"
		"    \"windowName\": {\"type\": \"string\"}"
		"  }"
		"}",
		tool_mouse_down);

	mcp_register_tool("mouse_up",
		"Release a held mouse button.",
		"{"
		"  \"type\": \"object\","
		"  \"properties\": {"
		"    \"button\": {\"description\": \"1=left, 2=middle, 3=right (or string)\", \"default\": 1}"
		"  }"
		"}",
		tool_mouse_up);

	/* ── Tools surfaced by OTelux self-verify — docs/proposed-tools.md ── */

	mcp_register_tool("get_clipboard",
		"Read the current OS clipboard text. Uses wl-paste / xclip / xsel "
		"depending on what's installed. Returns empty string if no owner.",
		"{"
		"  \"type\": \"object\","
		"  \"properties\": {"
		"    \"selection\": {\"type\": \"string\", \"enum\": [\"clipboard\", \"primary\"], \"default\": \"clipboard\"}"
		"  }"
		"}",
		tool_get_clipboard);

	mcp_register_tool("set_clipboard",
		"Write text to the OS clipboard. Uses wl-copy / xclip / xsel.",
		"{"
		"  \"type\": \"object\","
		"  \"properties\": {"
		"    \"text\": {\"type\": \"string\"},"
		"    \"selection\": {\"type\": \"string\", \"enum\": [\"clipboard\", \"primary\"], \"default\": \"clipboard\"}"
		"  },"
		"  \"required\": [\"text\"]"
		"}",
		tool_set_clipboard);

	mcp_register_tool("hover_text",
		"Move the mouse over an OCR-located word, wait for a tooltip to "
		"render, and return only the text that became newly visible.",
		"{"
		"  \"type\": \"object\","
		"  \"properties\": {"
		"    \"text\": {\"type\": \"string\", \"description\": \"Visible text to hover over\"},"
		"    \"windowId\": {\"type\": \"string\"},"
		"    \"windowName\": {\"type\": \"string\"},"
		"    \"settleMs\": {\"type\": \"number\", \"default\": 800, \"description\": \"How long to wait for tooltip to appear\"}"
		"  },"
		"  \"required\": [\"text\"]"
		"}",
		tool_hover_text);

	mcp_register_tool("read_file",
		"Read a file from disk and return its contents. Requires "
		"deskpal to be started with --allow-fs.",
		"{"
		"  \"type\": \"object\","
		"  \"properties\": {"
		"    \"path\": {\"type\": \"string\"},"
		"    \"encoding\": {\"type\": \"string\", \"enum\": [\"utf8\"], \"default\": \"utf8\"},"
		"    \"maxBytes\": {\"type\": \"number\", \"default\": 1048576}"
		"  },"
		"  \"required\": [\"path\"]"
		"}",
		tool_read_file);

	mcp_register_tool("exec",
		"Run a short shell command and return stdout+stderr. Requires "
		"deskpal to be started with --allow-exec. Capped at 60s timeout "
		"and 64KiB output.",
		"{"
		"  \"type\": \"object\","
		"  \"properties\": {"
		"    \"command\": {\"type\": \"string\"},"
		"    \"timeoutMs\": {\"type\": \"number\", \"default\": 5000, \"description\": \"Max 60000\"}"
		"  },"
		"  \"required\": [\"command\"]"
		"}",
		tool_exec);
}
