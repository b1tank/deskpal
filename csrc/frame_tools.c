/* Deskpal capture-bound visual observation MCP tools. */
#include "frame_tools.h"

#include "capture_target.h"
#include "captures.h"
#include "frame_settle.h"
#include "frame_state.h"
#include "mcp.h"
#include "screenshot.h"

#include <stdint.h>
#include <stdio.h>

static const char *json_str(const cJSON *obj, const char *key, const char *def)
{
	const cJSON *item = cJSON_GetObjectItem(obj, key);
	return cJSON_IsString(item) ? item->valuestring : def;
}

static int json_int(const cJSON *obj, const char *key, int def)
{
	const cJSON *item = cJSON_GetObjectItem(obj, key);
	return cJSON_IsNumber(item) ? item->valueint : def;
}

static double json_double(const cJSON *obj, const char *key, double def)
{
	const cJSON *item = cJSON_GetObjectItem(obj, key);
	return cJSON_IsNumber(item) ? item->valuedouble : def;
}

static int capture_settle_frame(
	const DeskpalCapture *base, void *data, ScreenshotFrame *frame,
	char *error, size_t error_len)
{
	(void)data;
	if (screenshot_capture_frame(base->window_id, frame) == 0) return 0;
	snprintf(error, error_len, "Could not capture exact X11 window frame");
	return -1;
}

static int frame_settle_cancelled(void *data)
{
	(void)data;
	return mcp_request_cancelled();
}

static cJSON *frame_diff_json(const FrameStateDiff *diff)
{
	cJSON *json = cJSON_CreateObject();
	cJSON_AddBoolToObject(json, "comparable", diff->comparable);
	cJSON_AddBoolToObject(json, "changed", diff->changed);
	cJSON_AddNumberToObject(json, "changedPixels",
	                       (double)diff->changed_pixels);
	cJSON_AddNumberToObject(json, "totalPixels", (double)diff->total_pixels);
	cJSON_AddNumberToObject(json, "changedFraction", diff->changed_fraction);
	cJSON_AddNumberToObject(json, "maxChannelDelta", diff->max_channel_delta);
	if (diff->changed && diff->comparable) {
		cJSON *bounds = cJSON_CreateObject();
		cJSON_AddNumberToObject(bounds, "x", diff->x);
		cJSON_AddNumberToObject(bounds, "y", diff->y);
		cJSON_AddNumberToObject(bounds, "width", diff->width);
		cJSON_AddNumberToObject(bounds, "height", diff->height);
		cJSON_AddItemToObject(json, "changedBounds", bounds);
	}
	return json;
}

