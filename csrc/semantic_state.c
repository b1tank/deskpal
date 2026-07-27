/*
 * deskpal — Privacy-safe canonical semantic snapshots and bounded diffs
 * SPDX-License-Identifier: MIT
 */
#include "semantic_state.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define SEMANTIC_SNAPSHOT_LIMIT (256 * 1024)
#define SEMANTIC_DIFF_ITEM_LIMIT 64
#define SEMANTIC_RECORD_LIMIT 300

typedef struct {
	cJSON *record;
	const char *key;
} SemanticRecord;

static cJSON *canonical_string_array(const cJSON *source)
{
	cJSON *result = cJSON_CreateArray();
	if (!cJSON_IsArray(source)) return result;
	int count = cJSON_GetArraySize(source);
	char **values = calloc((size_t)count, sizeof(*values));
	if (!values) return result;
	int used = 0;
	for (int i = 0; i < count; i++) {
		const cJSON *item = cJSON_GetArrayItem(source, i);
		if (cJSON_IsString(item)) values[used++] = item->valuestring;
	}
	for (int i = 0; i < used; i++)
		for (int j = i + 1; j < used; j++)
			if (strcmp(values[i], values[j]) > 0) {
				char *swap = values[i]; values[i] = values[j]; values[j] = swap;
			}
	for (int i = 0; i < used; i++)
		cJSON_AddItemToArray(result, cJSON_CreateString(values[i]));
	free(values);
	return result;
}

static cJSON *projection_record(const cJSON *node)
{
	const cJSON *locator = cJSON_GetObjectItem(node, "locator");
	const cJSON *role = cJSON_GetObjectItem(node, "role");
	const cJSON *path = cJSON_GetObjectItem(node, "path");
	const cJSON *bus = cJSON_GetObjectItem(locator, "busName");
	const cJSON *object = cJSON_GetObjectItem(locator, "objectPath");
	const cJSON *pid = cJSON_GetObjectItem(locator, "processId");
	if (!cJSON_IsString(role) || !cJSON_IsArray(path) ||
	    !cJSON_IsString(bus) || !cJSON_IsString(object) || !cJSON_IsNumber(pid))
		return NULL;
	char *path_text = cJSON_PrintUnformatted(path);
	if (!path_text) return NULL;
	size_t key_len = strlen(bus->valuestring) + strlen(object->valuestring) +
		strlen(role->valuestring) + strlen(path_text) + 64;
	char *key = malloc(key_len);
	if (!key) { free(path_text); return NULL; }
	snprintf(key, key_len, "%s|%s|%.0f|%s|%s", bus->valuestring,
	         object->valuestring, pid->valuedouble, role->valuestring, path_text);
	free(path_text);
	cJSON *record = cJSON_CreateObject();
	cJSON_AddStringToObject(record, "key", key);
	free(key);
	cJSON_AddStringToObject(record, "role", role->valuestring);
	cJSON_AddItemToObject(record, "path", cJSON_Duplicate(path, 1));
	const char *fields[] = { "states", "bounds", "value", "selection" };
	for (size_t i = 0; i < sizeof(fields) / sizeof(fields[0]); i++) {
		const cJSON *field = cJSON_GetObjectItem(node, fields[i]);
		if (field) cJSON_AddItemToObject(record, fields[i], cJSON_Duplicate(field, 1));
	}
	cJSON_AddItemToObject(record, "actions",
	                     canonical_string_array(cJSON_GetObjectItem(node, "actions")));
	return record;
}

static void collect_records(const cJSON *nodes, SemanticRecord *records,
                            int *count, int *truncated)
{
	const cJSON *node = NULL;
	cJSON_ArrayForEach(node, nodes) {
		cJSON *record = projection_record(node);
		if (record) {
			if (*count < SEMANTIC_RECORD_LIMIT) {
				records[*count].record = record;
				records[*count].key = cJSON_GetObjectItem(record, "key")->valuestring;
				(*count)++;
			} else {
				*truncated = 1;
				cJSON_Delete(record);
			}
		}
		collect_records(cJSON_GetObjectItem(node, "children"), records,
		                count, truncated);
	}
}

static int compare_records(const void *left, const void *right)
{
	return strcmp(((const SemanticRecord *)left)->key,
	              ((const SemanticRecord *)right)->key);
}

