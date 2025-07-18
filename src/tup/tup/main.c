/* vim: set ts=8 sw=8 sts=8 noet tw=78:
 *
 * tup - A file-based build system
 *
 * Copyright (C) 2008-2024  Mike Shal <marfey@gmail.com>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License along
 * with this program; if not, write to the Free Software Foundation, Inc.,
 * 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA.
 */

#ifdef __linux__
#define _GNU_SOURCE
#include <sched.h>
#endif

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <errno.h>
#include "tup/colors.h"
#include "tup/config.h"
#include "tup/lock.h"
#include "tup/monitor.h"
#include "tup/fileio.h"
#include "tup/pel_group.h"
#include "tup/updater.h"
#include "tup/debug.h"
#include "tup/graph.h"
#include "tup/db.h"
#include "tup/init.h"
#include "tup/version.h"
#include "tup/path.h"
#include "tup/entry.h"
#include "tup/varsed.h"
#include "tup/access_event.h"
#include "tup/option.h"
#include "tup/privs.h"
#include "tup/flist.h"
#include "tup/vardb.h"
#include "tup/variant.h"
#include "tup/container.h"
#include "tup/array_size.h"
#include "tup/luaparser.h"

static struct help {
	const char *command;
	const char *altcommand;
	const char *args;
	const char *desc;
} helpers[] = {
	{"init", NULL, "[directory]", "Creates a '.tup' directory in the specified directory and initializes the tup database. If a directory name is unspecified, it defaults to creating '.tup' in the current directory. This defines the top of your project, as viewed by tup."},
	{"upd", NULL, "[<output_1> ... <output_n>]", "Legacy secondary command. Calling 'tup upd' is equivalent to simply calling 'tup'."},
	{"refactor", "ref", "", "The refactor command can be used to help refactor Tupfiles. This will cause tup to run through the parsing phase, but not execute any commands. If any Tupfiles that are parsed result in changes to the database, these are reported as errors."},
	{"monitor", NULL, "", "*LINUX ONLY* Starts the inotify-based file monitor. The monitor must scan the filesystem once and initialize watches on each directory. Then when you make changes to the files, the monitor will see them and write them directly into the database. With the monitor running, 'tup' does not need to do the initial scan, and can start constructing the build graph immediately."},
	{"stop", NULL, "", "Kills the monitor if it is running."},
	{"variant", NULL, "foo.config [bar.config] [...]", "For each argument, this command creates a variant directory with tup.config symlinked (Windows: copied) to the specified config file."},
	{"dbconfig", NULL, "", "Displays the current tup database configuration. These are internal values used by tup."},
	{"options", NULL, "", "Displays all of the current tup options, as well as where they originated."},
	{"graph", NULL, "[--dirs] [--ghosts] [--env] [--combine] [--stickies] [<output_1> ... <output_n>]", "Prints out a graphviz .dot format graph of the tup database to stdout. By default it only displays the parts of the graph that have changes. If you provide additional arguments, they are assumed to be files that you want to graph."},
	{"todo", NULL, "[<output_1> ... <output_n>]", "Prints out the next steps in the tup process that will execute when updating the given outputs. If no outputs are specified then it prints the steps needed to update the whole project."},
	{"generate", NULL, "[--config config-file] script.sh (or script.bat on Windows)", "The generate command will parse all Tupfiles and create a shell script that can build the program without running in a tup environment. The expected usage is in continuous integration environments that aren't compatible with tup's dependency checking (eg: if FUSE is not supported). On Windows, if the script filename has a \".bat\" extension, then the output will be a batch script instead of a shell script."},
	{"varsed", NULL, "", "The varsed command is used as a subprogram in a Tupfile; you would not run it manually at the command-line. It is used to read one file, and replace any variable references and write the output to a second file. Variable references are of the form @VARIABLE@, and are replaced with the corresponding value of the @-variable."},
	{"scan", NULL, "", "You shouldn't ever need to run this, unless you want to make the database reflect the filesystem before running 'tup graph'. Scan is called automatically by 'upd' if the monitor isn't running."},
};

static void print_help(struct help *h, const char *argv0);
static int entry(int argc, char **argv);
static int type(int argc, char **argv);
static int tupid(int argc, char **argv);
static int inputs(int argc, char **argv);
static int graph_cb(void *arg, struct tup_entry *tent);
static int graph(int argc, char **argv);
static int compiledb(int argc, char **argv);
static int commandline(int argc, char **argv);
/* Testing commands */
static int mlink(int argc, char **argv);
static int variant(int argc, char **argv);
static int node_exists(int argc, char **argv);
static int link_exists(const char *cmd, int argc, char **argv);
static int touch(int argc, char **argv);
static int node(int argc, char **argv);
static int varshow(int argc, char **argv);
static int dbconfig(int argc, char **argv);
static int options(int argc, char **argv);
static int fake_mtime(int argc, char **argv);
static int fake_parser_version(int argc, char **argv);
static int waitmon(void);
static int flush(void);
static int ghost_check(void);

static void version(void);

