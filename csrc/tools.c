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
#include "captures.h"
#include "indicator.h"
#include "accessibility.h"
#include "ocr.h"
#include "sessions.h"
#include "uinput.h"
#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <signal.h>
#include <stdarg.h>
#include <time.h>
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

	if (wid_str && wid_str[0]) {
		char *end = NULL;
		errno = 0;
		unsigned long wid = strtoul(wid_str, &end, 0);
		WindowInfo info;
		if (errno != 0 || end == wid_str || *end != '\0' || wid == 0 ||
		    x11_get_window_info(wid, &info) != 0 || !info.viewable)
			return 0;
		return wid;
	}
	if (wname && wname[0]) return x11_find_window(wname);
	return 0;
}

static int explicit_window_requested(const cJSON *params)
{
	const cJSON *wid = params ? cJSON_GetObjectItem(params, "windowId") : NULL;
	const cJSON *name = params ? cJSON_GetObjectItem(params, "windowName") : NULL;
	return wid != NULL || name != NULL;
}

static int window_is_viewable(unsigned long wid)
{
	WindowInfo info;
	return wid != 0 && x11_get_window_info(wid, &info) == 0 && info.viewable;
}

static int revalidate_target(const cJSON *params, unsigned long expected,
	                         WindowInfo *info)
{
	unsigned long current = explicit_window_requested(params)
		? resolve_window(params) : expected;
	if (current == 0 || current != expected ||
	    x11_get_window_info(current, info) != 0 || !info->viewable)
		return -1;
	return 0;
}

static void usleep_ms(int ms)
{
	usleep(ms * 1000);
}

static void append_text(char *buffer, size_t buffer_len, size_t *position,
	                    const char *format, ...)
{
	if (*position >= buffer_len) return;
	va_list args;
	va_start(args, format);
	int written = vsnprintf(buffer + *position, buffer_len - *position,
	                        format, args);
	va_end(args);
	if (written < 0) return;
	size_t available = buffer_len - *position;
	if ((size_t)written >= available)
		*position = buffer_len - 1;
	else
		*position += (size_t)written;
}

static int capture_root_to_file(const char *path)
{
	size_t png_len = 0;
	uint8_t *png = screenshot_capture_png(0, &png_len);
	if (png) {
		FILE *file = fopen(path, "wb");
		if (!file) {
			free(png);
			return -1;
		}
		size_t written = fwrite(png, 1, png_len, file);
		int close_result = fclose(file);
		free(png);
		return written == png_len && close_result == 0 ? 0 : -1;
	}

	/* GNOME's screenshot service can route through the host session even
	 * when DISPLAY points at Xvfb. Never use it from an isolated session. */
	if (getenv("DESKPAL_HEADLESS_ACTIVE")) return -1;

	char cmd[256];
	snprintf(cmd, sizeof(cmd),
		"gnome-screenshot -f \"%s\" 2>/dev/null"
		" || grim \"%s\" 2>/dev/null", path, path);
	unlink(path);
	if (system(cmd) != 0) return -1;
	return access(path, R_OK) == 0 ? 0 : -1;
}

static int png_dimensions(const uint8_t *png, size_t png_len,
	                      int *width, int *height)
{
	static const uint8_t signature[] = { 0x89, 'P', 'N', 'G', '\r', '\n', 0x1a, '\n' };
	if (!png || png_len < 24 || memcmp(png, signature, sizeof(signature)) != 0)
		return -1;
	*width = (int)((uint32_t)png[16] << 24 | (uint32_t)png[17] << 16 |
	               (uint32_t)png[18] << 8 | png[19]);
	*height = (int)((uint32_t)png[20] << 24 | (uint32_t)png[21] << 16 |
	                (uint32_t)png[22] << 8 | png[23]);
	return *width > 0 && *height > 0 ? 0 : -1;
}

static int read_binary_file(const char *path, uint8_t **data, size_t *length)
{
	FILE *file = fopen(path, "rb");
	if (!file) return -1;
	if (fseek(file, 0, SEEK_END) != 0) { fclose(file); return -1; }
	long size = ftell(file);
	if (size <= 0 || fseek(file, 0, SEEK_SET) != 0) {
		fclose(file);
		return -1;
	}
	uint8_t *buffer = malloc((size_t)size);
	if (!buffer) { fclose(file); return -1; }
	size_t read_count = fread(buffer, 1, (size_t)size, file);
	int close_result = fclose(file);
	if (read_count != (size_t)size || close_result != 0) {
		free(buffer);
		return -1;
	}
	*data = buffer;
	*length = (size_t)size;
	return 0;
}

static int write_all(int fd, const uint8_t *data, size_t length)
{
	size_t offset = 0;
	while (offset < length) {
		ssize_t written = write(fd, data + offset, length - offset);
		if (written < 0 && errno == EINTR) continue;
		if (written <= 0) return -1;
		offset += (size_t)written;
	}
	return 0;
}

static long long monotonic_milliseconds(void)
{
	struct timespec now;
	clock_gettime(CLOCK_MONOTONIC, &now);
	return (long long)now.tv_sec * 1000 + now.tv_nsec / 1000000;
}

static int wait_child_deadline(pid_t child, int timeout_ms, int *status)
{
	long long deadline = monotonic_milliseconds() + timeout_ms;
	for (;;) {
		pid_t waited = waitpid(child, status, WNOHANG);
		if (waited == child) return 0;
		if (waited < 0 && errno == EINTR) continue;
		if (waited < 0) return -1;
		if (monotonic_milliseconds() >= deadline) {
			kill(child, SIGTERM);
			for (int i = 0; i < 20; i++) {
				do {
					waited = waitpid(child, status, WNOHANG);
				} while (waited < 0 && errno == EINTR);
				if (waited == child) return 1;
				usleep(50000);
			}
			kill(child, SIGKILL);
			do {
				waited = waitpid(child, status, 0);
			} while (waited < 0 && errno == EINTR);
			return 1;
		}
		usleep(10000);
	}
}

static int resize_png(uint8_t **png, size_t *png_len,
	                  int source_width, int source_height,
	                  int max_width, int max_height,
	                  int *image_width, int *image_height)
{
	double scale = 1.0;
	if (max_width > 0 && source_width > max_width)
		scale = (double)max_width / source_width;
	if (max_height > 0 && source_height * scale > max_height)
		scale = (double)max_height / source_height;

	*image_width = source_width;
	*image_height = source_height;
	if (scale >= 1.0) return 0;

	int target_width = (int)(source_width * scale + 0.5);
	int target_height = (int)(source_height * scale + 0.5);
	if (target_width < 1) target_width = 1;
	if (target_height < 1) target_height = 1;

	char input_path[] = "/tmp/deskpal_scale_in_XXXXXX.png";
	char output_path[] = "/tmp/deskpal_scale_out_XXXXXX.png";
	int input_fd = mkstemps(input_path, 4);
	int output_fd = mkstemps(output_path, 4);
	if (input_fd < 0 || output_fd < 0) {
		if (input_fd >= 0) close(input_fd);
		if (output_fd >= 0) close(output_fd);
		unlink(input_path);
		unlink(output_path);
		return -1;
	}
	close(output_fd);

	int write_result = write_all(input_fd, *png, *png_len);
	int input_close = close(input_fd);
	if (write_result != 0 || input_close != 0) {
		unlink(input_path);
		unlink(output_path);
		return -1;
	}

	char geometry[64];
	snprintf(geometry, sizeof(geometry), "%dx%d!", target_width, target_height);
	pid_t child = fork();
	if (child == 0) {
		int null_fd = open("/dev/null", O_WRONLY);
		if (null_fd >= 0) {
			dup2(null_fd, STDERR_FILENO);
			if (null_fd > STDERR_FILENO) close(null_fd);
		}
		execlp("convert", "convert", input_path, "-filter", "Lanczos",
		       "-resize", geometry, output_path, (char *)NULL);
		_exit(127);
	}
	int timeout_ms = 5000;
	const char *test_timeout = getenv("DESKPAL_TEST_SCALE_TIMEOUT_MS");
	if (test_timeout && test_timeout[0]) {
		int parsed = atoi(test_timeout);
		if (parsed >= 50 && parsed <= 5000) timeout_ms = parsed;
	}
	int status = 0;
	int wait_result = child < 0
		? -1 : wait_child_deadline(child, timeout_ms, &status);
	if (wait_result != 0 ||
	    !WIFEXITED(status) || WEXITSTATUS(status) != 0) {
		unlink(input_path);
		unlink(output_path);
		return -1;
	}

	uint8_t *resized = NULL;
	size_t resized_len = 0;
	int read_result = read_binary_file(output_path, &resized, &resized_len);
	unlink(input_path);
	unlink(output_path);
	if (read_result != 0) return -1;

	free(*png);
	*png = resized;
	*png_len = resized_len;
	*image_width = target_width;
	*image_height = target_height;
	return 0;
}

typedef struct {
	uint8_t *png;
	size_t png_len;
	int source_width;
	int source_height;
	int image_width;
	int image_height;
} CapturedImage;

static int capture_target_image(unsigned long target, int full_screen,
                                int max_width, int max_height,
                                CapturedImage *image,
                                char *error, size_t error_len)
{
	memset(image, 0, sizeof(*image));
	image->png = screenshot_capture_png(target, &image->png_len);

	if (!image->png && target != 0) {
		char path[64];
		snprintf(path, sizeof(path), "/tmp/deskpal_ss_%d.png", getpid());
		char command[256];
		snprintf(command, sizeof(command),
			"import -window 0x%lx png:\"%s\" 2>/dev/null", target, path);
		unlink(path);
		if (system(command) == 0)
			(void)read_binary_file(path, &image->png, &image->png_len);
		unlink(path);
	}

	if (!image->png && full_screen && !getenv("DESKPAL_HEADLESS_ACTIVE")) {
		char path[64];
		snprintf(path, sizeof(path), "/tmp/deskpal_ss_%d.png", getpid());
		char command[256];
		snprintf(command, sizeof(command),
			"gnome-screenshot -f \"%s\" 2>/dev/null"
			" || grim \"%s\" 2>/dev/null", path, path);
		unlink(path);
		if (system(command) == 0)
			(void)read_binary_file(path, &image->png, &image->png_len);
		unlink(path);
	}

	if (!image->png) {
		snprintf(error, error_len, "Screenshot failed: could not capture window");
		return -1;
	}
	if (png_dimensions(image->png, image->png_len,
	                   &image->source_width, &image->source_height) != 0) {
		free(image->png);
		image->png = NULL;
		snprintf(error, error_len, "Screenshot failed: invalid PNG dimensions");
		return -1;
	}
	if (resize_png(&image->png, &image->png_len,
	               image->source_width, image->source_height,
	               max_width, max_height,
	               &image->image_width, &image->image_height) != 0) {
		free(image->png);
		image->png = NULL;
		snprintf(error, error_len,
		         "Screenshot downscaling failed. Ensure ImageMagick 'convert' is installed");
		return -1;
	}
	return 0;
}

/* ── screenshot ──────────────────────────────────────────────────────────── */

