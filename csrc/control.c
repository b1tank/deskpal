/*
 * deskpal — visible-desktop control arbitration
 *
 * A lazy advisory lock prevents multiple MCP hosts from racing the same
 * pointer, keyboard, clipboard, or window manager. Private Xvfb sessions are
 * independent and bypass this lock.
 *
 * Copyright (c) 2026 deskpal contributors
 * SPDX-License-Identifier: MIT
 */
#include "control.h"

#include <errno.h>
#include <fcntl.h>
#include <ctype.h>
#include <pwd.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

static int g_control_fd = -1;

int control_tool_requires_lock(const char *tool_name)
{
	static const char *mutating_tools[] = {
		"focus_window", "click", "click_text", "launch_app",
		"launch_isolated_app",
		"type_text", "key_press", "resize_window", "mouse_move",
		"scroll", "drag", "mouse_down", "mouse_up", "set_clipboard",
		"hover_text", "exec", NULL
	};

	for (int i = 0; mutating_tools[i]; i++) {
		if (strcmp(tool_name, mutating_tools[i]) == 0) return 1;
	}
	return 0;
}

static int lock_path(char *path, size_t path_len)
{
	const char *display = getenv("DISPLAY");
	if (!display || !display[0]) display = "none";
	char canonical[256] = { 0 };
	const char *colon = strrchr(display, ':');
	if (colon) {
		size_t host_len = (size_t)(colon - display);
		char host[128] = { 0 };
		if (host_len >= sizeof(host)) return -1;
		memcpy(host, display, host_len);
		for (size_t i = 0; host[i]; i++)
			host[i] = (char)tolower((unsigned char)host[i]);
		const char *number = colon + 1;
		size_t number_len = strcspn(number, ".");
		if (number_len == 0 || number_len >= 32) return -1;
		char number_text[32];
		memcpy(number_text, number, number_len);
		number_text[number_len] = '\0';
		char *number_end = NULL;
		errno = 0;
		unsigned long display_number = strtoul(number_text, &number_end, 10);
		if (errno != 0 || number_end == number_text || *number_end != '\0')
			return -1;
		int local = host[0] == '\0' || strcmp(host, "unix") == 0 ||
			strcmp(host, "unix/") == 0 || strcmp(host, "localhost") == 0 ||
			strcmp(host, "127.0.0.1") == 0 || strcmp(host, "[::1]") == 0;
		if (snprintf(canonical, sizeof(canonical), "%s:%lu",
		             local ? "local" : host, display_number)
		    >= (int)sizeof(canonical))
			return -1;
	} else if (snprintf(canonical, sizeof(canonical), "%s", display)
	           >= (int)sizeof(canonical)) {
		return -1;
	}

	unsigned long hash = 2166136261u;
	for (const unsigned char *p = (const unsigned char *)canonical; *p; p++) {
		hash ^= *p;
		hash *= 16777619u;
	}
	char directory[512];
	uid_t uid = getuid();
	if (snprintf(directory, sizeof(directory), "/run/user/%ld", (long)uid)
	    >= (int)sizeof(directory))
		return -1;
	struct stat info;
	if (lstat(directory, &info) != 0 || !S_ISDIR(info.st_mode) ||
	    info.st_uid != uid || (info.st_mode & (S_IWGRP | S_IWOTH)) != 0) {
		struct passwd *account = getpwuid(uid);
		if (!account || !account->pw_dir || !account->pw_dir[0]) return -1;
		if (snprintf(directory, sizeof(directory), "%s/.deskpal", account->pw_dir)
		    >= (int)sizeof(directory))
			return -1;
		if (mkdir(directory, 0700) != 0 && errno != EEXIST) return -1;
		if (lstat(directory, &info) != 0 || !S_ISDIR(info.st_mode) ||
		    info.st_uid != uid || (info.st_mode & (S_IRWXG | S_IRWXO)) != 0)
			return -1;
	}

	return snprintf(path, path_len, "%s/deskpal-%08lx-control.lock",
	                directory, hash & 0xffffffffUL) < (int)path_len ? 0 : -1;
}

int control_acquire(char *error, size_t error_len)
{
	if (g_control_fd >= 0) return 0;

	char path[512];
	if (lock_path(path, sizeof(path)) != 0) {
		snprintf(error, error_len, "desktop control lock path is too long");
		return -1;
	}

	int flags = O_RDWR | O_CREAT | O_CLOEXEC;
#ifdef O_NOFOLLOW
	flags |= O_NOFOLLOW;
#endif
	int fd = open(path, flags, 0600);
	if (fd < 0) {
		snprintf(error, error_len, "could not open desktop control lock: %s",
		         strerror(errno));
		return -1;
	}

	struct stat info;
	if (fstat(fd, &info) != 0 || !S_ISREG(info.st_mode) ||
	    info.st_uid != getuid() || info.st_nlink != 1 ||
	    (info.st_mode & (S_IWGRP | S_IWOTH)) != 0) {
		snprintf(error, error_len,
		         "desktop control lock has unsafe ownership or permissions");
		close(fd);
		return -1;
	}

	if (flock(fd, LOCK_EX | LOCK_NB) != 0) {
		char owner[96] = { 0 };
		ssize_t count = pread(fd, owner, sizeof(owner) - 1, 0);
		if (count > 0) owner[count] = '\0';
		snprintf(error, error_len,
			"visible desktop control is already held%s%s. "
			"Close the other deskpal MCP session before trying again",
			owner[0] ? " by " : "", owner[0] ? owner : "");
		close(fd);
		return -1;
	}

	char owner[96];
	snprintf(owner, sizeof(owner), "deskpal pid %ld", (long)getpid());
	if (ftruncate(fd, 0) != 0 || pwrite(fd, owner, strlen(owner), 0) < 0) {
		snprintf(error, error_len, "could not record desktop control owner: %s",
		         strerror(errno));
		flock(fd, LOCK_UN);
		close(fd);
		return -1;
	}

	g_control_fd = fd;
	return 0;
}

void control_cleanup(void)
{
	if (g_control_fd < 0) return;
	flock(g_control_fd, LOCK_UN);
	close(g_control_fd);
	g_control_fd = -1;
}