int main(int argc, char **argv)
{
	int rc = 0;
	int x;
	int cmd_arg = 0;
	const char *cmd = NULL;
	int clear_autoupdate = 0;
	int show_help = 0;
	int orig_argc;
	char *tupexe = argv[0];
	char **orig_argv;

	/* Skip 'tup' executable argument */
	argc--;
	argv++;
	orig_argc = argc;
	orig_argv = argv;
	for(x=0; x<argc; x++) {
		if(!cmd && argv[x][0] != '-') {
			cmd = argv[x];
			cmd_arg = x;
		}
		if(strcmp(argv[x], "--debug-sql") == 0) {
			tup_db_enable_sql_debug();
		} else if(strcmp(argv[x], "--debug-fuse") == 0) {
			server_enable_debug();
		} else if(strcmp(argv[x], "-h") == 0 ||
			  strcmp(argv[x], "--help") == 0) {
			show_help = 1;
		}
	}

	if(show_help) {
		if(!cmd) {
			fprintf(stderr, "%s [--debug-sql] [--debug-fuse] [SECONDARY_COMMAND] [ARGS]\n\n", tupexe);
			fprintf(stderr, "tup [<output_1> ... <output_n>]\n");
			fprintf(stderr, "\nUpdates the set of outputs based on the dependency graph and the current state of the filesystem. If no outputs are specified then the whole project is updated.\n");
			fprintf(stderr, "\nSECONDARY COMMANDS\n\n");
			for(x=0; x<ARRAY_SIZE(helpers); x++) {
				fprintf(stderr, "%s %s\n", tupexe, helpers[x].command);
			}
		} else {
			for(x=0; x<ARRAY_SIZE(helpers); x++) {
				if(strcmp(cmd, helpers[x].command) == 0 ||
				   (helpers[x].altcommand && strcmp(cmd, helpers[x].altcommand) == 0)) {
					print_help(&helpers[x], tupexe);
					return 0;
				}
			}
			fprintf(stderr, "tup: No help found for secondary command: %s\n", cmd);
		}
		return 0;
	}

	if(NULL == cmd) {
		cmd = "upd";
	} else {
		argc = argc - (cmd_arg + 1);
		argv = &argv[cmd_arg + 1];
	}

	if(argc > 0) {
		if(strcmp(argv[0], "--version") == 0 ||
		   strcmp(argv[0], "-v") == 0) {
			version();
			return 0;
		}
	}

	/* Commands that can run as a sub-process to tup (eg: in a :-rule) */
	if(strcmp(cmd, "varsed") == 0) {
		if(tup_drop_privs() < 0)
			return 1;
		return varsed(argc, argv);
	}

	/* Check if we are a sub-process by looking for the vardict environment
	 * variable that gets set.
	 */
	if(getenv(TUP_VARDICT_NAME)) {
		fprintf(stderr, "tup error: Command '%s' is not valid when running as a sub-process, or is unknown.\n", cmd);
		return -1;
	}

	/* Commands that should run before running an implicit `tup init' */
	if(strcmp(cmd, "init") == 0) {
		if(tup_drop_privs() < 0)
			return 1;
		return init_command(argc, argv);
	} else if(strcmp(cmd, "version") == 0) {
		if(tup_drop_privs() < 0)
			return 1;
		version();
		return 0;
	} else if(strcmp(cmd, "generate") == 0) {
		if(generate(argc, argv) < 0)
			return 1;
		tup_valgrind_cleanup();
		return 0;
	} else if(strcmp(cmd, "privileged") == 0) {
#ifdef __linux__
		if(unshare(CLONE_NEWUSER) == 0) {
			return 1;
		}
#endif
		return tup_privileged();
	} else if(strcmp(cmd, "server") == 0) {
		printf("%s\n", TUP_SERVER);
		return 0;
	}

	/* Process all of the Tupfile.ini files. Runs `tup init' if necessary */
	tup_temporarily_drop_privs();
	if(tup_option_process_ini() != 0)
		return 1;
	tup_restore_privs();

	/* Commands that don't use a normal tup_init() */
	if(strcmp(cmd, "stop") == 0) {
		if(tup_drop_privs() < 0)
			return 1;
		if(find_tup_dir() < 0) {
			fprintf(stderr, "No .tup directory found - unable to stop the file monitor.\n");
			return -1;
		}
		if(open_tup_top() < 0)
			return -1;
		return stop_monitor(TUP_MONITOR_SHUTDOWN);
	} else if(strcmp(cmd, "waitmon") == 0) {
		if(tup_drop_privs() < 0)
			return 1;
		if(find_tup_dir() < 0) {
			fprintf(stderr, "No .tup directory found - unable to stop the file monitor.\n");
			return -1;
		}
		if(open_tup_top() < 0)
			return -1;
		if(waitmon() < 0)
			return 1;
		return 0;
	}

	/* Pass all arguments so we capture any flags before the command */
	if(tup_init(orig_argc, orig_argv) < 0)
		return 1;

	if(strcmp(cmd, "monitor") == 0) {
		rc = monitor(argc, argv);
	} else if(strcmp(cmd, "entry") == 0) {
		rc = entry(argc, argv);
	} else if(strcmp(cmd, "type") == 0) {
		rc = type(argc, argv);
	} else if(strcmp(cmd, "tupid") == 0) {
		rc = tupid(argc, argv);
	} else if(strcmp(cmd, "inputs") == 0) {
		rc = inputs(argc, argv);
	} else if(strcmp(cmd, "graph") == 0) {
		rc = graph(argc, argv);
	} else if(strcmp(cmd, "compiledb") == 0) {
		rc = compiledb(argc, argv);
	} else if(strcmp(cmd, "commandline") == 0) {
		rc = commandline(argc, argv);
	} else if(strcmp(cmd, "scan") == 0) {
		int pid;
		if(monitor_get_pid(0, &pid) < 0)
			return -1;
		if(pid > 0) {
			fprintf(stderr, "tup error: monitor appears to be running as pid %i - not doing scan.\n - Run 'tup stop' if you want to kill the monitor and use scan instead.\n", pid);
			rc = 1;
		} else {
			rc = tup_scan();
		}
	} else if(strcmp(cmd, "link") == 0) {
		rc = mlink(argc, argv);
	} else if(strcmp(cmd, "read") == 0) {
		rc = updater(argc, argv, 1);
	} else if(strcmp(cmd, "parse") == 0) {
		rc = updater(argc, argv, 2);
	} else if(strcmp(cmd, "upd") == 0) {
		rc = updater(argc, argv, 0);
	} else if(strcmp(cmd, "refactor") == 0 ||
		  strcmp(cmd, "ref") == 0) {
		rc = updater(argc, argv, -2);
	} else if(strcmp(cmd, "autoupdate") == 0) {
		rc = updater(argc, argv, 0);
		clear_autoupdate = 1;
	} else if(strcmp(cmd, "autoparse") == 0) {
		rc = updater(argc, argv, 2);
		clear_autoupdate = 1;
	} else if(strcmp(cmd, "todo") == 0) {
		rc = todo(argc, argv);
	} else if(strcmp(cmd, "variant") == 0) {
		rc = variant(argc, argv);
	} else if(strcmp(cmd, "node_exists") == 0) {
		rc = node_exists(argc, argv);
	} else if(strcmp(cmd, "normal_exists") == 0 ||
		  strcmp(cmd, "sticky_exists") == 0) {
		rc = link_exists(cmd, argc, argv);
		/* Since an error code of <0 gets converted to 1, we have to
		 * make 1 (meaning the link exists) something else.
		 */
		if(rc == 1)
			rc = 11;
	} else if(strcmp(cmd, "flags_exists") == 0) {
		rc = tup_db_check_flags(TUP_FLAGS_CONFIG | TUP_FLAGS_CREATE | TUP_FLAGS_MODIFY | TUP_FLAGS_TRANSIENT);
	} else if(strcmp(cmd, "create_flags_exists") == 0) {
		rc = tup_db_check_flags(TUP_FLAGS_CREATE);
	} else if(strcmp(cmd, "touch") == 0) {
		rc = touch(argc, argv);
	} else if(strcmp(cmd, "node") == 0) {
		rc = node(argc, argv);
	} else if(strcmp(cmd, "varshow") == 0) {
		rc = varshow(argc, argv);
	} else if(strcmp(cmd, "dbconfig") == 0) {
		rc = dbconfig(argc, argv);
	} else if(strcmp(cmd, "options") == 0) {
		rc = options(argc, argv);
	} else if(strcmp(cmd, "fake_mtime") == 0) {
		rc = fake_mtime(argc, argv);
	} else if(strcmp(cmd, "fake_parser_version") == 0) {
		rc = fake_parser_version(argc, argv);
	} else if(strcmp(cmd, "flush") == 0) {
		rc = flush();
	} else if(strcmp(cmd, "ghost_check") == 0) {
		rc = ghost_check();
	} else if(strcmp(cmd, "monitor_supported") == 0) {
		rc = monitor_supported();
	} else {
		/* Use the original arguments, since the arg we pulled out for
		 * the cmd is actually a file to update, not a command.
		 */
		rc = updater(orig_argc, orig_argv, 0);
	}

	if(clear_autoupdate) {
		if(tup_db_begin() < 0)
			return -1;
		if(tup_db_config_set_int(AUTOUPDATE_PID, -1) < 0) {
			fprintf(stderr, "tup error: Unable to clear the autoupdate pid.\n");
			rc = 1;
		}
		if(tup_db_commit() < 0)
			return -1;
	}

	if(tup_cleanup() < 0)
		rc = 1;
	tup_valgrind_cleanup();
	if(rc < 0)
		return 1;
	return rc;
}

