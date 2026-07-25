/*
 * deskpal — Isolated Xvfb session management
 *
 * Each isolated session is a child deskpal MCP server running under Xvfb.
 * The parent remains connected to the user's desktop and proxies only calls
 * carrying the child session's ID.
 *
 * Copyright (c) 2026 deskpal contributors
 * SPDX-License-Identifier: MIT
 */
#include "sessions.h"
#include "mcp.h"
#include "tools.h"
#include "control.h"

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#define MAX_ISOLATED_SESSIONS 8
#define DEFAULT_RESPONSE_TIMEOUT_MS 15000
#define RESPONSE_TIMEOUT_GRACE_MS 5000
#define MAX_SESSION_RESPONSE_BYTES (64 * 1024 * 1024)

typedef struct {
	int used;
	char id[32];
	pid_t pid;
	FILE *to_child;
	int from_child_fd;
	int broken;
	unsigned long next_request_id;
} IsolatedSession;

static IsolatedSession g_sessions[MAX_ISOLATED_SESSIONS];
static unsigned int g_next_session_id = 1;
static struct sigaction g_previous_sigpipe;
static int g_sigpipe_handler_installed = 0;

static const char *json_string(const cJSON *obj, const char *key,
                               const char *fallback)
{
	const cJSON *item = obj ? cJSON_GetObjectItem(obj, key) : NULL;
	return item && cJSON_IsString(item) ? item->valuestring : fallback;
}

static IsolatedSession *find_session(const char *session_id)
{
	if (!session_id) return NULL;
	for (int i = 0; i < MAX_ISOLATED_SESSIONS; i++) {
		if (g_sessions[i].used && strcmp(g_sessions[i].id, session_id) == 0)
			return &g_sessions[i];
	}
	return NULL;
}

/* 0=running, 1=exited but unreaped, -1=no longer an owned child. */
static int child_state(pid_t pid)
{
	siginfo_t info = { 0 };
	for (;;) {
		if (waitid(P_PID, (id_t)pid, &info,
		           WEXITED | WNOHANG | WNOWAIT) == 0)
			return info.si_pid == pid ? 1 : 0;
		if (errno == EINTR) continue;
		return -1;
	}
}

static int wait_child_exit_without_reaping(pid_t pid, int attempts)
{
	for (int i = 0; i < attempts; i++) {
		int state = child_state(pid);
		if (state != 0) return state;
		usleep(50000);
	}
	return 0;
}

static void reap_child(pid_t pid, int attempts)
{
	int status = 0;
	for (int i = 0; i < attempts; i++) {
		pid_t waited = waitpid(pid, &status, WNOHANG);
		if (waited == pid || (waited < 0 && errno == ECHILD)) return;
		if (waited < 0 && errno != EINTR) return;
		usleep(50000);
	}
}

static void stop_session(IsolatedSession *session)
{
	if (!session || !session->used) return;

	if (session->to_child) {
		fclose(session->to_child);
		session->to_child = NULL;
	}
	if (session->from_child_fd >= 0) {
		close(session->from_child_fd);
		session->from_child_fd = -1;
	}

	if (session->pid > 0) {
		pid_t process_group = session->pid;
		if (child_state(session->pid) >= 0) {
			kill(-process_group, SIGTERM);
			wait_child_exit_without_reaping(session->pid, 20);
			/* The unreaped leader keeps its PID/PGID reserved until after the
			 * final group signal, so PID reuse cannot redirect this SIGKILL. */
			kill(-process_group, SIGKILL);
			reap_child(session->pid, 20);
		}
	}

	memset(session, 0, sizeof(*session));
	session->from_child_fd = -1;
}

static long long monotonic_ms(void)
{
	struct timespec now;
	clock_gettime(CLOCK_MONOTONIC, &now);
	return (long long)now.tv_sec * 1000 + now.tv_nsec / 1000000;
}