cJSON *tool_screenshot(const cJSON *params)
{
	int full_screen = json_bool(params, "fullScreen", 0);
	int max_width = json_int(params, "maxWidth", 0);
	int max_height = json_int(params, "maxHeight", 0);
	if (max_width < 0 || max_width > 8192 || max_height < 0 || max_height > 8192)
		return mcp_tool_error_result("maxWidth/maxHeight must be between 0 and 8192");
	unsigned long wid = resolve_window(params);
	if (!full_screen && !wid && explicit_window_requested(params))
		return mcp_text_result("Window not found");

	unsigned long target = 0; /* 0 = root for full_screen */
	if (!full_screen) {
		target = wid ? wid : x11_get_active_window();
	}

	CapturedImage image;
	char capture_error[256];
	if (capture_target_image(target, full_screen, max_width, max_height,
	                         &image, capture_error, sizeof(capture_error)) != 0)
		return mcp_text_result(capture_error);

	char *b64 = screenshot_base64_encode(image.png, image.png_len);
	free(image.png);

	if (!b64) {
		return mcp_text_result("Screenshot failed: base64 encoding error");
	}

	cJSON *result = mcp_image_result(b64, "image/png");
	free(b64);

	cJSON *metadata = cJSON_CreateObject();
	cJSON_AddNumberToObject(metadata, "sourceWidth", image.source_width);
	cJSON_AddNumberToObject(metadata, "sourceHeight", image.source_height);
	cJSON_AddNumberToObject(metadata, "imageWidth", image.image_width);
	cJSON_AddNumberToObject(metadata, "imageHeight", image.image_height);
	cJSON_AddNumberToObject(metadata, "coordinateScaleX",
	                       (double)image.source_width / image.image_width);
	cJSON_AddNumberToObject(metadata, "coordinateScaleY",
	                       (double)image.source_height / image.image_height);
	DeskpalCapture capture = {0};
	if (full_screen) {
		if (captures_store_desktop(image.source_width, image.source_height,
		                           image.image_width, image.image_height, &capture) != 0) {
			cJSON_Delete(metadata);
			cJSON_Delete(result);
			return mcp_tool_error_result("Could not assign screenshot capture ID");
		}
		cJSON_AddStringToObject(metadata, "captureId", capture.id);
		cJSON_AddStringToObject(metadata, "captureTarget", "desktop");
		cJSON_AddStringToObject(metadata, "captureCoordinateSpace", "image-pixels");
	}
	cJSON_AddItemToObject(result, "screenshot", metadata);

	if (full_screen) {
		char note[192];
		snprintf(note, sizeof(note),
			"Capture ID: %s. agent_cursor_move coordinates use pixels in this %dx%d image.",
			capture.id, image.image_width, image.image_height);
		cJSON *content = cJSON_GetObjectItem(result, "content");
		cJSON *text_item = cJSON_CreateObject();
		cJSON_AddStringToObject(text_item, "type", "text");
		cJSON_AddStringToObject(text_item, "text", note);
		cJSON_AddItemToArray(content, text_item);
	}

	if (image.image_width != image.source_width ||
	    image.image_height != image.source_height) {
		char note[256];
		snprintf(note, sizeof(note),
			"Screenshot downscaled from %dx%d to %dx%d. Input-tool coordinates "
			"use source pixels; multiply image x by %.4f and y by %.4f.",
			image.source_width, image.source_height,
			image.image_width, image.image_height,
			(double)image.source_width / image.image_width,
			(double)image.source_height / image.image_height);
		cJSON *content = cJSON_GetObjectItem(result, "content");
		cJSON *text_item = cJSON_CreateObject();
		cJSON_AddStringToObject(text_item, "type", "text");
		cJSON_AddStringToObject(text_item, "text", note);
		cJSON_AddItemToArray(content, text_item);
	}
	return result;
}

/* ── agent cursor indicator ──────────────────────────────────────────────── */

#define INDICATOR_STATUS_LIMIT (64 * 1024)
#define INDICATOR_SETTLE_TIMEOUT_MS 1200

static cJSON *structured_text_result(cJSON *payload, const char *metadata_key)
{
	char *text = cJSON_PrintUnformatted(payload);
	if (!text) {
		cJSON_Delete(payload);
		return mcp_tool_error_result("Could not serialize indicator result");
	}
	cJSON *result = mcp_text_result(text);
	free(text);
	cJSON_AddItemToObject(result, metadata_key, payload);
	return result;
}

static int resolve_app_state_target(const cJSON *params, WindowInfo *target,
                                    const char **error_code,
                                    char *error, size_t error_len)
{
	*error_code = "invalid_target";
	const cJSON *id = cJSON_GetObjectItem(params, "windowId");
	const cJSON *name = cJSON_GetObjectItem(params, "windowName");
	if ((id != NULL) == (name != NULL)) {
		snprintf(error, error_len,
		         "Specify exactly one of windowId or exact windowName");
		return -1;
	}
	if (id) {
		if (!cJSON_IsString(id) || !id->valuestring[0]) {
			snprintf(error, error_len, "windowId must be a non-empty string");
			return -1;
		}
		char *end = NULL;
		errno = 0;
		unsigned long wid = strtoul(id->valuestring, &end, 0);
		if (errno || end == id->valuestring || *end || wid == 0 ||
		    x11_get_window_info(wid, target) != 0 || !target->viewable) {
			*error_code = "target_unavailable";
			snprintf(error, error_len, "Exact windowId is unavailable or not viewable");
			return -1;
		}
	} else {
		if (!cJSON_IsString(name) || !name->valuestring[0]) {
			snprintf(error, error_len, "windowName must be a non-empty exact title");
			return -1;
		}
		int matches = 0;
		int complete = 0;
		if (x11_find_window_exact(name->valuestring, target,
		                          &matches, &complete) != 0 || !complete) {
			*error_code = "target_traversal_incomplete";
			snprintf(error, error_len,
			         "Exact window traversal was incomplete; retry the observation");
			return -1;
		}
		if (matches == 0) {
			*error_code = "target_not_found_or_unsupported_backend";
			snprintf(error, error_len,
			         "Exact windowName was not found; the target may be native Wayland");
			return -1;
		}
		if (matches != 1) {
			*error_code = "target_ambiguous";
			snprintf(error, error_len,
			         "Exact windowName is ambiguous (%d live matches)", matches);
			return -1;
		}
	}
	if (target->pid <= 0 || !target->title[0] || !target->app_class[0]) {
		*error_code = "target_identity_incomplete";
		snprintf(error, error_len,
		         "Target lacks the PID, title, or class required for stable identity");
		return -1;
	}
	return 0;
}

static int same_window_identity(const WindowInfo *left, const WindowInfo *right)
{
	return left->id == right->id && left->pid == right->pid &&
	       strcmp(left->title, right->title) == 0 &&
	       strcmp(left->app_class, right->app_class) == 0;
}

static int same_window_geometry(const WindowInfo *left, const WindowInfo *right)
{
	return left->x == right->x && left->y == right->y &&
	       left->width == right->width && left->height == right->height;
}

static cJSON *window_geometry_json(const WindowInfo *window)
{
	cJSON *geometry = cJSON_CreateObject();
	cJSON_AddNumberToObject(geometry, "x", window->x);
	cJSON_AddNumberToObject(geometry, "y", window->y);
	cJSON_AddNumberToObject(geometry, "width", window->width);
	cJSON_AddNumberToObject(geometry, "height", window->height);
	return geometry;
}

static cJSON *window_identity_json(const WindowInfo *window)
{
	cJSON *identity = cJSON_CreateObject();
	char window_id[32];
	snprintf(window_id, sizeof(window_id), "0x%lx", window->id);
	cJSON_AddStringToObject(identity, "backend", "x11");
	cJSON_AddStringToObject(identity, "windowId", window_id);
	cJSON_AddStringToObject(identity, "title", window->title);
	cJSON_AddStringToObject(identity, "class", window->app_class);
	cJSON_AddNumberToObject(identity, "processId", (double)window->pid);
	cJSON_AddItemToObject(identity, "geometry", window_geometry_json(window));
	return identity;
}

static int count_semantic_nodes(const cJSON *nodes, int *actionable)
{
	if (!cJSON_IsArray(nodes)) return 0;
	int count = 0;
	const cJSON *node = NULL;
	cJSON_ArrayForEach(node, nodes) {
		count++;
		const cJSON *actions = cJSON_GetObjectItem(node, "actions");
		if (cJSON_IsArray(actions) && cJSON_GetArraySize(actions) > 0)
			(*actionable)++;
		count += count_semantic_nodes(cJSON_GetObjectItem(node, "children"),
		                              actionable);
	}
	return count;
}

static void filter_semantic_process(cJSON *semantic, long process_id)
{
	cJSON *applications = cJSON_GetObjectItem(semantic, "applications");
	int original_matches = cJSON_IsArray(applications)
		? cJSON_GetArraySize(applications) : 0;
	if (cJSON_IsArray(applications)) {
		for (int i = cJSON_GetArraySize(applications) - 1; i >= 0; i--) {
			cJSON *application = cJSON_GetArrayItem(applications, i);
			const cJSON *pid = cJSON_GetObjectItem(application, "processId");
			if (!cJSON_IsNumber(pid) || (long)pid->valuedouble != process_id)
				cJSON_DeleteItemFromArray(applications, i);
		}
	}
	int app_count = cJSON_IsArray(applications)
		? cJSON_GetArraySize(applications) : 0;
	int window_count = 0;
	int node_count = 0;
	int actionable_count = 0;
	const cJSON *application = NULL;
	cJSON_ArrayForEach(application, applications) {
		const cJSON *windows = cJSON_GetObjectItem(application, "windows");
		window_count += cJSON_IsArray(windows) ? cJSON_GetArraySize(windows) : 0;
		const cJSON *window = NULL;
		cJSON_ArrayForEach(window, windows)
			node_count += count_semantic_nodes(
				cJSON_GetObjectItem(window, "nodes"), &actionable_count);
	}
	cJSON_ReplaceItemInObject(semantic, "matchedApplicationCount",
	                         cJSON_CreateNumber(app_count));
	cJSON_ReplaceItemInObject(semantic, "matchedWindowCount",
	                         cJSON_CreateNumber(window_count));
	cJSON_ReplaceItemInObject(semantic, "nodeCount",
	                         cJSON_CreateNumber(node_count));
	cJSON_ReplaceItemInObject(semantic, "actionableNodeCount",
	                         cJSON_CreateNumber(actionable_count));
	cJSON_AddNumberToObject(semantic, "targetProcessId", (double)process_id);
	cJSON_AddNumberToObject(semantic, "exactTitleApplicationMatchesBeforeFilter",
	                       original_matches);
	cJSON_AddBoolToObject(semantic, "targetProcessMatched", app_count == 1);
	cJSON_AddBoolToObject(semantic, "targetWindowMatched",
	                     app_count == 1 && window_count == 1);
	const cJSON *available = cJSON_GetObjectItem(semantic, "available");
	if (cJSON_IsTrue(available) && node_count == 0)
		cJSON_ReplaceItemInObject(semantic, "capability",
		                         cJSON_CreateString("empty"));
	cJSON_AddStringToObject(semantic, "coordinateSpace", "atspi-logical");
}

#define APP_STATE_METADATA_LIMIT (3 * 1024 * 1024)

static cJSON *app_state_error_result(const char *code, const char *message,
                                     int retry_recommended)
{
	cJSON *payload = cJSON_CreateObject();
	cJSON_AddStringToObject(payload, "code", code);
	cJSON_AddStringToObject(payload, "message", message);
	cJSON_AddBoolToObject(payload, "retryRecommended", retry_recommended);
	cJSON *result = structured_text_result(payload, "appStateError");
	cJSON_AddBoolToObject(result, "isError", 1);
	return result;
}

