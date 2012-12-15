/*
 * Copyright 2012, Landon Fuller <landonf@bikemonkey.org>.
 * All Rights Reserved.
 * Distributed under the terms of the MIT License.
 *
 * Implements a front-end arch-aware driver for GNU as(1).
 * Based on the design of Apple's as/driver.c in cctools-836.
 */

#define FATELF_UTILS 1
#include "fatelf-utils.h"
#include <stdbool.h>

#if TODO
static const char *exec_paths[] = {
		"../libexec/as/",
		"../local/libexec/as/",
		NULL
};
#endif

#define MAX_FILE_DEPTH 100

typedef struct as_flag {
	const char opt;
	const char *long_opt;
	const bool accepts_arg;
	const bool fat_arg;
	const bool single_dash;
} as_flag;

static const as_flag as_flags[] = {
		/* opt	long_opt	accept_arg	fat_arg		single_dash (long opt) */
		{ 'o',	NULL,		true,		false,		false	},
		{ 'I',	NULL,		true,		false,		false	},
		{ '\0', "defsyms",	true,		false,		false	},
		{ '\0',	"arch",		true,		true,		true	},

		// MIPS-specific arguments
		{ 'G',	NULL,		true,		false,		false	},
};


typedef struct arg_table {
	int argc;
	char **argv;
	size_t argv_length;
} arg_table;

static void append_argument (arg_table *args, const char *argument);
static void parse_arguments (arg_table *input_args, arg_table *output_args,
		arg_table *fat_args, int depth);
static void parse_argument_file (const char *fname, arg_table *output_args,
		arg_table *fat_args, int depth);

/* Look up the given option in the as_flags table. */
static const as_flag *find_flag (const char opt,
		const char *long_opt,
		bool single_dash)
{
	int i;

	for (i = 0; i < sizeof(as_flags) / sizeof(struct as_flag); i++) {
		const as_flag *flag = &as_flags[i];
		if (opt != '\0' && flag->opt == opt) {
			return flag;
		} else if (long_opt != NULL && flag->long_opt != NULL) {
			if (strcmp(long_opt, flag->long_opt) == 0 &&
					single_dash == flag->single_dash)
			{
				return flag;
			}
		}
	}

	return NULL;
}

/* Append an argument to the provided args table. The caller is responsible
 * for free()'ing the argument added to the table. */
static void append_argument (arg_table *args, const char *argument) {
	/* Ensure adequate space in the buffer. We grow in blocks of
	 * 10 -- this number was arbitrarily chosen. */
	if (args->argv == NULL) {
		args->argv_length = 10;
		args->argv = xmalloc(sizeof(char *) * args->argv_length);
	} else if (args->argc == args->argv_length) {
		args->argv_length += 10;
		args->argv = xrealloc(args->argv, sizeof(char *) * args->argv_length);
	}

	args->argv[args->argc] = xstrdup(argument);
	args->argc++;
}

/* Parse an as(1) @FILE, which contains command line arguments, seperated
 * by whitespace. */
static void parse_argument_file (const char *fname,
		arg_table *output_args,
		arg_table *fat_args,
		int depth)
{
	arg_table file_args;
	char optbuf[8192];
	int scancount;
	FILE *file;
	int fd;
	int i;

	/* Protect against infinite recursion */
	if (depth >= MAX_FILE_DEPTH)
		xfail("Exceeded maximum number of supported @FILE includes in '%s'",
			fname);

	/* Open the input file */
	fd = xopen(fname, O_RDONLY, 0);
	file = fdopen(fd, "r");

	/* Configure table to hold parsed arguments */
	file_args.argc = 0;
	file_args.argv = NULL;
	file_args.argv_length = 0;

	do {
		optbuf[sizeof(optbuf)-1] = '\0';
		scancount = fscanf(file, "%8192s", optbuf);
		if (scancount == EOF)
			break;

		if (scancount == 0)
			xfail("Failed to parse input file: %s", fname);

		if (optbuf[sizeof(optbuf)-1] != '\0')
			xfail("Unable to handle options larger than 8192");

		append_argument(&file_args, optbuf);
	} while (1);

	/* Recursively parse the input arguments */
	parse_arguments(&file_args, output_args, fat_args, depth);

	/* Clean up */
	xclose(fname, fd);
	for (i = 0; i < file_args.argc; i++)
		free(file_args.argv[i]);
}

/* Parse all arguments from input_args, appending as(1) arguments to
 * output_args, and FAT-specific arguments to fat_args. */
