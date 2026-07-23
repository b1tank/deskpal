/*
 * deskpal — GNOME logical-cursor indicator backend
 *
 * Copyright (c) 2026 deskpal contributors
 * SPDX-License-Identifier: MIT
 */
#include "indicator.h"

#include <ctype.h>
#include <dbus/dbus.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/random.h>
#include <time.h>
#include <unistd.h>

#define INDICATOR_SERVICE "org.deskpal.Indicator"
#define INDICATOR_PATH "/org/deskpal/Indicator"
#define INDICATOR_INTERFACE "org.deskpal.Indicator"
#define INDICATOR_TIMEOUT_MS 1000
#define OWNED_CURSOR_LIMIT 16
#define REMOTE_CURSOR_ID_LEN 65

#define CALL_ERROR (-1)
#define CALL_FALSE 0
#define CALL_TRUE 1

typedef struct {
	int active;
	char logical_id[DESKPAL_INDICATOR_CURSOR_ID_LEN];
	char remote_id[REMOTE_CURSOR_ID_LEN];
} OwnedCursor;

static DBusConnection *connection;
static OwnedCursor owned[OWNED_CURSOR_LIMIT];
static char owner_prefix[24];

static void set_error(char *error, size_t error_len, const char *message)
{
	if (!error || error_len == 0) return;
	snprintf(error, error_len, "%s", message ? message : "Indicator call failed");
}

static uint64_t monotonic_nonce(void)
{
	uint64_t nonce = 0;
	if (getrandom(&nonce, sizeof(nonce), GRND_NONBLOCK) ==
	    (ssize_t)sizeof(nonce) && nonce != 0)
		return nonce;
	struct timespec now;
	clock_gettime(CLOCK_MONOTONIC, &now);
	return ((uint64_t)(unsigned int)getpid() << 32) ^
	       (uint64_t)now.tv_sec ^ (uint64_t)now.tv_nsec;
}

static void ensure_owner_prefix(void)
{
	if (owner_prefix[0]) return;
	uint64_t nonce = monotonic_nonce();
	snprintf(owner_prefix, sizeof(owner_prefix), "dp-%08x-%08x-",
	         (unsigned int)getpid(), (unsigned int)nonce);
}

static int valid_cursor_id(const char *cursor_id)
{
	if (!cursor_id || !cursor_id[0]) return 0;
	size_t len = strlen(cursor_id);
	if (len >= DESKPAL_INDICATOR_CURSOR_ID_LEN) return 0;
	for (size_t i = 0; i < len; i++) {
		unsigned char c = (unsigned char)cursor_id[i];
		if (!isalnum(c) && c != '_' && c != '-' && c != '.') return 0;
	}
	return 1;
}

static int valid_color(const char *color)
{
	if (!color || color[0] != '#' || strlen(color) != 7) return 0;
	for (int i = 1; i < 7; i++)
		if (!isxdigit((unsigned char)color[i])) return 0;
	return 1;
}

static int valid_label(const char *label)
{
	if (!label) return 1;
	size_t len = strlen(label);
	if (len > 48) return 0;
	for (size_t i = 0; i < len; i++) {
		unsigned char c = (unsigned char)label[i];
		if (c < 0x20 || c == 0x7f) return 0;
	}
	return 1;
}

static DBusConnection *get_connection(char *error, size_t error_len)
{
	if (connection && dbus_connection_get_is_connected(connection))
		return connection;
	if (connection) {
		dbus_connection_close(connection);
		dbus_connection_unref(connection);
		connection = NULL;
	}

	DBusError dbus_error;
	dbus_error_init(&dbus_error);
	connection = dbus_bus_get_private(DBUS_BUS_SESSION, &dbus_error);
	if (!connection) {
		set_error(error, error_len,
		          dbus_error.message ? dbus_error.message :
		          "Could not connect to the session D-Bus");
		dbus_error_free(&dbus_error);
		return NULL;
	}
	dbus_connection_set_exit_on_disconnect(connection, FALSE);
	dbus_error_free(&dbus_error);
	return connection;
}