static int response_timeout_ms(const char *tool_name, const cJSON *arguments)
{
	long long timeout = DEFAULT_RESPONSE_TIMEOUT_MS;
	const cJSON *seconds = arguments
		? cJSON_GetObjectItem(arguments, "timeout") : NULL;
	if ((strcmp(tool_name, "launch_app") == 0 ||
	     strcmp(tool_name, "wait_for_window") == 0) &&
	    seconds && cJSON_IsNumber(seconds) && seconds->valueint > 0) {
		long long requested = (long long)seconds->valueint * 1000 +
			RESPONSE_TIMEOUT_GRACE_MS;
		if (requested > timeout) timeout = requested;
	}

	const cJSON *milliseconds = arguments
		? cJSON_GetObjectItem(arguments, "timeoutMs") : NULL;
	if (strcmp(tool_name, "exec") == 0 &&
	    milliseconds && cJSON_IsNumber(milliseconds) &&
	    milliseconds->valueint > 0) {
		long long tool_timeout = milliseconds->valueint;
		if (tool_timeout > 60000) tool_timeout = 60000;
		long long requested = tool_timeout + RESPONSE_TIMEOUT_GRACE_MS;
		if (requested > timeout) timeout = requested;
	}

	if (strcmp(tool_name, "type_text") == 0) {
		const cJSON *text = arguments
			? cJSON_GetObjectItem(arguments, "text") : NULL;
		const cJSON *delay = arguments
			? cJSON_GetObjectItem(arguments, "delay") : NULL;
		long long delay_ms = delay && cJSON_IsNumber(delay)
			? delay->valueint : 12;
		if (delay_ms > 0 && text && cJSON_IsString(text)) {
			long long text_length = (long long)strlen(text->valuestring);
			long long duration = delay_ms > INT_MAX / (text_length + 1)
				? INT_MAX : delay_ms * text_length;
			long long requested = duration + RESPONSE_TIMEOUT_GRACE_MS;
			if (requested > timeout) timeout = requested;
		}
	}

	if (strcmp(tool_name, "hover_text") == 0) {
		const cJSON *settle = arguments
			? cJSON_GetObjectItem(arguments, "settleMs") : NULL;
		long long settle_ms = settle && cJSON_IsNumber(settle)
			? settle->valueint : 800;
		if (settle_ms > 0) {
			long long requested = DEFAULT_RESPONSE_TIMEOUT_MS + settle_ms;
			if (requested > timeout) timeout = requested;
		}
	}

	return timeout > INT_MAX ? INT_MAX : (int)timeout;
}

static char *read_child_response(IsolatedSession *session,
	                             int timeout_ms,
	                             char *error, size_t error_len)
{
	size_t length = 0;
	size_t capacity = 4096;
	char *line = malloc(capacity);
	if (!line) {
		snprintf(error, error_len, "out of memory reading isolated response");
		session->broken = 1;
		return NULL;
	}

	long long deadline = monotonic_ms() + timeout_ms;
	for (;;) {
		long long remaining = deadline - monotonic_ms();
		if (remaining <= 0) {
			snprintf(error, error_len, "isolated session response timed out");
			break;
		}

		struct pollfd poll_fd = {
			.fd = session->from_child_fd,
			.events = POLLIN
		};
		int poll_result = poll(&poll_fd, 1, (int)remaining);
		if (poll_result < 0 && errno == EINTR) continue;
		if (poll_result <= 0) {
			snprintf(error, error_len, poll_result == 0
				? "isolated session response timed out"
				: "isolated session response failed: %s",
				poll_result == 0 ? "" : strerror(errno));
			break;
		}
		if (poll_fd.revents & (POLLERR | POLLNVAL)) {
			snprintf(error, error_len, "isolated session output failed");
			break;
		}

		char chunk[8192];
		ssize_t count = read(session->from_child_fd, chunk, sizeof(chunk));
		if (count < 0 && errno == EINTR) continue;
		if (count <= 0) {
			snprintf(error, error_len, "isolated session closed its output");
			break;
		}

		char *newline = memchr(chunk, '\n', (size_t)count);
		size_t append = newline ? (size_t)(newline - chunk) : (size_t)count;
		if (length + append + 1 > MAX_SESSION_RESPONSE_BYTES) {
			snprintf(error, error_len, "isolated session response is too large");
			break;
		}
		if (length + append + 1 > capacity) {
			size_t new_capacity = capacity;
			while (new_capacity < length + append + 1)
				new_capacity *= 2;
			char *resized = realloc(line, new_capacity);
			if (!resized) {
				snprintf(error, error_len,
					"out of memory reading isolated response");
				break;
			}
			line = resized;
			capacity = new_capacity;
		}
		memcpy(line + length, chunk, append);
		length += append;
		if (newline) {
			if (append + 1 != (size_t)count) {
				snprintf(error, error_len,
					"isolated session returned unexpected extra output");
				break;
			}
			line[length] = '\0';
			return line;
		}
	}

	free(line);
	session->broken = 1;
	return NULL;
}

