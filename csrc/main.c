/*
 * deskpal — Entry point
 *
 * Copyright (c) 2026 deskpal contributors
 * SPDX-License-Identifier: MIT
 */
#include "mcp.h"
#include "x11.h"
#include "ocr.h"
#include "sessions.h"
#include "tools.h"
#include "control.h"
#include "captures.h"
#include "indicator.h"
#include "accessibility.h"
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define DESKPAL_HEADLESS_ENV "DESKPAL_HEADLESS_ACTIVE"
#define XVFB_RUN_PATH "/usr/bin/xvfb-run"

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
		"  --no-uinput     Use XTest only; intended for nested/test displays\n"
		"  -h, --help      Show this help and exit\n");
}

static int parse_screen_size(const char *value, int *width, int *height)
{
	char trailing;
	int parsed_width = 0;
	int parsed_height = 0;

	if (sscanf(value, "%dx%d%c", &parsed_width, &parsed_height, &trailing) != 2 ||
	    parsed_width < 320 || parsed_width > 16384 ||
	    parsed_height < 200 || parsed_height > 16384) {
		return -1;
	}

	*width = parsed_width;
	*height = parsed_height;
	return 0;
}

static int reexec_headless(int argc, char **argv, int width, int height)
{
	int log_fd = fcntl(STDERR_FILENO, F_DUPFD, 3);
	if (log_fd < 0) {
		perror("deskpal: could not preserve stderr for headless mode");
		return -1;
	}

	char server_args[64];
	char shell_command[64];
	snprintf(server_args, sizeof(server_args),
		"-screen 0 %dx%dx24", width, height);
	snprintf(shell_command, sizeof(shell_command),
		"exec \"$@\" 2>&%d %d>&-", log_fd, log_fd);

	char **headless_argv = calloc((size_t)argc + 9, sizeof(*headless_argv));
	if (!headless_argv) {
		close(log_fd);
		return -1;
	}

	int pos = 0;
	headless_argv[pos++] = XVFB_RUN_PATH;
	headless_argv[pos++] = "--auto-servernum";
	headless_argv[pos++] = "--server-args";
	headless_argv[pos++] = server_args;
	headless_argv[pos++] = "/bin/sh";
	headless_argv[pos++] = "-c";
	headless_argv[pos++] = shell_command;
	headless_argv[pos++] = "deskpal-headless";
	for (int i = 0; i < argc; i++)
		headless_argv[pos++] = argv[i];

	setenv(DESKPAL_HEADLESS_ENV, "1", 1);
	unsetenv("WAYLAND_DISPLAY");
	unsetenv("XDG_RUNTIME_DIR");
	unsetenv("DBUS_SESSION_BUS_ADDRESS");
	unsetenv("SESSION_MANAGER");
	unsetenv("AT_SPI_BUS_ADDRESS");
	unsetenv("AT_SPI_BUS");
	unsetenv("AT_SPI_DISPLAY");
	setenv("XDG_SESSION_TYPE", "x11", 1);
	setenv("GDK_BACKEND", "x11", 1);
	setenv("QT_QPA_PLATFORM", "xcb", 1);

	execv(headless_argv[0], headless_argv);
	fprintf(stderr, "deskpal: failed to start xvfb-run: %s\n", strerror(errno));
	free(headless_argv);
	close(log_fd);
	return -1;
}

int main(int argc, char **argv)
{
	int xvfb_child = 0;
	int disable_uinput = 0;
	int screen_width = 1920;
	int screen_height = 1080;
	int control_lock_fd = -1;
	int control_lock_fd_count = 0;

	for (int i = 1; i < argc; i++) {
		if (strcmp(argv[i], "--xvfb-child") == 0) {
			xvfb_child = 1;
		} else if (strcmp(argv[i], "--screen-size") == 0) {
			if (++i >= argc ||
			    parse_screen_size(argv[i], &screen_width, &screen_height) != 0) {
				fprintf(stderr, "deskpal: --screen-size must be WIDTHxHEIGHT\n");
				return 2;
			}
		} else if (strcmp(argv[i], "--allow-fs") == 0) {
			deskpal_allow_fs = 1;
		} else if (strcmp(argv[i], "--allow-exec") == 0) {
			deskpal_allow_exec = 1;
		} else if (strcmp(argv[i], "--no-uinput") == 0) {
			disable_uinput = 1;
		} else if (strcmp(argv[i], "--control-lock-fd") == 0) {
			control_lock_fd_count++;
			if (++i >= argc) return 2;
			char *end = NULL;
			long value = strtol(argv[i], &end, 10);
			if (end == argv[i] || *end != '\0' || value < 3 || value > 1048576)
				return 2;
			control_lock_fd = (int)value;
		} else if (strcmp(argv[i], "-h") == 0 || strcmp(argv[i], "--help") == 0) {
			print_usage();
			return 0;
		} else {
			fprintf(stderr, "deskpal: unknown argument: %s\n", argv[i]);
			print_usage();
			return 2;
		}
	}
	if (!xvfb_child) {
		if (control_lock_fd_count) {
			fprintf(stderr, "deskpal: inherited control lock is internal-only\n");
			return 2;
		}
		unsetenv(DESKPAL_HEADLESS_ENV);
	} else if (!getenv(DESKPAL_HEADLESS_ENV)) {
		if (control_lock_fd_count != 1) {
			fprintf(stderr, "deskpal: isolated child requires inherited control lock\n");
			return 2;
		}
		if (reexec_headless(argc, argv, screen_width, screen_height) != 0)
			return 1;
	} else {
		char control_error[320];
		if (control_lock_fd_count != 1 ||
		    control_adopt_fd(control_lock_fd, control_error,
		    sizeof(control_error)) != 0) {
			fprintf(stderr, "deskpal: invalid inherited control lock%s%s\n",
				control_lock_fd_count == 1 ? ": " : "",
				control_lock_fd_count == 1 ? control_error : "");
			return 2;
		}
	}
	/* Disable stdout buffering for MCP stdio transport */
	setvbuf(stdout, NULL, _IONBF, 0);
	setvbuf(stderr, NULL, _IONBF, 0);

	if (x11_init(!xvfb_child && !disable_uinput) != 0) {
		fprintf(stderr, "deskpal: failed to connect to X11 display\n");
		return 1;
	}

	/* OCR is optional — warn but continue if not available */
	if (!ocr_init()) {
		fprintf(stderr, "deskpal: tesseract not found, OCR tools disabled. "
		        "Install: sudo apt install tesseract-ocr\n");
	}

	if (accessibility_init() != 0) {
		fprintf(stderr, "deskpal: AT-SPI accessibility backend unavailable\n");
	}

	tools_register_all();
	sessions_init();

	int rc = mcp_run();

	sessions_cleanup_all();
	indicator_cleanup();
	captures_cleanup();
	control_cleanup();
	accessibility_cleanup();
	ocr_cleanup();
	x11_cleanup();
	return rc;
}