static DBusMessage *call_method(DBusMessage *message, int *mutation_issued,
                                int *outcome_unknown,
                                char *error, size_t error_len)
{
	if (mutation_issued) *mutation_issued = 0;
	if (outcome_unknown) *outcome_unknown = 0;
	DBusConnection *bus = get_connection(error, error_len);
	if (!bus) {
		dbus_message_unref(message);
		return NULL;
	}

	DBusError dbus_error;
	dbus_error_init(&dbus_error);
	if (mutation_issued) *mutation_issued = 1;
	DBusMessage *reply = dbus_connection_send_with_reply_and_block(
		bus, message, INDICATOR_TIMEOUT_MS, &dbus_error);
	dbus_message_unref(message);
	if (!reply) {
		if (dbus_error_has_name(&dbus_error, DBUS_ERROR_NO_REPLY)) {
			if (outcome_unknown) *outcome_unknown = 1;
			set_error(error, error_len,
			          "Indicator call timed out; mutation outcome is unknown");
		} else {
			set_error(error, error_len,
			          dbus_error.message ? dbus_error.message :
			          "Indicator D-Bus call failed");
		}
		dbus_error_free(&dbus_error);
		return NULL;
	}
	dbus_error_free(&dbus_error);
	return reply;
}

static DBusMessage *new_method_call(const char *method,
                                    char *error, size_t error_len)
{
	DBusMessage *message = dbus_message_new_method_call(
		INDICATOR_SERVICE, INDICATOR_PATH, INDICATOR_INTERFACE, method);
	if (!message) set_error(error, error_len, "Out of memory creating D-Bus call");
	return message;
}

static int call_boolean_method(DBusMessage *message, int *mutation_issued,
                               int *outcome_unknown,
                               char *error, size_t error_len)
{
	DBusMessage *reply = call_method(message, mutation_issued, outcome_unknown,
	                                 error, error_len);
	if (!reply) return CALL_ERROR;

	dbus_bool_t value = FALSE;
	DBusError dbus_error;
	dbus_error_init(&dbus_error);
	int parsed = dbus_message_get_args(reply, &dbus_error,
	                                   DBUS_TYPE_BOOLEAN, &value,
	                                   DBUS_TYPE_INVALID);
	dbus_message_unref(reply);
	if (!parsed) {
		set_error(error, error_len,
		          dbus_error.message ? dbus_error.message :
		          "Indicator returned an invalid response");
		dbus_error_free(&dbus_error);
		return CALL_ERROR;
	}
	dbus_error_free(&dbus_error);
	return value ? CALL_TRUE : CALL_FALSE;
}

static int call_show(const char *remote_id, int x, int y,
                     const char *color, const char *label,
                     int *mutation_issued, int *outcome_unknown,
                     char *error, size_t error_len)
{
	DBusMessage *message = new_method_call("ShowCursor", error, error_len);
	if (!message) return CALL_ERROR;
	dbus_int32_t dbus_x = x;
	dbus_int32_t dbus_y = y;
	if (!dbus_message_append_args(message,
	                              DBUS_TYPE_STRING, &remote_id,
	                              DBUS_TYPE_INT32, &dbus_x,
	                              DBUS_TYPE_INT32, &dbus_y,
	                              DBUS_TYPE_STRING, &color,
	                              DBUS_TYPE_STRING, &label,
	                              DBUS_TYPE_INVALID)) {
		dbus_message_unref(message);
		set_error(error, error_len, "Out of memory building ShowCursor call");
		return CALL_ERROR;
	}
	return call_boolean_method(message, mutation_issued, outcome_unknown,
	                           error, error_len);
}