static void print_help(struct help *h, const char *tupexe)
{
	fprintf(stderr, "%s %s %s\n", tupexe, h->command, h->args);
	if(h->altcommand) {
		fprintf(stderr, "%s %s %s\n", tupexe, h->altcommand, h->args);
	}
	fprintf(stderr, "\n%s\n", h->desc);
}

static int entry(int argc, char **argv)
{
	struct tup_entry *tent;
	int x;

	if(tup_db_begin() < 0)
		return -1;
	for(x=0; x<argc; x++) {
		char *endptr;
		tupid_t tupid;

		tupid = strtol(argv[x], &endptr, 10);
		if(!*endptr) {
			if(tup_entry_add(tupid, &tent) < 0)
				return -1;
		} else {
			if(gimme_tent(argv[x], &tent) < 0) {
				fprintf(stderr, "No tent :(\n");
				return -1;
			}
		}
		if(tent) {
			print_tup_entry(stdout, tent);
			printf("\n");
		}
	}
	if(tup_db_commit() < 0)
		return -1;
	return 0;
}

static int type(int argc, char **argv)
{
	struct tup_entry *tent;
	int x;

	if(tup_db_begin() < 0)
		return -1;
	for(x=0; x<argc; x++) {
		char *endptr;
		tupid_t tupid;

		tupid = strtol(argv[x], &endptr, 10);
		if(!*endptr) {
			if(tup_entry_add(tupid, &tent) < 0)
				return -1;
		} else {
			if(gimme_tent(argv[x], &tent) < 0) {
				fprintf(stderr, "No tent :(\n");
				return -1;
			}
		}
		if(tent) {
			printf("%s\n", tup_db_type(tent->type));
		}
	}
	if(tup_db_commit() < 0)
		return -1;
	return 0;
}

