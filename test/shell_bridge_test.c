#include "shell_bridge.h"

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void expect_valid(const char *method, const char *json)
{
	char error[256] = {0};
	cJSON *response = NULL;
	assert(shell_bridge_parse_response(method, json, &response,
	                                   error, sizeof(error)) == 0);
	assert(response != NULL);
	cJSON_Delete(response);
}

static void expect_invalid(const char *method, const char *json)
{
	char error[256] = {0};
	cJSON *response = (cJSON *)1;
	assert(shell_bridge_parse_response(method, json, &response,
	                                   error, sizeof(error)) == -1);
	assert(response == NULL);
	assert(error[0] != '\0');
}

int main(void)
{
	expect_valid("GetCapabilities",
		"{\"protocolVersion\":1,\"backend\":\"gnome-shell-extension\","
		"\"shellInstanceId\":\"instance-1\","
		"\"coordinateSpace\":\"gnome-stage-logical\","
		"\"capabilities\":{\"windowEnumeration\":true,"
		"\"monitorLayout\":true,\"windowCapture\":false,"
		"\"foregroundWindowManagement\":false,\"surfaceInput\":false,"
		"\"backgroundInput\":false},"
		"\"limits\":{\"maxWindows\":256,\"maxStringCharacters\":512}}" );

	expect_valid("ListWindows",
		"{\"protocolVersion\":1,\"shellInstanceId\":\"instance-1\","
		"\"complete\":true,\"windows\":[{"
		"\"surfaceId\":\"gnome-window-1\",\"generation\":1,"
		"\"geometryRevision\":2,\"title\":\"Editor\","
		"\"appId\":\"editor.desktop\",\"wmClass\":null,\"pid\":42,"
		"\"bounds\":{\"x\":10,\"y\":20,\"width\":800,\"height\":600},"
		"\"workspace\":0,\"monitor\":0,\"scale\":1,"
		"\"focused\":true,\"hidden\":false,\"clientType\":\"wayland\","
		"\"backend\":\"gnome-shell-extension\"}]}" );

	expect_valid("GetMonitorLayout",
		"{\"protocolVersion\":1,\"shellInstanceId\":\"instance-1\","
		"\"coordinateSpace\":\"gnome-stage-logical\","
		"\"stageWidth\":1920,\"stageHeight\":1080,\"primaryIndex\":0,"
		"\"complete\":true,\"monitors\":[{\"index\":0,\"x\":0,\"y\":0,"
		"\"width\":1920,\"height\":1080,\"scale\":1,\"primary\":true}]}" );

	expect_invalid("GetCapabilities",
		"{\"protocolVersion\":2,\"shellInstanceId\":\"instance-1\"}");
	expect_invalid("ListWindows",
		"{\"protocolVersion\":1,\"shellInstanceId\":\"instance-1\","
		"\"complete\":true,\"windows\":[{\"surfaceId\":\"bad\"}]}" );
	expect_invalid("ListWindows",
		"{\"protocolVersion\":1,\"shellInstanceId\":\"bad:instance\","
		"\"complete\":true,\"windows\":[]}" );
	expect_invalid("UnknownMethod",
		"{\"protocolVersion\":1,\"shellInstanceId\":\"instance-1\"}");

	char *oversized = malloc(DESKPAL_SHELL_BRIDGE_RESPONSE_LIMIT + 2);
	assert(oversized != NULL);
	memset(oversized, 'x', DESKPAL_SHELL_BRIDGE_RESPONSE_LIMIT + 1);
	oversized[DESKPAL_SHELL_BRIDGE_RESPONSE_LIMIT + 1] = '\0';
	expect_invalid("ListWindows", oversized);
	free(oversized);

	puts("PASS: Shell bridge responses are versioned, bounded, and fail closed");
	return 0;
}
