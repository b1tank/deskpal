/*
 * deskpal — bounded read-only GNOME Shell bridge client
 *
 * Copyright (c) 2026 deskpal contributors
 * SPDX-License-Identifier: MIT
 */
#include "shell_bridge.h"

#include <dbus/dbus.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define SHELL_BRIDGE_SERVICE "org.deskpal.ShellBridge"
#define SHELL_BRIDGE_PATH "/org/deskpal/ShellBridge"
#define SHELL_BRIDGE_INTERFACE "org.deskpal.ShellBridge1"
#define SHELL_BRIDGE_TIMEOUT_MS 1000
#define SHELL_BRIDGE_ID_LIMIT 64
#define SHELL_BRIDGE_STRING_LIMIT 2048
#define SHELL_BRIDGE_MAX_MONITORS 32

static void set_error(char *error, size_t error_len, const char *message)
{
	if (!error || error_len == 0) return;
	snprintf(error, error_len, "%s",
	         message ? message : "GNOME Shell bridge call failed");
}

static int bounded_string_or_null(const cJSON *value, size_t limit)
{
	return cJSON_IsNull(value) ||
	       (cJSON_IsString(value) && value->valuestring &&
	        strlen(value->valuestring) <= limit);
}

static int positive_integer(const cJSON *value)
{
	return cJSON_IsNumber(value) && isfinite(value->valuedouble) &&
	       value->valuedouble >= 1 &&
	       value->valuedouble == floor(value->valuedouble);
}

static int finite_number(const cJSON *value)
{
	return cJSON_IsNumber(value) && isfinite(value->valuedouble);
}

static int validate_common(const cJSON *root)
{
	const cJSON *version = cJSON_GetObjectItem(root, "protocolVersion");
	const cJSON *instance = cJSON_GetObjectItem(root, "shellInstanceId");
	return cJSON_IsObject(root) && cJSON_IsNumber(version) &&
	       version->valuedouble == DESKPAL_SHELL_BRIDGE_PROTOCOL_VERSION &&
	       cJSON_IsString(instance) && instance->valuestring &&
	       instance->valuestring[0] &&
	       strlen(instance->valuestring) <= SHELL_BRIDGE_ID_LIMIT;
}

static int validate_capabilities(const cJSON *root)
{
	const cJSON *backend = cJSON_GetObjectItem(root, "backend");
	const cJSON *coordinate = cJSON_GetObjectItem(root, "coordinateSpace");
	const cJSON *capabilities = cJSON_GetObjectItem(root, "capabilities");
	const cJSON *limits = cJSON_GetObjectItem(root, "limits");
	if (!cJSON_IsString(backend) ||
	    strcmp(backend->valuestring, "gnome-shell-extension") != 0 ||
	    !cJSON_IsString(coordinate) ||
	    strcmp(coordinate->valuestring, "gnome-stage-logical") != 0 ||
	    !cJSON_IsObject(capabilities) || !cJSON_IsObject(limits))
		return 0;
	const char *boolean_keys[] = {
		"windowEnumeration", "monitorLayout", "windowCapture",
		"foregroundWindowManagement", "surfaceInput", "backgroundInput",
	};
	for (size_t i = 0; i < sizeof(boolean_keys) / sizeof(boolean_keys[0]); i++)
		if (!cJSON_IsBool(cJSON_GetObjectItem(capabilities, boolean_keys[i])))
			return 0;
	const cJSON *max_windows = cJSON_GetObjectItem(limits, "maxWindows");
	const cJSON *max_string = cJSON_GetObjectItem(limits, "maxStringCharacters");
	return cJSON_IsNumber(max_windows) &&
	       max_windows->valuedouble == DESKPAL_SHELL_BRIDGE_MAX_WINDOWS &&
	       positive_integer(max_string) && max_string->valuedouble <= 512;
}

static int validate_bounds(const cJSON *bounds)
{
	if (!cJSON_IsObject(bounds)) return 0;
	const cJSON *x = cJSON_GetObjectItem(bounds, "x");
	const cJSON *y = cJSON_GetObjectItem(bounds, "y");
	const cJSON *width = cJSON_GetObjectItem(bounds, "width");
	const cJSON *height = cJSON_GetObjectItem(bounds, "height");
	return finite_number(x) && finite_number(y) && finite_number(width) &&
	       finite_number(height) && width->valuedouble > 0 &&
	       height->valuedouble > 0;
}

