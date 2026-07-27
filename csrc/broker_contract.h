/*
 * deskpal — Backend-neutral trusted desktop broker contract
 * SPDX-License-Identifier: MIT
 */
#ifndef DESKPAL_BROKER_CONTRACT_H
#define DESKPAL_BROKER_CONTRACT_H

#include <stddef.h>
#include <stdint.h>

#define DESKPAL_BROKER_ID_LEN 64
#define DESKPAL_BROKER_OPERATION_ID_LEN 64
#define DESKPAL_BROKER_OWNER_LEN 128

typedef enum {
	BROKER_CAP_OBSERVE = 1u << 0,
	BROKER_CAP_CAPTURE = 1u << 1,
	BROKER_CAP_POINTER = 1u << 2,
	BROKER_CAP_KEYBOARD = 1u << 3,
	BROKER_CAP_SCROLL = 1u << 4,
	BROKER_CAP_DRAG = 1u << 5,
	BROKER_CAP_BACKGROUND = 1u << 6,
	BROKER_CAP_CANCELLATION = 1u << 7,
	BROKER_CAP_COMPLETION = 1u << 8,
} BrokerCapability;

typedef struct {
	char broker_instance_id[DESKPAL_BROKER_ID_LEN];
	char surface_id[DESKPAL_BROKER_ID_LEN];
	uint64_t generation;
	uint64_t geometry_revision;
	uint32_t capabilities;
	int protected_surface;
} BrokerSurfaceIdentity;

typedef enum {
	BROKER_OPERATION_ACCEPTED = 1,
	BROKER_OPERATION_DISPATCHING,
	BROKER_OPERATION_DISPATCHED,
	BROKER_OPERATION_COMPLETED,
	BROKER_OPERATION_CANCELLED_BEFORE_DISPATCH,
	BROKER_OPERATION_OUTCOME_UNKNOWN,
	BROKER_OPERATION_FAILED_BEFORE_DISPATCH,
	BROKER_OPERATION_REVOKED,
} BrokerOperationState;

typedef struct {
	char operation_id[DESKPAL_BROKER_OPERATION_ID_LEN];
	char owner[DESKPAL_BROKER_OWNER_LEN];
	BrokerSurfaceIdentity target;
	BrokerOperationState state;
	int dispatch_attempted;
	int delivery_accepted;
} BrokerOperation;

typedef enum {
	BROKER_ERROR_NONE = 0,
	BROKER_ERROR_BROKER_UNAVAILABLE,
	BROKER_ERROR_CAPABILITY_UNAVAILABLE,
	BROKER_ERROR_BACKGROUND_UNAVAILABLE,
	BROKER_ERROR_PERMISSION_REQUIRED,
	BROKER_ERROR_PERMISSION_DENIED,
	BROKER_ERROR_GRANT_EXPIRED,
	BROKER_ERROR_GRANT_REVOKED,
	BROKER_ERROR_SURFACE_NOT_FOUND,
	BROKER_ERROR_SURFACE_AMBIGUOUS,
	BROKER_ERROR_SURFACE_REPLACED,
	BROKER_ERROR_SURFACE_PROTECTED,
	BROKER_ERROR_GEOMETRY_CHANGED,
	BROKER_ERROR_FRAME_STALE,
	BROKER_ERROR_COORDINATE_SPACE_UNSUPPORTED,
	BROKER_ERROR_OPERATION_CANCELLED,
	BROKER_ERROR_OPERATION_OUTCOME_UNKNOWN,
	BROKER_ERROR_BROKER_RESTARTED,
	BROKER_ERROR_SESSION_LOCKED,
	BROKER_ERROR_RATE_LIMITED,
	BROKER_ERROR_OVERFLOW,
	BROKER_ERROR_INTERNAL,
} BrokerError;

int broker_surface_identity_valid(const BrokerSurfaceIdentity *identity);
int broker_surface_identity_equal(const BrokerSurfaceIdentity *left,
                                  const BrokerSurfaceIdentity *right);
int broker_surface_supports(const BrokerSurfaceIdentity *identity,
                            uint32_t required_capabilities);

int broker_operation_init(BrokerOperation *operation,
                          const char *operation_id,
                          const char *owner,
                          const BrokerSurfaceIdentity *target);
int broker_operation_transition(BrokerOperation *operation,
                                BrokerOperationState next_state);
int broker_operation_terminal(BrokerOperationState state);
const char *broker_operation_state_name(BrokerOperationState state);
const char *broker_error_name(BrokerError error);

#endif /* DESKPAL_BROKER_CONTRACT_H */
