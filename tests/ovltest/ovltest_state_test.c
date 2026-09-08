/* SPDX-License-Identifier: MIT */
/* Exercise internal transitions with synthetic DRM resources, without a device. */
int ovltest_program_main(int argc, char **argv);

#define main ovltest_program_main
#define drmModeAtomicAddProperty test_add_property
#include "ovltest.c"
#undef drmModeAtomicAddProperty
#undef main

#include <sys/wait.h>

static unsigned int property_calls;
static int property_error;

int test_add_property(drmModeAtomicReqPtr req, uint32_t object,
		      uint32_t property, uint64_t value)
{
	(void)req;
	(void)object;
	(void)property;
	(void)value;
	property_calls++;
	return property_error ? property_error : (int)property_calls;
}

static void test_writeback_mode_failure(void)
{
	drmModeConnector connector = {
		.connector_id = 10,
		.connector_type = DRM_MODE_CONNECTOR_WRITEBACK,
	};
	drmModeRes res = { .count_connectors = 1 };
	struct connector connectors[] = { { .connector = &connector } };
	struct resources resources = { .res = &res, .connectors = connectors };
	struct device dev = { .resources = &resources };
	struct test_state state = {};
	drmModeModeInfo *owned;

	state.pipe_count = 1;
	state.pipes = calloc(1, sizeof(*state.pipes));
	assert(state.pipes);
	assert(parse_connector(state.pipes, "10@20:800x600") == 0);
	assert(pipe_resolve_connectors(&dev, state.pipes) == 0);
	assert(pipe_find_crtc_and_mode(&dev, state.pipes) < 0);
	owned = state.pipes[0].owned_mode;
	assert(owned && !state.pipes[0].mode);
	assert(pipe_find_crtc_and_mode(&dev, state.pipes) < 0);
	assert(state.pipes[0].owned_mode == owned);
	free_test_state(&state);
}

int main(void)
{
	test_writeback_mode_failure();
	puts("ovltest state tests: PASS");
	return 0;
}
