/*
 * deskpal — Privacy-safe canonical semantic snapshots and bounded diffs
 * SPDX-License-Identifier: MIT
 */
#ifndef DESKPAL_SEMANTIC_STATE_H
#define DESKPAL_SEMANTIC_STATE_H

#include "cJSON.h"
#include "captures.h"

typedef struct {
	char *json;
	char revision[DESKPAL_SEMANTIC_REVISION_LEN];
	int complete;
} SemanticStateSnapshot;

int semantic_state_build(const cJSON *semantic, SemanticStateSnapshot *snapshot);
void semantic_state_clear(SemanticStateSnapshot *snapshot);
cJSON *semantic_state_diff(const DeskpalCapture *base,
                           const SemanticStateSnapshot *current);

#endif /* DESKPAL_SEMANTIC_STATE_H */
