/*-
 * Copyright (c) 2014 The FreeBSD Foundation
 * All rights reserved.
 *
 * This software was developed by Edward Tomasz Napierala under sponsorship
 * from the FreeBSD Foundation.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE AUTHOR AND CONTRIBUTORS ``AS IS'' AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED.  IN NO EVENT SHALL THE AUTHOR OR CONTRIBUTORS BE LIABLE
 * FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS
 * OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY
 * OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 *
 * $FreeBSD$
 */

#include <sys/types.h>
#include <sys/time.h>
#include <sys/ioctl.h>
#include <sys/param.h>
#include <sys/linker.h>
#include <sys/mount.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <sys/utsname.h>
#include <assert.h>
#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <libgen.h>
#include <netdb.h>
#include <signal.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <libutil.h>

#include "autofs_ioctl.h"

#include "common.h"

#define AUTOMOUNTD_PIDFILE	"/var/run/automountd.pid"

static int nchildren = 0;
static int autofs_fd;
static int request_id;

static void
done(int request_error)
{
	struct autofs_daemon_done add;
	int error;

	memset(&add, 0, sizeof(add));
	add.add_id = request_id;
	add.add_error = request_error;

	log_debugx("completing request %d with error %d",
	    request_id, request_error);

	error = ioctl(autofs_fd, AUTOFSDONE, &add);
	if (error != 0) {
		/*
		 * Do this instead of log_err() to avoid calling
		 * done() again with error, from atexit handler.
		 */
		log_warn("AUTOFSDONE");
	}
	quick_exit(1);
}

/*
 * Remove "fstype=whatever" from optionsp and return the "whatever" part.
 */
static char *
pick_option(const char *option, char **optionsp)
{
	char *tofree, *pair, *newoptions;
	char *picked = NULL;
	bool first = true;

	tofree = *optionsp;

	newoptions = calloc(strlen(*optionsp) + 1, 1);
	if (newoptions == NULL)
		log_err(1, "calloc");

	while ((pair = strsep(optionsp, ",")) != NULL) {
		/*
		 * XXX: strncasecmp(3) perhaps?
		 */
		if (strncmp(pair, option, strlen(option)) == 0) {
			picked = checked_strdup(pair + strlen(option));
		} else {
			if (first == false)
				strcat(newoptions, ",");
			else
				first = false;
			strcat(newoptions, pair);
		}
	}

	free(tofree);
	*optionsp = newoptions;

	return (picked);
}

static void
create_subtree(struct node *node)
{
	struct node *child;
	char *path;

	/*
	 * Skip wildcard nodes.
	 */
	if (strcmp(node->n_key, "*") == 0)
		return;

	path = node_path(node);
	//log_debugx("creating directory %s", path);
	create_directory(path);
	free(path);

	TAILQ_FOREACH(child, &node->n_children, n_next)
		create_subtree(child);
}

static void
exit_callback(void)
{

	done(EIO);
}

static void
handle_request(const struct autofs_daemon_request *adr, char *cmdline_options)
{
	const char *map;
	struct node *root, *parent, *node;
	FILE *f;
	char *mount_cmd, *options, *fstype, *retrycnt;
	int error, ret;

	log_debugx("got request %d: from %s, path %s, prefix \"%s\", "
	    "key \"%s\", options \"%s\"", adr->adr_id, adr->adr_from,
	    adr->adr_path, adr->adr_prefix, adr->adr_key, adr->adr_options);

	/*
	 * Try to notify the kernel about any problems.
	 */
	request_id = adr->adr_id;
	atexit(exit_callback);

	if (strncmp(adr->adr_from, "map ", 4) != 0) {
		log_errx(1, "invalid mountfrom \"%s\"; failing request",
		    adr->adr_from);
	}

	map = adr->adr_from + 4; /* 4 for strlen("map "); */
	root = node_new_root();
	if (adr->adr_prefix[0] == 0 || strcmp(adr->adr_prefix, "/") == 0) {
		parent = root;
	} else {
		parent = node_new_map(root, checked_strdup(adr->adr_prefix),
		    checked_strdup(adr->adr_options), checked_strdup(map),
		    checked_strdup("[kernel request]"), lineno);
	}
	parse_map(parent, map, adr->adr_key[0] != '\0' ? adr->adr_key : NULL);
	if (adr->adr_key[0] != '\0')
		node_expand_wildcard(root, adr->adr_key);
	node = node_find(root, adr->adr_path);
	if (node == NULL) {
		log_errx(1, "map %s does not contain key for \"%s\"; "
		    "failing mount", map, adr->adr_path);
	}

	node_expand_defined(node);
	node_expand_ampersand(node, adr->adr_key);

	if (node->n_location == NULL) {
		/*
		 * Not a mountpoint; create directories in the autofs mount
		 * and complete the request.
		 */
		create_subtree(node);
		done(0);

		log_debugx("nothing to mount; exiting");

		/*
		 * Exit without calling exit_callback().
		 */
		quick_exit(0);
	}

	options = node_options(node);

	/*
	 * Prepend options passed via automountd(8) command line.
	 */
	if (cmdline_options != NULL)
		options = separated_concat(cmdline_options, options, ',');

	/*
	 * Append "automounted".
	 */
	options = separated_concat(options, "automounted", ',');

	/*
	 * Figure out fstype.
	 */
	fstype = pick_option("fstype=", &options);
	if (fstype == NULL) {
		log_debugx("fstype not specified in options; "
		    "defaulting to \"nfs\"");
		fstype = checked_strdup("nfs");
	}

	if (strcmp(fstype, "nfs") == 0) {
		/*
		 * The mount_nfs(8) command defaults to retry undefinitely.
		 * We don't want that behaviour, because it leaves mount_nfs(8)
		 * instances and automountd(8) children hanging forever.
		 * Disable retries unless the option was passed explicitly.
		 */
		retrycnt = pick_option("retrycnt=", &options);
		if (retrycnt == NULL) {
			log_debugx("retrycnt not specified in options; "
			    "defaulting to 1");
			options = separated_concat(options,
			    separated_concat("retrycnt", "1", '='), ',');
		} else {
			options = separated_concat(options,
			    separated_concat("retrycnt", retrycnt, '='), ',');
		}
	}

	ret = asprintf(&mount_cmd, "mount -t %s -o %s %s %s", fstype, options,
	    node->n_location, adr->adr_path);
	if (ret < 0)
		log_err(1, "asprintf");

	log_debugx("will execute \"%s\"", mount_cmd);

	/*
	 * XXX: Passing mount command error messages to syslog.
	 */
	f = popen(mount_cmd, "r");
	if (f == NULL)
		log_err(1, "cannot execute \"%s\"", mount_cmd);
	error = pclose(f);
	if (error != 0)
		log_errx(1, "failed to execute \"%s\"", mount_cmd);

	done(0);
	log_debugx("mount done; exiting");

	/*
	 * Exit without calling exit_callback().
	 */
	quick_exit(0);
}