static int send_child_notification(IsolatedSession *session, const char *method)
{
	cJSON *notification = cJSON_CreateObject();
	cJSON_AddStringToObject(notification, "jsonrpc", "2.0");
	cJSON_AddStringToObject(notification, "method", method);
	cJSON_AddItemToObject(notification, "params", cJSON_CreateObject());
	char *json = cJSON_PrintUnformatted(notification);
	cJSON_Delete(notification);
	if (!json) return -1;
	int failed = fprintf(session->to_child, "%s\n", json) < 0 ||
		fflush(session->to_child) != 0;
	free(json);
	if (failed) session->broken = 1;
	return failed ? -1 : 0;
}

static cJSON *child_rpc(IsolatedSession *session, const char *method,
	                    const cJSON *params, int timeout_ms,
	                    char *error, size_t error_len)
{
	if (!session || !session->to_child || session->from_child_fd < 0 ||
	    session->broken) {
		snprintf(error, error_len, "isolated session is not running");
		return NULL;
	}

	if (child_state(session->pid) != 0) {
		snprintf(error, error_len, "isolated session exited unexpectedly");
		session->broken = 1;
		return NULL;
	}

	cJSON *request = cJSON_CreateObject();
	cJSON_AddStringToObject(request, "jsonrpc", "2.0");
	unsigned long request_id = ++session->next_request_id;
	cJSON_AddNumberToObject(request, "id", request_id);
	cJSON_AddStringToObject(request, "method", method);
	if (params)
		cJSON_AddItemToObject(request, "params", cJSON_Duplicate(params, 1));

	char *request_json = cJSON_PrintUnformatted(request);
	cJSON_Delete(request);
	if (!request_json) {
		snprintf(error, error_len, "could not encode isolated request");
		return NULL;
	}

	int write_failed = fprintf(session->to_child, "%s\n", request_json) < 0 ||
		fflush(session->to_child) != 0;
	free(request_json);
	if (write_failed) {
		snprintf(error, error_len, "could not write to isolated session");
		session->broken = 1;
		return NULL;
	}

	char *line = read_child_response(session, timeout_ms, error, error_len);
	if (!line) return NULL;
	cJSON *response = cJSON_Parse(line);
	free(line);
	if (!response) {
		snprintf(error, error_len, "isolated session returned invalid JSON");
		session->broken = 1;
		return NULL;
	}
	const cJSON *response_id = cJSON_GetObjectItem(response, "id");
	if (!response_id || !cJSON_IsNumber(response_id) ||
	    (unsigned long)response_id->valuedouble != request_id) {
		snprintf(error, error_len, "isolated session response ID mismatch");
		session->broken = 1;
		cJSON_Delete(response);
		return NULL;
	}

	const cJSON *rpc_error = cJSON_GetObjectItem(response, "error");
	if (rpc_error) {
		const cJSON *message = cJSON_GetObjectItem(rpc_error, "message");
		snprintf(error, error_len, "%s",
			message && cJSON_IsString(message)
			? message->valuestring : "isolated tool call failed");
		cJSON_Delete(response);
		return NULL;
	}

	const cJSON *result = cJSON_GetObjectItem(response, "result");
	cJSON *copy = result ? cJSON_Duplicate(result, 1) : NULL;
	cJSON_Delete(response);
	if (!copy) {
		snprintf(error, error_len, "isolated session returned no result");
		session->broken = 1;
	}
	return copy;
}

