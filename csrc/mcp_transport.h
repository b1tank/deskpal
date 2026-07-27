/*
 * deskpal — Bounded newline-delimited MCP input transport
 * SPDX-License-Identifier: MIT
 */
#ifndef DESKPAL_MCP_TRANSPORT_H
#define DESKPAL_MCP_TRANSPORT_H

#include "cJSON.h"

typedef struct McpInputTransport McpInputTransport;

McpInputTransport *mcp_input_transport_new(int fd);
void mcp_input_transport_free(McpInputTransport *transport);

/* Return 1 with an allocated line, 0 on clean EOF, or -1 on transport error. */
int mcp_input_transport_next(McpInputTransport *transport, char **line);

/* Non-blockingly consume matching notifications/cancelled messages while
 * preserving every other complete input line for the main request loop. */
int mcp_input_transport_poll_cancel(McpInputTransport *transport,
                                    const cJSON *active_request_id,
                                    int *cancelled,
                                    int *disconnected);

#endif /* DESKPAL_MCP_TRANSPORT_H */