static int tupid(int argc, char **argv)
{
	struct tup_entry *tent;
	int x;

	if(tup_db_begin() < 0)
		return -1;
	for(x=0; x<argc; x++) {
		if(gimme_tent(argv[x], &tent) < 0) {
			fprintf(stderr, "No tent :(\n");
			return -1;
		}
		if(tent) {
			printf("%lli\n", tent->tnode.tupid);
		} else {
			fprintf(stderr, "tup error: entry not found for '%s'\n", argv[x]);
			return -1;
		}
	}
	if(tup_db_commit() < 0)
		return -1;
	return 0;
}

static int inputs(int argc, char **argv)
{
	int x;

	if(tup_db_begin() < 0)
		return -1;
	for(x=0; x<argc; x++) {
		struct tent_entries inputs = TENT_ENTRIES_INITIALIZER;
		struct tent_tree *tt;
		tupid_t cmdid;

		cmdid = strtol(argv[x], NULL, 10);
		if(cmdid <= 0) {
			fprintf(stderr, "tup error: %s is not a valid command ID.\n", argv[x]);
			return -1;
		}
		if(tup_db_get_inputs(cmdid, NULL, &inputs, NULL) < 0)
			return -1;
		RB_FOREACH(tt, tent_entries, &inputs) {
			if(tt->tent->type != TUP_NODE_GHOST) {
				print_tup_entry(stdout, tt->tent);
				printf("\n");
			}
		}
	}
	if(tup_db_commit() < 0)
		return -1;
	return 0;
}

static int show_dirs;
static int show_ghosts;
static int show_env;
static int graph_cb(void *arg, struct tup_entry *tent)
{
	struct graph *g = arg;
	struct node *n;

	if(!show_ghosts && tent->type == TUP_NODE_GHOST)
		return 0;
	if(!show_env) {
		if(tent->tnode.tupid == env_dt() ||
		   tent->dt == env_dt())
			return 0;
	}
	/* We need to load dirs/generated dirs when !g->cur, because that is
	 * how we get 'tup graph dir' to load all nodes/sub-nodes of the
	 * directory - the actual dir nodes are pruned in dump_graph():
	 */
	if(!show_dirs && g->cur) {
		if(tent->type == TUP_NODE_DIR ||
		   tent->type == TUP_NODE_GENERATED_DIR)
			return 0;
	}

	n = find_node(g, tent->tnode.tupid);
	if(n != NULL)
		goto edge_create;
	n = create_node(g, tent);
	if(!n)
		return -1;

edge_create:
	if(n->expanded == 0) {
		n->expanded = 1;
		if(node_remove_list(&g->node_list, n) < 0)
			return -1;
		if(node_insert_head(&g->plist, n) < 0)
			return -1;
	}
	if(g->cur)
		if(create_edge(g->cur, n, TUP_LINK_NORMAL) < 0)
			return -1;
	return 0;
}

static int graph(int argc, char **argv)
{
	int x;
	struct graph g;
	struct node *n;
	int combine;
	int default_graph = 1;
	int stickies = 0;
	int pruned = -1;
	tupid_t tupid;
	tupid_t sub_dir_dt;

	if(tup_db_begin() < 0)
		return -1;
	show_dirs = tup_option_get_flag("graph.dirs");
	show_ghosts = tup_option_get_flag("graph.ghosts");
	show_env = tup_option_get_flag("graph.environment");
	combine = tup_option_get_flag("graph.combine");

	if(create_graph(&g, -1) < 0)
		return -1;

	sub_dir_dt = get_sub_dir_dt();
	if(sub_dir_dt < 0)
		return -1;

	for(x=0; x<argc; x++) {
		struct tup_entry *tent;

		if(strcmp(argv[x], "--dirs") == 0) {
			show_dirs = 1;
			continue;
		}
		if(strcmp(argv[x], "--ghosts") == 0) {
			show_ghosts = 1;
			continue;
		}
		if(strcmp(argv[x], "--env") == 0) {
			show_env = 1;
			continue;
		}
		if(strcmp(argv[x], "--combine") == 0) {
			combine = 1;
			continue;
		}
		if(strcmp(argv[x], "--stickies") == 0) {
			stickies = 1;
			continue;
		}
		if(strcmp(argv[x], "--prune") == 0) {
			pruned = x;
			break;
		}

		tent = get_tent_dt(sub_dir_dt, argv[x]);
		if(!tent) {
			fprintf(stderr, "Unable to find tupid for: '%s'\n", argv[x]);
			return -1;
		}

		n = find_node(&g, tent->tnode.tupid);
		if(n == NULL) {
			n = create_node(&g, tent);
			if(!n)
				return -1;
			n->expanded = 1;
			if(node_remove_list(&g.node_list, n) < 0)
				return -1;
			if(node_insert_head(&g.plist, n) < 0)
				return -1;
		}
		default_graph = 0;
	}
	if(default_graph) {
		if(tup_db_select_node_by_flags(graph_cb, &g, TUP_FLAGS_CREATE) < 0)
			return -1;
		if(tup_db_select_node_by_flags(graph_cb, &g, TUP_FLAGS_MODIFY) < 0)
			return -1;
	}

	while(!TAILQ_EMPTY(&g.plist)) {
		g.cur = TAILQ_FIRST(&g.plist);
		if(tup_db_select_node_by_link(graph_cb, &g, g.cur->tnode.tupid) < 0)
			return -1;
		if(g.cur->tent->type == TUP_NODE_GROUP) {
			if(tup_db_select_node_by_distinct_group_link(build_graph_group_cb, &g, g.cur->tnode.tupid) < 0)
				return -1;
		}
		if(node_remove_list(&g.plist, g.cur) < 0)
			return -1;
		if(node_insert_head(&g.node_list, g.cur) < 0)
			return -1;

		if(strcmp(g.cur->tent->name.s, TUP_CONFIG) != 0) {
			tupid = g.cur->tnode.tupid;
			g.cur = NULL;
			if(tup_db_select_node_dir(graph_cb, &g, tupid) < 0)
				return -1;
		}
	}

	if(stickies)
		if(add_graph_stickies(&g) < 0)
			return -1;

	if(pruned != -1) {
		int num_pruned;
		if(prune_graph(&g, argc-pruned, argv+pruned, &num_pruned, GRAPH_PRUNE_ALL, 0) < 0)
			return -1;
	}
	dump_graph(&g, stdout, show_dirs, combine);

	destroy_graph(&g);
	if(tup_db_commit() < 0)
		return -1;
	return 0;
}