static IsolatedSession *start_session(const char *screen_size,
	                                  char *error, size_t error_len)
{
	IsolatedSession *session = NULL;
	for (int i = 0; i < MAX_ISOLATED_SESSIONS; i++) {
		if (!g_sessions[i].used) {
			session = &g_sessions[i];
			break;
		}
	}
	if (!session) {
		snprintf(error, error_len, "too many isolated sessions (maximum %d)",
			MAX_ISOLATED_SESSIONS);
		return NULL;
	}

	/* Keep the executable reachable even if Ninja atomically replaces the
	 * on-disk binary while this MCP server is running. The parent remains
	 * alive for the entire session startup, so its procfs executable link is
	 * stable across the intermediate xvfb-run exec chain. */
	char executable[64];
	snprintf(executable, sizeof(executable), "/proc/%ld/exe", (long)getpid());

	int input_pipe[2] = { -1, -1 };
	int output_pipe[2] = { -1, -1 };
	if (pipe2(input_pipe, O_CLOEXEC) != 0) {
		snprintf(error, error_len, "could not create isolated session pipes: %s",
			strerror(errno));
		return NULL;
	}
	if (pipe2(output_pipe, O_CLOEXEC) != 0) {
		close(input_pipe[0]);
		close(input_pipe[1]);
		snprintf(error, error_len, "could not create isolated session pipes: %s",
			strerror(errno));
		return NULL;
	}

	pid_t pid = fork();
	if (pid < 0) {
		close(input_pipe[0]);
		close(input_pipe[1]);
		close(output_pipe[0]);
		close(output_pipe[1]);
		snprintf(error, error_len, "could not fork isolated session: %s",
			strerror(errno));
		return NULL;
	}

	if (pid == 0) {
		if (setpgid(0, 0) != 0 ||
		    dup2(input_pipe[0], STDIN_FILENO) < 0 ||
		    dup2(output_pipe[1], STDOUT_FILENO) < 0) {
			_exit(126);
		}
		close(input_pipe[0]);
		close(input_pipe[1]);
		close(output_pipe[0]);
		close(output_pipe[1]);
		char control_error[256];
		if (control_export_to_fd(3, control_error,
		    sizeof(control_error)) != 0) {
			fprintf(stderr, "deskpal: %s\n", control_error);
			_exit(126);
		}
		int closed_all = 0;
	#ifdef SYS_close_range
		closed_all = syscall(SYS_close_range, 4U, ~0U, 0) == 0;
	#endif
		if (!closed_all) {
			long max_fd = sysconf(_SC_OPEN_MAX);
			if (max_fd < 0) max_fd = 1024;
			for (int fd = 4; fd < max_fd; fd++) close(fd);
		}
		unsetenv("DESKPAL_HEADLESS_ACTIVE");

		char *child_argv[12];
		int arg = 0;
		child_argv[arg++] = executable;
		child_argv[arg++] = "--xvfb-child";
		child_argv[arg++] = "--screen-size";
		child_argv[arg++] = (char *)screen_size;
		child_argv[arg++] = "--control-lock-fd";
		child_argv[arg++] = "3";
		if (deskpal_allow_fs) child_argv[arg++] = "--allow-fs";
		if (deskpal_allow_exec) child_argv[arg++] = "--allow-exec";
		child_argv[arg] = NULL;
		execv(executable, child_argv);
		fprintf(stderr, "deskpal: could not exec isolated child: %s\n",
			strerror(errno));
		_exit(127);
	}

	setpgid(pid, pid);
	close(input_pipe[0]);
	close(output_pipe[1]);
	memset(session, 0, sizeof(*session));
	session->used = 1;
	session->pid = pid;
	session->from_child_fd = output_pipe[0];
	snprintf(session->id, sizeof(session->id), "xvfb-%u", g_next_session_id++);
	session->to_child = fdopen(input_pipe[1], "w");
	if (!session->to_child) {
		close(input_pipe[1]);
		snprintf(error, error_len, "could not open isolated session streams");
		stop_session(session);
		return NULL;
	}
	setvbuf(session->to_child, NULL, _IOLBF, 0);

	cJSON *initialize_params = cJSON_CreateObject();
	cJSON_AddStringToObject(initialize_params, "protocolVersion", "2024-11-05");
	cJSON_AddItemToObject(initialize_params, "capabilities", cJSON_CreateObject());
	cJSON *client_info = cJSON_CreateObject();
	cJSON_AddStringToObject(client_info, "name", "deskpal-session-proxy");
	cJSON_AddStringToObject(client_info, "version", "1.0");
	cJSON_AddItemToObject(initialize_params, "clientInfo", client_info);
	cJSON *initialize_result = child_rpc(session, "initialize", initialize_params,
		                                  DEFAULT_RESPONSE_TIMEOUT_MS,
		                                  error, error_len);
	cJSON_Delete(initialize_params);
	if (!initialize_result) {
		stop_session(session);
		return NULL;
	}
	cJSON_Delete(initialize_result);
	if (send_child_notification(session, "notifications/initialized") != 0) {
		snprintf(error, error_len,
			"could not complete isolated session initialization");
		stop_session(session);
		return NULL;
	}
	return session;
}

static void session_sigpipe_handler(int signal_number)
{
	(void)signal_number;
}

void sessions_init(void)
{
	struct sigaction action = { 0 };
	action.sa_handler = session_sigpipe_handler;
	sigemptyset(&action.sa_mask);
	if (sigaction(SIGPIPE, &action, &g_previous_sigpipe) == 0)
		g_sigpipe_handler_installed = 1;
}

int sessions_active_count(void)
{
	int count = 0;
	for (int i = 0; i < MAX_ISOLATED_SESSIONS; i++)
		if (g_sessions[i].used) count++;
	return count;
}

