/* Deterministic tests for the backend-neutral broker contract. */
#include "broker_contract.h"

#include <stdio.h>
#include <string.h>

#define CHECK(condition) do { \
	if (!(condition)) { \
		fprintf(stderr, "broker_contract_test failed at line %d\n", __LINE__); \
		return 1; \
	} \
} while (0)

static BrokerSurfaceIdentity surface(void)
{
	BrokerSurfaceIdentity identity = {
		.generation = 7,
		.geometry_revision = 11,
		.capabilities = BROKER_CAP_OBSERVE | BROKER_CAP_CAPTURE |
		                BROKER_CAP_POINTER | BROKER_CAP_BACKGROUND |
		                BROKER_CAP_CANCELLATION | BROKER_CAP_COMPLETION,
	};
	snprintf(identity.broker_instance_id,
	         sizeof(identity.broker_instance_id), "broker-instance-1");
	snprintf(identity.surface_id, sizeof(identity.surface_id), "surface-42");
	return identity;
}

int main(void)
{
	BrokerSurfaceIdentity first = surface();
	BrokerSurfaceIdentity same = first;
	CHECK(broker_surface_identity_valid(&first));
	CHECK(broker_surface_identity_equal(&first, &same));
	CHECK(broker_surface_supports(
		&first, BROKER_CAP_POINTER | BROKER_CAP_BACKGROUND));
	same.generation++;
	CHECK(!broker_surface_identity_equal(&first, &same));
	same = first;
	same.protected_surface = 1;
	CHECK(!broker_surface_supports(&same, BROKER_CAP_CAPTURE));
	same = first;
	same.capabilities = BROKER_CAP_BACKGROUND;
	CHECK(!broker_surface_identity_valid(&same));

	BrokerOperation operation;
	CHECK(broker_operation_init(
		&operation, "operation-1", ":1.25", &first) == 0);
	CHECK(operation.state == BROKER_OPERATION_ACCEPTED);
	CHECK(!operation.dispatch_attempted && !operation.delivery_accepted);
	CHECK(broker_operation_transition(
		&operation, BROKER_OPERATION_DISPATCHING) == 0);
	CHECK(operation.dispatch_attempted && !operation.delivery_accepted);
	CHECK(broker_operation_transition(
		&operation, BROKER_OPERATION_DISPATCHED) == 0);
	CHECK(operation.delivery_accepted);
	CHECK(broker_operation_transition(
		&operation, BROKER_OPERATION_COMPLETED) == 0);
	CHECK(broker_operation_terminal(operation.state));
	CHECK(broker_operation_transition(
		&operation, BROKER_OPERATION_DISPATCHING) == -1);
	CHECK(strcmp(broker_operation_state_name(operation.state), "completed") == 0);

	CHECK(broker_operation_init(
		&operation, "operation-2", ":1.25", &first) == 0);
	CHECK(broker_operation_transition(
		&operation, BROKER_OPERATION_CANCELLED_BEFORE_DISPATCH) == 0);
	CHECK(!operation.dispatch_attempted && broker_operation_terminal(operation.state));

	CHECK(broker_operation_init(
		&operation, "operation-3", ":1.25", &first) == 0);
	CHECK(broker_operation_transition(
		&operation, BROKER_OPERATION_DISPATCHING) == 0);
	CHECK(broker_operation_transition(
		&operation, BROKER_OPERATION_OUTCOME_UNKNOWN) == 0);
	CHECK(operation.dispatch_attempted && !operation.delivery_accepted);
	CHECK(strcmp(broker_error_name(BROKER_ERROR_BACKGROUND_UNAVAILABLE),
	             "backgroundUnavailable") == 0);
	CHECK(strcmp(broker_error_name((BrokerError)999), "invalid") == 0);

	puts("PASS: broker identity, capabilities, errors, and operation states");
	return 0;
}