cJSON *tool_get_app_state(const cJSON *params)
{
	int max_width = json_int(params, "maxWidth", 1920);
	int max_height = json_int(params, "maxHeight", 1080);
	int max_depth = json_int(params, "semanticMaxDepth", 4);
	int max_nodes = json_int(params, "semanticMaxNodes", 120);
	int include_text = json_bool(params, "includeText", 0);
	int include_attributes = json_bool(params, "includeAttributes", 0);
	if (max_width < 0 || max_width > 8192 || max_height < 0 || max_height > 8192)
		return mcp_tool_error_result("maxWidth/maxHeight must be between 0 and 8192");
	if (max_depth < 1 || max_depth > 8 || max_nodes < 1 || max_nodes > 300)
		return mcp_tool_error_result("semanticMaxDepth/maxNodes exceed app-state bounds");

	char error[256] = {0};
	const char *target_error_code = "invalid_target";
	WindowInfo before;
	if (resolve_app_state_target(params, &before, &target_error_code,
	                             error, sizeof(error)) != 0)
		return app_state_error_result(target_error_code, error,
			strcmp(target_error_code, "target_traversal_incomplete") == 0 ||
			strcmp(target_error_code, "target_unavailable") == 0);
	unsigned long focused_before = x11_get_active_window();

	CapturedImage image;
	if (capture_target_image(before.id, 0, max_width, max_height,
	                         &image, error, sizeof(error)) != 0)
		return mcp_tool_error_result(error);

	cJSON *semantic = accessibility_tree_exact(NULL, before.title,
		max_depth, max_nodes, 0, include_text, include_attributes);
	if (!semantic) semantic = cJSON_CreateObject();
	filter_semantic_process(semantic, before.pid);

	WindowInfo after;
	int resolved_after = resolve_app_state_target(
		params, &after, &target_error_code, error, sizeof(error)) == 0;
	unsigned long focused_after = x11_get_active_window();
	if (!resolved_after || !same_window_identity(&before, &after)) {
		free(image.png);
		cJSON_Delete(semantic);
		return app_state_error_result("target_replaced_during_observation",
			"Target disappeared, was replaced, or became ambiguous during observation",
			1);
	}

	int geometry_stable = same_window_geometry(&before, &after);
	int focus_stable = focused_before == focused_after;
	int transform_supported = image.source_width == before.width &&
	                          image.source_height == before.height;
	int stable = geometry_stable && focus_stable && transform_supported;

	DeskpalCapture capture = {0};
	if (stable && captures_store_window(before.id, before.pid,
	                                   before.title, before.app_class,
	                                   before.x, before.y,
	                                   before.width, before.height,
	                                   image.source_width, image.source_height,
	                                   image.image_width, image.image_height,
	                                   &capture) != 0) {
		free(image.png);
		cJSON_Delete(semantic);
		return mcp_tool_error_result("Could not register stable app-state capture");
	}

	char *base64 = screenshot_base64_encode(image.png, image.png_len);
	free(image.png);
	if (!base64) {
		cJSON_Delete(semantic);
		return mcp_tool_error_result("App-state image base64 encoding failed");
	}
	cJSON *result = mcp_image_result(base64, "image/png");
	free(base64);

	cJSON *state = cJSON_CreateObject();
	cJSON_AddItemToObject(state, "target", window_identity_json(&before));
	cJSON *focus = cJSON_CreateObject();
	char before_focus_id[32];
	char after_focus_id[32];
	snprintf(before_focus_id, sizeof(before_focus_id), "0x%lx", focused_before);
	snprintf(after_focus_id, sizeof(after_focus_id), "0x%lx", focused_after);
	cJSON_AddStringToObject(focus, "activeWindowIdBefore", before_focus_id);
	cJSON_AddStringToObject(focus, "activeWindowIdAfter", after_focus_id);
	cJSON_AddBoolToObject(focus, "targetFocusedBefore", focused_before == before.id);
	cJSON_AddBoolToObject(focus, "targetFocusedAfter", focused_after == before.id);
	cJSON_AddItemToObject(state, "focus", focus);

	cJSON *image_meta = cJSON_CreateObject();
	cJSON_AddNumberToObject(image_meta, "sourceWidth", image.source_width);
	cJSON_AddNumberToObject(image_meta, "sourceHeight", image.source_height);
	cJSON_AddNumberToObject(image_meta, "imageWidth", image.image_width);
	cJSON_AddNumberToObject(image_meta, "imageHeight", image.image_height);
	cJSON_AddNumberToObject(image_meta, "coordinateScaleX",
	                       (double)image.source_width / image.image_width);
	cJSON_AddNumberToObject(image_meta, "coordinateScaleY",
	                       (double)image.source_height / image.image_height);
	cJSON_AddItemToObject(state, "image", image_meta);

	cJSON *transform = cJSON_CreateObject();
	cJSON_AddStringToObject(transform, "imageSpace", "window-image-pixels");
	cJSON_AddStringToObject(transform, "targetSpace", "desktop-stage-pixels");
	cJSON_AddNumberToObject(transform, "offsetX", before.x);
	cJSON_AddNumberToObject(transform, "offsetY", before.y);
	cJSON_AddNumberToObject(transform, "scaleX",
	                       (double)image.source_width / image.image_width);
	cJSON_AddNumberToObject(transform, "scaleY",
	                       (double)image.source_height / image.image_height);
	cJSON_AddBoolToObject(transform, "supported", transform_supported);
	cJSON_AddItemToObject(state, "transform", transform);
	if (stable) cJSON_AddStringToObject(state, "captureId", capture.id);
	cJSON_AddItemToObject(state, "semantic", semantic);

	cJSON *consistency = cJSON_CreateObject();
	cJSON_AddBoolToObject(consistency, "identityStable", 1);
	cJSON_AddBoolToObject(consistency, "geometryStable", geometry_stable);
	cJSON_AddBoolToObject(consistency, "focusStable", focus_stable);
	cJSON_AddBoolToObject(consistency, "transformSupported", transform_supported);
	cJSON_AddBoolToObject(consistency, "stable", stable);
	cJSON_AddBoolToObject(consistency, "retryRecommended", !stable);
	if (!geometry_stable)
		cJSON_AddItemToObject(consistency, "geometryAfter",
		                     window_geometry_json(&after));
	cJSON_AddItemToObject(state, "consistency", consistency);
	cJSON_AddBoolToObject(state, "sharedPointerMoved", 0);
	cJSON_AddBoolToObject(state, "inputDelivered", 0);
	cJSON_AddBoolToObject(state, "focusChanged", 0);
	cJSON_AddBoolToObject(state, "stackingChanged", 0);
	cJSON_AddBoolToObject(state, "clipboardChanged", 0);

	char *state_text = cJSON_PrintUnformatted(state);
	if (!state_text || strlen(state_text) > APP_STATE_METADATA_LIMIT) {
		free(state_text);
		cJSON_Delete(state);
		cJSON_Delete(result);
		return mcp_tool_error_result(
			"App-state metadata exceeded the 3 MiB safety limit; reduce semanticMaxNodes or disable text/attributes");
	}
	cJSON *content = cJSON_GetObjectItem(result, "content");
	cJSON *text_item = cJSON_CreateObject();
	cJSON_AddStringToObject(text_item, "type", "text");
	cJSON_AddStringToObject(text_item, "text", state_text);
	cJSON_AddItemToArray(content, text_item);
	free(state_text);
	cJSON_AddItemToObject(result, "appState", state);
	return result;
}

static cJSON *owned_indicator_status(char *error, size_t error_len)
{
	char *raw = NULL;
	if (indicator_get_status(&raw, error, error_len) != 0) return NULL;
	if (strlen(raw) > INDICATOR_STATUS_LIMIT) {
		free(raw);
		snprintf(error, error_len, "Indicator status exceeded the 64 KiB limit");
		return NULL;
	}
	cJSON *status = cJSON_Parse(raw);
	free(raw);
	if (!status || !cJSON_IsObject(status)) {
		cJSON_Delete(status);
		snprintf(error, error_len, "Indicator returned invalid status JSON");
		return NULL;
	}

	const cJSON *stage_width = cJSON_GetObjectItem(status, "stageWidth");
	const cJSON *stage_height = cJSON_GetObjectItem(status, "stageHeight");
	const cJSON *coordinate_space = cJSON_GetObjectItem(status, "coordinateSpace");
	const cJSON *all_cursors = cJSON_GetObjectItem(status, "cursors");
	if (!cJSON_IsNumber(stage_width) || !cJSON_IsNumber(stage_height) ||
	    stage_width->valuedouble != stage_width->valueint ||
	    stage_height->valuedouble != stage_height->valueint ||
	    stage_width->valueint <= 0 || stage_width->valueint > 65536 ||
	    stage_height->valueint <= 0 || stage_height->valueint > 65536 ||
	    !cJSON_IsString(coordinate_space) ||
	    strcmp(coordinate_space->valuestring, "gnome-stage-logical") != 0 ||
	    !cJSON_IsArray(all_cursors)) {
		cJSON_Delete(status);
		snprintf(error, error_len, "Indicator status has an invalid geometry contract");
		return NULL;
	}

	cJSON *owned_cursors = cJSON_CreateArray();
	if (!owned_cursors) {
		cJSON_Delete(status);
		snprintf(error, error_len, "Out of memory filtering indicator status");
		return NULL;
	}
	const cJSON *cursor = NULL;
	cJSON_ArrayForEach(cursor, all_cursors) {
		const cJSON *remote_id = cJSON_GetObjectItem(cursor, "cursorId");
		if (!cJSON_IsString(remote_id)) continue;
		char logical_id[DESKPAL_INDICATOR_CURSOR_ID_LEN];
		if (indicator_logical_id_for_remote(remote_id->valuestring,
		                                    logical_id, sizeof(logical_id)) != 0)
			continue;
		cJSON *copy = cJSON_Duplicate(cursor, 1);
		if (!copy) continue;
		cJSON_ReplaceItemInObject(copy, "cursorId", cJSON_CreateString(logical_id));
		cJSON_AddItemToArray(owned_cursors, copy);
	}
	if (!cJSON_ReplaceItemInObject(status, "cursors", owned_cursors)) {
		cJSON_Delete(owned_cursors);
		cJSON_Delete(status);
		snprintf(error, error_len, "Could not filter indicator cursor ownership");
		return NULL;
	}
	cJSON_AddBoolToObject(status, "available", 1);
	return status;
}

static int indicator_has_single_full_stage_monitor(const cJSON *status)
{
	const cJSON *monitors = cJSON_GetObjectItem(status, "monitors");
	if (!cJSON_IsArray(monitors) || cJSON_GetArraySize(monitors) != 1) return 0;
	const cJSON *monitor = cJSON_GetArrayItem(monitors, 0);
	const cJSON *stage_width = cJSON_GetObjectItem(status, "stageWidth");
	const cJSON *stage_height = cJSON_GetObjectItem(status, "stageHeight");
	const cJSON *x = cJSON_GetObjectItem(monitor, "x");
	const cJSON *y = cJSON_GetObjectItem(monitor, "y");
	const cJSON *width = cJSON_GetObjectItem(monitor, "width");
	const cJSON *height = cJSON_GetObjectItem(monitor, "height");
	const cJSON *scale = cJSON_GetObjectItem(monitor, "scale");
	return cJSON_IsNumber(x) && x->valueint == 0 &&
	       cJSON_IsNumber(y) && y->valueint == 0 &&
	       cJSON_IsNumber(width) && width->valueint == stage_width->valueint &&
	       cJSON_IsNumber(height) && height->valueint == stage_height->valueint &&
	       cJSON_IsNumber(scale) && scale->valuedouble > 0;
}

static const cJSON *owned_cursor_by_id(const cJSON *status, const char *cursor_id)
{
	const cJSON *cursors = cJSON_GetObjectItem(status, "cursors");
	const cJSON *cursor = NULL;
	cJSON_ArrayForEach(cursor, cursors) {
		const cJSON *id = cJSON_GetObjectItem(cursor, "cursorId");
		if (cJSON_IsString(id) && strcmp(id->valuestring, cursor_id) == 0)
			return cursor;
	}
	return NULL;
}

static cJSON *indicator_side_effect_payload(const char *cursor_id)
{
	cJSON *payload = cJSON_CreateObject();
	cJSON_AddStringToObject(payload, "cursorId", cursor_id);
	cJSON_AddBoolToObject(payload, "inputDelivered", 0);
	cJSON_AddBoolToObject(payload, "sharedPointerMoved", 0);
	cJSON_AddBoolToObject(payload, "focusChanged", 0);
	cJSON_AddBoolToObject(payload, "stackingChanged", 0);
	cJSON_AddBoolToObject(payload, "clipboardChanged", 0);
	return payload;
}

cJSON *tool_agent_cursor_status(const cJSON *params)
{
	(void)params;
	char error[256] = {0};
	cJSON *status = owned_indicator_status(error, sizeof(error));
	if (status) return structured_text_result(status, "indicator");

	cJSON *unavailable = cJSON_CreateObject();
	cJSON_AddBoolToObject(unavailable, "available", 0);
	cJSON_AddStringToObject(unavailable, "blocker", error);
	return structured_text_result(unavailable, "indicator");
}

static int window_capture_still_valid(const DeskpalCapture *capture)
{
	WindowInfo current;
	return x11_get_window_info(capture->window_id, &current) == 0 &&
	       current.viewable && current.pid == capture->process_id &&
	       strcmp(current.title, capture->title) == 0 &&
	       strcmp(current.app_class, capture->app_class) == 0 &&
	       current.x == capture->window_x && current.y == capture->window_y &&
	       current.width == capture->window_width &&
	       current.height == capture->window_height;
}

