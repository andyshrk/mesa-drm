#include "ovltest_script.h"

#include <ctype.h>
#include <errno.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>

struct variable {
	char *name;
	char *value;
};

enum parser_branch {
	BRANCH_NONE,
	BRANCH_NUMBERED,
	BRANCH_IGNORED,
};

struct parser {
	struct variable *variables;
	size_t variable_count;
	struct ovl_script *script;
	enum parser_branch branch;
	long selector;
	char **branch_echoes;
	size_t branch_echo_count;
	bool branch_dump_summary;
	bool have_branch_test;
	size_t branch_test_index;
	bool in_function;
	bool assignments_closed;
};

static void set_parse_error(char *error, size_t error_size, unsigned int line, const char *message)
{
	if (error && error_size)
		snprintf(error, error_size, "line %u: %s", line, message);
}

static void free_strings(char **strings, size_t count)
{
	size_t i;

	for (i = 0; i < count; i++)
		free(strings[i]);
	free(strings);
}

static void free_variables(struct variable *variables, size_t count)
{
	size_t i;

	for (i = 0; i < count; i++) {
		free(variables[i].name);
		free(variables[i].value);
	}
	free(variables);
}

static void free_tests(struct ovl_script_test *tests, size_t count)
{
	size_t i;

	for (i = 0; i < count; i++) {
		free_strings(tests[i].argv, tests[i].argc);
		free_strings(tests[i].echoes, tests[i].echo_count);
	}
	free(tests);
}

void ovl_script_free(struct ovl_script *script)
{
	if (!script)
		return;

	free_strings(script->echoes, script->echo_count);
	free_tests(script->tests, script->test_count);
	memset(script, 0, sizeof(*script));
}

int ovl_script_parse_interval(const char *value, double *seconds)
{
	char *end;
	double parsed;

	errno = 0;
	parsed = strtod(value, &end);
	if (errno || end == value || *end != '\0' || !isfinite(parsed) ||
	    parsed < 0.0 || parsed > INT32_MAX)
		return -EINVAL;

	*seconds = parsed;
	return 0;
}

bool ovl_id_is_used(const uint32_t *ids, size_t count, uint32_t id)
{
	size_t i;

	for (i = 0; i < count; i++) {
		if (ids[i] == id)
			return true;
	}

	return false;
}

static char *trim(char *line)
{
	char *end;

	while (isspace((unsigned char)*line))
		line++;

	end = line + strlen(line);
	while (end > line && isspace((unsigned char)end[-1]))
		end--;
	*end = '\0';

	return line;
}

static int append_logical_line(char **logical, const char *physical)
{
	size_t old_len;
	size_t physical_len;
	char *expanded;

	old_len = *logical ? strlen(*logical) : 0;
	physical_len = strlen(physical);
	expanded = realloc(*logical, old_len + physical_len + 1);
	if (!expanded)
		return -ENOMEM;

	memcpy(expanded + old_len, physical, physical_len + 1);
	*logical = expanded;
	return 0;
}

static const struct variable *find_variable(const struct parser *parser, const char *name,
					     size_t name_len)
{
	size_t i;

	for (i = 0; i < parser->variable_count; i++) {
		if (strlen(parser->variables[i].name) == name_len &&
		    strncmp(parser->variables[i].name, name, name_len) == 0)
			return &parser->variables[i];
	}

	return NULL;
}

static bool valid_name_start(char c)
{
	return isalpha((unsigned char)c) || c == '_';
}

static bool valid_name_char(char c)
{
	return isalnum((unsigned char)c) || c == '_';
}