static int call_move(const char *remote_id, int x, int y,
                     const char *color, const char *label,
                     int *mutation_issued, int *outcome_unknown,
                     char *error, size_t error_len)
{
	DBusMessage *message = new_method_call("MoveCursorStyled", error, error_len);
	if (!message) return CALL_ERROR;
	dbus_int32_t dbus_x = x;
	dbus_int32_t dbus_y = y;
	if (!dbus_message_append_args(message,
	                              DBUS_TYPE_STRING, &remote_id,
	                              DBUS_TYPE_INT32, &dbus_x,
	                              DBUS_TYPE_INT32, &dbus_y,
	                              DBUS_TYPE_STRING, &color,
	                              DBUS_TYPE_STRING, &label,
	                              DBUS_TYPE_INVALID)) {
		dbus_message_unref(message);
		set_error(error, error_len, "Out of memory building MoveCursorStyled call");
		return CALL_ERROR;
	}
	return call_boolean_method(message, mutation_issued, outcome_unknown,
	                           error, error_len);
}

static int call_hide(const char *remote_id, int *mutation_issued,
                     int *outcome_unknown, char *error, size_t error_len)
{
	DBusMessage *message = new_method_call("HideCursor", error, error_len);
	if (!message) return CALL_ERROR;
	if (!dbus_message_append_args(message,
	                              DBUS_TYPE_STRING, &remote_id,
	                              DBUS_TYPE_INVALID)) {
		dbus_message_unref(message);
		set_error(error, error_len, "Out of memory building HideCursor call");
		return CALL_ERROR;
	}
	return call_boolean_method(message, mutation_issued, outcome_unknown,
	                           error, error_len);
}

static OwnedCursor *find_owned(const char *cursor_id)
{
	for (int i = 0; i < OWNED_CURSOR_LIMIT; i++)
		if (owned[i].active && strcmp(owned[i].logical_id, cursor_id) == 0)
			return &owned[i];
	return NULL;
}

static OwnedCursor *free_owned_slot(void)
{
	for (int i = 0; i < OWNED_CURSOR_LIMIT; i++)
		if (!owned[i].active) return &owned[i];
	return NULL;
}

int indicator_get_status(char **json, char *error, size_t error_len)
{
	if (!json) {
		set_error(error, error_len, "Missing indicator status output");
		return -1;
	}
	*json = NULL;
	DBusMessage *message = new_method_call("GetStatus", error, error_len);
	if (!message) return -1;
	DBusMessage *reply = call_method(message, NULL, NULL, error, error_len);
	if (!reply) return -1;

	const char *value = NULL;
	DBusError dbus_error;
	dbus_error_init(&dbus_error);
	int parsed = dbus_message_get_args(reply, &dbus_error,
	                                   DBUS_TYPE_STRING, &value,
	                                   DBUS_TYPE_INVALID);
	if (parsed && value) *json = strdup(value);
	dbus_message_unref(reply);
	if (!parsed || !value || !*json) {
		set_error(error, error_len,
		          dbus_error.message ? dbus_error.message :
		          "Indicator returned an invalid status response");
		dbus_error_free(&dbus_error);
		free(*json);
		*json = NULL;
		return -1;
	}
	dbus_error_free(&dbus_error);
	return 0;
}