static void parse_arguments (arg_table *input_args,
		arg_table *output_args,
		arg_table *fat_args,
		int depth)
{
	int i;
	for (i = 0; i < input_args->argc; i++) {
		const char *arg = input_args->argv[i];

		const as_flag *flag = NULL;

		/*
		 * Determine whether the argument(s) accept an argument, or are
		 * fat-specific arguments that should not be passed to as(1). We
		 * skip --, which is used to inform as(1) that it should read from
		 * stdin.
		 *
		 * as(1) additionally allows for single-letter flags to be grouped
		 * such that -abc is the same as -a -b -c, so we must extract and
		 * parse those individually.
		 */
		if (arg[0] == '-' && arg[1] != '-') {
			const char *opt = arg+1;

			/* Special case any single-dash "long" opts, eg, -arch. */
			flag = find_flag('\0', opt, true);
			if (flag == NULL) {
				/* Handle grouped single-char flags. We only need to interpret
				 * the first argument that matches a known flag; we can leave
				 * detection of missing arguments, etc, to as(1). */
				for (opt = arg+1; *opt != '\0'; opt++) {
					flag = find_flag(*opt, NULL, true);
					if (flag != NULL)
						break;
				}
			}
		} else if (arg[0] == '-' && arg[1] == '-' && arg[2] != '\0') {
			const char *opt = arg+2;
			flag = find_flag('\0', opt, false);
		} else if (arg[0] == '@' && arg[1] != '\0') {
			/* Recursively parse the argument @FILE */
			parse_argument_file(arg+1, output_args, fat_args, depth + 1);
		}

		if (flag != NULL && flag->fat_arg) {
			append_argument(fat_args, arg);
			if (flag->accepts_arg) {
				i++;
				if (i < input_args->argc)
					append_argument(fat_args, input_args->argv[i]);
			}
		} else {
			append_argument(output_args, arg);

			/* If the argument accepts a flag, drop the flag into place */
			if (flag != NULL && flag->accepts_arg) {
				i++;
				if (i < input_args->argc)
					append_argument(output_args, input_args->argv[i]);
			}
		}
	}
}

int main(int argc, const char **argv)
{
	const char *fat_arch;
	arg_table input_args;
	arg_table as_args;
	arg_table fat_args;
	int i;

	/* Not that this should be possible */
	if (argc < 1)
		return 1;

	/* Determine the install prefix of our binary */
	char *prefix = realpath(argv[0], NULL);
	{
		if (prefix == NULL)
			xfail("Could not resolve absolute path to %s", argv[0]);

		char *prefix_tail = rindex(prefix, '/');
		if (prefix_tail == NULL)
			xfail("Could not find enclosing directory of path %s", prefix);
		*prefix_tail = '\0';
	}

	/* Configure our input/output argument tables */
	as_args.argc = 0;
	as_args.argv_length = 0;
	as_args.argv = NULL;
	append_argument(&as_args, "as");

	fat_args.argc = 0;
	fat_args.argv_length = 0;
	fat_args.argv = NULL;

	input_args.argc = argc - 1;
	input_args.argv_length = input_args.argc;
	input_args.argv = (char **) argv + 1;

	/* Parse all input arguments */
	parse_arguments(&input_args, &as_args, &fat_args, 0);

	/* Parse our fat arguments */
	fat_arch = NULL;
	for (i = 0; i < fat_args.argc; i++) {
		const char *arg = fat_args.argv[i];
		if (strcmp(arg, "-arch") == 0) {
			if (fat_arch != NULL)
				xfail("more than one -arch option (not allowed, use cc(1) instead)");

			i++;
			if (i >= fat_args.argc)
				xfail("missing argument to -arch option");

			fat_arch = fat_args.argv[i];
		} else {
			/* Should never happen; these arguments have already
			 * been validated against the list of defined fat arguments. */
			xfail("Unknown argument %s", arg);
		}
	}

	// TODO! Correct path to as(1)
	free(as_args.argv[0]);
	as_args.argv[0] = xstrdup("as-todo");

	printf("as arguments: ");
	for (i = 0; i < as_args.argc; i++) {
		printf("%s ", as_args.argv[i]);
	}
	printf("\n");

	printf("fat arguments: ");
	for (i = 0; i < fat_args.argc; i++) {
		printf("%s ", fat_args.argv[i]);
	}
	printf("\n");

	if (fat_arch != NULL)
		printf("FAT Arch: %s\n", fat_arch);

	/* Clean up */
	free(prefix);
	for (i = 0; i < as_args.argc; i++)
		free(as_args.argv[i]);

	for (i = 0; i < fat_args.argc; i++)
		free(fat_args.argv[i]);

	return 0;
} // main

// end of fatelf-as.c ...