cJSON *tool_wait_for_frame_stable(const cJSON *params)
{
	const char *capture_id = json_str(params, "captureId", NULL);
	int timeout_ms = json_int(params, "timeoutMs", 3000);
	int stable_ms = json_int(params, "stableMs", 200);
	int interval_ms = json_int(params, "intervalMs", 50);
	int tolerance = json_int(params, "tolerance", 0);
	DeskpalCapture base = {0};
	int lookup = captures_lookup(capture_id, &base);
	if (lookup == -2)
		return mcp_tool_error_result(
			"captureId is stale; take a fresh get_app_state observation");
	if (lookup != 0 || base.target != DESKPAL_CAPTURE_WINDOW ||
	    !base.frame_revision[0])
		return mcp_tool_error_result(
			"captureId must identify a stable get_app_state observation with source pixels");

	long long started_ms = frame_settle_monotonic_ms();
	FrameSettleResult settled = {0};
	int rc = frame_settle_wait(
		&base, started_ms + timeout_ms, stable_ms, interval_ms, tolerance,
		capture_validate_window, capture_settle_frame, NULL,
		frame_settle_cancelled, NULL, &settled);
	cJSON *payload = cJSON_CreateObject();
	cJSON_AddStringToObject(payload, "baseCaptureId", base.id);
	const char *status = settled.status == FRAME_SETTLE_SETTLED ? "settled" :
		settled.status == FRAME_SETTLE_TIMEOUT ? "timeout" :
		settled.status == FRAME_SETTLE_CANCELLED ? "cancelled" : "error";
	cJSON_AddStringToObject(payload, "status", status);
	cJSON_AddBoolToObject(payload, "settled",
	                     settled.status == FRAME_SETTLE_SETTLED);
	cJSON_AddBoolToObject(payload, "timedOut",
	                     settled.status == FRAME_SETTLE_TIMEOUT);
	cJSON_AddBoolToObject(payload, "cancelled",
	                     settled.status == FRAME_SETTLE_CANCELLED);
	cJSON_AddNumberToObject(payload, "timeoutMs", timeout_ms);
	cJSON_AddNumberToObject(payload, "stableMs", stable_ms);
	cJSON_AddNumberToObject(payload, "intervalMs", interval_ms);
	cJSON_AddNumberToObject(payload, "tolerance", tolerance);
	cJSON_AddNumberToObject(payload, "elapsedMs",
	                       frame_settle_monotonic_ms() - started_ms);
	cJSON_AddNumberToObject(payload, "sampleCount", settled.sample_count);
	cJSON_AddNumberToObject(payload, "changeCount", settled.change_count);
	cJSON_AddNumberToObject(payload, "stableForMs", settled.stable_for_ms);
	cJSON_AddBoolToObject(payload, "changedFromCapture",
	                     settled.changed_from_capture);
	cJSON_AddStringToObject(payload, "baseRevision", base.frame_revision);
	if (settled.final_signature.revision[0])
		cJSON_AddStringToObject(payload, "finalRevision",
		                      settled.final_signature.revision);
	if (settled.change_count > 0) {
		cJSON_AddItemToObject(payload, "lastChange",
		                     frame_diff_json(&settled.last_change));
		cJSON_AddItemToObject(payload, "largestChange",
		                     frame_diff_json(&settled.largest_change));
	}
	if (settled.error[0]) cJSON_AddStringToObject(payload, "error", settled.error);
	cJSON_AddBoolToObject(payload, "inputDelivered", 0);
	cJSON_AddBoolToObject(payload, "sharedPointerMoved", 0);
	cJSON_AddBoolToObject(payload, "focusChanged", 0);
	cJSON_AddBoolToObject(payload, "stackingChanged", 0);
	cJSON_AddBoolToObject(payload, "clipboardChanged", 0);
	cJSON *result = mcp_structured_tool_result(payload, "frameSettle");
	if (rc < 0 || settled.status == FRAME_SETTLE_CANCELLED)
		cJSON_AddBoolToObject(result, "isError", 1);
	frame_settle_result_clear(&settled);
	return result;
}

static int source_region_to_projection(
	const DeskpalCapture *base, const cJSON *region,
	int *x, int *y, int *width, int *height)
{
	int source_x = json_int(region, "x", -1);
	int source_y = json_int(region, "y", -1);
	int source_width = json_int(region, "width", -1);
	int source_height = json_int(region, "height", -1);
	if (source_x < 0 || source_y < 0 || source_width < 1 || source_height < 1 ||
	    source_x > base->source_width - source_width ||
	    source_y > base->source_height - source_height ||
	    base->frame_projection_width < 1 || base->frame_projection_height < 1)
		return -1;
	int x0 = (int)((int64_t)source_x * base->frame_projection_width /
	               base->source_width);
	int y0 = (int)((int64_t)source_y * base->frame_projection_height /
	               base->source_height);
	int x1 = (int)(((int64_t)(source_x + source_width) *
	                base->frame_projection_width + base->source_width - 1) /
	               base->source_width);
	int y1 = (int)(((int64_t)(source_y + source_height) *
	                base->frame_projection_height + base->source_height - 1) /
	               base->source_height);
	if (x1 > base->frame_projection_width) x1 = base->frame_projection_width;
	if (y1 > base->frame_projection_height) y1 = base->frame_projection_height;
	*x = x0;
	*y = y0;
	*width = x1 - x0;
	*height = y1 - y0;
	return *width > 0 && *height > 0 ? 0 : -1;
}

