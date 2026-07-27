/*
 * deskpal — Exact bounded AT-SPI window identity shared by observation routes
 * SPDX-License-Identifier: MIT
 */
#ifndef DESKPAL_SEMANTIC_IDENTITY_H
#define DESKPAL_SEMANTIC_IDENTITY_H

#define DESKPAL_SEMANTIC_BUS_NAME_LEN 256
#define DESKPAL_SEMANTIC_OBJECT_PATH_LEN 1025

typedef struct {
	unsigned int process_id;
	char bus_name[DESKPAL_SEMANTIC_BUS_NAME_LEN];
	char window_object_path[DESKPAL_SEMANTIC_OBJECT_PATH_LEN];
} SemanticWindowIdentity;

#endif /* DESKPAL_SEMANTIC_IDENTITY_H */
