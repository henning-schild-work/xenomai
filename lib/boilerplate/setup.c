/*
 * Copyright (C) 2015 Philippe Gerum <rpm@xenomai.org>.
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.

 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA.
 */
#include <sys/types.h>
#include <sys/mman.h>
#include <sched.h>
#include <getopt.h>
#include <string.h>
#include <errno.h>
#include <stdlib.h>
#include <unistd.h>
#include <memory.h>
#include <malloc.h>
#include <assert.h>
#include <xeno_config.h>
#include <boilerplate/setup.h>
#include <boilerplate/lock.h>
#include <boilerplate/debug.h>
#include <boilerplate/ancillaries.h>

struct base_setup_data __base_setup_data = {
	.no_mlock = 0,
	.no_sanity = !CONFIG_XENO_SANITY,
	.silent_mode = 0,
	.arg0 = NULL,
};

pid_t __node_id;

static DEFINE_PRIVATE_LIST(skins);

static const struct option base_options[] = {
	{
#define help_opt	0
		.name = "help",
	},
	{
#define no_mlock_opt	1
		.name = "no-mlock",
		.flag = &__base_setup_data.no_mlock,
		.val = 1
	},
	{
#define affinity_opt	2
		.name = "cpu-affinity",
		.has_arg = 1,
	},
	{
#define silent_opt	3
		.name = "silent",
		.flag = &__base_setup_data.silent_mode,
		.val = 1
	},
	{
#define version_opt	4
		.name = "version",
	},
	{
#define dumpconfig_opt	5
		.name = "dump-config",
	},
	{
#define no_sanity_opt	6
		.name = "no-sanity",
		.flag = &__base_setup_data.no_sanity,
		.val = 1
	},
	{
#define sanity_opt	7
		.name = "sanity",
		.flag = &__base_setup_data.no_sanity,
	},
	{
		/* sentinel */
	}
};

static inline void print_version(void)
{
	fprintf(stderr, "%s\n", xenomai_version_string);
}

static inline void dump_configuration(void)
{
	int n;

	for (n = 0; config_strings[n]; n++)
		puts(config_strings[n]);
}

static int collect_cpu_affinity(const char *cpu_list)
{
	char *s = strdup(cpu_list), *p, *n;
	int ret, cpu;

	n = s;
	while ((p = strtok(n, ",")) != NULL) {
		cpu = atoi(p);
		if (cpu >= CPU_SETSIZE) {
			free(s);
			warning("invalid CPU number '%d'", cpu);
			return -EINVAL;
		}
		CPU_SET(cpu, &__base_setup_data.cpu_affinity);
		n = NULL;
	}

	free(s);

	/*
	 * Check we may use this affinity, at least one CPU from the
	 * given set should be available for running threads. Since
	 * CPU affinity will be inherited by children threads, we only
	 * have to set it here.
	 *
	 * NOTE: we don't clear __base_setup_data.cpu_affinity on
	 * entry to this routine to allow cumulative --cpu-affinity
	 * options to appear in the command line arguments.
	 */
	ret = sched_setaffinity(0, sizeof(__base_setup_data.cpu_affinity),
				&__base_setup_data.cpu_affinity);
	if (ret) {
		warning("invalid CPU in affinity list '%s'", cpu_list);
		return -errno;
	}

	return 0;
}

static inline char **prep_args(int argc, char *const argv[], int *largcp)
{
	int in, out, n, maybe_arg, lim;
	char **uargv, *p;

	uargv = malloc(argc * sizeof(char *));
	if (uargv == NULL)
		return NULL;

	for (n = 0; n < argc; n++) {
		uargv[n] = strdup(argv[n]);
		if (uargv[n] == NULL)
			return NULL;
	}

	lim = argc;
	in = maybe_arg = 0;
	while (in < lim) {
		if ((uargv[in][0] == '-' && uargv[in][1] != '-') ||
		    (maybe_arg && uargv[in][0] != '-')) {
			p = strdup(uargv[in]);
			for (n = in, out = in + 1; out < argc; out++, n++) {
				free(uargv[n]);
				uargv[n] = strdup(uargv[out]);
			}
			free(uargv[argc - 1]);
			uargv[argc - 1] = p;
			if (*p == '-')
				maybe_arg = 1;
			lim--;
		} else {
			in++;
			maybe_arg = 0;
		}
	}

	*largcp = lim;

	return uargv;
}

