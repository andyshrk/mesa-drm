#include "ovltest_script.h"

#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static int write_script(const char *text, char *path, size_t path_size)
{
	int fd;
	int ret;

	snprintf(path, path_size, "/tmp/ovltest-script-XXXXXX");
	fd = mkstemp(path);
	if (fd < 0)
		return -errno;

	if (write(fd, text, strlen(text)) < 0) {
		ret = -errno;
		close(fd);
		unlink(path);
		return ret;
	}

	return close(fd) ? -errno : 0;
}

static void test_parse_sample_script(void)
{
	static const char sample_script[] = "#!/bin/sh\n"
		"CONN=`printf 81`\n"
		"CRTC=74\n"
		"C0=59\n"
		"W=800\n"
		"echo \"Conn: $CONN\"\n"
		"dump_summary_later()\n"
		"{\n"
		"    sleep \"${1:-1}\"\n"
		"    cat /sys/kernel/debug/dri/0/summary\n"
		"}\n"
		"if [ $# -eq 0 ]; then\n"
		"modetest -M rockchip -s $CONN@$CRTC:${W}x1280\n"
		"elif [ \"$1\" = \"1\" ]; then\n"
		"dump_summary_later 2\n"
		"/data/ovltest -M rockchip -s $CONN@$CRTC:${W}x1280 \\\n"
		"  -P $C0@$CRTC:800x1280@AB24 -F /data/one.bin\n"
		"elif [ \"$1\" = \"1\" ]; then\n"
		"echo \"second $C0\"\n"
		"./ovltest -M rockchip -s $CONN@$CRTC:${W}x1280 \\\n"
		"  -P $C0@$CRTC:640x360@NV12 -F /data/two.bin\n"
		"else\n"
		"/data/ovltest --must-be-ignored\n"
		"fi\n";
	char path[] = "/tmp/ovltest-script-XXXXXX";
	struct ovl_script script = {};
	char error[128];
	int ret;

	ret = write_script(sample_script, path, sizeof(path));
	assert(ret == 0);

	ret = ovl_script_parse_file(path, &script, error, sizeof(error));
	if (ret)
		fprintf(stderr, "parse failed: %d: %s\n", ret, error);
	assert(ret == 0);
	assert(script.echo_count == 1);
	if (strcmp(script.echoes[0], "Conn: 81") != 0)
		fprintf(stderr, "echo=%zu:%s:end\n", strlen(script.echoes[0]), script.echoes[0]);
	assert(strcmp(script.echoes[0], "Conn: 81") == 0);
	assert(script.test_count == 2);
	assert(script.tests[0].selector == 1);
	assert(script.tests[1].selector == 1);
	assert(script.tests[0].dump_summary);
	assert(!script.tests[1].dump_summary);
	assert(script.tests[0].argc == 9);
	assert(script.tests[0].argv[9] == NULL);
	assert(strcmp(script.tests[0].argv[0], "/data/ovltest") == 0);
	assert(strcmp(script.tests[0].argv[4], "81@74:800x1280") == 0);
	assert(script.tests[1].echo_count == 1);
	assert(strcmp(script.tests[1].echoes[0], "second 59") == 0);
	assert(strcmp(script.tests[1].argv[8], "/data/two.bin") == 0);

	ovl_script_free(&script);
	unlink(path);
}

static void test_parse_edge_cases(void)
{
	static const char script_text[] =
		"# comment with = and an unmatched \" ${1:-1}\n"
		"LONG=12345678901234567890\n"
		"COPY=$LONG$\n"
		"EMPTY=`true`\n"
		"MULTI=`printf 'a\\n012345678901234567890123456789\\nlast\\n\\n'`\n"
		"echo \"COPY=$COPY\"\n"
		"echo \"$MULTI\"\n"
		"\\\n"
		"echo continued\n"
		"if [ \"$LONG\" = \"1\" ]; then\n"
		"ovltest --ignored\n"
		"fi\n"
		"if [ \"$1\" = \"1\" ]; then\n"
		"ovltest -F \"$EMPTY\"\n"
		"fi\n";
	char path[64];
	struct ovl_script script = {};
	char error[128];
	int ret;

	ret = write_script(script_text, path, sizeof(path));
	assert(ret == 0);
	ret = ovl_script_parse_file(path, &script, error, sizeof(error));
	if (ret)
		fprintf(stderr, "parse failed: %s\n", error);
	assert(ret == 0);
	assert(script.echo_count == 3);
	assert(!strcmp(script.echoes[0], "COPY=12345678901234567890$"));
	assert(!strcmp(script.echoes[1], "a\n012345678901234567890123456789\nlast"));
	assert(!strcmp(script.echoes[2], "continued"));
	assert(script.test_count == 1);
	assert(script.tests[0].argc == 3);
	assert(!strcmp(script.tests[0].argv[2], ""));
	ovl_script_free(&script);
	unlink(path);
}