static int validate_window(const cJSON *window)
{
	if (!cJSON_IsObject(window)) return 0;
	const cJSON *surface = cJSON_GetObjectItem(window, "surfaceId");
	const cJSON *generation = cJSON_GetObjectItem(window, "generation");
	const cJSON *geometry = cJSON_GetObjectItem(window, "geometryRevision");
	const cJSON *backend = cJSON_GetObjectItem(window, "backend");
	const cJSON *client = cJSON_GetObjectItem(window, "clientType");
	const cJSON *focused = cJSON_GetObjectItem(window, "focused");
	const cJSON *hidden = cJSON_GetObjectItem(window, "hidden");
	if (!cJSON_IsString(surface) || !surface->valuestring ||
	    !surface->valuestring[0] ||
	    strlen(surface->valuestring) > SHELL_BRIDGE_ID_LIMIT ||
	    !positive_integer(generation) || !positive_integer(geometry) ||
	    !cJSON_IsString(backend) ||
	    strcmp(backend->valuestring, "gnome-shell-extension") != 0 ||
	    !cJSON_IsString(client) ||
	    (strcmp(client->valuestring, "wayland") != 0 &&
	     strcmp(client->valuestring, "x11") != 0 &&
	     strcmp(client->valuestring, "unknown") != 0) ||
	    !cJSON_IsBool(focused) || !cJSON_IsBool(hidden) ||
	    !validate_bounds(cJSON_GetObjectItem(window, "bounds")))
		return 0;
	const char *string_keys[] = {"title", "appId", "wmClass"};
	for (size_t i = 0; i < sizeof(string_keys) / sizeof(string_keys[0]); i++)
		if (!bounded_string_or_null(cJSON_GetObjectItem(window, string_keys[i]),
		                            SHELL_BRIDGE_STRING_LIMIT))
			return 0;
	const cJSON *pid = cJSON_GetObjectItem(window, "pid");
	const cJSON *workspace = cJSON_GetObjectItem(window, "workspace");
	const cJSON *monitor = cJSON_GetObjectItem(window, "monitor");
	const cJSON *scale = cJSON_GetObjectItem(window, "scale");
	return (cJSON_IsNull(pid) || finite_number(pid)) &&
	       (cJSON_IsNull(workspace) || finite_number(workspace)) &&
	       (cJSON_IsNull(monitor) || finite_number(monitor)) &&
	       finite_number(scale) && scale->valuedouble > 0;
}

static int validate_windows(const cJSON *root)
{
	if (!cJSON_IsBool(cJSON_GetObjectItem(root, "complete"))) return 0;
	const cJSON *windows = cJSON_GetObjectItem(root, "windows");
	if (!cJSON_IsArray(windows) ||
	    cJSON_GetArraySize(windows) > DESKPAL_SHELL_BRIDGE_MAX_WINDOWS)
		return 0;
	const cJSON *window = NULL;
	cJSON_ArrayForEach(window, windows)
		if (!validate_window(window)) return 0;
	return 1;
}

static int validate_monitor(const cJSON *monitor)
{
	if (!cJSON_IsObject(monitor)) return 0;
	const char *number_keys[] = {"index", "x", "y", "width", "height", "scale"};
	for (size_t i = 0; i < sizeof(number_keys) / sizeof(number_keys[0]); i++)
		if (!finite_number(cJSON_GetObjectItem(monitor, number_keys[i]))) return 0;
	return cJSON_GetObjectItem(monitor, "width")->valuedouble > 0 &&
	       cJSON_GetObjectItem(monitor, "height")->valuedouble > 0 &&
	       cJSON_GetObjectItem(monitor, "scale")->valuedouble > 0 &&
	       cJSON_IsBool(cJSON_GetObjectItem(monitor, "primary"));
}

static int validate_monitors(const cJSON *root)
{
	const cJSON *coordinate = cJSON_GetObjectItem(root, "coordinateSpace");
	const cJSON *monitors = cJSON_GetObjectItem(root, "monitors");
	if (!cJSON_IsString(coordinate) ||
	    strcmp(coordinate->valuestring, "gnome-stage-logical") != 0 ||
	    !cJSON_IsBool(cJSON_GetObjectItem(root, "complete")) ||
	    !positive_integer(cJSON_GetObjectItem(root, "stageWidth")) ||
	    !positive_integer(cJSON_GetObjectItem(root, "stageHeight")) ||
	    !finite_number(cJSON_GetObjectItem(root, "primaryIndex")) ||
	    !cJSON_IsArray(monitors) || cJSON_GetArraySize(monitors) == 0 ||
	    cJSON_GetArraySize(monitors) > SHELL_BRIDGE_MAX_MONITORS)
		return 0;
	const cJSON *monitor = NULL;
	cJSON_ArrayForEach(monitor, monitors)
		if (!validate_monitor(monitor)) return 0;
	return 1;
}