static int compiledb(int argc, char **argv)
{
	int rc;
	struct variant *variant;
	rc = updater(argc, argv, 2);
	if(rc < 0)
		return -1;

	LIST_FOREACH(variant, get_variant_list(), list) {
		if(variant->enabled) {
			int fd;
			int dfd;

			dfd = tup_entry_open(variant->tent->parent);
			fd = openat(dfd, "compile_commands.json", O_CREAT | O_WRONLY | O_TRUNC, 0666);
			if(fd < 0) {
				perror("compile_commands.json");
				fprintf(stderr, "tup error: Unable to create compile_commands.json at the top of the tup hierarchy.\n");
				return -1;
			}
			FILE *f = fdopen(fd, "w");
			if(tup_db_create_compile_db(f, variant) < 0)
				return -1;
			fclose(f);
			close(dfd);
		}
	}
	return 0;
}

static int commandline(int argc, char **argv)
{
	struct tup_entry *tent;
	int x;

	color_disable();
	if(tup_db_begin() < 0)
		return -1;
	if(variant_load() < 0)
		return -1;
	printf("[\n");
	for(x=0; x<argc; x++) {
		if(gimme_tent(argv[x], &tent) < 0) {
			fprintf(stderr, "No tent :(\n");
			return -1;
		}
		if(tent) {
			if(tup_db_print_commandline(tent) < 0)
				return -1;
		} else {
			fprintf(stderr, "tup error: entry not found for '%s'\n", argv[x]);
			return -1;
		}
	}
	printf("\n]\n");
	if(tup_db_commit() < 0)
		return -1;
	return 0;
}

static int mlink(int argc, char **argv)
{
	/* This only works for files in the top-level directory. It's only
	 * used by the benchmarking suite, and in fact may just disappear
	 * entirely. I wouldn't use it for any other purpose.
	 */
	int type;
	int x;
	tupid_t cmdid;
	struct tup_entry *tent;
	struct tup_entry *root_tent;

	if(argc < 4) {
		fprintf(stderr, "Usage: %s cmd -iread_file -owrite_file\n",
			argv[0]);
		return 1;
	}

	if(tup_db_begin() < 0)
		return -1;
	if(tup_entry_add(DOT_DT, &root_tent) < 0)
		return -1;
	cmdid = create_command_file(DOT_DT, argv[1], NULL, 0, NULL, 0);
	if(cmdid < 0) {
		return -1;
	}

	for(x=2; x<argc; x++) {
		char *name = argv[x];
		if(name[0] == '-') {
			if(name[1] == 'i') {
				type = 0;
			} else if(name[1] == 'o') {
				type = 1;
			} else {
				fprintf(stderr, "Invalid argument: '%s'\n",
					name);
				return 1;
			}
		} else {
			fprintf(stderr, "Invalid argument: '%s'\n", name);
			return 1;
		}

		if(tup_db_select_tent(root_tent, name+2, &tent) < 0)
			return -1;
		if(!tent)
			return 1;

		if(type == 0) {
			if(tup_db_create_link(tent->tnode.tupid, cmdid, TUP_LINK_NORMAL) < 0)
				return -1;
		} else {
			if(tup_db_create_link(cmdid, tent->tnode.tupid, TUP_LINK_NORMAL) < 0)
				return -1;
		}
	}
	if(tup_db_commit() < 0)
		return -1;

	return 0;
}

static int dir_empty(const char *dirname)
{
	struct flist f = FLIST_INITIALIZER;
	if(chdir(dirname) < 0) {
		perror(dirname);
		fprintf(stderr, "tup error: Unable to chdir to variant directory.\n");
		return -1;
	}
	flist_foreach(&f, ".") {
		if(f.filename[0] == '.')
			continue;
		return 0;
	}
	if(fchdir(tup_top_fd()) < 0) {
		perror("fchdir");
		fprintf(stderr, "tup error: Unable to fchdir to the top of the tup hierarchy.\n");
		return -1;
	}
	return 1;
}