static inline void pack_args(int *argcp, int *largcp, char **argv)
{
	int in, out;

	for (in = out = 0; in < *argcp; in++) {
		if (*argv[in])
			argv[out++] = argv[in];
		else {
			free(argv[in]);
			(*largcp)--;
		}
	}

	*argcp = out;
}

static struct option *build_option_array(int *base_opt_startp)
{
	struct skin_descriptor *skin;
	struct option *options, *q;
	const struct option *p;
	int nopts;

	nopts = sizeof(base_options) / sizeof(base_options[0]);

	if (!pvlist_empty(&skins)) {
		pvlist_for_each_entry(skin, &skins, __reserved.next) {
			p = skin->options;
			if (p) {
				while (p->name) {
					nopts++;
					p++;
				}
			}
		}
	}

	options = malloc(sizeof(*options) * nopts);
	if (options == NULL)
		return NULL;

	q = options;

	if (!pvlist_empty(&skins)) {
		pvlist_for_each_entry(skin, &skins, __reserved.next) {
			p = skin->options;
			if (p) {
				skin->__reserved.opt_start = q - options;
				while (p->name)
					memcpy(q++, p++, sizeof(*q));
			}
			skin->__reserved.opt_end = q - options;
		}
	}

	*base_opt_startp = q - options;
	memcpy(q, base_options, sizeof(base_options));

	return options;
}

static void usage(void)
{
	struct skin_descriptor *skin;

	print_version();
        fprintf(stderr, "usage: program <options>, where options may be:\n");
        fprintf(stderr, "--no-mlock                       do not lock memory at init (Mercury only)\n");
        fprintf(stderr, "--cpu-affinity=<cpu[,cpu]...>    set CPU affinity of threads\n");
        fprintf(stderr, "--[no-]sanity                    disable/enable sanity checks\n");
        fprintf(stderr, "--silent                         tame down verbosity\n");
        fprintf(stderr, "--version                        get version information\n");
        fprintf(stderr, "--dump-config                    dump configuration settings\n");

	if (pvlist_empty(&skins))
		return;

	pvlist_for_each_entry(skin, &skins, __reserved.next) {
		if (skin->help)
			skin->help();
	}
}

static int parse_base_options(int *argcp, char *const **argvp,
			      int *largcp, char ***uargvp,
			      const struct option *options,
			      int base_opt_start)
{
	int c, lindex, ret, n;
	char **uargv;

	/*
	 * Prepare a user argument vector we can modify, copying the
	 * one we have been given by the application code in
	 * xenomai_init(). This vector will be expunged from Xenomai
	 * proper options as we discover them.
	 */
	uargv = prep_args(*argcp, *argvp, largcp);
	if (uargv == NULL)
		return -ENOMEM;

	__base_setup_data.arg0 = uargv[0];
	*uargvp = uargv;
	opterr = 0;

	/*
	 * NOTE: since we pack the argument vector on the fly while
	 * processing the options, optarg should be considered as
	 * volatile by skin option handlers; i.e. strdup() is required
	 * if the value has to be retained. Values from the user
	 * vector returned by xenomai_init() live in permanent memory
	 * though.
	 */

	for (;;) {
		lindex = -1;
		c = getopt_long(*largcp, uargv, "", options, &lindex);
		if (c == EOF)
			break;
		if (lindex == -1)
			continue;

		switch (lindex - base_opt_start) {
		case affinity_opt:
			ret = collect_cpu_affinity(optarg);
			if (ret)
				return ret;
			break;
		case no_mlock_opt:
		case no_sanity_opt:
		case sanity_opt:
		case silent_opt:
			break;
		case version_opt:
			print_version();
			exit(0);
		case dumpconfig_opt:
			dump_configuration();
			exit(0);
		case help_opt:
			usage();
			exit(0);
		default:
			/* Skin option, don't process yet. */
			continue;
		}

		/*
		 * Clear the first byte of the base option we found
		 * (including any companion argument), pack_args()
		 * will expunge all options we have already handled.
		 *
		 * NOTE: this code relies on the fact that only long
		 * options with double-dash markers can be parsed here
		 * after prep_args() did its job (we do not support
		 * -foo as a long option). This is aimed at reserving
		 * use of short options to the application layer,
		 * sharing only the long option namespace with the
		 * Xenomai core libs.
		 */
		n = optind - 1;
		if (uargv[n][0] != '-' || uargv[n][1] != '-')
			/* Clear the separate argument value. */
			uargv[n--][0] = '\0';
		uargv[n][0] = '\0'; /* Clear the option switch. */
	}

	pack_args(argcp, largcp, uargv);

	optind = 0;

	return 0;
}

