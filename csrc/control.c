/*
 * deskpal — visible-desktop control arbitration
 *
 * A lazy advisory lock prevents multiple MCP hosts from racing the same
 * pointer, keyboard, clipboard, or window manager. Private Xvfb children
 * inherit the parent's already-locked open-file description.
 *
 * Copyright (c) 2026 deskpal contributors
 * SPDX-License-Identifier: MIT
 */
#include "control.h"

#include <errno.h>
#include <fcntl.h>
#include <pwd.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

static int g_control_fd = -1;
static int g_control_fd_adopted = 0;

static int lock_path(char *path, size_t path_len);

int control_tool_requires_lock(const char *tool_name)
{
	static const char *mutating_tools[] = {
		"focus_window", "click", "click_text", "launch_app",
		"launch_isolated_app",
		"type_text", "key_press", "resize_window", "mouse_move",
		"scroll", "drag", "mouse_down", "mouse_up", "set_clipboard",
		"hover_text", "accessibility_action",
		"agent_cursor_move", "agent_cursor_hide",
		"agent_semantic_press", "agent_semantic_set_text",
		"agent_semantic_set_value", "agent_semantic_select",
		"agent_semantic_replace_text_range", "exec", NULL
	};

	for (int i = 0; mutating_tools[i]; i++) {
		if (strcmp(tool_name, mutating_tools[i]) == 0) return 1;
	}
	return 0;
}

static int validate_lock_fd(int fd, char *error, size_t error_len)
{
	char path[512];
	if (lock_path(path, sizeof(path)) != 0) {
		snprintf(error, error_len, "desktop control lock path is too long");
		return -1;
	}
	struct stat expected;
	struct stat actual;
	if (lstat(path, &expected) != 0 || fstat(fd, &actual) != 0 ||
	    !S_ISREG(actual.st_mode) || actual.st_uid != getuid() ||
	    actual.st_nlink != 1 || (actual.st_mode & (S_IWGRP | S_IWOTH)) != 0 ||
	    expected.st_dev != actual.st_dev || expected.st_ino != actual.st_ino) {
		snprintf(error, error_len,
			"inherited desktop control descriptor is not the active lock file");
		return -1;
	}
	if (flock(fd, LOCK_EX | LOCK_NB) != 0) {
		snprintf(error, error_len,
			"inherited desktop control descriptor does not own the active lock");
		return -1;
	}
	return 0;
}

int control_export_to_fd(int target_fd, char *error, size_t error_len)
{
	if (g_control_fd < 0) {
		snprintf(error, error_len, "visible desktop control is not held");
		return -1;
	}
	if (target_fd < 3 || (target_fd != g_control_fd &&
	    dup2(g_control_fd, target_fd) < 0)) {
		snprintf(error, error_len, "could not export desktop control lock: %s",
			strerror(errno));
		return -1;
	}
	int flags = fcntl(target_fd, F_GETFD);
	if (flags < 0 || fcntl(target_fd, F_SETFD, flags & ~FD_CLOEXEC) != 0) {
		snprintf(error, error_len, "could not preserve desktop control lock: %s",
			strerror(errno));
		if (target_fd != g_control_fd) close(target_fd);
		return -1;
	}
	return 0;
}

int control_adopt_fd(int fd, char *error, size_t error_len)
{
	if (g_control_fd >= 0 || fd < 3 ||
	    validate_lock_fd(fd, error, error_len) != 0)
		return -1;
	int flags = fcntl(fd, F_GETFD);
	if (flags < 0 || fcntl(fd, F_SETFD, flags | FD_CLOEXEC) != 0) {
		snprintf(error, error_len, "could not secure inherited control lock: %s",
			strerror(errno));
		return -1;
	}
	g_control_fd = fd;
	g_control_fd_adopted = 1;
	return 0;
}

static int lock_path(char *path, size_t path_len)
{
	const char *canonical = "visible-desktop";

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
	g_control_fd_adopted = 0;
	return 0;
}

int control_is_held(void)
{
	return g_control_fd >= 0;
}

int control_is_adopted(void)
{
	return g_control_fd_adopted;
}

int control_release(char *error, size_t error_len)
{
	if (g_control_fd < 0) return 0;
	if (g_control_fd_adopted) {
		snprintf(error, error_len,
		         "isolated child cannot release inherited desktop control");
		return -1;
	}
	/* Clear our owner text while still holding the lock. Clearing it after
	 * unlock could race and erase the next owner's metadata. */
	if (ftruncate(g_control_fd, 0) != 0) {
		snprintf(error, error_len, "could not clear desktop control owner: %s",
		         strerror(errno));
		return -1;
	}
	if (flock(g_control_fd, LOCK_UN) != 0) {
		snprintf(error, error_len, "could not release desktop control: %s",
		         strerror(errno));
		return -1;
	}
	close(g_control_fd);
	g_control_fd = -1;
	g_control_fd_adopted = 0;
	return 0;
}

void control_cleanup(void)
{
	if (g_control_fd < 0) return;
	if (!g_control_fd_adopted) {
		char ignored[1];
		if (control_release(ignored, sizeof(ignored)) == 0) return;
		/* Shutdown must not retain the kernel lock if metadata cleanup fails. */
		flock(g_control_fd, LOCK_UN);
	}
	close(g_control_fd);
	g_control_fd = -1;
	g_control_fd_adopted = 0;
}