static int create_variant(const char *config_path)
{
	char dirname[PATH_MAX];
	char linkpath[PATH_MAX];
	char linkdest[PATH_MAX];
	const char *dot;
	const char *last_slash;
	const char *filename;

	last_slash = strrchr(config_path, '/');
	if(last_slash) {
		filename = last_slash + 1;
	} else {
		filename = config_path;
	}

	dot = strchr(filename, '.');
	if(dot) {
		int len = dot-filename;
		snprintf(dirname, sizeof(dirname), "build-%.*s", len, filename);
	} else {
		snprintf(dirname, sizeof(dirname), "build-%s", filename);
	}
	dirname[sizeof(dirname)-1] = 0;
	if(fchdir(tup_top_fd()) < 0) {
		perror("fchdir");
		fprintf(stderr, "tup error: Unable to fchdir to the top of the tup hierarchy.\n");
		return -1;
	}
	if(mkdir(dirname, 0777) < 0) {
		if(errno == EEXIST) {
			int rc;
			rc = dir_empty(dirname);
			if(rc < 0)
				return -1;
			if(!rc) {
				fprintf(stderr, "tup error: Variant directory '%s' already exists and is not empty.\n", dirname);
				return -1;
			}
		} else {
			perror(dirname);
			fprintf(stderr, "tup error: Unable to create variant directory '%s' at the top of the tup hierarchy.\n", dirname);
			return -1;
		}
	}
	if(get_sub_dir_len()) {
		if(snprintf(linkpath, sizeof(linkpath), "../%s/%s", get_sub_dir(), config_path) >= (signed)sizeof(linkpath)) {
			fprintf(stderr, "tup error: linkpath is too small to fit the tup.config symlink path.\n");
			return -1;
		}
	} else {
		if(snprintf(linkpath, sizeof(linkpath), "../%s", config_path) >= (signed)sizeof(linkpath)) {
			fprintf(stderr, "tup error: linkpath is too small to fit the tup.config symlink path.\n");
			return -1;
		}
	}
	if(snprintf(linkdest, sizeof(linkdest), "%s/tup.config", dirname) >= (signed)sizeof(linkdest)) {
		fprintf(stderr, "tup error: linkdest is too small to fit the tup.config symlink destination.\n");
		return -1;
	}
#ifdef _WIN32
	char srcpath[PATH_MAX];
	if(snprintf(srcpath, sizeof(srcpath), "%s/%s", get_sub_dir(), config_path) >= (signed)sizeof(srcpath)) {
		fprintf(stderr, "tup error: srcpath is too small to fit the tup.config source path.\n");
		return -1;
	}
	if(CopyFileA(srcpath, linkdest, TRUE) == 0) {
		fprintf(stderr, "tup error: Unable to copy the config file %s to destination: %s\n", srcpath, linkdest);
		return -1;
	}
#else
	if(symlink(linkpath, linkdest) < 0) {
		perror(linkdest);
		fprintf(stderr, "tup error: Unable to create tup.config symlink for config file: %s\n", config_path);
		return -1;
	}
#endif
	printf("tup: Added variant '%s' using config file '%s'\n", dirname, config_path);
	return 0;
}

static int variant(int argc, char **argv)
{
	int x;

	if(argc < 1) {
		fprintf(stderr, "Usage: variant foo.config [bar.config] [...]\n");
		fprintf(stderr, "This will create a build-foo directory with a tup.config symlink to foo.config\n");
		return -1;
	}
	if(tup_db_begin() < 0)
		return -1;
	for(x=0; x<argc; x++) {
		if(create_variant(argv[x]) < 0)
			return -1;
	}
	if(tup_db_commit() < 0)
		return -1;
	return 0;
}

static int node_exists(int argc, char **argv)
{
	int x;
	struct tup_entry *tent;
	struct tup_entry *dtent;
	tupid_t dt;

	if(tup_db_begin() < 0)
		return -1;
	if(argc < 2) {
		fprintf(stderr, "Usage: node_exists dir [n1] [n2...]\n");
		return -1;
	}
	dt = find_dir_tupid(argv[0]);
	if(dt < 0)
		return -1;
	for(x=1; x<argc; x++) {
		char *p = argv[x];
		if(tup_entry_add(dt, &dtent) < 0)
			return -1;
		if(tup_db_select_tent(dtent, argv[x], &tent) < 0)
			return -1;
		if(!tent) {
			/* Path replacement is a hack for windows to work. This
			 * is only used by test code to check that commands &
			 * files actually make it into the database. But for
			 * wildcarding, windows will use '\\' instead of '/'
			 * for the separator, so we have to replace those in
			 * the strings. This is potentially the wrong thing to
			 * do in some situations, but only test code will
			 * break.
			 */
			while((p = strchr(p, '/')) != NULL) {
				/* Don't translate "./foo" to ".\foo" */
				if(p == argv[x] || p[-1] != '.') {
					*p = path_sep();
				}
				p++;
			}
			if(tup_db_select_tent(dtent, argv[x], &tent) < 0)
				return -1;
			if(!tent)
				return -1;
		}
	}
	if(tup_db_commit() < 0)
		return -1;
	return 0;
}