cJSON *tool_agent_cursor_move(const cJSON *params)
{
	const char *capture_id = json_str(params, "captureId", NULL);
	const char *cursor_id = json_str(params, "cursorId", "primary");
	const char *color = json_str(params, "color", "#36C5F0");
	const char *label = json_str(params, "label", cursor_id);
	const cJSON *x_item = cJSON_GetObjectItem(params, "x");
	const cJSON *y_item = cJSON_GetObjectItem(params, "y");
	if (!capture_id || !capture_id[0] || strlen(capture_id) >= DESKPAL_CAPTURE_ID_LEN)
		return mcp_tool_error_result("captureId must identify a recent stable capture");
	if (!cJSON_IsNumber(x_item) || !cJSON_IsNumber(y_item) ||
	    x_item->valuedouble != x_item->valueint ||
	    y_item->valuedouble != y_item->valueint)
		return mcp_tool_error_result("x and y must be integral image pixels");

	DeskpalCapture capture;
	int lookup = captures_lookup(capture_id, &capture);
	if (lookup == -2)
		return mcp_tool_error_result("captureId is stale; take a fresh observation");
	if (lookup != 0)
		return mcp_tool_error_result("captureId is unknown or no longer retained");
	int image_x = x_item->valueint;
	int image_y = y_item->valueint;
	if (image_x < 0 || image_x >= capture.image_width ||
	    image_y < 0 || image_y >= capture.image_height)
		return mcp_tool_error_result("x/y fall outside the capture image bounds");
	if (capture.target == DESKPAL_CAPTURE_WINDOW &&
	    !window_capture_still_valid(&capture))
		return mcp_tool_error_result(
			"Window capture target was replaced or its geometry changed; observe again");

	char error[256] = {0};
	cJSON *before = owned_indicator_status(error, sizeof(error));
	if (!before) return mcp_tool_error_result(error);
	if (!indicator_has_single_full_stage_monitor(before)) {
		cJSON_Delete(before);
		return mcp_tool_error_result(
			"agent_cursor_move currently supports exactly one full-stage monitor; mixed-scale and multi-monitor layouts fail closed");
	}
	int stage_width = cJSON_GetObjectItem(before, "stageWidth")->valueint;
	int stage_height = cJSON_GetObjectItem(before, "stageHeight")->valueint;
	int stage_x = 0;
	int stage_y = 0;
	if (capture.target == DESKPAL_CAPTURE_DESKTOP) {
		double source_scale_x = (double)stage_width / capture.source_width;
		double source_scale_y = (double)stage_height / capture.source_height;
		double scale_max = fmax(source_scale_x, source_scale_y);
		if (scale_max <= 0 ||
		    fabs(source_scale_x - source_scale_y) > scale_max * 0.01) {
			cJSON_Delete(before);
			return mcp_tool_error_result(
				"Capture and GNOME stage geometry no longer share one coordinate transform");
		}
		stage_x = (int)lround((double)image_x * stage_width / capture.image_width);
		stage_y = (int)lround((double)image_y * stage_height / capture.image_height);
	} else if (capture.target == DESKPAL_CAPTURE_WINDOW) {
		stage_x = capture.window_x + (int)lround(
			(double)image_x * capture.source_width / capture.image_width);
		stage_y = capture.window_y + (int)lround(
			(double)image_y * capture.source_height / capture.image_height);
	} else {
		cJSON_Delete(before);
		return mcp_tool_error_result("captureId has an unsupported target type");
	}
	cJSON_Delete(before);
	if (stage_x < 0 || stage_y < 0 ||
	    stage_x >= stage_width || stage_y >= stage_height)
		return mcp_tool_error_result(
			"Resolved cursor point falls outside the current GNOME stage");

	int created = 0;
	int mutation_issued = 0;
	int outcome_unknown = 0;
	if (indicator_move_owned(cursor_id, stage_x, stage_y, color, label,
	                         &created, &mutation_issued, &outcome_unknown,
	                         error, sizeof(error)) != 0) {
		cJSON *payload = indicator_side_effect_payload(cursor_id);
		cJSON_AddStringToObject(payload, "captureId", capture_id);
		cJSON_AddBoolToObject(payload, "mutationIssued", mutation_issued);
		cJSON_AddBoolToObject(payload, "actionOutcomeUnknown", outcome_unknown);
		cJSON_AddBoolToObject(payload, "indicatorMoved", 0);
		cJSON_AddBoolToObject(payload, "verified", 0);
		cJSON_AddStringToObject(payload, "error", error);
		cJSON *result = structured_text_result(payload, "indicator");
		cJSON_AddBoolToObject(result, "isError", 1);
		return result;
	}

	cJSON *settled_status = NULL;
	const cJSON *settled_cursor = NULL;
	for (int elapsed = 0; elapsed <= INDICATOR_SETTLE_TIMEOUT_MS; elapsed += 20) {
		settled_status = owned_indicator_status(error, sizeof(error));
		if (!settled_status) break;
		settled_cursor = owned_cursor_by_id(settled_status, cursor_id);
		const cJSON *state = settled_cursor
			? cJSON_GetObjectItem(settled_cursor, "state") : NULL;
		const cJSON *target_x = settled_cursor
			? cJSON_GetObjectItem(settled_cursor, "x") : NULL;
		const cJSON *target_y = settled_cursor
			? cJSON_GetObjectItem(settled_cursor, "y") : NULL;
		if (cJSON_IsString(state) && strcmp(state->valuestring, "idle") == 0 &&
		    cJSON_IsNumber(target_x) && target_x->valueint == stage_x &&
		    cJSON_IsNumber(target_y) && target_y->valueint == stage_y)
			break;
		settled_cursor = NULL;
		cJSON_Delete(settled_status);
		settled_status = NULL;
		usleep_ms(20);
	}

	cJSON *payload = indicator_side_effect_payload(cursor_id);
	cJSON_AddStringToObject(payload, "captureId", capture_id);
	cJSON *image_position = cJSON_CreateObject();
	cJSON_AddNumberToObject(image_position, "x", image_x);
	cJSON_AddNumberToObject(image_position, "y", image_y);
	cJSON_AddItemToObject(payload, "imagePosition", image_position);
	cJSON *stage_position = cJSON_CreateObject();
	cJSON_AddNumberToObject(stage_position, "x", stage_x);
	cJSON_AddNumberToObject(stage_position, "y", stage_y);
	cJSON_AddItemToObject(payload, "stagePosition", stage_position);
	int verified = settled_cursor != NULL;
	cJSON_AddBoolToObject(payload, "created", created);
	cJSON_AddBoolToObject(payload, "mutationIssued", 1);
	cJSON_AddBoolToObject(payload, "actionOutcomeUnknown", !verified);
	cJSON_AddBoolToObject(payload, "indicatorMoved", verified);
	cJSON_AddBoolToObject(payload, "verified", verified);
	if (verified)
		cJSON_AddItemToObject(payload, "cursor", cJSON_Duplicate(settled_cursor, 1));
	else
		cJSON_AddStringToObject(payload, "error",
			error[0] ? error : "Indicator movement did not settle before timeout");
	cJSON_Delete(settled_status);

	cJSON *result = structured_text_result(payload, "indicator");
	if (!verified) cJSON_AddBoolToObject(result, "isError", 1);
	return result;
}

cJSON *tool_agent_cursor_hide(const cJSON *params)
{
	const char *cursor_id = json_str(params, "cursorId", "primary");
	int hidden = 0;
	int mutation_issued = 0;
	int outcome_unknown = 0;
	char error[256] = {0};
	int rc = indicator_hide_owned(cursor_id, &hidden, &mutation_issued,
	                              &outcome_unknown,
	                              error, sizeof(error));
	cJSON *payload = indicator_side_effect_payload(cursor_id);
	cJSON_AddBoolToObject(payload, "mutationIssued", mutation_issued);
	cJSON_AddBoolToObject(payload, "actionOutcomeUnknown", outcome_unknown);
	cJSON_AddBoolToObject(payload, "hidden", rc == 0 && hidden);
	cJSON_AddBoolToObject(payload, "verified", rc == 0 && !outcome_unknown);
	if (rc != 0) cJSON_AddStringToObject(payload, "error", error);
	cJSON *result = structured_text_result(payload, "indicator");
	if (rc != 0) cJSON_AddBoolToObject(result, "isError", 1);
	return result;
}

/* ── environment status ──────────────────────────────────────────────────── */

static cJSON *environment_capability(int available, const char *backend,
                                     int shared_seat, int non_interfering)
{
	cJSON *capability = cJSON_CreateObject();
	cJSON_AddBoolToObject(capability, "available", available);
	cJSON_AddStringToObject(capability, "backend", backend);
	cJSON_AddBoolToObject(capability, "sharedSeat", shared_seat);
	cJSON_AddBoolToObject(capability, "nonInterfering", non_interfering);
	return capability;
}

static void add_environment_notice(cJSON *array, const char *id,
                                   const char *severity, const char *message)
{
	cJSON *notice = cJSON_CreateObject();
	cJSON_AddStringToObject(notice, "id", id);
	if (severity) cJSON_AddStringToObject(notice, "severity", severity);
	cJSON_AddStringToObject(notice, "message", message);
	cJSON_AddItemToArray(array, notice);
}