static char *expand_variables(const struct parser *parser, const char *text,
			      char *error, size_t error_size)
{
	size_t i;
	size_t len = 0;
	size_t allocated = strlen(text) + 1;
	char *expanded = malloc(allocated);
	const struct variable *variable;
	size_t name_len;
	size_t value_len;

	if (!expanded) {
		set_parse_error(error, error_size, 0, "out of memory");
		return NULL;
	}

	for (i = 0; text[i]; i++) {
		if (len + 2 > allocated) {
			char *grown = realloc(expanded, len + 2);

			if (!grown)
				goto oom;
			expanded = grown;
			allocated = len + 2;
		}
		if (text[i] != '$') {
			expanded[len++] = text[i];
			continue;
		}

		if (text[i + 1] == '{') {
			name_len = 0;
			while (valid_name_char(text[i + 2 + name_len]))
				name_len++;
			if (name_len == 0 || text[i + 2 + name_len] != '}') {
				set_parse_error(error, error_size, 0, "invalid variable reference");
				goto error;
			}
			variable = find_variable(parser, &text[i + 2], name_len);
			i += name_len + 2;
		} else if (valid_name_start(text[i + 1])) {
			name_len = 1;
			while (valid_name_char(text[i + 1 + name_len]))
				name_len++;
			variable = find_variable(parser, &text[i + 1], name_len);
			i += name_len;
		} else {
			expanded[len++] = '$';
			continue;
		}

		value_len = variable ? strlen(variable->value) : 0;
		if (len + value_len + 1 > allocated) {
			char *grown = realloc(expanded, len + value_len + 1);

			if (!grown)
				goto oom;
			expanded = grown;
			allocated = len + value_len + 1;
		}
		memcpy(expanded + len, variable ? variable->value : "", value_len);
		len += value_len;
	}

	expanded[len] = '\0';
	return expanded;

oom:
	set_parse_error(error, error_size, 0, "out of memory");
error:
	free(expanded);
	return NULL;
}

static char *strip_matching_quotes(char *value)
{
	size_t len = strlen(value);

	if (len >= 2 && ((value[0] == '"' && value[len - 1] == '"') ||
			 (value[0] == '\'' && value[len - 1] == '\''))) {
		value[len - 1] = '\0';
		return value + 1;
	}

	return value;
}

static int run_command_substitution(const char *command, char **result,
				    char *error, size_t error_size)
{
	FILE *pipe;
	char *output = NULL;
	char *line = NULL;
	size_t size = 0;
	ssize_t read;
	size_t len = 0;
	int status;

	pipe = popen(command, "r");
	if (!pipe) {
		set_parse_error(error, error_size, 0, "failed to run command");
		return -errno;
	}

	while ((read = getline(&line, &size, pipe)) >= 0) {
		char *grown = realloc(output, len + read + 1);

		if (!grown)
			goto oom;
		output = grown;
		memcpy(output + len, line, read + 1);
		len += read;
	}
	free(line);

	if (ferror(pipe)) {
		set_parse_error(error, error_size, 0, "failed to read command output");
		free(output);
		pclose(pipe);
		return -EIO;
	}

	status = pclose(pipe);
	if (status == -1 || !WIFEXITED(status) || WEXITSTATUS(status) != 0) {
		set_parse_error(error, error_size, 0, "command substitution failed");
		free(output);
		return -EINVAL;
	}

	if (!output)
		output = strdup("");
	if (!output)
		return -ENOMEM;

	while (len && output[len - 1] == '\n')
		output[--len] = '\0';
	*result = output;
	return 0;

oom:
	set_parse_error(error, error_size, 0, "out of memory");
	free(line);
	free(output);
	pclose(pipe);
	return -ENOMEM;
}

static bool is_assignment(const char *line)
{
	if (!valid_name_start(*line))
		return false;
	while (valid_name_char(*line))
		line++;
	return *line == '=';
}

