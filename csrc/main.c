/*
 * deskpal — Entry point
 *
 * Copyright (c) 2026 deskpal contributors
 * SPDX-License-Identifier: MIT
 */
#include "mcp.h"
#include "x11.h"
#include "ocr.h"
#include "tools.h"
#include <stdio.h>
#include <stdlib.h>

int main(void)
{
	/* Disable stdout buffering for MCP stdio transport */
	setvbuf(stdout, NULL, _IONBF, 0);
	setvbuf(stderr, NULL, _IONBF, 0);

	if (x11_init() != 0) {
		fprintf(stderr, "deskpal: failed to connect to X11 display\n");
		return 1;
	}

	/* OCR is optional — warn but continue if not available */
	if (!ocr_init()) {
		fprintf(stderr, "deskpal: tesseract not found, OCR tools disabled. "
		        "Install: sudo apt install tesseract-ocr\n");
	}

	tools_register_all();

	int rc = mcp_run();

	ocr_cleanup();
	x11_cleanup();
	return rc;
}