static void test_parse_error_line(void)
{
	static const char script_text[] =
		"echo first \\\n"
		"continued\n"
		"if [ \"$1\" = \"1\" ]; then\n"
		"ovltest -F \"unterminated\n"
		"fi\n";
	char path[64];
	struct ovl_script script = {};
	char error[128];
	int ret;

	ret = write_script(script_text, path, sizeof(path));
	assert(ret == 0);
	ret = ovl_script_parse_file(path, &script, error, sizeof(error));
	assert(ret == -EINVAL);
	assert(!strncmp(error, "line 4: ", 8));
	ovl_script_free(&script);
	unlink(path);
}

static void test_script_options(void)
{
	struct ovl_script_options options;
	char program[] = "ovltest";
	char opt_s[] = "-S";
	char opt_r[] = "-R";
	char opt_i[] = "-i";
	char opt_m[] = "-M";
	char opt_f[] = "-F";
	char script[] = "script.sh";
	char half[] = "0.5";
	char module[] = "rockchip";
	char one[] = "one.sh";
	char two[] = "two.sh";
	char positional[] = "extra";
	char one_second[] = "1s";
	char negative_one[] = "-1";
	char nan_value[] = "nan";
	char huge_value[] = "1e100";
	char operand_r[] = "-R";
	char *sequential[] = { program, opt_s, script };
	char *random[] = { program, opt_r, script, opt_i, half };
	char *normal[] = { program, opt_m, module };
	char *normal_operand[] = { program, opt_f, operand_r };
	char *interval_first[] = { program, opt_i, half, opt_r, script };
	char *conflict[] = { program, opt_s, one, opt_r, two };
	char *repeated[] = { program, opt_r, one, opt_r, two };
	char *repeated_interval[] = { program, opt_r, script, opt_i, half, opt_i, half };
	char *missing[] = { program, opt_r };
	char *unknown[] = { program, opt_r, script, opt_m, module };
	char *extra[] = { program, opt_r, script, positional };
	char *option_like_path[] = { program, opt_s, operand_r };
	char *invalid_suffix[] = { program, opt_r, script, opt_i, one_second };
	char *invalid_negative[] = { program, opt_r, script, opt_i, negative_one };
	char *invalid_nan[] = { program, opt_r, script, opt_i, nan_value };
	char *invalid_range[] = { program, opt_r, script, opt_i, huge_value };

	assert(ovl_script_parse_options(3, sequential, &options) == 0);
	assert(strcmp(options.script, "script.sh") == 0);
	assert(options.interval == 2.0);
	assert(options.order == OVL_SCRIPT_ORDER_SEQUENTIAL);

	assert(ovl_script_parse_options(5, random, &options) == 0);
	assert(strcmp(options.script, "script.sh") == 0);
	assert(options.interval == 0.5);
	assert(options.order == OVL_SCRIPT_ORDER_RANDOM);

	assert(ovl_script_parse_options(3, normal, &options) == 0);
	assert(options.script == NULL);
	assert(options.interval == 2.0);
	assert(options.order == OVL_SCRIPT_ORDER_SEQUENTIAL);

	assert(ovl_script_parse_options(3, normal_operand, &options) == 0);
	assert(options.script == NULL);
	assert(options.interval == 2.0);
	assert(options.order == OVL_SCRIPT_ORDER_SEQUENTIAL);

	assert(ovl_script_parse_options(5, interval_first, &options) == 0);
	assert(options.script == NULL);
	assert(options.interval == 2.0);
	assert(options.order == OVL_SCRIPT_ORDER_SEQUENTIAL);

	assert(ovl_script_parse_options(5, conflict, &options) == -EINVAL);
	assert(ovl_script_parse_options(5, repeated, &options) == -EINVAL);
	assert(ovl_script_parse_options(7, repeated_interval, &options) == -EINVAL);
	assert(ovl_script_parse_options(2, missing, &options) == -EINVAL);
	assert(ovl_script_parse_options(5, unknown, &options) == -EINVAL);
	assert(ovl_script_parse_options(4, extra, &options) == -EINVAL);

	assert(ovl_script_parse_options(3, option_like_path, &options) == 0);
	assert(strcmp(options.script, "-R") == 0);
	assert(options.interval == 2.0);
	assert(options.order == OVL_SCRIPT_ORDER_SEQUENTIAL);

	assert(ovl_script_parse_options(5, invalid_suffix, &options) == -EINVAL);
	assert(ovl_script_parse_options(5, invalid_negative, &options) == -EINVAL);
	assert(ovl_script_parse_options(5, invalid_nan, &options) == -EINVAL);
	assert(ovl_script_parse_options(5, invalid_range, &options) == -EINVAL);
}