int indicator_move_owned(const char *cursor_id, int x, int y,
                         const char *color, const char *label,
                         int *created, int *mutation_issued,
                         int *outcome_unknown,
                         char *error, size_t error_len)
{
	if (created) *created = 0;
	if (mutation_issued) *mutation_issued = 0;
	if (outcome_unknown) *outcome_unknown = 0;
	if (!valid_cursor_id(cursor_id)) {
		set_error(error, error_len,
		          "cursorId must be 1-40 letters, digits, dots, underscores, or hyphens");
		return -1;
	}
	if (!valid_color(color)) {
		set_error(error, error_len, "color must be #RRGGBB");
		return -1;
	}
	if (!valid_label(label)) {
		set_error(error, error_len,
		          "label must be at most 48 characters without control characters");
		return -1;
	}
	const char *safe_label = label && label[0] ? label : cursor_id;

	OwnedCursor *cursor = find_owned(cursor_id);
	if (cursor) {
		int moved = call_move(cursor->remote_id, x, y, color, safe_label,
		                      mutation_issued, outcome_unknown, error, error_len);
		if (moved == CALL_TRUE) return 0;
		if (moved == CALL_ERROR) return -1;
		/* A known false means the Shell lost this cursor, so recreating it is
		 * safe. Never retry a timeout or another unknown mutation outcome. */
		int shown = call_show(cursor->remote_id, x, y, color, safe_label,
		                      mutation_issued, outcome_unknown, error, error_len);
		if (shown != CALL_TRUE) {
			if (shown == CALL_FALSE)
				set_error(error, error_len, "Indicator rejected ShowCursor");
			return -1;
		}
		if (created) *created = 1;
		return 0;
	}

	cursor = free_owned_slot();
	if (!cursor) {
		set_error(error, error_len, "This Deskpal session owns too many cursors");
		return -1;
	}
	ensure_owner_prefix();
	int written = snprintf(cursor->remote_id, sizeof(cursor->remote_id), "%s%s",
	                       owner_prefix, cursor_id);
	if (written < 0 || (size_t)written >= sizeof(cursor->remote_id)) {
		set_error(error, error_len, "Namespaced cursor ID is too long");
		memset(cursor, 0, sizeof(*cursor));
		return -1;
	}
	int shown = call_show(cursor->remote_id, x, y, color, safe_label,
	                      mutation_issued, outcome_unknown, error, error_len);
	if (shown != CALL_TRUE) {
		if (shown == CALL_FALSE)
			set_error(error, error_len, "Indicator rejected ShowCursor");
		memset(cursor, 0, sizeof(*cursor));
		return -1;
	}
	snprintf(cursor->logical_id, sizeof(cursor->logical_id), "%s", cursor_id);
	cursor->active = 1;
	if (created) *created = 1;
	return 0;
}

int indicator_hide_owned(const char *cursor_id, int *hidden,
                         int *mutation_issued, int *outcome_unknown,
                         char *error, size_t error_len)
{
	if (hidden) *hidden = 0;
	if (mutation_issued) *mutation_issued = 0;
	if (outcome_unknown) *outcome_unknown = 0;
	if (!valid_cursor_id(cursor_id)) {
		set_error(error, error_len, "Invalid cursorId");
		return -1;
	}
	OwnedCursor *cursor = find_owned(cursor_id);
	if (!cursor) return 0;
	int result = call_hide(cursor->remote_id, mutation_issued,
	                       outcome_unknown, error, error_len);
	if (result == CALL_ERROR) return -1;
	if (hidden) *hidden = result == CALL_TRUE;
	memset(cursor, 0, sizeof(*cursor));
	return 0;
}

int indicator_logical_id_for_remote(const char *remote_id,
                                    char *cursor_id, size_t cursor_id_len)
{
	if (!remote_id || !cursor_id || cursor_id_len == 0) return -1;
	for (int i = 0; i < OWNED_CURSOR_LIMIT; i++) {
		if (owned[i].active && strcmp(owned[i].remote_id, remote_id) == 0) {
			if (strlen(owned[i].logical_id) >= cursor_id_len) return -1;
			snprintf(cursor_id, cursor_id_len, "%s", owned[i].logical_id);
			return 0;
		}
	}
	return -1;
}

void indicator_cleanup(void)
{
	/* Closing the private connection is the ownership release. GNOME removes
	 * every cursor for its vanished unique bus name, including after crashes.
	 * Avoid serial HideCursor calls, which could add one timeout per cursor. */
	if (connection) {
		dbus_connection_close(connection);
		dbus_connection_unref(connection);
		connection = NULL;
	}
	memset(owned, 0, sizeof(owned));
	owner_prefix[0] = '\0';
}
