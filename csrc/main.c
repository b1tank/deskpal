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
#include <string.h>

/* Security gates — off by default. Enable via --allow-fs / --allow-exec.
 * See docs/proposed-tools.md §3 and §4 for rationale. */
int deskpal_allow_fs = 0;
int deskpal_allow_exec = 0;

static void print_usage(void)
{
	fprintf(stderr,
		"Usage: deskpal [options]\n"
		"\n"
		"Runs an MCP server on stdio.\n"
		"\n"
		"Options:\n"
		"  --allow-fs      Enable read_file tool (off by default)\n"
		"  --allow-exec    Enable exec tool (off by default)\n"
		"  -h, --help      Show this help and exit\n");
}

int main(int argc, char **argv)
{
	for (int i = 1; i < argc; i++) {
		if (strcmp(argv[i], "--allow-fs") == 0) {
			deskpal_allow_fs = 1;
		} else if (strcmp(argv[i], "--allow-exec") == 0) {
			deskpal_allow_exec = 1;
		} else if (strcmp(argv[i], "-h") == 0 || strcmp(argv[i], "--help") == 0) {
			print_usage();
			return 0;
		} else {
			fprintf(stderr, "deskpal: unknown argument: %s\n", argv[i]);
			print_usage();
			return 2;
		}
	}

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
