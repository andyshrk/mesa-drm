/*
 * Copyright 2026 Intel Corporation
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS, WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR SOFTWARE OR
 * OTHER DEALINGS IN THE SOFTWARE.
 */

#ifndef OVLTEST_SCRIPT_H
#define OVLTEST_SCRIPT_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

struct ovl_script_test {
	long selector;
	int argc;
	char **argv;
	char **echoes;
	size_t echo_count;
	bool dump_summary;
};

struct ovl_script {
	char **echoes;
	size_t echo_count;
	struct ovl_script_test *tests;
	size_t test_count;
};

enum ovl_script_order {
	OVL_SCRIPT_ORDER_SEQUENTIAL,
	OVL_SCRIPT_ORDER_RANDOM,
};

struct ovl_script_options {
	const char *script;
	double interval;
	enum ovl_script_order order;
};

typedef int (*ovl_script_check_command_fn)(const struct ovl_script_test *test,
					   const char **device, const char **module);

int ovl_script_parse_file(const char *path, struct ovl_script *script,
			  char *error, size_t error_size);
void ovl_script_free(struct ovl_script *script);
bool ovl_id_is_used(const uint32_t *ids, size_t count, uint32_t id);
int ovl_script_parse_options(int argc, char **argv, struct ovl_script_options *options);
int ovl_script_check_commands(const struct ovl_script *script,
			      ovl_script_check_command_fn check_command, char **device, char **module);

#endif