static int check_same_target(const struct ovl_script_test *test,
			     const char **device, const char **module)
{
	(void)test;
	*device = "/dev/dri/card0";
	*module = "rockchip";
	return 0;
}

static int check_other_target(const struct ovl_script_test *test,
			      const char **device, const char **module)
{
	*device = test->selector == 1 ? "/dev/dri/card0" : "/dev/dri/card1";
	*module = "rockchip";
	return 0;
}

static int check_failed(const struct ovl_script_test *test,
			const char **device, const char **module)
{
	(void)test;
	*device = "/dev/dri/card0";
	*module = "rockchip";
	return -EINVAL;
}

static void test_check_commands(void)
{
	struct ovl_script_test tests[] = { { .selector = 1 }, { .selector = 2 } };
	struct ovl_script script = { .tests = tests, .test_count = 2 };
	char *device = NULL;
	char *module = NULL;

	assert(ovl_script_check_commands(&script, check_same_target, &device, &module) == 0);
	assert(strcmp(device, "/dev/dri/card0") == 0);
	assert(strcmp(module, "rockchip") == 0);
	free(device);
	free(module);

	device = NULL;
	module = NULL;
	assert(ovl_script_check_commands(&script, check_other_target, &device, &module) == -EINVAL);
	assert(device == NULL && module == NULL);

	assert(ovl_script_check_commands(&script, check_failed, &device, &module) == -EINVAL);
	assert(device == NULL && module == NULL);
}

static void test_id_lookup(void)
{
	const uint32_t ids[] = { 59, 64, 69 };

	assert(ovl_id_is_used(ids, 3, 64));
	assert(!ovl_id_is_used(ids, 3, 65));
}

static void test_full_plane_selection(void)
{
	const uint32_t all[] = { 59, 64, 69, 70 };
	const uint32_t used[] = { 59, 69 };
	bool disabled[4];
	size_t i;

	for (i = 0; i < 4; i++)
		disabled[i] = !ovl_id_is_used(used, 2, all[i]);

	assert(!disabled[0]);
	assert(disabled[1]);
	assert(!disabled[2]);
	assert(disabled[3]);
}

static void test_parse_variable_forms_and_quoted_path(void)
{
	static const char script_text[] = "NAME=alpha\n"
		"OTHER=beta\n"
		"PICTURE=\"/data/my picture.bin\"\n"
		"echo \"$NAME ${OTHER} RGB:$RGB DSI:$DSI ${RGB}\"\n"
		"if [ \"$1\" = \"1\" ]; then\n"
		"ovltest -F \"$PICTURE\" -A \"$RGB$DSI\"\n"
		"fi\n";
	char path[] = "/tmp/ovltest-script-XXXXXX";
	struct ovl_script script = {};
	char error[128];
	int ret;

	ret = write_script(script_text, path, sizeof(path));
	assert(ret == 0);

	ret = ovl_script_parse_file(path, &script, error, sizeof(error));
	if (ret)
		fprintf(stderr, "parse failed: %d: %s\n", ret, error);
	assert(ret == 0);
	assert(script.echo_count == 1);
	assert(strcmp(script.echoes[0], "alpha beta RGB: DSI: ") == 0);
	assert(script.test_count == 1);
	assert(script.tests[0].argc == 5);
	assert(script.tests[0].argv[5] == NULL);
	assert(strcmp(script.tests[0].argv[2], "/data/my picture.bin") == 0);
	assert(strcmp(script.tests[0].argv[4], "") == 0);

	ovl_script_free(&script);
	unlink(path);
}