cJSON *tool_verify_frame_change(const cJSON *params)
{
	const char *capture_id = json_str(params, "captureId", NULL);
	const cJSON *region = cJSON_GetObjectItem(params, "region");
	int timeout_ms = json_int(params, "timeoutMs", 3000);
	int stable_ms = json_int(params, "stableMs", 200);
	int interval_ms = json_int(params, "intervalMs", 50);
	int tolerance = json_int(params, "tolerance", 0);
	double min_changed = json_double(params, "minChangedFraction", 0.001);
	double max_outside = json_double(params, "maxOutsideChangedFraction", 1.0);
	DeskpalCapture base = {0};
	int lookup = captures_lookup(capture_id, &base);
	if (lookup == -2)
		return mcp_tool_error_result(
			"captureId is stale; take a fresh get_app_state observation");
	if (lookup != 0 || base.target != DESKPAL_CAPTURE_WINDOW ||
	    !base.frame_projection || !base.frame_revision[0])
		return mcp_tool_error_result(
			"captureId has no retained visual projection for verification");
	int projection_x, projection_y, projection_width, projection_height;
	if (!region || source_region_to_projection(
	    &base, region, &projection_x, &projection_y,
	    &projection_width, &projection_height) != 0)
		return mcp_tool_error_result(
			"region must fall completely inside the captured source frame");

	long long started_ms = frame_settle_monotonic_ms();
	FrameSettleResult settled = {0};
	int settle_rc = frame_settle_wait(
		&base, started_ms + timeout_ms, stable_ms, interval_ms, tolerance,
		capture_validate_window, capture_settle_frame, NULL,
		frame_settle_cancelled, NULL, &settled);
	FrameStateDiff inside = {0};
	FrameStateDiff outside = {0};
	ScreenshotFrame original = {
		.pixels = base.frame_projection,
		.length = base.frame_projection_len,
		.width = base.frame_projection_width,
		.height = base.frame_projection_height,
		.depth = 24,
	};
	int compared = settle_rc == 1 && frame_state_compare_region(
		&original, &settled.final_projection, tolerance,
		projection_x, projection_y, projection_width, projection_height,
		&inside, &outside) == 0;
	int verified = compared && inside.changed_fraction >= min_changed &&
	               outside.changed_fraction <= max_outside;

	cJSON *payload = cJSON_CreateObject();
	cJSON_AddStringToObject(payload, "baseCaptureId", base.id);
	cJSON_AddBoolToObject(payload, "untrustedContent", 1);
	cJSON_AddStringToObject(payload, "contentWarning",
		"Visual change measurements are derived from application-controlled pixels.");
	cJSON_AddBoolToObject(payload, "verified", verified);
	cJSON_AddStringToObject(payload, "status",
		verified ? "verified" :
		settled.status == FRAME_SETTLE_TIMEOUT ? "timeout" :
		settled.status == FRAME_SETTLE_CANCELLED ? "cancelled" :
		settle_rc == 1 ? "postcondition_failed" : "error");
	cJSON_AddBoolToObject(payload, "settled", settle_rc == 1);
	cJSON_AddBoolToObject(payload, "cancelled",
	                     settled.status == FRAME_SETTLE_CANCELLED);
	cJSON_AddNumberToObject(payload, "elapsedMs",
	                       frame_settle_monotonic_ms() - started_ms);
	cJSON_AddNumberToObject(payload, "tolerance", tolerance);
	cJSON_AddNumberToObject(payload, "minChangedFraction", min_changed);
	cJSON_AddNumberToObject(payload, "maxOutsideChangedFraction", max_outside);
	cJSON *source_region = cJSON_Duplicate(region, 1);
	cJSON_AddItemToObject(payload, "sourceRegion", source_region);
	cJSON *projection = cJSON_CreateObject();
	cJSON_AddNumberToObject(projection, "width", base.frame_projection_width);
	cJSON_AddNumberToObject(projection, "height", base.frame_projection_height);
	cJSON_AddNumberToObject(projection, "regionX", projection_x);
	cJSON_AddNumberToObject(projection, "regionY", projection_y);
	cJSON_AddNumberToObject(projection, "regionWidth", projection_width);
	cJSON_AddNumberToObject(projection, "regionHeight", projection_height);
	cJSON_AddStringToObject(projection, "sampling", "box-average");
	cJSON_AddItemToObject(payload, "projection", projection);
	if (compared) {
		cJSON_AddItemToObject(payload, "insideRegion", frame_diff_json(&inside));
		cJSON_AddItemToObject(payload, "outsideRegion", frame_diff_json(&outside));
	}
	if (settled.error[0]) cJSON_AddStringToObject(payload, "error", settled.error);
	cJSON_AddBoolToObject(payload, "actionAttributed", 0);
	cJSON_AddBoolToObject(payload, "inputDelivered", 0);
	cJSON_AddBoolToObject(payload, "sharedPointerMoved", 0);
	cJSON_AddBoolToObject(payload, "focusChanged", 0);
	cJSON_AddBoolToObject(payload, "stackingChanged", 0);
	cJSON_AddBoolToObject(payload, "clipboardChanged", 0);
	cJSON *result = mcp_structured_tool_result(payload, "frameVerification");
	if (!verified) cJSON_AddBoolToObject(result, "isError", 1);
	frame_settle_result_clear(&settled);
	return result;
}
