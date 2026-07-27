/*
 * deskpal — Bounded newline-delimited MCP input transport
 * SPDX-License-Identifier: MIT
 */
#include "mcp_transport.h"

#include <errno.h>
#include <poll.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define MCP_INPUT_LIMIT (16 * 1024 * 1024)
#define MCP_QUEUED_LINE_LIMIT 64

struct McpInputTransport {
	int fd;
	char *buffer;
	size_t length;
	size_t capacity;
	char *queued[MCP_QUEUED_LINE_LIMIT];
	int queued_count;
	size_t queued_bytes;
	int eof;
	int failed;
};

static int ensure_capacity(McpInputTransport *transport, size_t required)
{
	if (required > MCP_INPUT_LIMIT) return -1;
	if (required <= transport->capacity) return 0;
	size_t capacity = transport->capacity ? transport->capacity : 8192;
	while (capacity < required) {
		if (capacity > MCP_INPUT_LIMIT / 2) {
			capacity = MCP_INPUT_LIMIT;
			break;
		}
		capacity *= 2;
	}
	char *buffer = realloc(transport->buffer, capacity);
	if (!buffer) return -1;
	transport->buffer = buffer;
	transport->capacity = capacity;
	return 0;
}

static int read_more(McpInputTransport *transport, int blocking)
{
	if (transport->eof || transport->failed) return transport->failed ? -1 : 0;
	struct pollfd descriptor = {
		.fd = transport->fd,
		.events = POLLIN | POLLHUP,
	};
	int ready;
	do {
		ready = poll(&descriptor, 1, blocking ? -1 : 0);
	} while (ready < 0 && errno == EINTR);
	if (ready < 0) {
		transport->failed = 1;
		return -1;
	}
	if (ready == 0) return 0;
	if (descriptor.revents & (POLLERR | POLLNVAL)) {
		transport->failed = 1;
		return -1;
	}
	if (!(descriptor.revents & (POLLIN | POLLHUP))) return 0;
	if (transport->length >= MCP_INPUT_LIMIT ||
	    ensure_capacity(transport,
	        transport->length > MCP_INPUT_LIMIT - 8192
	            ? MCP_INPUT_LIMIT : transport->length + 8192) != 0) {
		transport->failed = 1;
		return -1;
	}
	ssize_t count;
	do {
		count = read(transport->fd, transport->buffer + transport->length,
		             transport->capacity - transport->length);
	} while (count < 0 && errno == EINTR);
	if (count < 0) {
		if (!blocking && (errno == EAGAIN || errno == EWOULDBLOCK)) return 0;
		transport->failed = 1;
		return -1;
	}
	if (count == 0) {
		transport->eof = 1;
		return 0;
	}
	transport->length += (size_t)count;
	return 1;
}

static int extract_line(McpInputTransport *transport, char **line,
                        int allow_final)
{
	char *newline = transport->length
		? memchr(transport->buffer, '\n', transport->length) : NULL;
	if (!newline && !(allow_final && transport->eof && transport->length))
		return 0;
	size_t length = newline ? (size_t)(newline - transport->buffer)
	                        : transport->length;
	if (length && transport->buffer[length - 1] == '\r') length--;
	if (memchr(transport->buffer, '\0', length)) {
		transport->failed = 1;
		return -1;
	}
	char *copy = malloc(length + 1);
	if (!copy) return -1;
	memcpy(copy, transport->buffer, length);
	copy[length] = '\0';
	size_t consumed = newline
		? (size_t)(newline - transport->buffer) + 1 : transport->length;
	memmove(transport->buffer, transport->buffer + consumed,
	        transport->length - consumed);
	transport->length -= consumed;
	*line = copy;
	return 1;
}

static int queue_line(McpInputTransport *transport, char *line)
{
	size_t length = strlen(line) + 1;
	if (transport->queued_count >= MCP_QUEUED_LINE_LIMIT ||
	    transport->queued_bytes + length > MCP_INPUT_LIMIT)
		return -1;
	transport->queued[transport->queued_count++] = line;
	transport->queued_bytes += length;
	return 0;
}

static char *pop_line(McpInputTransport *transport)
{
	if (!transport->queued_count) return NULL;
	char *line = transport->queued[0];
	transport->queued_bytes -= strlen(line) + 1;
	memmove(&transport->queued[0], &transport->queued[1],
	        sizeof(transport->queued[0]) *
	        (size_t)(transport->queued_count - 1));
	transport->queued_count--;
	return line;
}

static int is_cancel_for(const char *line, const cJSON *active_request_id)
{
	cJSON *message = cJSON_Parse(line);
	if (!message) return 0;
	const cJSON *method = cJSON_GetObjectItem(message, "method");
	const cJSON *params = cJSON_GetObjectItem(message, "params");
	const cJSON *request_id = params
		? cJSON_GetObjectItem(params, "requestId") : NULL;
	int is_cancel = cJSON_IsString(method) &&
		strcmp(method->valuestring, "notifications/cancelled") == 0;
	int matches = is_cancel && active_request_id && request_id &&
		cJSON_Compare(active_request_id, request_id, 1);
	cJSON_Delete(message);
	return is_cancel ? (matches ? 2 : 1) : 0;
}

McpInputTransport *mcp_input_transport_new(int fd)
{
	if (fd < 0) return NULL;
	McpInputTransport *transport = calloc(1, sizeof(*transport));
	if (!transport) return NULL;
	transport->fd = fd;
	return transport;
}

void mcp_input_transport_free(McpInputTransport *transport)
{
	if (!transport) return;
	free(transport->buffer);
	for (int i = 0; i < transport->queued_count; i++)
		free(transport->queued[i]);
	free(transport);
}

int mcp_input_transport_next(McpInputTransport *transport, char **line)
{
	if (!transport || !line) return -1;
	*line = pop_line(transport);
	if (*line) return 1;
	for (;;) {
		int extracted = extract_line(transport, line, 1);
		if (extracted != 0) return extracted;
		if (transport->eof) return 0;
		if (read_more(transport, 1) < 0) return -1;
	}
}

int mcp_input_transport_poll_cancel(McpInputTransport *transport,
                                    const cJSON *active_request_id,
                                    int *cancelled,
                                    int *disconnected)
{
	if (!transport || !cancelled || !disconnected) return -1;
	for (;;) {
		char *line = NULL;
		int extracted;
		while ((extracted = extract_line(transport, &line, 0)) == 1) {
			int cancel = is_cancel_for(line, active_request_id);
			if (cancel == 2) *cancelled = 1;
			if (!cancel && queue_line(transport, line) != 0) {
				free(line);
				transport->failed = 1;
				return -1;
			}
			if (cancel) free(line);
			line = NULL;
		}
		if (extracted < 0) return -1;
		int read_result = read_more(transport, 0);
		if (read_result < 0) return -1;
		if (read_result == 0) break;
	}
	if (transport->eof) *disconnected = 1;
	return 0;
}