static int parse_skin_options(int *argcp, int largc, char **uargv,
			      const struct option *options)
{
	struct skin_descriptor *skin;
	int lindex, n, c, ret;

	for (;;) {
		lindex = -1;
		c = getopt_long(largc, uargv, "", options, &lindex);
		if (c == EOF)
			break;
		if (lindex == -1)
			continue; /* Not handled here. */
		pvlist_for_each_entry(skin, &skins, __reserved.next) {
			if (skin->parse_option == NULL)
				continue;
			if (lindex < skin->__reserved.opt_start ||
			    lindex >= skin->__reserved.opt_end)
				continue;
			lindex -= skin->__reserved.opt_start;
			ret = skin->parse_option(lindex, optarg);
			if (ret == 0)
				break;
			return ret;
		}
		n = optind - 1;
		if (uargv[n][0] != '-' || uargv[n][1] != '-')
			/* Clear the separate argument value. */
			uargv[n--][0] = '\0';
		uargv[n][0] = '\0'; /* Clear the option switch. */
	}

	pack_args(argcp, &largc, uargv);

	optind = 0;

	return 0;
}

void xenomai_init(int *argcp, char *const **argvp)
{
	int ret, largc, base_opt_start;
	struct skin_descriptor *skin;
	struct option *options;
	static int init_done;
	char **uargv = NULL;
	struct service svc;

	if (init_done) {
		warning("duplicate call to %s() ignored", __func__);
		warning("(xeno-config --no-auto-init disables implicit call)");
		return;
	}

	boilerplate_init();

	/* Our node id. is the tid of the main thread. */
	__node_id = get_thread_pid();

	/* No ifs, no buts: we must be called over the main thread. */
	assert(getpid() == __node_id);

	/* Define default CPU affinity, i.e. no particular affinity. */
	CPU_ZERO(&__base_setup_data.cpu_affinity);

	/*
	 * Build the global option array, merging the base and
	 * per-skin option sets.
	 */
	options = build_option_array(&base_opt_start);
	if (options == NULL) {
		ret = -ENOMEM;
		goto fail;
	}

	/*
	 * Parse the base options first, to bootstrap the core with
	 * the right config values.
	 */
	ret = parse_base_options(argcp, argvp, &largc, &uargv,
				 options, base_opt_start);
	if (ret)
		goto fail;

#ifndef CONFIG_SMP
	if (__base_setup_data.no_sanity == 0) {
		ret = get_static_cpu_count();
		if (ret > 0)
			early_panic("running non-SMP libraries on SMP kernel?\n"
	    "              build with --enable-smp or disable check with --no-sanity");
	}
#endif

	ret = debug_init();
	if (ret) {
		warning("failed to initialize debugging features");
		goto fail;
	}

#ifdef CONFIG_XENO_MERCURY
	if (__base_setup_data.no_mlock == 0) {
		ret = mlockall(MCL_CURRENT | MCL_FUTURE);
		if (ret) {
			ret = -errno;
			warning("failed to lock memory");
			goto fail;
		}
	}
#endif

	/*
	 * Now that we have bootstrapped the core, we may call the
	 * skin handlers for parsing their own options, which in turn
	 * may create system objects on the fly.
	 */
	if (!pvlist_empty(&skins)) {
		ret = parse_skin_options(argcp, largc, uargv, options);
		if (ret)
			goto fail;

		CANCEL_DEFER(svc);

		pvlist_for_each_entry(skin, &skins, __reserved.next) {
			ret = skin->init();
			if (ret)
				break;
		}

		CANCEL_RESTORE(svc);

		if (ret) {
			warning("skin %s won't initialize", skin->name);
			goto fail;
		}
	}

	free(options);

#ifdef CONFIG_XENO_DEBUG
	if (__base_setup_data.silent_mode == 0) {
		warning("Xenomai compiled with %s debug enabled,\n"
			"                              "
			"%shigh latencies expected [--enable-debug=%s]",
#ifdef CONFIG_XENO_DEBUG_FULL
			"full", "very ", "full"
#else
			"partial", "", "partial"
#endif
			);
	}
#endif

	/*
	 * The final user arg vector only contains options we could
	 * not handle. The caller should be able to process them, or
	 * bail out.
	 */
	*argvp = uargv;
	init_done = 1;

	return;
fail:
	early_panic("initialization failed, %s", symerror(ret));
}

void __register_skin(struct skin_descriptor *p)
{
	pvlist_append(&p->__reserved.next, &skins);
}