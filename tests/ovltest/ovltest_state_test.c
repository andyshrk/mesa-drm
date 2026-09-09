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

static void test_partial_mode_failure(void)
{
	drmModeModeInfo mode = { .name = "800x600", .hdisplay = 800, .vdisplay = 600 };
	drmModeConnector connector = { .connector_id = 10, .count_modes = 1, .modes = &mode };
	drmModeCrtc crtc = { .crtc_id = 20 };
	drmModeRes res = { .count_connectors = 1, .count_crtcs = 1 };
	struct connector connectors[] = { { .connector = &connector } };
	struct crtc crtcs[] = { { .crtc = &crtc } };
	struct resources resources = { .res = &res, .connectors = connectors, .crtcs = crtcs };
	struct device dev = { .resources = &resources };
	struct test_state state = {};
	unsigned int calls = property_calls;

	state.pipe_count = 2;
	state.pipes = calloc(state.pipe_count, sizeof(*state.pipes));
	assert(state.pipes);
	assert(parse_connector(&state.pipes[0], "10@20:800x600") == 0);
	assert(parse_connector(&state.pipes[1], "10@21:800x600") == 0);
	assert(pipe_resolve_connectors(&dev, &state.pipes[0]) == 0);
	assert(pipe_resolve_connectors(&dev, &state.pipes[1]) == 0);
	assert(atomic_set_mode(&dev, state.pipes, state.pipe_count, true) < 0);
	assert(property_calls == calls);
	free_test_state(&state);
}

static void test_optional_properties(void)
{
	drmModeRes res = {};
	drmModePlaneRes plane_res = { .count_planes = 1 };
	drmModePlane plane = { .plane_id = 1 };
	drmModeObjectProperties props = {};
	drmModePropertyRes rotation = { .prop_id = 2, .name = "rotation" };
	drmModePropertyRes *info[] = { &rotation };
	uint32_t ids[] = { 2 };
	struct plane planes[] = { { .plane = &plane, .props = &props, .props_info = info } };
	struct resources resources = { .res = &res, .plane_res = &plane_res, .planes = planes };
	struct device dev = { .resources = &resources };

	assert(add_property(&dev, 1, "rotation", DRM_MODE_ROTATE_0) < 0);
	assert(add_property(&dev, 1, "zpos", 1) < 0);
	assert(add_property_optional(&dev, 1, "rotation", DRM_MODE_ROTATE_0) == 0);
	assert(add_property_optional(&dev, 1, "zpos", 1) == 0);
	assert(add_property_optional(&dev, 1, "test_optional", 0) == 0);
	assert(add_property(&dev, 1, "rotation", DRM_MODE_ROTATE_90) < 0);
	assert(add_property(&dev, 1, "FB_ID", 1) < 0);
	assert(add_property_optional(&dev, 2, "rotation", DRM_MODE_ROTATE_0) < 0);
	planes[0].props = NULL;
	assert(add_property_optional(&dev, 1, "rotation", DRM_MODE_ROTATE_0) < 0);
	planes[0].props = &props;
	assert(property_calls == 0);
	props.count_props = 1;
	props.props = ids;
	assert(add_property(&dev, 1, "rotation", DRM_MODE_ROTATE_0) == 0);
	assert(add_property_optional(&dev, 1, "rotation", DRM_MODE_ROTATE_0) == 0);
	property_error = -ENOMEM;
	assert(add_property(&dev, 1, "rotation", DRM_MODE_ROTATE_0) < 0);
	assert(add_property_optional(&dev, 1, "rotation", DRM_MODE_ROTATE_0) < 0);
	assert(property_calls == 4);
	property_error = 0;
}

static void test_check_script_command_ids(void)
{
	char program[] = "ovltest";
	char opt_s[] = "-s";
	char opt_p[] = "-P";
	char good[] = "10@20:800x600";
	char bad[] = "@20:800x600";
	char plane[] = "@20:800x600";
	char *argv[] = { program, opt_s, bad, NULL, NULL, NULL };
	struct ovl_script_test test = { .argc = 3, .argv = argv };
	const char *device = NULL;
	const char *module = NULL;

	assert(check_script_command(&test, &device, &module) == -EINVAL);
	argv[2] = good;
	assert(check_script_command(&test, &device, &module) == 0);
	argv[3] = opt_p;
	argv[4] = plane;
	test.argc = 5;
	assert(check_script_command(&test, &device, &module) == -EINVAL);
}

static void test_stop_wait(void)
{
	sigset_t blocked;
	sigset_t previous;
	pid_t child;
	int status;

	signal(SIGINT, handle_signal);
	signal(SIGTERM, handle_signal);
	stop_requested = 1;
	assert(wait_interval(60) == 1);
	stop_requested = 0;
	assert(wait_interval(0.01) == 0);
	sigemptyset(&blocked);
	sigaddset(&blocked, SIGTERM);
	assert(sigprocmask(SIG_BLOCK, &blocked, &previous) == 0);
	assert(raise(SIGTERM) == 0);
	assert(sigprocmask(SIG_SETMASK, &previous, NULL) == 0);
	assert(wait_interval(60) == 1);
	stop_requested = 0;
	child = fork();
	assert(child >= 0);
	if (!child) {
		usleep(20000);
		kill(getppid(), SIGINT);
		_exit(0);
	}
	assert(wait_interval(60) == 1);
	assert(waitpid(child, &status, 0) == child);
	assert(WIFEXITED(status) && WEXITSTATUS(status) == 0);
}

int main(void)
{
	test_optional_properties();
	test_writeback_mode_failure();
	test_partial_mode_failure();
	test_check_script_command_ids();
	test_stop_wait();
	puts("ovltest state tests: PASS");
	return 0;
}