static int parse_assignment(struct parser *parser, const char *line, char *error, size_t error_size)
{
	const char *equal = strchr(line, '=');
	size_t name_len = equal - line;
	char *name;
	char *value = NULL;
	char *expanded;
	struct variable *grown;
	struct variable *entry = NULL;
	size_t i;
	int ret;

	if (name_len == 0 || !valid_name_start(line[0]))
		return set_parse_error(error, error_size, 0, "invalid variable name"), -EINVAL;
	for (i = 1; i < name_len; i++) {
		if (!valid_name_char(line[i]))
			return set_parse_error(error, error_size, 0,
					       "invalid variable name"), -EINVAL;
	}

	for (i = 0; i < parser->variable_count; i++) {
		if (strlen(parser->variables[i].name) == name_len &&
		    strncmp(parser->variables[i].name, line, name_len) == 0) {
			entry = &parser->variables[i];
			break;
		}
	}

	name = strndup(line, name_len);
	if (!name)
		return -ENOMEM;

	expanded = expand_variables(parser, equal + 1, error, error_size);
	if (!expanded) {
		free(name);
		return -ENOMEM;
	}

	if (expanded[0] == '`' && expanded[strlen(expanded) - 1] == '`' && strlen(expanded) >= 2) {
		char *command;

		expanded[strlen(expanded) - 1] = '\0';
		command = strdup(expanded + 1);
		free(expanded);
		if (!command) {
			free(name);
			return -ENOMEM;
		}

		ret = run_command_substitution(command, &value, error, error_size);
		free(command);
		if (ret) {
			free(name);
			return ret;
		}
	} else {
		value = strdup(strip_matching_quotes(expanded));
		free(expanded);
		if (!value) {
			free(name);
			return -ENOMEM;
		}
	}

	if (entry) {
		free(entry->value);
		entry->value = value;
		free(name);
		return 0;
	}

	grown = realloc(parser->variables, (parser->variable_count + 1) * sizeof(*grown));
	if (!grown) {
		free(name);
		free(value);
		return -ENOMEM;
	}
	parser->variables = grown;
	parser->variables[parser->variable_count].name = name;
	parser->variables[parser->variable_count].value = value;
	parser->variable_count++;
	return 0;
}

static int append_string(char ***strings, size_t *count, char *string)
{
	char **grown = realloc(*strings, (*count + 1) * sizeof(*grown));

	if (!grown)
		return -ENOMEM;

	*strings = grown;
	(*strings)[(*count)++] = string;
	return 0;
}

static int append_echo(char ***echoes, size_t *count, char *echo)
{
	return append_string(echoes, count, echo);
}

static int append_word(char ***argv, int *argc, char *word)
{
	char **grown = realloc(*argv, (*argc + 2) * sizeof(*grown));

	if (!grown)
		return -ENOMEM;

	*argv = grown;
	(*argv)[(*argc)++] = word;
	(*argv)[*argc] = NULL;
	return 0;
}

static int split_words(const char *line, int *argc, char ***argv, char *error, size_t error_size)
{
	enum { STATE_UNQUOTED, STATE_SINGLE, STATE_DOUBLE } state = STATE_UNQUOTED;
	const char *p;
	char *word = NULL;
	size_t word_size = strlen(line) + 1;
	size_t len = 0;
	bool have_word = false;
	bool escaped = false;
	int ret;

	*argc = 0;
	*argv = NULL;

	for (p = line; ; p++) {
		if (!*p || (!escaped && state == STATE_UNQUOTED && isspace((unsigned char)*p))) {
			if (have_word) {
				char *saved;

				if (!word) {
					word = strdup("");
					if (!word)
						goto oom;
				} else {
					saved = realloc(word, len + 1);
					if (!saved)
						goto oom;
					word = saved;
					word[len] = '\0';
				}
				ret = append_word(argv, argc, word);
				if (ret)
					goto append_failed;
				word = NULL;
				len = 0;
				have_word = false;
			}

			if (!*p)
				break;
			continue;
		}

		if (escaped) {
			have_word = true;
			if (!word) {
				word = malloc(word_size);
				if (!word)
					goto oom;
			}
			word[len++] = *p;
			escaped = false;
			continue;
		}

		if (*p == '\\' && state != STATE_SINGLE) {
			escaped = true;
			have_word = true;
			continue;
		}

		if (state == STATE_UNQUOTED && *p == '\'') {
			state = STATE_SINGLE;
			have_word = true;
			continue;
		}
		if (state == STATE_UNQUOTED && *p == '"') {
			state = STATE_DOUBLE;
			have_word = true;
			continue;
		}
		if (state == STATE_SINGLE && *p == '\'') {
			state = STATE_UNQUOTED;
			continue;
		}
		if (state == STATE_DOUBLE && *p == '"') {
			state = STATE_UNQUOTED;
			continue;
		}

		have_word = true;
		if (!word) {
			word = malloc(word_size);
			if (!word)
				goto oom;
		}
		word[len++] = *p;
	}

	if (state != STATE_UNQUOTED || escaped) {
		set_parse_error(error, error_size, 0, "unterminated quote");
		free(word);
		free_strings(*argv, *argc);
		*argv = NULL;
		*argc = 0;
		return -EINVAL;
	}

	return 0;

append_failed:
	free(word);
	free_strings(*argv, *argc);
	*argv = NULL;
	*argc = 0;
	return ret;
oom:
	set_parse_error(error, error_size, 0, "out of memory");
	free(word);
	free_strings(*argv, *argc);
	*argv = NULL;
	*argc = 0;
	return -ENOMEM;
}