int shell_bridge_parse_response(const char *method, const char *json,
                                cJSON **response,
                                char *error, size_t error_len)
{
	if (response) *response = NULL;
	if (!method || !json || !response) {
		set_error(error, error_len, "Invalid Shell bridge parser arguments");
		return -1;
	}
	size_t length = strnlen(json, DESKPAL_SHELL_BRIDGE_RESPONSE_LIMIT + 1);
	if (length == 0 || length > DESKPAL_SHELL_BRIDGE_RESPONSE_LIMIT) {
		set_error(error, error_len, "Shell bridge response exceeded its 256 KiB limit");
		return -1;
	}
	cJSON *root = cJSON_ParseWithLength(json, length);
	if (!root || !validate_common(root)) {
		cJSON_Delete(root);
		set_error(error, error_len, "Shell bridge returned an invalid protocol envelope");
		return -1;
	}
	int valid = 0;
	if (strcmp(method, "GetCapabilities") == 0)
		valid = validate_capabilities(root);
	else if (strcmp(method, "ListWindows") == 0)
		valid = validate_windows(root);
	else if (strcmp(method, "GetMonitorLayout") == 0)
		valid = validate_monitors(root);
	if (!valid) {
		cJSON_Delete(root);
		set_error(error, error_len, "Shell bridge returned an invalid method response");
		return -1;
	}
	*response = root;
	return 0;
}

static int call_method(const char *method, cJSON **response,
                       char *error, size_t error_len)
{
	if (response) *response = NULL;
	DBusError dbus_error;
	dbus_error_init(&dbus_error);
	DBusConnection *connection = dbus_bus_get_private(DBUS_BUS_SESSION, &dbus_error);
	if (!connection) {
		set_error(error, error_len, dbus_error.message ? dbus_error.message :
		          "Could not connect to the session D-Bus");
		dbus_error_free(&dbus_error);
		return -1;
	}
	dbus_connection_set_exit_on_disconnect(connection, FALSE);
	DBusMessage *message = dbus_message_new_method_call(
		SHELL_BRIDGE_SERVICE, SHELL_BRIDGE_PATH, SHELL_BRIDGE_INTERFACE, method);
	if (!message) {
		set_error(error, error_len, "Out of memory creating Shell bridge call");
		dbus_connection_close(connection);
		dbus_connection_unref(connection);
		return -1;
	}
	DBusMessage *reply = dbus_connection_send_with_reply_and_block(
		connection, message, SHELL_BRIDGE_TIMEOUT_MS, &dbus_error);
	dbus_message_unref(message);
	if (!reply) {
		set_error(error, error_len, dbus_error.message ? dbus_error.message :
		          "Shell bridge D-Bus call failed");
		dbus_error_free(&dbus_error);
		dbus_connection_close(connection);
		dbus_connection_unref(connection);
		return -1;
	}
	const char *json = NULL;
	int parsed = dbus_message_get_args(reply, &dbus_error,
	                                   DBUS_TYPE_STRING, &json,
	                                   DBUS_TYPE_INVALID);
	int result = -1;
	if (!parsed || !json)
		set_error(error, error_len, dbus_error.message ? dbus_error.message :
		          "Shell bridge returned a non-string response");
	else
		result = shell_bridge_parse_response(method, json, response,
		                                     error, error_len);
	dbus_error_free(&dbus_error);
	dbus_message_unref(reply);
	dbus_connection_close(connection);
	dbus_connection_unref(connection);
	return result;
}

int shell_bridge_get_capabilities(cJSON **response,
                                  char *error, size_t error_len)
{
	return call_method("GetCapabilities", response, error, error_len);
}

int shell_bridge_list_windows(cJSON **response,
                              char *error, size_t error_len)
{
	return call_method("ListWindows", response, error, error_len);
}

int shell_bridge_get_monitor_layout(cJSON **response,
                                    char *error, size_t error_len)
{
	return call_method("GetMonitorLayout", response, error, error_len);
}