static void capture_window_identity(const cJSON *semantic,
                                    SemanticStateSnapshot *snapshot)
{
	const cJSON *applications = cJSON_GetObjectItem(semantic, "applications");
	const cJSON *application = cJSON_IsArray(applications) &&
		cJSON_GetArraySize(applications) == 1
		? cJSON_GetArrayItem(applications, 0) : NULL;
	const cJSON *windows = application
		? cJSON_GetObjectItem(application, "windows") : NULL;
	const cJSON *window = cJSON_IsArray(windows) &&
		cJSON_GetArraySize(windows) == 1 ? cJSON_GetArrayItem(windows, 0) : NULL;
	const cJSON *nodes = window ? cJSON_GetObjectItem(window, "nodes") : NULL;
	const cJSON *root = cJSON_IsArray(nodes) && cJSON_GetArraySize(nodes) > 0
		? cJSON_GetArrayItem(nodes, 0) : NULL;
	const cJSON *path = root ? cJSON_GetObjectItem(root, "path") : NULL;
	const cJSON *locator = root ? cJSON_GetObjectItem(root, "locator") : NULL;
	const cJSON *bus = locator ? cJSON_GetObjectItem(locator, "busName") : NULL;
	const cJSON *object = locator
		? cJSON_GetObjectItem(locator, "objectPath") : NULL;
	const cJSON *pid = locator ? cJSON_GetObjectItem(locator, "processId") : NULL;
	if (!cJSON_IsArray(path) || cJSON_GetArraySize(path) != 0 ||
	    !cJSON_IsString(bus) || !cJSON_IsString(object) ||
	    !cJSON_IsNumber(pid) || pid->valuedouble < 1 ||
	    pid->valuedouble > UINT32_MAX || pid->valuedouble != pid->valueint ||
	    strlen(bus->valuestring) >=
	        sizeof(snapshot->window_identity.bus_name) ||
	    strlen(object->valuestring) >=
	        sizeof(snapshot->window_identity.window_object_path))
		return;
	snprintf(snapshot->window_identity.bus_name,
	         sizeof(snapshot->window_identity.bus_name), "%s",
	         bus->valuestring);
	snprintf(snapshot->window_identity.window_object_path,
	         sizeof(snapshot->window_identity.window_object_path), "%s",
	         object->valuestring);
	snapshot->window_identity.process_id = (unsigned int)pid->valuedouble;
}

int semantic_state_build(const cJSON *semantic, SemanticStateSnapshot *snapshot)
{
	if (!semantic || !snapshot) return -1;
	memset(snapshot, 0, sizeof(*snapshot));
	capture_window_identity(semantic, snapshot);
	SemanticRecord records[SEMANTIC_RECORD_LIMIT] = {0};
	int count = 0;
	int truncated = cJSON_IsTrue(cJSON_GetObjectItem(semantic, "truncated")) ||
		cJSON_IsTrue(cJSON_GetObjectItem(semantic, "partial")) ||
		cJSON_IsTrue(cJSON_GetObjectItem(semantic, "incomplete"));
	const cJSON *application = NULL;
	cJSON_ArrayForEach(application, cJSON_GetObjectItem(semantic, "applications")) {
		const cJSON *window = NULL;
		cJSON_ArrayForEach(window, cJSON_GetObjectItem(application, "windows"))
			collect_records(cJSON_GetObjectItem(window, "nodes"), records,
			                &count, &truncated);
	}
	qsort(records, (size_t)count, sizeof(records[0]), compare_records);
	cJSON *array = cJSON_CreateArray();
	for (int i = 0; i < count; i++) cJSON_AddItemToArray(array, records[i].record);
	snapshot->json = cJSON_PrintUnformatted(array);
	cJSON_Delete(array);
	if (!snapshot->json || strlen(snapshot->json) > SEMANTIC_SNAPSHOT_LIMIT) {
		semantic_state_clear(snapshot);
		return -1;
	}
	uint64_t hash = UINT64_C(1469598103934665603);
	for (const unsigned char *p = (const unsigned char *)snapshot->json; *p; p++) {
		hash ^= *p;
		hash *= UINT64_C(1099511628211);
	}
	snprintf(snapshot->revision, sizeof(snapshot->revision),
	         "fnv1a64-%016llx", (unsigned long long)hash);
	snapshot->complete = !truncated;
	return 0;
}

void semantic_state_clear(SemanticStateSnapshot *snapshot)
{
	if (!snapshot) return;
	free(snapshot->json);
	memset(snapshot, 0, sizeof(*snapshot));
}

static const cJSON *record_by_key(const cJSON *records, const char *key)
{
	const cJSON *record = NULL;
	cJSON_ArrayForEach(record, records) {
		const cJSON *candidate = cJSON_GetObjectItem(record, "key");
		if (cJSON_IsString(candidate) && strcmp(candidate->valuestring, key) == 0)
			return record;
	}
	return NULL;
}