void sessions_cleanup_all(void)
{
	for (int i = 0; i < MAX_ISOLATED_SESSIONS; i++)
		stop_session(&g_sessions[i]);
	if (g_sigpipe_handler_installed) {
		sigaction(SIGPIPE, &g_previous_sigpipe, NULL);
		g_sigpipe_handler_installed = 0;
	}
}

cJSON *sessions_forward_tool(const char *session_id, const char *tool_name,
	                         const cJSON *arguments)
{
	IsolatedSession *session = find_session(session_id);
	if (!session) {
		char message[128];
		snprintf(message, sizeof(message),
			"Unknown or closed isolated session: %s", session_id);
		return mcp_tool_error_result(message);
	}

	cJSON *forwarded_arguments = arguments
		? cJSON_Duplicate(arguments, 1) : cJSON_CreateObject();
	cJSON_DeleteItemFromObject(forwarded_arguments, "sessionId");
	cJSON *params = cJSON_CreateObject();
	cJSON_AddStringToObject(params, "name", tool_name);
	cJSON_AddItemToObject(params, "arguments", forwarded_arguments);

	char error[256];
	cJSON *result = child_rpc(session, "tools/call", params,
		                       response_timeout_ms(tool_name, arguments),
		                       error, sizeof(error));
	cJSON_Delete(params);
	if (!result) {
		char message[320];
		snprintf(message, sizeof(message), "Isolated session %s failed: %s",
			session_id, error);
		if (session->broken) stop_session(session);
		return mcp_tool_error_result(message);
	}
	return result;
}

cJSON *tool_launch_isolated_app(const cJSON *params)
{
	if (getenv("DESKPAL_HEADLESS_ACTIVE"))
		return mcp_tool_error_result("Cannot create a nested isolated session");

	const char *screen_size = json_string(params, "screenSize", "1920x1080");
	char error[256];
	IsolatedSession *session = start_session(screen_size, error, sizeof(error));
	if (!session) {
		char message[320];
		snprintf(message, sizeof(message), "Could not start isolated session: %s",
			error);
		return mcp_tool_error_result(message);
	}

	cJSON *arguments = params ? cJSON_Duplicate(params, 1) : cJSON_CreateObject();
	cJSON_DeleteItemFromObject(arguments, "screenSize");
	cJSON *call_params = cJSON_CreateObject();
	cJSON_AddStringToObject(call_params, "name", "launch_app");
	cJSON_AddItemToObject(call_params, "arguments", arguments);
	cJSON *result = child_rpc(session, "tools/call", call_params,
		                       response_timeout_ms("launch_app", arguments),
		                       error, sizeof(error));
	cJSON_Delete(call_params);
	if (!result) {
		char message[320];
		snprintf(message, sizeof(message),
			"Could not launch app in isolated session: %s", error);
		stop_session(session);
		return mcp_tool_error_result(message);
	}
	const cJSON *launch_error = cJSON_GetObjectItem(result, "isError");
	if (launch_error && cJSON_IsTrue(launch_error)) {
		stop_session(session);
		return result;
	}

	cJSON_AddStringToObject(result, "sessionId", session->id);
	cJSON *content = cJSON_GetObjectItem(result, "content");
	cJSON *first = content && cJSON_IsArray(content)
		? cJSON_GetArrayItem(content, 0) : NULL;
	cJSON *text = first ? cJSON_GetObjectItem(first, "text") : NULL;
	if (text && cJSON_IsString(text)) {
		const char *original = text->valuestring;
		size_t needed = strlen(original) + strlen(session->id) * 2 + 128;
		char *annotated = malloc(needed);
		if (annotated) {
			snprintf(annotated, needed,
				"Isolated session: %s\nPass sessionId=\"%s\" to every subsequent UI tool for this app.\n\n%s",
				session->id, session->id, original);
			cJSON_ReplaceItemInObject(first, "text", cJSON_CreateString(annotated));
			free(annotated);
		}
	}
	return result;
}

cJSON *tool_close_isolated_session(const cJSON *params)
{
	const char *session_id = json_string(params, "sessionId", NULL);
	IsolatedSession *session = find_session(session_id);
	if (!session) {
		char message[128];
		snprintf(message, sizeof(message),
			"Unknown or closed isolated session: %s",
			session_id ? session_id : "(missing)");
		return mcp_tool_error_result(message);
	}

	char closed_id[sizeof(session->id)];
	snprintf(closed_id, sizeof(closed_id), "%s", session->id);
	stop_session(session);
	char message[96];
	snprintf(message, sizeof(message), "Closed isolated session %s", closed_id);
	return mcp_text_result(message);
}