static bool starts_with(const char *line, const char *prefix)
{
	size_t len = strlen(prefix);

	if (len && isspace((unsigned char)prefix[len - 1]))
		return strncmp(line, prefix, len) == 0;

	return strncmp(line, prefix, len) == 0 &&
	       (line[len] == '\0' || isspace((unsigned char)line[len]));
}

static bool command_is(const char *word, const char *basename)
{
	const char *slash = strrchr(word, '/');

	return strcmp(slash ? slash + 1 : word, basename) == 0;
}

static bool command_line_is(const char *line, const char *name)
{
	size_t len = strlen(name);

	return strncmp(line, name, len) == 0 &&
	       (line[len] == '\0' || isspace((unsigned char)line[len]));
}

static int parse_selector(const char *line, long *selector)
{
	int argc;
	char **argv;
	int i;
	long value;
	char *end;

	if (split_words(line, &argc, &argv, NULL, 0))
		return false;

	for (i = 1; i + 1 < argc; i++) {
		if (!strcmp(argv[i], "=") && !strcmp(argv[i - 1], "$1"))
			break;
	}
	if (i + 1 >= argc) {
		free_strings(argv, argc);
		return false;
	}

	errno = 0;
	value = strtol(argv[i + 1], &end, 10);
	if (errno || end == argv[i + 1] || *end != '\0') {
		free_strings(argv, argc);
		return false;
	}

	free_strings(argv, argc);
	*selector = value;
	return true;
}

static int append_test(struct parser *parser, int argc, char **argv)
{
	struct ovl_script_test *grown;
	struct ovl_script_test *test;

	grown = realloc(parser->script->tests, (parser->script->test_count + 1) * sizeof(*grown));
	if (!grown)
		return -ENOMEM;
	parser->script->tests = grown;

	test = &parser->script->tests[parser->script->test_count++];
	test->selector = parser->selector;
	test->argc = argc;
	test->argv = argv;
	test->echoes = parser->branch_echoes;
	test->echo_count = parser->branch_echo_count;
	test->dump_summary = parser->branch_dump_summary;
	parser->branch_echoes = NULL;
	parser->branch_echo_count = 0;
	parser->branch_dump_summary = false;
	parser->have_branch_test = true;
	parser->branch_test_index = parser->script->test_count - 1;
	return 0;
}

static void reset_branch_data(struct parser *parser)
{
	free_strings(parser->branch_echoes, parser->branch_echo_count);
	parser->branch_echoes = NULL;
	parser->branch_echo_count = 0;
	parser->branch_dump_summary = false;
	parser->have_branch_test = false;
}