static int link_exists(const char *cmd, int argc, char **argv)
{
	struct tup_entry *tenta;
	struct tup_entry *tentb;
	struct tup_entry *dtenta;
	struct tup_entry *dtentb;
	int exists;
	int style;
	tupid_t dta, dtb;

	if(tup_db_begin() < 0)
		return -1;
	if(argc != 4) {
		fprintf(stderr, "tup error: %s requires two dir/name pairs.\n", argv[0]);
		return -1;
	}
	if(strcmp(cmd, "normal_exists") == 0) {
		style = TUP_LINK_NORMAL;
	} else if(strcmp(cmd, "sticky_exists") == 0) {
		style = TUP_LINK_STICKY;
	} else {
		fprintf(stderr, "[31mError: link_exists called with unknown style: %s\n", cmd);
		return -1;
	}
	dta = find_dir_tupid(argv[0]);
	if(dta < 0) {
		fprintf(stderr, "[31mError: dir '%s' doesn't exist.[0m\n", argv[0]);
		return -1;
	}

	if(tup_entry_add(dta, &dtenta) < 0)
		return -1;
	if(tup_db_select_tent(dtenta, argv[1], &tenta) < 0)
		return -1;
	if(!tenta) {
		fprintf(stderr, "[31mError: node '%s' doesn't exist.[0m\n", argv[1]);
		return -1;
	}

	dtb = find_dir_tupid(argv[2]);
	if(dtb < 0) {
		fprintf(stderr, "[31mError: dir '%s' doesn't exist.[0m\n", argv[2]);
		return -1;
	}

	if(tup_entry_add(dtb, &dtentb) < 0)
		return -1;
	if(tup_db_select_tent(dtentb, argv[3], &tentb) < 0)
		return -1;
	if(!tentb) {
		fprintf(stderr, "[31mError: node '%s' doesn't exist.[0m\n", argv[3]);
		return -1;
	}
	if(tup_db_link_exists(tenta->tnode.tupid, tentb->tnode.tupid, style, &exists) < 0)
		return -1;
	if(tup_db_commit() < 0)
		return -1;

	return exists;
}

static int touch(int argc, char **argv)
{
	int x;
	tupid_t sub_dir_dt;

	if(tup_db_begin() < 0)
		return -1;
	if(chdir(get_sub_dir()) < 0) {
		perror("chdir");
		return -1;
	}
	sub_dir_dt = get_sub_dir_dt();
	if(sub_dir_dt < 0)
		return -1;

	for(x=0; x<argc; x++) {
		struct stat buf;
		struct path_element *pel = NULL;
		struct tup_entry *dtent;
		tupid_t dt;

		if(lstat(argv[x], &buf) < 0) {
			close(open(argv[x], O_WRONLY | O_CREAT, 0666));
			if(lstat(argv[x], &buf) < 0) {
				fprintf(stderr, "lstat: ");
				perror(argv[x]);
				return -1;
			}
		}

		dt = find_dir_tupid_dt(sub_dir_dt, argv[x], &pel, 0, 0);
		if(dt <= 0) {
			fprintf(stderr, "Error finding dt for dir '%s' relative to dir %lli\n", argv[x], sub_dir_dt);
			return -1;
		}
		if(tup_entry_add(dt, &dtent) < 0)
			return -1;
		if(S_ISDIR(buf.st_mode)) {
			if(tup_db_create_node(dtent, pel->path, TUP_NODE_DIR) == NULL)
				return -1;
		} else if(S_ISREG(buf.st_mode) || S_ISLNK(buf.st_mode)) {
			if(tup_file_mod_mtime(dt, pel->path, MTIME(buf), 1, 0, NULL) < 0)
				return -1;
		}
		free_pel(pel);
	}
	if(tup_db_commit() < 0)
		return -1;
	return 0;
}

static int node(int argc, char **argv)
{
	int x;
	tupid_t sub_dir_dt;

	if(tup_db_begin() < 0)
		return -1;
	sub_dir_dt = get_sub_dir_dt();
	if(sub_dir_dt < 0)
		return -1;

	for(x=0; x<argc; x++) {
		tupid_t dt;
		struct path_element *pel = NULL;
		struct timespec mtime = {-1, 0};

		dt = find_dir_tupid_dt(sub_dir_dt, argv[x], &pel, 0, 0);
		if(dt <= 0) {
			fprintf(stderr, "Unable to find dir '%s' relative to %lli\n", argv[x], sub_dir_dt);
			return -1;
		}
		if(create_name_file(dt, pel->path, mtime, NULL) < 0) {
			fprintf(stderr, "Unable to create node for '%s' in dir %lli\n", pel->path, dt);
			return -1;
		}
		free_pel(pel);
	}
	if(tup_db_commit() < 0)
		return -1;
	return 0;
}

static int varshow_vdb(struct vardb *vdb)
{
	const char *color1 = "";
	const char *color2 = "";
	struct string_tree *st;
	struct var_entry *ve;

	RB_FOREACH(st, string_entries, &vdb->root) {
		ve = container_of(st, struct var_entry, var);
		if(ve->tent->type == TUP_NODE_GHOST) {
			color1 = "[47;30m";
			color2 = "[0m";
		}
		printf(" - Var[%s%s%s] = '%s'\n", color1, ve->var.s, color2, ve->value);
	}
	return 0;
}