static int fields_equal(const cJSON *left, const cJSON *right)
{
	if (!left && !right) return 1;
	if (!left || !right) return 0;
	return cJSON_Compare(left, right, 1);
}

cJSON *semantic_state_diff(const DeskpalCapture *base,
                           const SemanticStateSnapshot *current)
{
	if (!base || !current) return NULL;
	cJSON *diff = cJSON_CreateObject();
	cJSON_AddStringToObject(diff, "baseCaptureId", base->id);
	cJSON_AddBoolToObject(diff, "sameTarget", 1);
	cJSON_AddBoolToObject(diff, "baseProjectionComplete", base->semantic_complete);
	cJSON_AddBoolToObject(diff, "currentProjectionComplete", current->complete);
	if (!base->semantic_complete || !current->complete) {
		cJSON_AddBoolToObject(diff, "comparable", 0);
		cJSON_AddStringToObject(diff, "reason",
		                      "base_or_current_projection_incomplete");
		cJSON_AddBoolToObject(diff, "changed", 0);
		cJSON_AddBoolToObject(diff, "truncated", 1);
		return diff;
	}
	cJSON_AddBoolToObject(diff, "comparable", 1);
	cJSON *added = cJSON_CreateArray();
	cJSON *removed = cJSON_CreateArray();
	cJSON *updated = cJSON_CreateArray();
	cJSON_AddItemToObject(diff, "added", added);
	cJSON_AddItemToObject(diff, "removed", removed);
	cJSON_AddItemToObject(diff, "updated", updated);
	int truncated = 0;
	cJSON *base_records = cJSON_Parse(base->semantic_snapshot);
	cJSON *current_records = cJSON_Parse(current->json);
	if (!cJSON_IsArray(base_records) || !cJSON_IsArray(current_records)) {
		cJSON_Delete(base_records); cJSON_Delete(current_records);
		cJSON_AddBoolToObject(diff, "baseAvailable", 0);
		cJSON_AddBoolToObject(diff, "changed", 0);
		cJSON_AddBoolToObject(diff, "truncated", 1);
		return diff;
	}
	cJSON_AddBoolToObject(diff, "baseAvailable", 1);
	const char *fields[] = { "states", "bounds", "actions", "value", "selection" };
	const cJSON *record = NULL;
	cJSON_ArrayForEach(record, current_records) {
		const char *key = cJSON_GetObjectItem(record, "key")->valuestring;
		const cJSON *old = record_by_key(base_records, key);
		if (!old) {
			if (cJSON_GetArraySize(added) < SEMANTIC_DIFF_ITEM_LIMIT)
				cJSON_AddItemToArray(added, cJSON_Duplicate(record, 1));
			else truncated = 1;
			continue;
		}
		cJSON *changed = cJSON_CreateArray();
		for (size_t i = 0; i < sizeof(fields) / sizeof(fields[0]); i++)
			if (!fields_equal(cJSON_GetObjectItem(old, fields[i]),
			                  cJSON_GetObjectItem(record, fields[i])))
				cJSON_AddItemToArray(changed, cJSON_CreateString(fields[i]));
		if (cJSON_GetArraySize(changed)) {
			if (cJSON_GetArraySize(updated) < SEMANTIC_DIFF_ITEM_LIMIT) {
				cJSON *item = cJSON_CreateObject();
				cJSON_AddStringToObject(item, "key", key);
				cJSON_AddStringToObject(item, "role",
				                      cJSON_GetObjectItem(record, "role")->valuestring);
				cJSON_AddItemToObject(item, "path",
				                     cJSON_Duplicate(cJSON_GetObjectItem(record, "path"), 1));
				cJSON_AddItemToObject(item, "changedFields", changed);
				cJSON_AddItemToArray(updated, item);
			} else { truncated = 1; cJSON_Delete(changed); }
		} else cJSON_Delete(changed);
	}
	cJSON_ArrayForEach(record, base_records) {
		const char *key = cJSON_GetObjectItem(record, "key")->valuestring;
		if (!record_by_key(current_records, key)) {
			if (cJSON_GetArraySize(removed) < SEMANTIC_DIFF_ITEM_LIMIT)
				cJSON_AddItemToArray(removed, cJSON_Duplicate(record, 1));
			else truncated = 1;
		}
	}
	cJSON_AddBoolToObject(diff, "changed",
	                     cJSON_GetArraySize(added) || cJSON_GetArraySize(removed) ||
	                     cJSON_GetArraySize(updated));
	cJSON_AddBoolToObject(diff, "truncated", truncated);
	cJSON_Delete(base_records); cJSON_Delete(current_records);
	return diff;
}