int ovl_script_parse_file(const char *path, struct ovl_script *script,
			  char *error, size_t error_size)
{
	FILE *file;
	struct parser parser = {};
	struct ovl_script parsed = {};
	char *line = NULL;
	size_t line_size = 0;
	ssize_t read;
	unsigned int line_number = 0;
	unsigned int logical_line = 0;
	char *logical = NULL;
	bool continuation = false;
	int ret = 0;

	if (error && error_size)
		error[0] = '\0';

	file = fopen(path, "r");
	if (!file) {
		ret = -errno;
		set_parse_error(error, error_size, 0, strerror(errno));
		return ret;
	}

	parser.script = &parsed;

	while ((read = getline(&line, &line_size, file)) >= 0) {
		char *trimmed;
		size_t trimmed_len;
		char *expanded;

		line_number++;
		if (!continuation)
			logical_line = line_number;
		trimmed = trim(line);
		trimmed_len = strlen(trimmed);
		if (!continuation && (!trimmed_len || trimmed[0] == '#'))
			continue;
		ret = append_logical_line(&logical, trimmed);
		if (ret) {
			set_parse_error(error, error_size, logical_line, "out of memory");
			break;
		}

		if (trimmed_len && trimmed[trimmed_len - 1] == '\\') {
			continuation = true;
			logical[strlen(logical) - 1] = '\0';
			continue;
		}
		continuation = false;
		trimmed = logical;

		if (parser.in_function) {
			if (strcmp(trimmed, "}") == 0)
				parser.in_function = false;
			free(logical);
			logical = NULL;
			continue;
		}

		if (starts_with(trimmed, "dump_summary_later()")) {
			parser.in_function = true;
			free(logical);
			logical = NULL;
			continue;
		}

		if (starts_with(trimmed, "if ") || starts_with(trimmed, "elif ")) {
			long selector;

			parser.assignments_closed = true;
			reset_branch_data(&parser);
			expanded = expand_variables(&parser, trimmed, error, error_size);
			if (!expanded) {
				ret = -EINVAL;
				goto line_error;
			}
			if (parse_selector(expanded, &selector)) {
				parser.branch = BRANCH_NUMBERED;
				parser.selector = selector;
			} else {
				parser.branch = BRANCH_IGNORED;
			}
			free(expanded);
		} else if (starts_with(trimmed, "else")) {
			reset_branch_data(&parser);
			parser.branch = BRANCH_IGNORED;
		} else if (starts_with(trimmed, "fi")) {
			reset_branch_data(&parser);
			parser.branch = BRANCH_NONE;
		} else if (!parser.branch && !parser.assignments_closed &&
			   is_assignment(trimmed)) {
			ret = parse_assignment(&parser, trimmed, error, error_size);
			if (ret)
				goto line_error;
		} else {
			int argc;
			char **argv;

			expanded = expand_variables(&parser, trimmed, error, error_size);
			if (!expanded) {
				ret = -EINVAL;
				goto line_error;
			}

			if (starts_with(expanded, "echo ")) {
				char *text = expanded + 5;
				char *echo;

				text = strip_matching_quotes(text);
				echo = strdup(text);
				if (!echo) {
					free(expanded);
					ret = -ENOMEM;
					goto line_error;
				}
				if (parser.branch == BRANCH_NUMBERED && parser.have_branch_test) {
					struct ovl_script_test *test;

					test = &parser.script->tests[parser.branch_test_index];
					ret = append_echo(&test->echoes, &test->echo_count, echo);
				} else if (parser.branch == BRANCH_NUMBERED) {
					ret = append_echo(&parser.branch_echoes,
							  &parser.branch_echo_count,
							  echo);
				} else if (!parser.branch) {
					ret = append_echo(&parsed.echoes, &parsed.echo_count, echo);
				} else {
					free(echo);
					echo = NULL;
				}
				if (ret)
					free(echo);
			} else if (parser.branch == BRANCH_NUMBERED &&
				   command_line_is(expanded, "dump_summary_later")) {
				if (parser.have_branch_test)
					parser.script->tests[parser.branch_test_index].dump_summary = true;
				else
					parser.branch_dump_summary = true;
			} else if (parser.branch == BRANCH_NUMBERED) {
				ret = split_words(expanded, &argc, &argv, error, error_size);
				if (!ret && argc && command_is(argv[0], "ovltest"))
					ret = append_test(&parser, argc, argv);
				else if (!ret)
					free_strings(argv, argc);
				else if (argv)
					free_strings(argv, argc);
			}
			free(expanded);
			if (ret)
				goto line_error;
		}

		free(logical);
		logical = NULL;
		continue;

line_error:
		if (!ret)
			ret = -EINVAL;
		if (error && error_size && !strncmp(error, "line 0: ", 8)) {
			char *detail = strdup(error + 8);

			if (detail) {
				set_parse_error(error, error_size, logical_line, detail);
				free(detail);
			}
		}
		break;
	}

	if (read < 0 && ferror(file)) {
		set_parse_error(error, error_size, line_number, strerror(errno));
		ret = -errno;
	}
	if (!ret && (continuation || parser.in_function)) {
		set_parse_error(error, error_size, line_number, "unterminated construct");
		ret = -EINVAL;
	}
	if (!ret && parsed.test_count == 0) {
		set_parse_error(error, error_size, line_number, "no ovltest commands found");
		ret = -ENOENT;
	}

	free(logical);
	free(line);
	fclose(file);
	reset_branch_data(&parser);
	free_variables(parser.variables, parser.variable_count);
	if (ret) {
		ovl_script_free(&parsed);
		return ret;
	}

	*script = parsed;
	return 0;
}