static int
wait_for_children(bool block)
{
	pid_t pid;
	int status;
	int num = 0;

	for (;;) {
		/*
		 * If "block" is true, wait for at least one process.
		 */
		if (block && num == 0)
			pid = wait4(-1, &status, 0, NULL);
		else
			pid = wait4(-1, &status, WNOHANG, NULL);
		if (pid <= 0)
			break;
		if (WIFSIGNALED(status)) {
			log_warnx("child process %d terminated with signal %d",
			    pid, WTERMSIG(status));
		} else if (WEXITSTATUS(status) != 0) {
			log_warnx("child process %d terminated with exit status %d",
			    pid, WEXITSTATUS(status));
		} else {
			log_debugx("child process %d terminated gracefully", pid);
		}
		num++;
	}

	return (num);
}

static void
usage_automountd(void)
{

	fprintf(stderr, "usage: automountd [-D name=value][-m maxproc]"
	    "[-o opts][-Tdv]\n");
	exit(1);
}

int
main_automountd(int argc, char **argv)
{
	struct pidfh *pidfh;
	pid_t pid, otherpid;
	const char *pidfile_path = AUTOMOUNTD_PIDFILE;
	char *options = NULL;
	struct autofs_daemon_request request;
	int ch, debug = 0, error, maxproc = 30, retval, saved_errno;
	bool dont_daemonize = false;

	defined_init();

	while ((ch = getopt(argc, argv, "D:Tdm:o:v")) != -1) {
		switch (ch) {
		case 'D':
			defined_parse_and_add(optarg);
			break;
		case 'T':
			/*
			 * For compatibility with other implementations,
			 * such as OS X.
			 */
			debug++;
			break;
		case 'd':
			dont_daemonize = true;
			debug++;
			break;
		case 'm':
			maxproc = atoi(optarg);
			break;
		case 'o':
			if (options == NULL) {
				options = checked_strdup(optarg);
			} else {
				options =
				    separated_concat(options, optarg, ',');
			}
			break;
		case 'v':
			debug++;
			break;
		case '?':
		default:
			usage_automountd();
		}
	}
	argc -= optind;
	if (argc != 0)
		usage_automountd();

	log_init(debug);

	pidfh = pidfile_open(pidfile_path, 0600, &otherpid);
	if (pidfh == NULL) {
		if (errno == EEXIST) {
			log_errx(1, "daemon already running, pid: %jd.",
			    (intmax_t)otherpid);
		}
		log_err(1, "cannot open or create pidfile \"%s\"",
		    pidfile_path);
	}

	autofs_fd = open(AUTOFS_PATH, O_RDWR | O_CLOEXEC);
	if (autofs_fd < 0 && errno == ENOENT) {
		saved_errno = errno;
		retval = kldload("autofs");
		if (retval != -1)
			autofs_fd = open(AUTOFS_PATH, O_RDWR | O_CLOEXEC);
		else
			errno = saved_errno;
	}
	if (autofs_fd < 0)
		log_err(1, "failed to open %s", AUTOFS_PATH);

	if (dont_daemonize == false) {
		if (daemon(0, 0) == -1) {
			log_warn("cannot daemonize");
			pidfile_remove(pidfh);
			exit(1);
		}
	} else {
		lesser_daemon();
	}

	pidfile_write(pidfh);

	for (;;) {
		log_debugx("waiting for request from the kernel");

		memset(&request, 0, sizeof(request));
		error = ioctl(autofs_fd, AUTOFSREQUEST, &request);
		if (error != 0) {
			if (errno == EINTR) {
				nchildren -= wait_for_children(false);
				assert(nchildren >= 0);
				continue;
			}

			log_err(1, "AUTOFSREQUEST");
		}

		if (dont_daemonize) {
			log_debugx("not forking due to -d flag; "
			    "will exit after servicing a single request");
		} else {
			nchildren -= wait_for_children(false);
			assert(nchildren >= 0);

			while (maxproc > 0 && nchildren >= maxproc) {
				log_debugx("maxproc limit of %d child processes hit; "
				    "waiting for child process to exit", maxproc);
				nchildren -= wait_for_children(true);
				assert(nchildren >= 0);
			}
			log_debugx("got request; forking child process #%d",
			    nchildren);
			nchildren++;

			pid = fork();
			if (pid < 0)
				log_err(1, "fork");
			if (pid > 0)
				continue;
		}

		pidfile_close(pidfh);
		handle_request(&request, options);
	}

	pidfile_close(pidfh);

	return (0);
}