cJSON *tool_get_environment_status(const cJSON *params)
{
	(void)params;
	int headless = getenv("DESKPAL_HEADLESS_ACTIVE") != NULL;
	int wayland = x11_is_wayland();
	int pointer_uinput = uinput_available();
	int keyboard_uinput = uinput_kbd_available();
	int semantic_available = accessibility_available();
	int ocr_enabled = ocr_available();

	char indicator_error[256] = {0};
	cJSON *indicator_status = owned_indicator_status(
		indicator_error, sizeof(indicator_error));
	int indicator_available = indicator_status != NULL;
	int indicator_single_monitor = indicator_available &&
		indicator_has_single_full_stage_monitor(indicator_status);

	cJSON *payload = cJSON_CreateObject();
	cJSON_AddStringToObject(payload, "scope", headless ? "isolated" : "visible-desktop");
	cJSON_AddStringToObject(payload, "displayServer", wayland ? "wayland-xwayland" : "x11");
	cJSON_AddBoolToObject(payload, "sharedSeat", !headless);

	const char *window_backend = wayland ? "x11-xwayland" : "x11";
	const char *capture_backend = headless ? "x11" : "x11-with-host-fallback";
	cJSON *backends = cJSON_CreateObject();
	cJSON_AddStringToObject(backends, "windowDiscovery", window_backend);
	cJSON_AddStringToObject(backends, "capture", capture_backend);
	cJSON_AddStringToObject(backends, "semantic",
	                      semantic_available ? "atspi" : "unavailable");
	cJSON_AddStringToObject(backends, "pointer",
	                      pointer_uinput ? "uinput-and-xtest" : "xtest");
	cJSON_AddStringToObject(backends, "keyboard",
	                      keyboard_uinput ? "uinput-and-xtest" : "xtest");
	cJSON_AddStringToObject(backends, "indicator",
	                      indicator_available ? "gnome-shell-dbus" : "unavailable");
	cJSON_AddItemToObject(payload, "selectedBackends", backends);

	cJSON *capabilities = cJSON_CreateObject();
	cJSON_AddItemToObject(capabilities, "windowDiscovery",
	                     environment_capability(1, window_backend, 0, 1));
	cJSON_AddItemToObject(capabilities, "capture",
	                     environment_capability(1, capture_backend, 0, 1));
	cJSON_AddItemToObject(capabilities, "pointerInput",
	                     environment_capability(1,
	                         pointer_uinput ? "uinput-and-xtest" : "xtest",
	                         !headless, headless));
	cJSON_AddItemToObject(capabilities, "keyboardInput",
	                     environment_capability(1,
	                         keyboard_uinput ? "uinput-and-xtest" : "xtest",
	                         !headless, headless));
	cJSON_AddItemToObject(capabilities, "semantic",
	                     environment_capability(semantic_available, "atspi", 0, 0));
	cJSON_AddItemToObject(capabilities, "ocr",
	                     environment_capability(ocr_enabled, "tesseract", 0, 1));
	cJSON_AddItemToObject(capabilities, "agentCursor",
	                     environment_capability(indicator_available,
	                         indicator_available ? "gnome-shell-dbus" : "unavailable",
	                         0, indicator_available));
	cJSON_AddItemToObject(capabilities, "nativeWaylandSurfaceControl",
	                     environment_capability(0, "unavailable", 0, 0));
	cJSON_AddItemToObject(capabilities, "backgroundPixelInput",
	                     environment_capability(0, "unavailable", 0, 0));
	cJSON_AddItemToObject(capabilities, "processLaunch",
	                     environment_capability(deskpal_allow_exec, "deskpal-exec-gate", 0, headless));
	cJSON_AddItemToObject(capabilities, "filesystem",
	                     environment_capability(deskpal_allow_fs, "deskpal-fs-gate", 0, 1));
	cJSON_AddItemToObject(capabilities, "accessibilityStatus", accessibility_status());
	if (indicator_status)
		cJSON_AddItemToObject(capabilities, "agentCursorStatus", indicator_status);
	cJSON_AddItemToObject(payload, "capabilities", capabilities);

	cJSON *blockers = cJSON_CreateArray();
	if (!headless)
		add_environment_notice(blockers, "non_interfering_pixel_input_unavailable",
			"high", "No trusted compositor broker is installed; generic pointer and keyboard tools use the human's shared seat.");
	if (wayland)
		add_environment_notice(blockers, "native_wayland_surface_control_unavailable",
			"high", "The compatibility desktop backend can control X11/Xwayland surfaces but not arbitrary native Wayland surfaces.");
	if (!semantic_available)
		add_environment_notice(blockers, "atspi_unavailable", "medium",
			"Verified semantic inspection and actions are unavailable in this process.");
	if (!headless && !indicator_available)
		add_environment_notice(blockers, "agent_cursor_unavailable", "medium",
			indicator_error[0] ? indicator_error : "The GNOME logical-cursor service is unavailable.");
	else if (!headless && !indicator_single_monitor)
		add_environment_notice(blockers, "agent_cursor_monitor_layout_unsupported",
			"medium", "Capture-bound cursor movement currently requires one monitor covering the full GNOME stage.");
	if (!deskpal_allow_exec)
		add_environment_notice(blockers, "process_launch_disabled", "low",
			"Process-launch tools are disabled until Deskpal starts with --allow-exec.");
	cJSON_AddItemToObject(payload, "blockers", blockers);

	cJSON *risks = cJSON_CreateArray();
	if (!headless) {
		add_environment_notice(risks, "shared_pointer", "high",
			"mouse_move, click, drag, and scroll may move or use the human's logical pointer.");
		add_environment_notice(risks, "focus_and_stacking", "high",
			"Compatibility actions may focus or raise X11/Xwayland windows.");
		add_environment_notice(risks, "global_clipboard", "medium",
			"Clipboard tools read or replace the desktop session clipboard.");
	}
	cJSON_AddItemToObject(payload, "risks", risks);

	cJSON *setup = cJSON_CreateArray();
	if (!headless && !indicator_available)
		add_environment_notice(setup, "install_agent_cursor", NULL,
			"On supported GNOME 42 systems, run npm run indicator:install, log out and back in, then run scripts/indicator.sh enable.");
	if (!semantic_available)
		add_environment_notice(setup, "enable_accessibility", NULL,
			"Install the AT-SPI runtime and explicitly enable the target application's accessibility support; never change it silently.");
	if (!deskpal_allow_exec)
		add_environment_notice(setup, "enable_process_launch", NULL,
			"Restart Deskpal with --allow-exec only when arbitrary process execution is intended.");
	if (!headless)
		add_environment_notice(setup, "prefer_non_interfering_routes", NULL,
			"Prefer app APIs and verified AT-SPI actions; use launch_isolated_app for disposable GUI work until a compositor broker is available.");
	cJSON_AddItemToObject(payload, "setupActions", setup);

	return structured_text_result(payload, "environment");
}

/* ── list_windows ────────────────────────────────────────────────────────── */

cJSON *tool_list_windows(const cJSON *params)
{
	const char *name_filter = json_str(params, "name", NULL);
	int include_all = json_bool(params, "includeAll", 0);

	WindowInfo windows[50];
	int count = x11_list_windows(windows, 50, name_filter, include_all);

	int scale = x11_get_scale_factor();

	char buf[8192];
	size_t pos = 0;

	for (int i = 0; i < count && pos < sizeof(buf) - 256; i++) {
		append_text(buf, sizeof(buf), &pos,
			"[%lu] \"%s\" class=\"%s\" pid=%ld\n"
			"  Position: %d,%d (screen: 0)\n"
			"  Geometry: %dx%d\n\n",
			windows[i].id, windows[i].title, windows[i].app_class,
			windows[i].pid,
			windows[i].x, windows[i].y,
			windows[i].width, windows[i].height);
	}

	if (count > 0) {
		append_text(buf, sizeof(buf), &pos, "Display scale: %dx", scale);
	} else {
		snprintf(buf, sizeof(buf), "No visible windows found");
	}

	return mcp_text_result(buf);
}

/* ── accessibility ──────────────────────────────────────────────────────── */

#define ACCESSIBILITY_RESPONSE_LIMIT (3 * 1024 * 1024)

static cJSON *accessibility_result(cJSON *payload, const char *operation)
{
	if (!payload) return mcp_tool_error_result("Accessibility query failed");
	char *json = cJSON_PrintUnformatted(payload);
	if (!json) {
		cJSON_Delete(payload);
		return mcp_tool_error_result("Accessibility query serialization failed");
	}
	size_t json_length = strlen(json);
	if (json_length > ACCESSIBILITY_RESPONSE_LIMIT) {
		free(json);
		cJSON_Delete(payload);
		return mcp_tool_error_result(
			"Accessibility response exceeded the 3 MiB content safety limit; narrow application/window filters or reduce maxNodes");
	}
	cJSON *result = mcp_text_result(json);
	free(json);
	cJSON_Delete(payload);
	char *wire = cJSON_PrintUnformatted(result);
	if (!wire || strlen(wire) > ACCESSIBILITY_RESPONSE_LIMIT) {
		free(wire);
		cJSON_Delete(result);
		return mcp_tool_error_result(
			"Accessibility MCP content exceeded the 3 MiB safety limit; narrow application/window filters or reduce maxNodes");
	}
	free(wire);
	(void)operation;
	return result;
}

cJSON *tool_accessibility_status(const cJSON *params)
{
	(void)params;
	return accessibility_result(accessibility_status(), "status");
}

cJSON *tool_get_accessibility_tree(const cJSON *params)
{
	const char *application = json_str(params, "application", NULL);
	const char *window = json_str(params, "window", NULL);
	int max_depth = json_int(params, "maxDepth", 8);
	int max_nodes = json_int(params, "maxNodes", 300);
	int include_offscreen = json_bool(params, "includeOffscreen", 0);
	int include_text = json_bool(params, "includeText", 0);
	int include_attributes = json_bool(params, "includeAttributes", 0);
	if ((!application || !application[0]) && (!window || !window[0]))
		return mcp_tool_error_result(
			"Specify application or window to scope accessibility tree lookup");
	return accessibility_result(accessibility_tree(application, window,
		max_depth, max_nodes, include_offscreen, include_text,
		include_attributes), "tree");
}

cJSON *tool_get_focused_element(const cJSON *params)
{
	const char *application = json_str(params, "application", NULL);
	const char *window = json_str(params, "window", NULL);
	int include_text = json_bool(params, "includeText", 0);
	if ((!application || !application[0]) && (!window || !window[0]))
		return mcp_tool_error_result(
			"Specify application or window to bound focused-element lookup");
	return accessibility_result(
		accessibility_focused_element(application, window, include_text),
		"focused element");
}

cJSON *tool_accessibility_action(const cJSON *params)
{
	cJSON *payload = accessibility_action(params);
	int success = payload && cJSON_IsTrue(cJSON_GetObjectItem(payload, "success"));
	cJSON *result = accessibility_result(payload, "action");
	if (!success && result && !cJSON_GetObjectItem(result, "isError"))
		cJSON_AddBoolToObject(result, "isError", 1);
	return result;
}

/* ── find_window ─────────────────────────────────────────────────────────── */

cJSON *tool_find_window(const cJSON *params)
{
	const char *name = json_str(params, "name", NULL);
	if (!name) name = json_str(params, "windowName", "");
	if (!name[0])
		return mcp_tool_error_result("name must be a non-empty string");
	unsigned long wid = x11_find_window(name);
	if (!wid) {
		char msg[256];
		snprintf(msg, sizeof(msg), "No window found matching \"%s\"", name);
		return mcp_text_result(msg);
	}

	WindowInfo info;
	if (x11_get_window_info(wid, &info) != 0 || !info.viewable) {
		char msg[256];
		snprintf(msg, sizeof(msg), "No window found matching \"%s\"", name);
		return mcp_text_result(msg);
	}

	char buf[512];
	snprintf(buf, sizeof(buf),
		"[%lu] \"%s\" class=\"%s\"\n  Position: %d,%d\n  Size: %dx%d\n  PID: %ld",
		info.id, info.title, info.app_class, info.x, info.y,
		info.width, info.height, info.pid);
	return mcp_text_result(buf);
}

/* ── focus_window ────────────────────────────────────────────────────────── */

cJSON *tool_focus_window(const cJSON *params)
{
	unsigned long wid = resolve_window(params);
	if (!wid) return mcp_text_result("Window not found");

	if (x11_focus_window(wid) != 0)
		return mcp_text_result("Window not found");

	char buf[64];
	snprintf(buf, sizeof(buf), "Focused window %lu", wid);
	return mcp_text_result(buf);
}

/* ── click ───────────────────────────────────────────────────────────────── */