static void test_parse_branch_output_scope(void)
{
	static const char script_text[] = "echo top\n"
		"if [ \"$1\" = \"1\" ]; then\n"
		"echo before\n"
		"ovltest -F first\n"
		"echo after\n"
		"dump_summary_later\n"
		"elif [ \"$1\" = \"2\" ]; then\n"
		"echo numbered\n"
		"ovltest -F second\n"
		"else\n"
		"echo ignored\n"
		"fi\n"
		"unknown-command --with=value\n";
	char path[] = "/tmp/ovltest-script-XXXXXX";
	struct ovl_script script = {};
	char error[128];
	int ret;

	ret = write_script(script_text, path, sizeof(path));
	assert(ret == 0);

	ret = ovl_script_parse_file(path, &script, error, sizeof(error));
	if (ret)
		fprintf(stderr, "parse failed: %d: %s\n", ret, error);
	assert(ret == 0);
	assert(script.echo_count == 1);
	assert(strcmp(script.echoes[0], "top") == 0);
	assert(script.test_count == 2);
	assert(script.tests[0].echo_count == 2);
	assert(strcmp(script.tests[0].echoes[0], "before") == 0);
	assert(strcmp(script.tests[0].echoes[1], "after") == 0);
	assert(script.tests[0].dump_summary);
	assert(script.tests[1].echo_count == 1);
	assert(strcmp(script.tests[1].echoes[0], "numbered") == 0);

	ovl_script_free(&script);
	unlink(path);
}

static void test_parse_ignores_summary_lookalike(void)
{
	static const char script_text[] = "if [ \"$1\" = \"1\" ]; then\n"
		"dump_summary_laterfoo\n"
		"ovltest -F value\n"
		"fi\n";
	char path[] = "/tmp/ovltest-script-XXXXXX";
	struct ovl_script script = {};
	char error[128];
	int ret;

	ret = write_script(script_text, path, sizeof(path));
	assert(ret == 0);

	ret = ovl_script_parse_file(path, &script, error, sizeof(error));
	assert(ret == 0);
	assert(script.test_count == 1);
	assert(!script.tests[0].dump_summary);

	ovl_script_free(&script);
	unlink(path);
}

static void assert_parse_error(const char *script_text, int expected)
{
	char path[] = "/tmp/ovltest-script-XXXXXX";
	struct ovl_script script = {};
	char error[128];
	int ret;

	ret = write_script(script_text, path, sizeof(path));
	assert(ret == 0);

	ret = ovl_script_parse_file(path, &script, error, sizeof(error));
	if (ret != expected)
		fprintf(stderr, "expected %d, got %d: %s\n", expected, ret, error);
	assert(ret == expected);
	assert(error[0] != '\0');
	ovl_script_free(&script);
	unlink(path);
}

static void test_parse_errors(void)
{
	assert_parse_error("", -ENOENT);
	assert_parse_error("echo no commands\n", -ENOENT);
	assert_parse_error("if [ \"$1\" = \"1\" ]; then\n"
		"ovltest -F \"/data/unterminated\n"
		"fi\n", -EINVAL);
	assert_parse_error("VALUE=`false`\n"
		"if [ \"$1\" = \"1\" ]; then\n"
		"ovltest -F value\n"
		"fi\n", -EINVAL);
}

int main(int argc, char **argv)
{
	if (argc == 2) {
		struct ovl_script script = {};
		char error[128];
		int ret;

		ret = ovl_script_parse_file(argv[1], &script, error, sizeof(error));
		if (ret) {
			fprintf(stderr, "%s\n", error);
			return 1;
		}
		printf("parsed %zu ovltest commands\n", script.test_count);
		ovl_script_free(&script);
		return 0;
	}

	test_parse_sample_script();
	test_parse_edge_cases();
	test_parse_error_line();
	test_script_options();
	test_check_commands();
	test_id_lookup();
	test_full_plane_selection();
	test_parse_variable_forms_and_quoted_path();
	test_parse_branch_output_scope();
	test_parse_ignores_summary_lookalike();
	test_parse_errors();

	puts("ovltest script tests: PASS");
	return 0;
}