static int varshow(int argc, char **argv)
{
	struct tup_entry *vartent;
	struct tup_entry *root_tent;
	struct variant *variant;
	if(tup_db_begin() < 0)
		return -1;
	if(tup_entry_add(DOT_DT, &root_tent) < 0)
		return -1;
	if(tup_db_select_tent(root_tent, TUP_CONFIG, &vartent) < 0)
		return -1;
	if(variant_add(vartent, 1, &variant) < 0)
		return -1;
	if(vartent) {
		if(argc == 1) {
			struct vardb vdb;
			if(vardb_init(&vdb) < 0)
				return -1;
			if(tup_db_get_vardb(vartent, &vdb) < 0)
				return -1;
			varshow_vdb(&vdb);
			vardb_close(&vdb);
		} else {
			int x;
			struct tup_entry *tent;
			for(x=0; x<argc; x++) {
				tent = tup_db_get_var(variant, argv[x], strlen(argv[x]), NULL);
				if(!tent) {
					fprintf(stderr, "Unable to find tupid for variable '%s'\n", argv[x]);
					continue;
				}
				if(tent->type == TUP_NODE_VAR) {
					struct var_entry *ve;
					ve = vardb_get(&variant->vdb, argv[x], strlen(argv[x]));
					if(!ve) {
						fprintf(stderr, "Unable to find vdb entry for variable '%s'\n", argv[x]);
						continue;
					}
					printf(" - Var[%s] = '%s'\n", argv[x], ve->value);
				} else if(tent->type == TUP_NODE_GHOST) {
					printf(" - Var[[47;30m%s[0m] is a ghost\n", argv[x]);
				} else {
					fprintf(stderr, "Variable '%s' has unknown type %i\n", argv[x], tent->type);
				}
			}
		}
	}
	if(tup_db_commit() < 0)
		return -1;
	return 0;
}

static int dbconfig(int argc, char **argv)
{
	if(argv || argc) {/* unused */}
	if(tup_db_show_config() < 0)
		return -1;
	return 0;
}

static int options(int argc, char **argv)
{
	if(argc || argv) {}
	if(tup_option_show() < 0)
		return -1;
	return 0;
}

static int fake_mtime(int argc, char **argv)
{
	struct tup_entry *tent;
	struct tup_entry *dtent;
	struct timespec mtime;
	tupid_t dt;
	tupid_t sub_dir_dt;
	struct path_element *pel = NULL;

	if(argc != 2) {
		fprintf(stderr, "tup error: fake_mtime requires a file and an mtime.\n");
		return -1;
	}
	if(tup_db_begin() < 0)
		return -1;
	sub_dir_dt = get_sub_dir_dt();
	if(sub_dir_dt < 0)
		return -1;
	dt = find_dir_tupid_dt(sub_dir_dt, argv[0], &pel, 0, 1);
	if(dt < 0) {
		fprintf(stderr, "tup error: Unable to find dt for node: %s\n", argv[0]);
		return -1;
	}
	if(tup_entry_add(dt, &dtent) < 0)
		return -1;
	if(tup_db_select_tent_part(dtent, pel->path, pel->len, &tent) < 0)
		return -1;
	if(tent == NULL) {
		fprintf(stderr, "Unable to find node '%.*s' in dir %lli\n", pel->len, pel->path, dt);
		return -1;
	}
	mtime.tv_sec = strtol(argv[1], NULL, 0);
	if(tup_db_set_mtime(tent, mtime) < 0)
		return -1;
	free_pel(pel);
	if(tup_db_commit() < 0)
		return -1;
	return 0;
}

static int fake_parser_version(int argc, char **argv)
{
	if(argc || argv) {}
	if(tup_db_begin() < 0)
		return -1;
	if(tup_db_config_set_int("parser_version", 0) < 0)
		return -1;
	if(tup_db_commit() < 0)
		return -1;
	return 0;
}

static int waitmon(void)
{
	int tries = 0;
	printf("Waitmon\n");
	while(1) {
		int pid;
		if(monitor_get_pid(0, &pid) < 0) {
			fprintf(stderr, "tup error: Unable to get the current monitor pid in waitmon()\n");
			return -1;
		}

		if(pid < 0) {
			usleep(100000);
			tries++;
			if(tries > 10) {
				printf(" -- waitmon (try again)\n");
				tries = 0;
			}
		} else {
			break;
		}
	}
	return 0;
}

static int flush(void)
{
	int autoupdate_pid;
	printf("Flush\n");
	while(1) {
		if(tup_db_begin() < 0)
			return -1;
		if(tup_db_config_get_int(AUTOUPDATE_PID, -1, &autoupdate_pid) < 0)
			return -1;
		if(tup_db_commit() < 0)
			return -1;
		if(autoupdate_pid < 0)
			break;
		printf(" -- flush (try again)\n");
		/* If we got the lock but autoupdate pid was set, it must've
		 * just started but not gotten the lock yet.  So we need to
		 * release our lock and wait a bit.
		 */
		tup_db_close();
		tup_lock_exit();
		usleep(10000);
		if(tup_lock_init() < 0)
			return -1;
		if(tup_db_open() != 0)
			return -1;
	}
	printf("Flushed.\n");
	return 0;
}

static int ghost_check(void)
{
	if(tup_db_begin() < 0)
		return -1;
	if(tup_db_debug_add_all_ghosts() < 0)
		return -1;
	if(tup_db_commit() < 0)
		return -1;
	return 0;
}

static void version(void)
{
	printf("tup %s\n", tup_version);
}
