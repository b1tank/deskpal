/*
 * deskpal — Backend-neutral trusted desktop broker contract
 * SPDX-License-Identifier: MIT
 */
#include "broker_contract.h"

#include <stdio.h>
#include <string.h>

static int bounded_id(const char *value, size_t capacity)
{
	return value && value[0] && strnlen(value, capacity) < capacity;
}

int broker_surface_identity_valid(const BrokerSurfaceIdentity *identity)
{
	if (!identity ||
	    !bounded_id(identity->broker_instance_id,
	                sizeof(identity->broker_instance_id)) ||
	    !bounded_id(identity->surface_id, sizeof(identity->surface_id)) ||
	    identity->generation == 0 || identity->geometry_revision == 0)
		return 0;
	uint32_t known = BROKER_CAP_OBSERVE | BROKER_CAP_CAPTURE |
		BROKER_CAP_POINTER | BROKER_CAP_KEYBOARD | BROKER_CAP_SCROLL |
		BROKER_CAP_DRAG | BROKER_CAP_BACKGROUND | BROKER_CAP_CANCELLATION |
		BROKER_CAP_COMPLETION;
	if (identity->capabilities & ~known) return 0;
	if ((identity->capabilities & BROKER_CAP_BACKGROUND) &&
	    !(identity->capabilities &
	      (BROKER_CAP_POINTER | BROKER_CAP_KEYBOARD |
	       BROKER_CAP_SCROLL | BROKER_CAP_DRAG)))
		return 0;
	return 1;
}

int broker_surface_identity_equal(const BrokerSurfaceIdentity *left,
                                  const BrokerSurfaceIdentity *right)
{
	return broker_surface_identity_valid(left) &&
	       broker_surface_identity_valid(right) &&
	       left->generation == right->generation &&
	       strcmp(left->broker_instance_id, right->broker_instance_id) == 0 &&
	       strcmp(left->surface_id, right->surface_id) == 0;
}

int broker_surface_supports(const BrokerSurfaceIdentity *identity,
                            uint32_t required_capabilities)
{
	return broker_surface_identity_valid(identity) &&
	       !identity->protected_surface && required_capabilities != 0 &&
	       (identity->capabilities & required_capabilities) ==
	           required_capabilities;
}

int broker_operation_terminal(BrokerOperationState state)
{
	return state == BROKER_OPERATION_COMPLETED ||
	       state == BROKER_OPERATION_CANCELLED_BEFORE_DISPATCH ||
	       state == BROKER_OPERATION_OUTCOME_UNKNOWN ||
	       state == BROKER_OPERATION_FAILED_BEFORE_DISPATCH ||
	       state == BROKER_OPERATION_REVOKED;
}

static int transition_allowed(BrokerOperationState from,
                              BrokerOperationState to)
{
	if (from == to) return 1;
	switch (from) {
	case BROKER_OPERATION_ACCEPTED:
		return to == BROKER_OPERATION_DISPATCHING ||
		       to == BROKER_OPERATION_CANCELLED_BEFORE_DISPATCH ||
		       to == BROKER_OPERATION_FAILED_BEFORE_DISPATCH ||
		       to == BROKER_OPERATION_REVOKED;
	case BROKER_OPERATION_DISPATCHING:
		return to == BROKER_OPERATION_DISPATCHED ||
		       to == BROKER_OPERATION_CANCELLED_BEFORE_DISPATCH ||
		       to == BROKER_OPERATION_FAILED_BEFORE_DISPATCH ||
		       to == BROKER_OPERATION_OUTCOME_UNKNOWN ||
		       to == BROKER_OPERATION_REVOKED;
	case BROKER_OPERATION_DISPATCHED:
		return to == BROKER_OPERATION_COMPLETED ||
		       to == BROKER_OPERATION_OUTCOME_UNKNOWN ||
		       to == BROKER_OPERATION_REVOKED;
	default:
		return 0;
	}
}

int broker_operation_init(BrokerOperation *operation,
                          const char *operation_id,
                          const char *owner,
                          const BrokerSurfaceIdentity *target)
{
	if (!operation || !bounded_id(operation_id, DESKPAL_BROKER_OPERATION_ID_LEN) ||
	    !bounded_id(owner, DESKPAL_BROKER_OWNER_LEN) ||
	    !broker_surface_identity_valid(target) || target->protected_surface)
		return -1;
	memset(operation, 0, sizeof(*operation));
	snprintf(operation->operation_id, sizeof(operation->operation_id), "%s",
	         operation_id);
	snprintf(operation->owner, sizeof(operation->owner), "%s", owner);
	operation->target = *target;
	operation->state = BROKER_OPERATION_ACCEPTED;
	return 0;
}

int broker_operation_transition(BrokerOperation *operation,
                                BrokerOperationState next_state)
{
	if (!operation || !transition_allowed(operation->state, next_state))
		return -1;
	operation->state = next_state;
	if (next_state == BROKER_OPERATION_DISPATCHING ||
	    next_state == BROKER_OPERATION_DISPATCHED ||
	    next_state == BROKER_OPERATION_COMPLETED ||
	    next_state == BROKER_OPERATION_OUTCOME_UNKNOWN)
		operation->dispatch_attempted = 1;
	if (next_state == BROKER_OPERATION_DISPATCHED ||
	    next_state == BROKER_OPERATION_COMPLETED)
		operation->delivery_accepted = 1;
	return 0;
}

const char *broker_operation_state_name(BrokerOperationState state)
{
	switch (state) {
	case BROKER_OPERATION_ACCEPTED: return "accepted";
	case BROKER_OPERATION_DISPATCHING: return "dispatching";
	case BROKER_OPERATION_DISPATCHED: return "dispatched";
	case BROKER_OPERATION_COMPLETED: return "completed";
	case BROKER_OPERATION_CANCELLED_BEFORE_DISPATCH:
		return "cancelledBeforeDispatch";
	case BROKER_OPERATION_OUTCOME_UNKNOWN: return "outcomeUnknown";
	case BROKER_OPERATION_FAILED_BEFORE_DISPATCH: return "failedBeforeDispatch";
	case BROKER_OPERATION_REVOKED: return "revoked";
	default: return "invalid";
	}
}

const char *broker_error_name(BrokerError error)
{
	static const char *names[] = {
		"none", "brokerUnavailable", "capabilityUnavailable",
		"backgroundUnavailable", "permissionRequired", "permissionDenied",
		"grantExpired", "grantRevoked", "surfaceNotFound",
		"surfaceAmbiguous", "surfaceReplaced", "surfaceProtected",
		"geometryChanged", "frameStale", "coordinateSpaceUnsupported",
		"operationCancelled", "operationOutcomeUnknown", "brokerRestarted",
		"sessionLocked", "rateLimited", "overflow", "internalError",
	};
	if (error < BROKER_ERROR_NONE || error > BROKER_ERROR_INTERNAL)
		return "invalid";
	return names[error];
}