cJSON *tool_click(const cJSON *params)
{
	unsigned long wid = resolve_window(params);
	if (!wid && explicit_window_requested(params))
		return mcp_text_result("Window not found");
	int x = json_int(params, "x", 0);
	int y = json_int(params, "y", 0);
	int button = json_button(params, "button", 1);
	int dbl = json_bool(params, "doubleClick", 0);

	unsigned long target = wid ? wid : x11_get_active_window();
	WindowInfo info;
	if (x11_get_window_info(target, &info) != 0) {
		return mcp_text_result("Could not get window position");
	}
	if (getenv("DESKPAL_HEADLESS_ACTIVE"))
		x11_focus_window(target);

	int abs_x = info.x + x;
	int abs_y = info.y + y;

	if (x11_is_wayland()) {
		/* On Wayland we cannot trust info.x/info.y from
		 * xdo_get_window_location to match the renderer's content
		 * origin (observed ~90 screen-px offset on mutter+HiDPI).
		 * Route the whole motion+press through xdotool --window,
		 * which uses the X server's coordinate system. */
		if (x11_window_mouse_move(target, x, y) != 0) {
			return mcp_text_result("Mouse move failed");
		}
		usleep_ms(10);
		if (!window_is_viewable(target))
			return mcp_text_result("Window not found");
		if (x11_click(button, dbl ? 2 : 1) != 0)
			return mcp_text_result("Click failed");
	} else {
		/* Move cursor to click position — with uinput this also
		 * gives the window compositor-level focus automatically. */
		x11_mouse_move(abs_x, abs_y);
		usleep_ms(10);
		if (!window_is_viewable(target))
			return mcp_text_result("Window not found");

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
					if (system(cmd2) != 0)
						return mcp_text_result("Click failed");
			} else {
					if (system(cmd) != 0)
						return mcp_text_result("Click failed");
			}
		} else {
				if (x11_click(button, dbl ? 2 : 1) != 0)
					return mcp_text_result("Click failed");
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
		/* XCB root capture can fail on Xwayland. Visible sessions may use
		 * compositor helpers; headless sessions must remain on their X server. */
		if (capture_root_to_file(tmp_png) == 0) goto do_ocr;
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
	char tsv_base[64], tsv_base2[64], tsv_base3[64];
	snprintf(tsv_base, sizeof(tsv_base), "/tmp/deskpal_tsv_%d", getpid());
	snprintf(tsv_base2, sizeof(tsv_base2), "/tmp/deskpal_tsv2_%d", getpid());
	snprintf(tsv_base3, sizeof(tsv_base3), "/tmp/deskpal_tsv3_%d", getpid());

	snprintf(cmd, sizeof(cmd),
		"tesseract \"%s\" \"%s\" -l eng --psm 3 tsv 2>/dev/null", tmp_png, tsv_base);
	int rc = system(cmd);

	snprintf(cmd, sizeof(cmd),
		"tesseract \"%s\" \"%s\" -l eng --psm 3 tsv 2>/dev/null", preproc_png, tsv_base2);
	int rc2 = system(cmd);

	/* Third pass: PSM 11 (sparse text) on the preprocessed image. PSM 3
	 * assumes a single uniform block of text and often misses small,
	 * scattered UI labels like dialog/toolbar buttons ("Cancel", "Save");
	 * PSM 11 finds text anywhere on the image, which is what desktop UIs
	 * need. Extra boxes are merged by the dedup pass below. */
	snprintf(cmd, sizeof(cmd),
		"tesseract \"%s\" \"%s\" -l eng --psm 11 tsv 2>/dev/null", preproc_png, tsv_base3);
	int rc3 = system(cmd);

	unlink(tmp_png);
	unlink(preproc_png);

	if (rc != 0 && rc2 != 0 && rc3 != 0) return 0;

	char tsv_file[80], tsv_file2[80], tsv_file3[80];
	snprintf(tsv_file, sizeof(tsv_file), "%s.tsv", tsv_base);
	snprintf(tsv_file2, sizeof(tsv_file2), "%s.tsv", tsv_base2);
	snprintf(tsv_file3, sizeof(tsv_file3), "%s.tsv", tsv_base3);

	const char *tsv_files[] = { tsv_file, tsv_file2, tsv_file3, NULL };
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
	unlink(tsv_file3);

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
	if (!wid && explicit_window_requested(params))
		return mcp_text_result("Window not found");
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
		if (revalidate_target(params, target, &info) != 0) {
			ocr_result_free(&ocr_result);
			return mcp_text_result("Window not found");
		}
		int abs_x = info.x + click_x;
		int abs_y = info.y + click_y;
		if (getenv("DESKPAL_HEADLESS_ACTIVE") &&
		    x11_focus_window(target) != 0) {
			ocr_result_free(&ocr_result);
			return mcp_text_result("Window not found");
		}

		if (x11_is_wayland()) {
			if (x11_window_mouse_move(target, click_x, click_y) != 0) {
				ocr_result_free(&ocr_result);
				return mcp_text_result("Mouse move failed");
			}
		} else {
			if (x11_mouse_move(abs_x, abs_y) != 0) {
				ocr_result_free(&ocr_result);
				return mcp_text_result("Mouse move failed");
			}
		}
		usleep_ms(10);
		if (revalidate_target(params, target, &info) != 0) {
			ocr_result_free(&ocr_result);
			return mcp_text_result("Window not found");
		}
		if (x11_click(button, 1) != 0) {
			ocr_result_free(&ocr_result);
			return mcp_text_result("Click failed");
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

	if (capture_root_to_file(ss_png) != 0) {
		return mcp_text_result("Full-screen capture failed");
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

		if (revalidate_target(params, target, &info) != 0) {
			ocr_result_free(&screen_ocr);
			return mcp_text_result("Window not found");
		}
		if (x11_mouse_move(abs_x, abs_y) != 0) {
			ocr_result_free(&screen_ocr);
			return mcp_text_result("Mouse move failed");
		}
		usleep_ms(10);
		if (revalidate_target(params, target, &info) != 0) {
			ocr_result_free(&screen_ocr);
			return mcp_text_result("Window not found");
		}
		if (x11_click(button, 1) != 0) {
			ocr_result_free(&screen_ocr);
			return mcp_text_result("Click failed");
		}

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
	if (!wid && explicit_window_requested(params))
		return mcp_text_result("Window not found");
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

static int is_headless_session_env(const char *name)
{
	static const char *reserved[] = {
		"DISPLAY",
		"XAUTHORITY",
		"WAYLAND_DISPLAY",
		"XDG_RUNTIME_DIR",
		"DBUS_SESSION_BUS_ADDRESS",
		"SESSION_MANAGER",
		"AT_SPI_BUS_ADDRESS",
		"AT_SPI_BUS",
		"AT_SPI_DISPLAY",
		"XDG_SESSION_TYPE",
		"GDK_BACKEND",
		"QT_QPA_PLATFORM",
		"ELECTRON_OZONE_PLATFORM_HINT",
		NULL
	};

	for (int i = 0; reserved[i]; i++) {
		if (strcmp(name, reserved[i]) == 0) return 1;
	}
	return 0;
}

static int is_valid_env_name(const char *name)
{
	if (!name || !(isalpha((unsigned char)name[0]) || name[0] == '_'))
		return 0;
	for (int i = 1; name[i]; i++) {
		if (!(isalnum((unsigned char)name[i]) || name[i] == '_')) return 0;
	}
	return 1;
}

static void kill_existing_processes(const char *pattern)
{
	pid_t pid = fork();
	if (pid == 0) {
		execlp("pkill", "pkill", "-f", pattern, (char *)NULL);
		_exit(127);
	}
	if (pid > 0) waitpid(pid, NULL, 0);
	usleep_ms(500);
}

static void report_launch_error(int fd, int error_number)
{
	while (write(fd, &error_number, sizeof(error_number)) < 0 && errno == EINTR) {}
	_exit(127);
}

static int launch_detached(const char *command, const cJSON *args,
	                       const cJSON *env, int headless, int force_x11,
	                       char *error, size_t error_len)
{
	int status_pipe[2];
	if (pipe2(status_pipe, O_CLOEXEC) != 0) {
		snprintf(error, error_len, "could not create launcher status pipe: %s",
			strerror(errno));
		return -1;
	}

	pid_t launcher = fork();
	if (launcher < 0) {
		int fork_error = errno;
		close(status_pipe[0]);
		close(status_pipe[1]);
		snprintf(error, error_len, "could not fork application launcher: %s",
			strerror(fork_error));
		return -1;
	}
	if (launcher == 0) {
		close(status_pipe[0]);
		pid_t app = fork();
		if (app < 0) report_launch_error(status_pipe[1], errno);
		if (app > 0) {
			close(status_pipe[1]);
			_exit(0);
		}

		/* Isolated (headless) launches must not inherit the host session
		 * bus; blank it so the app can't attach to the real desktop. On a
		 * visible-desktop launch we keep DBUS_SESSION_BUS_ADDRESS intact so
		 * D-Bus-activated apps (e.g. gnome-terminal) can start. */
		if (headless) {
			if (setenv("DBUS_SESSION_BUS_ADDRESS", "", 1) != 0)
				report_launch_error(status_pipe[1], errno);
			unsetenv("AT_SPI_BUS_ADDRESS");
			unsetenv("AT_SPI_BUS");
			unsetenv("AT_SPI_DISPLAY");
		}
		if (env && cJSON_IsObject(env)) {
			cJSON *item = NULL;
			cJSON_ArrayForEach(item, env) {
				if (cJSON_IsString(item) && is_valid_env_name(item->string) &&
				    !(headless && is_headless_session_env(item->string))) {
					if (setenv(item->string, item->valuestring, 1) != 0)
						report_launch_error(status_pipe[1], errno);
				}
			}
		}

		/* Window discovery and input are X11-backed. A visible launch that
		 * waits for a window must therefore prefer XWayland; otherwise a
		 * toolkit may create a native Wayland surface that Deskpal cannot
		 * discover or control. Apply this after caller env so the contract is
		 * deterministic. Native Wayland remains available with forceX11=false. */
		if (!headless && force_x11) {
			unsetenv("WAYLAND_DISPLAY");
			if (setenv("XDG_SESSION_TYPE", "x11", 1) != 0 ||
			    setenv("GDK_BACKEND", "x11", 1) != 0 ||
			    setenv("QT_QPA_PLATFORM", "xcb", 1) != 0 ||
			    setenv("ELECTRON_OZONE_PLATFORM_HINT", "x11", 1) != 0)
				report_launch_error(status_pipe[1], errno);
		}

		int null_fd = open("/dev/null", O_RDWR);
		if (null_fd < 0) report_launch_error(status_pipe[1], errno);
		if (dup2(null_fd, STDIN_FILENO) < 0 ||
		    dup2(null_fd, STDOUT_FILENO) < 0 ||
		    dup2(null_fd, STDERR_FILENO) < 0)
			report_launch_error(status_pipe[1], errno);
		if (null_fd > STDERR_FILENO) close(null_fd);

		int arg_count = args && cJSON_IsArray(args)
			? cJSON_GetArraySize(args) : 0;
		char **argv = calloc((size_t)arg_count + 2, sizeof(*argv));
		if (!argv) report_launch_error(status_pipe[1], ENOMEM);
		argv[0] = (char *)command;
		int pos = 1;
		for (int i = 0; i < arg_count; i++) {
			cJSON *arg = cJSON_GetArrayItem(args, i);
			if (cJSON_IsString(arg)) argv[pos++] = arg->valuestring;
		}
		argv[pos] = NULL;
		execvp(command, argv);
		report_launch_error(status_pipe[1], errno);
	}

	close(status_pipe[1]);
	int status = 0;
	while (waitpid(launcher, &status, 0) < 0) {
		if (errno == EINTR) continue;
		snprintf(error, error_len, "could not wait for application launcher: %s",
			strerror(errno));
		close(status_pipe[0]);
		return -1;
	}

	int exec_error = 0;
	ssize_t status_bytes;
	do {
		status_bytes = read(status_pipe[0], &exec_error, sizeof(exec_error));
	} while (status_bytes < 0 && errno == EINTR);
	close(status_pipe[0]);
	if (status_bytes > 0) {
		snprintf(error, error_len, "could not execute %s: %s",
			command, strerror(exec_error));
		return -1;
	}
	if (status_bytes < 0 || !WIFEXITED(status) || WEXITSTATUS(status) != 0) {
		snprintf(error, error_len, "application launcher exited unexpectedly");
		return -1;
	}
	return 0;
}

cJSON *tool_launch_app(const cJSON *params)
{
	const char *command = json_str(params, "command", "");
	if (!command[0]) return mcp_tool_error_result("Missing 'command' parameter");
	int headless = getenv("DESKPAL_HEADLESS_ACTIVE") != NULL;
	int kill_existing = json_bool(params, "killExisting", headless ? 0 : 1);
	int timeout = json_int(params, "timeout", 10);
	const cJSON *wait_item = cJSON_GetObjectItem(params, "waitForWindow");
	if (wait_item && (!cJSON_IsString(wait_item) || !wait_item->valuestring[0]))
		return mcp_tool_error_result(
			"waitForWindow must be a non-empty string when provided");
	const char *wait_title = wait_item ? wait_item->valuestring : NULL;
	int force_x11 = json_bool(params, "forceX11", wait_title != NULL);

	/* Extract basename */
	const char *basename = strrchr(command, '/');
	basename = basename ? basename + 1 : command;

	/* Kill existing */
	if (kill_existing && !headless) {
		kill_existing_processes(basename);
	}

	const cJSON *env = cJSON_GetObjectItem(params, "env");
	const cJSON *args = cJSON_GetObjectItem(params, "args");
	char launch_error[256];
	if (launch_detached(command, args, env, headless, force_x11,
	                    launch_error, sizeof(launch_error)) != 0)
		return mcp_tool_error_result(launch_error);

	/* Wait for window */
	const char *search_title = wait_title ? wait_title : basename;
	int deadline_ms = timeout * 1000;
	int elapsed = 0;

	while (elapsed < deadline_ms) {
		usleep_ms(500);
		elapsed += 500;
		unsigned long wid = wait_title
			? x11_find_window(search_title) : x11_find_app(search_title);
		if (wid) {
			WindowInfo info;
			if (x11_get_window_info(wid, &info) != 0 || !info.viewable)
				continue;
			char buf[512];
			snprintf(buf, sizeof(buf),
				"Launched \"%s\"\n[%lu] \"%s\" class=\"%s\"\n  Position: %d,%d\n  "
				"Size: %dx%d\n  PID: %ld",
				command, info.id, info.title, info.app_class,
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
	if (!wid && explicit_window_requested(params))
		return mcp_text_result("Window not found");
	if (wid) {
		if (x11_focus_window(wid) != 0)
			return mcp_text_result("Window not found");
		usleep_ms(50);
		if (!window_is_viewable(wid))
			return mcp_text_result("Window not found");
	}

	if (x11_type_text(text, delay) != 0)
		return mcp_text_result("Typing failed");

	char buf[64];
	snprintf(buf, sizeof(buf), "Typed %d characters", (int)strlen(text));
	return mcp_text_result(buf);
}

/* ── key_press ───────────────────────────────────────────────────────────── */

cJSON *tool_key_press(const cJSON *params)
{
	const char *keys = json_str(params, "keys", "");

	unsigned long wid = resolve_window(params);
	if (!wid && explicit_window_requested(params))
		return mcp_text_result("Window not found");
	if (wid) {
		if (x11_focus_window(wid) != 0)
			return mcp_text_result("Window not found");
		usleep_ms(50);
		if (!window_is_viewable(wid))
			return mcp_text_result("Window not found");
	}

	if (x11_key_press(keys) != 0)
		return mcp_text_result("Key press failed");

	char buf[128];
	snprintf(buf, sizeof(buf), "Pressed: %s", keys);
	return mcp_text_result(buf);
}

/* ── get_window_geometry ─────────────────────────────────────────────────── */

cJSON *tool_get_window_geometry(const cJSON *params)
{
	unsigned long wid = resolve_window(params);
	if (!wid && explicit_window_requested(params))
		return mcp_text_result("Window not found");
	if (!wid) wid = x11_get_active_window();

	WindowInfo info;
	if (x11_get_window_info(wid, &info) != 0) {
		return mcp_text_result("Window not found");
	}

	int scale = x11_get_scale_factor();

	char buf[512];
	snprintf(buf, sizeof(buf),
		"Window: [%lu] \"%s\" class=\"%s\"\n"
		"Position: %d,%d\n"
		"Size: %dx%d\n"
		"Scale: %dx\n"
		"PID: %ld",
		info.id, info.title, info.app_class, info.x, info.y,
		info.width, info.height, scale, info.pid);
	return mcp_text_result(buf);
}

/* ── resize_window ───────────────────────────────────────────────────────── */

cJSON *tool_resize_window(const cJSON *params)
{
	unsigned long wid = resolve_window(params);
	if (!wid && explicit_window_requested(params))
		return mcp_text_result("Window not found");
	if (!wid) wid = x11_get_active_window();

	int width = json_int(params, "width", 800);
	int height = json_int(params, "height", 600);

	if (x11_resize_window(wid, width, height) != 0)
		return mcp_text_result("Window resize failed");

	char buf[64];
	snprintf(buf, sizeof(buf), "Resized window %lu to %dx%d", wid, width, height);
	return mcp_text_result(buf);
}

/* ── wait_for_window ─────────────────────────────────────────────────────── */

cJSON *tool_wait_for_window(const cJSON *params)
{
	const cJSON *name_item = cJSON_GetObjectItem(params, "name");
	if (!name_item || !cJSON_IsString(name_item) || !name_item->valuestring[0])
		return mcp_tool_error_result("name must be a non-empty string");
	const char *name = name_item->valuestring;
	int timeout = json_int(params, "timeout", 10);

	int deadline_ms = timeout * 1000;
	int elapsed = 0;

	while (elapsed < deadline_ms) {
		unsigned long wid = x11_find_window(name);
		if (wid) {
			WindowInfo info;
			if (x11_get_window_info(wid, &info) != 0 || !info.viewable) {
				usleep_ms(500);
				elapsed += 500;
				continue;
			}
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
	if (!wid && explicit_window_requested(params))
		return mcp_text_result("Window not found");
	if (wid) {
		WindowInfo info;
		if (x11_get_window_info(wid, &info) == 0) {
			if (getenv("DESKPAL_HEADLESS_ACTIVE"))
				x11_focus_window(wid);
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
		return mcp_text_result("Window not found");
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
	if (!wid && explicit_window_requested(params))
		return mcp_text_result("Window not found");
	if (wid) {
		if (x11_focus_window(wid) != 0)
			return mcp_text_result("Window not found");
		usleep_ms(50);
	}

	int button = (strcmp(dir, "up") == 0) ? 4 : 5;
	if (x11_scroll(button, clicks) != 0)
		return mcp_text_result("Scroll failed");

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
	if (!wid && explicit_window_requested(params))
		return mcp_text_result("Window not found");
	if (wid) {
		if (x11_focus_window(wid) != 0)
			return mcp_text_result("Window not found");
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

	if (x11_drag(abs_from_x, abs_from_y, abs_to_x, abs_to_y, button, steps) != 0)
		return mcp_text_result("Drag failed");

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
	if (!wid && explicit_window_requested(params))
		return mcp_text_result("Window not found");
	if (wid && getenv("DESKPAL_HEADLESS_ACTIVE"))
		x11_focus_window(wid);
	if (wid && x >= 0 && y >= 0) {
		WindowInfo info;
		if (x11_get_window_info(wid, &info) == 0) {
			x11_mouse_move(info.x + x, info.y + y);
			usleep_ms(10);
			if (!window_is_viewable(wid))
				return mcp_text_result("Window not found");
		} else {
			return mcp_text_result("Window not found");
		}
	} else if (x >= 0 && y >= 0) {
		x11_mouse_move(x, y);
		usleep_ms(10);
	}

	if (x11_mouse_down(button) != 0)
		return mcp_text_result("Mouse down failed");

	char buf[64];
	snprintf(buf, sizeof(buf), "Mouse button %d pressed", button);
	return mcp_text_result(buf);
}

cJSON *tool_mouse_up(const cJSON *params)
{
	int button = json_button(params, "button", 1);
	if (x11_mouse_up(button) != 0)
		return mcp_text_result("Mouse up failed");

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
				"wl-copy%s >/dev/null 2>&1",
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
				"xclip -selection %s -i >/dev/null 2>&1", sel);
		}
		return 0;
	}
	if (access("/usr/bin/xsel", X_OK) == 0) {
		const char *flag = is_primary ? "-p" : "-b";
		if (mode == 'r') {
			snprintf(cmd_buf, cmd_len, "xsel %s -o 2>/dev/null", flag);
		} else {
			snprintf(cmd_buf, cmd_len,
				"xsel %s -i >/dev/null 2>&1", flag);
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
		int ocx = o->x + o->width / 2;
		int ocy = o->y + o->height / 2;
		/* Allow ±20 px drift in either axis. */
		if (strcasecmp(o->text, b->text) == 0 &&
		    abs(ocx - cx) <= 20 && abs(ocy - cy) <= 20)
			return 1;

		/* Tesseract may split one stable label into different fragments on
		 * consecutive passes. Substantial geometric overlap still means the
		 * pixels occupied by this word were already present. */
		int left = b->x > o->x ? b->x : o->x;
		int top = b->y > o->y ? b->y : o->y;
		int right = b->x + b->width < o->x + o->width
			? b->x + b->width : o->x + o->width;
		int bottom = b->y + b->height < o->y + o->height
			? b->y + b->height : o->y + o->height;
		if (right > left && bottom > top) {
			int overlap = (right - left) * (bottom - top);
			int b_area = b->width * b->height;
			int o_area = o->width * o->height;
			int smaller = b_area < o_area ? b_area : o_area;
			if (smaller > 0 && overlap * 2 >= smaller) return 1;
		}
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
	if (!wid && explicit_window_requested(params))
		return mcp_text_result("Window not found");
	unsigned long target = wid ? wid : x11_get_active_window();

	WindowInfo info;
	if (x11_get_window_info(target, &info) != 0) {
		return mcp_text_result("Could not get window info");
	}

	/* Locate the target text in the window. */
	OcrResult target_ocr = { 0 };
	ocr_screenshot(target, &target_ocr);
	if (target_ocr.count == 0) {
		ocr_result_free(&target_ocr);
		return mcp_text_result("OCR returned no words for the target window");
	}

	int n_matches = 0;
	OcrMatch *matches = ocr_find_text(&target_ocr, text, &n_matches);
	if (n_matches == 0) {
		free(matches);
		ocr_result_free(&target_ocr);
		char buf[160];
		snprintf(buf, sizeof(buf), "Text \"%s\" not found on screen", text);
		return mcp_text_result(buf);
	}
	OcrMatch hit = matches[0];
	free(matches);

	int rel_x = hit.x + hit.width / 2;
	int rel_y = hit.y + hit.height / 2;
	ocr_result_free(&target_ocr);
	if (revalidate_target(params, target, &info) != 0)
		return mcp_text_result("Window not found");
	int hover_x = info.x + rel_x;
	int hover_y = info.y + rel_y;

	/* Make sure the target is actually exposed before moving the real cursor;
	 * direct window capture can see an obscured window, but the pointer cannot. */
	if (x11_focus_window(target) != 0)
		return mcp_text_result("Window not found");
	usleep_ms(100);
	if (revalidate_target(params, target, &info) != 0)
		return mcp_text_result("Window not found");

	/* Tooltip windows live outside the target window. Capture the whole screen
	 * both before and after hovering so the diff uses one coordinate space and
	 * unchanged desktop text is not mistaken for tooltip content. */
	OcrResult before = { 0 };
	ocr_screenshot(0, &before);
	int move_result;
	if (x11_is_wayland()) {
		move_result = x11_window_mouse_move(target, rel_x, rel_y);
	} else {
		move_result = x11_mouse_move(hover_x, hover_y);
	}
	if (move_result != 0 || revalidate_target(params, target, &info) != 0) {
		ocr_result_free(&before);
		return mcp_text_result("Window not found");
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
		int cx = b->x + b->width / 2;
		int cy = b->y + b->height / 2;
		if (cx < hover_x - 400 || cx > hover_x + 800 ||
		    cy < hover_y - 150 || cy > hover_y + 500)
			continue;
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
	mcp_register_tool("launch_isolated_app",
		"Launch an app in a private Xvfb display for visual verification of a locally developed app without interrupting the user. "
		"Use launch_app instead when the goal is to control an app on the user's existing desktop. "
		"Returns a sessionId that must be passed to subsequent screenshot, window, and input tools.",
		"{"
		"  \"type\": \"object\","
		"  \"properties\": {"
		"    \"command\": {\"type\": \"string\", \"description\": \"Command to launch\"},"
		"    \"args\": {\"type\": \"array\", \"items\": {\"type\": \"string\"}},"
		"    \"waitForWindow\": {\"type\": \"string\"},"
		"    \"timeout\": {\"type\": \"number\", \"default\": 10},"
		"    \"screenSize\": {\"type\": \"string\", \"default\": \"1920x1080\", \"description\": \"Virtual display size as WIDTHxHEIGHT\"},"
		"    \"env\": {\"type\": \"object\"}"
		"  },"
		"  \"required\": [\"command\"]"
		"}",
		tool_launch_isolated_app);

	mcp_register_tool("close_isolated_session",
		"Close an isolated Xvfb verification session and all apps running in it. Call this when verification is complete.",
		"{"
		"  \"type\": \"object\","
		"  \"properties\": {"
		"    \"sessionId\": {\"type\": \"string\"}"
		"  },"
		"  \"required\": [\"sessionId\"]"
		"}",
		tool_close_isolated_session);

	mcp_register_tool("screenshot",
		"Capture a screenshot of a window or screen. Omit sessionId for the user's desktop; use the sessionId from launch_isolated_app for private app verification. Optional maxWidth/maxHeight downscale the image while preserving source-coordinate metadata.",
		"{"
		"  \"type\": \"object\","
		"  \"properties\": {"
		"    \"windowId\": {\"type\": \"string\", \"description\": \"X11 window ID\"},"
		"    \"windowName\": {\"type\": \"string\", \"description\": \"Window title substring\"},"
		"    \"fullScreen\": {\"type\": \"boolean\", \"description\": \"Capture entire screen\", \"default\": false},"
		"    \"maxWidth\": {\"type\": \"number\", \"minimum\": 0, \"maximum\": 8192, \"default\": 0, \"description\": \"Optional maximum output width; 0 keeps source size\"},"
		"    \"maxHeight\": {\"type\": \"number\", \"minimum\": 0, \"maximum\": 8192, \"default\": 0, \"description\": \"Optional maximum output height; 0 keeps source size\"}"
		"  }"
		"}",
		tool_screenshot);

	mcp_register_tool("get_environment_status",
		"Report the current Deskpal scope, selected backends, capabilities, blockers, shared-seat risks, and concrete setup actions. Use this before choosing a visible-desktop interaction route.",
		"{"
		"  \"type\": \"object\","
		"  \"properties\": {}"
		"}",
		tool_get_environment_status);

	mcp_register_tool("get_app_state",
		"Observe one exact X11/Xwayland app window without activating it. Returns an image, backend-scoped identity, focus and geometry consistency, image-to-stage transform, short-lived capture ID when stable, and bounded untrusted AT-SPI state. Native Wayland targets and ambiguous names fail closed.",
		"{"
		"  \"type\": \"object\","
		"  \"properties\": {"
		"    \"windowId\": {\"type\": \"string\", \"minLength\": 1, \"maxLength\": 64},"
		"    \"windowName\": {\"type\": \"string\", \"minLength\": 1, \"maxLength\": 255, \"description\": \"Exact case-sensitive title\"},"
		"    \"maxWidth\": {\"type\": \"integer\", \"minimum\": 0, \"maximum\": 8192, \"default\": 1920},"
		"    \"maxHeight\": {\"type\": \"integer\", \"minimum\": 0, \"maximum\": 8192, \"default\": 1080},"
		"    \"semanticMaxDepth\": {\"type\": \"integer\", \"minimum\": 1, \"maximum\": 8, \"default\": 4},"
		"    \"semanticMaxNodes\": {\"type\": \"integer\", \"minimum\": 1, \"maximum\": 300, \"default\": 120},"
		"    \"includeText\": {\"type\": \"boolean\", \"default\": false},"
		"    \"includeAttributes\": {\"type\": \"boolean\", \"default\": false}"
		"  },"
		"  \"oneOf\": [{\"required\": [\"windowId\"]}, {\"required\": [\"windowName\"]}]"
		"}",
		tool_get_app_state);

	mcp_register_tool("agent_cursor_status",
		"Report GNOME logical-cursor availability, stage and monitor geometry, and only the cursors owned by this Deskpal process. Indicator state is visual metadata, not proof of delivered input.",
		"{"
		"  \"type\": \"object\","
		"  \"properties\": {}"
		"}",
		tool_agent_cursor_status);

	mcp_register_tool("agent_cursor_move",
		"Create or move this Deskpal process's logical agent cursor to a point in a recent full-screen screenshot or stable get_app_state capture. Coordinates are pixels in the returned image, not source pixels. This moves only the visual indicator and never delivers application input.",
		"{"
		"  \"type\": \"object\","
		"  \"properties\": {"
		"    \"captureId\": {\"type\": \"string\", \"minLength\": 1, \"maxLength\": 63},"
		"    \"x\": {\"type\": \"integer\", \"minimum\": 0, \"maximum\": 32768, \"description\": \"X in capture image pixels\"},"
		"    \"y\": {\"type\": \"integer\", \"minimum\": 0, \"maximum\": 32768, \"description\": \"Y in capture image pixels\"},"
		"    \"cursorId\": {\"type\": \"string\", \"minLength\": 1, \"maxLength\": 40, \"default\": \"primary\"},"
		"    \"color\": {\"type\": \"string\", \"pattern\": \"^#[0-9A-Fa-f]{6}$\", \"default\": \"#36C5F0\"},"
		"    \"label\": {\"type\": \"string\", \"maxLength\": 48}"
		"  },"
		"  \"required\": [\"captureId\", \"x\", \"y\"]"
		"}",
		tool_agent_cursor_move);

	mcp_register_tool("agent_cursor_hide",
		"Hide one logical cursor owned by this Deskpal process. It cannot hide another process's cursor and does not use the global ClearAll operation.",
		"{"
		"  \"type\": \"object\","
		"  \"properties\": {"
		"    \"cursorId\": {\"type\": \"string\", \"minLength\": 1, \"maxLength\": 40, \"default\": \"primary\"}"
		"  }"
		"}",
		tool_agent_cursor_hide);

	mcp_register_tool("list_windows",
		"List top-level application windows on the user's desktop by default, or in an isolated verification session when sessionId is supplied. Set includeAll for recursive helper/dialog discovery.",
		"{"
		"  \"type\": \"object\","
		"  \"properties\": {"
		"    \"name\": {\"type\": \"string\", \"description\": \"Optional title or app-class filter\"},"
		"    \"includeAll\": {\"type\": \"boolean\", \"default\": false, \"description\": \"Include recursive helper/dialog windows\"}"
		"  }"
		"}",
		tool_list_windows);

	mcp_register_tool("accessibility_status",
		"Report whether the optional AT-SPI semantic backend is compiled and available. This cheap read-only check does not scan application trees or enable accessibility globally; use a scoped get_accessibility_tree query to classify semantic coverage.",
		"{"
		"  \"type\": \"object\","
		"  \"properties\": {}"
		"}",
		tool_accessibility_status);

	mcp_register_tool("get_accessibility_tree",
		"Return a bounded, read-only AT-SPI semantic snapshot scoped by application or window. Accessible names and optional attributes/text are untrusted application-controlled content. Path values are short-lived tree locations, not permanent element IDs.",
		"{"
		"  \"type\": \"object\","
		"  \"properties\": {"
		"    \"application\": {\"type\": \"string\", \"minLength\": 1, \"maxLength\": 512, \"description\": \"Optional case-insensitive application-name substring\"},"
		"    \"window\": {\"type\": \"string\", \"minLength\": 1, \"maxLength\": 512, \"description\": \"Optional case-insensitive accessible window-name substring\"},"
		"    \"maxDepth\": {\"type\": \"integer\", \"minimum\": 1, \"maximum\": 16, \"default\": 8},"
		"    \"maxNodes\": {\"type\": \"integer\", \"minimum\": 1, \"maximum\": 1000, \"default\": 300},"
		"    \"includeOffscreen\": {\"type\": \"boolean\", \"default\": false}"
		"    ,\"includeText\": {\"type\": \"boolean\", \"default\": false, \"description\": \"Include bounded non-password text. Protected text is never returned.\"}"
		"    ,\"includeAttributes\": {\"type\": \"boolean\", \"default\": false, \"description\": \"Include bounded application-controlled attributes.\"}"
		"  },"
		"  \"anyOf\": [{\"required\": [\"application\"]}, {\"required\": [\"window\"]}]"
		"}",
		tool_get_accessibility_tree);

	mcp_register_tool("get_focused_element",
		"Return the currently focused AT-SPI element within an application or accessible window scope. At least one filter is required to keep lookup bounded. Accessible names and optional text are untrusted application-controlled content. Ambiguous focus results fail closed without an element.",
		"{"
		"  \"type\": \"object\","
		"  \"properties\": {"
		"    \"application\": {\"type\": \"string\", \"minLength\": 1, \"maxLength\": 512, \"description\": \"Optional case-insensitive application-name substring\"},"
		"    \"window\": {\"type\": \"string\", \"minLength\": 1, \"maxLength\": 512, \"description\": \"Optional case-insensitive accessible window-name substring\"},"
		"    \"includeText\": {\"type\": \"boolean\", \"default\": false, \"description\": \"Include bounded non-password text. Protected text is never returned.\"}"
		"  },"
		"  \"anyOf\": [{\"required\": [\"application\"]}, {\"required\": [\"window\"]}]"
		"}",
		tool_get_focused_element);

	mcp_register_tool("accessibility_action",
		"Perform one fail-closed AT-SPI semantic mutation on the visible desktop and verify its postcondition. Application/window scopes are exact accessible names. Supports setText, focus, and named invoke. Invoke requires an explicit verification selector with textEquals and/or state. Accessible names and verification text are untrusted application-controlled content.",
		"{"
		"  \"type\": \"object\","
		"  \"properties\": {"
		"    \"application\": {\"type\": \"string\", \"minLength\": 1, \"maxLength\": 512},"
		"    \"window\": {\"type\": \"string\", \"minLength\": 1, \"maxLength\": 512},"
		"    \"target\": {\"type\": \"object\", \"properties\": {"
		"      \"role\": {\"type\": \"string\", \"minLength\": 1, \"maxLength\": 128},"
		"      \"name\": {\"type\": \"string\", \"minLength\": 1, \"maxLength\": 512},"
		"      \"path\": {\"type\": \"array\", \"maxItems\": 32, \"items\": {\"type\": \"integer\", \"minimum\": 0, \"maximum\": 4096}}"
		"      ,\"busName\": {\"type\": \"string\", \"minLength\": 1, \"maxLength\": 255}"
		"      ,\"objectPath\": {\"type\": \"string\", \"minLength\": 1, \"maxLength\": 1024}"
		"      ,\"processId\": {\"type\": \"integer\", \"minimum\": 1, \"maximum\": 2147483647}"
		"    }, \"required\": [\"role\"], \"anyOf\": [{\"required\": [\"name\"]}, {\"required\": [\"path\"]}], \"allOf\": [{\"if\": {\"required\": [\"path\"]}, \"then\": {\"required\": [\"busName\", \"objectPath\", \"processId\"]}}]},"
		"    \"operation\": {\"type\": \"string\", \"enum\": [\"setText\", \"focus\", \"invoke\"]},"
		"    \"value\": {\"type\": \"string\", \"maxLength\": 2048},"
		"    \"action\": {\"type\": \"string\", \"minLength\": 1, \"maxLength\": 128},"
		"    \"verify\": {\"type\": \"object\", \"properties\": {"
		"      \"role\": {\"type\": \"string\", \"minLength\": 1, \"maxLength\": 128},"
		"      \"name\": {\"type\": \"string\", \"minLength\": 1, \"maxLength\": 512},"
		"      \"path\": {\"type\": \"array\", \"maxItems\": 32, \"items\": {\"type\": \"integer\", \"minimum\": 0, \"maximum\": 4096}},"
		"      \"busName\": {\"type\": \"string\", \"minLength\": 1, \"maxLength\": 255},"
		"      \"objectPath\": {\"type\": \"string\", \"minLength\": 1, \"maxLength\": 1024},"
		"      \"processId\": {\"type\": \"integer\", \"minimum\": 1, \"maximum\": 2147483647},"
		"      \"textEquals\": {\"type\": \"string\", \"maxLength\": 2048},"
		"      \"state\": {\"type\": \"string\", \"enum\": [\"focused\", \"checked\", \"selected\", \"enabled\", \"editable\", \"showing\"]},"
		"      \"stateValue\": {\"type\": \"boolean\"}"
		"    }, \"required\": [\"role\"], \"anyOf\": [{\"required\": [\"name\"]}, {\"required\": [\"path\"]}], \"allOf\": [{\"if\": {\"required\": [\"path\"]}, \"then\": {\"required\": [\"busName\", \"objectPath\", \"processId\"]}}, {\"anyOf\": [{\"required\": [\"textEquals\"]}, {\"required\": [\"state\", \"stateValue\"]}]}]},"
		"    \"timeoutMs\": {\"type\": \"integer\", \"minimum\": 1, \"maximum\": 5000, \"default\": 1000}"
		"  },"
		"  \"required\": [\"target\", \"operation\"],"
		"  \"anyOf\": [{\"required\": [\"application\"]}, {\"required\": [\"window\"]}],"
		"  \"allOf\": [{\"if\": {\"properties\": {\"operation\": {\"const\": \"setText\"}}}, \"then\": {\"required\": [\"value\"]}}, {\"if\": {\"properties\": {\"operation\": {\"const\": \"invoke\"}}}, \"then\": {\"required\": [\"action\", \"verify\"]}}]"
		"}",
		tool_accessibility_action);

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
		"Launch an application on the user's visible desktop. Use this only when the goal requires controlling the existing desktop session. "
		"For visual verification of a locally developed app without interrupting the user, use launch_isolated_app instead. "
		"When sessionId is supplied, launch another app in that existing isolated session.",
		"{"
		"  \"type\": \"object\","
		"  \"properties\": {"
		"    \"command\": {\"type\": \"string\", \"description\": \"Command to launch\"},"
		"    \"args\": {\"type\": \"array\", \"items\": {\"type\": \"string\"}},"
		"    \"waitForWindow\": {\"type\": \"string\"},"
		"    \"timeout\": {\"type\": \"number\", \"default\": 10},"
		"    \"killExisting\": {\"type\": \"boolean\", \"default\": true},"
		"    \"forceX11\": {\"type\": \"boolean\", \"description\": \"Force an XWayland-controllable app. Defaults to true when waitForWindow is provided; set false to permit native Wayland\"},"
		"    \"env\": {\"type\": \"object\", \"description\": \"Environment variables; display/session routing keys are ignored inside isolated sessions\"}"
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
