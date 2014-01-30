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
#include <assert.h>
#include <err.h>
#include <errno.h>
#include <fcntl.h>
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

#include "automountd.h"
#include "mntopts.h"

#define DEFAULT_PIDFILE	"/var/run/automountd.pid"

extern FILE *yyin;
extern char *yytext;
extern int lineno;
extern int yylex(void);


static void	parse_map(struct node *parent, char *map);

static void
usage(void)
{

	fprintf(stderr, "usage: automountd [-d]\n");
	exit(1);
}

char *
checked_strdup(const char *s)
{
	char *c;

	c = strdup(s);
	if (c == NULL)
		log_err(1, "strdup");
	return (c);
}

static struct node *
node_new_root(void)
{
	struct node *n;

	n = calloc(1, sizeof(*n));
	if (n == NULL)
		err(1, "calloc");

	TAILQ_INIT(&n->n_children);

	return (n);
}

static struct node *
node_new(struct node *parent, char *mountpoint, char *map, char *options, char *config_file, int config_line)
{
	struct node *n;

	n = calloc(1, sizeof(*n));
	if (n == NULL)
		err(1, "calloc");

	TAILQ_INIT(&n->n_children);
	n->n_mountpoint = mountpoint;
	n->n_map = map;
	n->n_options = options;
	n->n_config_file = config_file;
	n->n_config_line = config_line;

	n->n_parent = parent;
	TAILQ_INSERT_TAIL(&parent->n_children, n, n_next);

	return (n);
}

static void
node_delete(struct node *n)
{
	struct node *n2, *tmp;

	TAILQ_FOREACH_SAFE(n2, &n->n_children, n_next, tmp)
		node_delete(n);

	if (n->n_parent != NULL)
		TAILQ_REMOVE(&n->n_parent->n_children, n, n_next);

	free(n);
}

static bool
node_is_include(const struct node *n)
{
	if (n->n_mountpoint == NULL)
		return (false);
	if (n->n_mountpoint[0] != '+')
		return (false);
	return (true);
}

static void
node_expand_includes(struct node *n)
{
	struct node *n2, *tmp;
	bool expanded;

	for (;;) {
		expanded = false;

		TAILQ_FOREACH_SAFE(n2, &n->n_children, n_next, tmp) {
			if (node_is_include(n2) == false)
				continue;

			expanded = true;
			log_warnx("cannot expand %s (%s:%d); not supported yet", n2->n_mountpoint, n2->n_config_file, n2->n_config_line);
			node_delete(n2);
		}

		if (expanded) {
			log_debugx("expanded some entries, go around");
			continue;
		}

		break;
	}
}

static bool
node_is_direct_map(const struct node *n)
{

	if (n->n_mountpoint == NULL)
		return (false);
	if (strcmp(n->n_mountpoint, "/-") != 0)
		return (false);
	return (true);
}

static void
node_expand_direct_maps(struct node *n)
{
	struct node *n2, *tmp;

	TAILQ_FOREACH_SAFE(n2, &n->n_children, n_next, tmp) {
		if (node_is_direct_map(n2) == false)
			continue;

		if (n2->n_map == NULL)
			errx(1, "missing map name at %s:%d\n", n2->n_config_file, n2->n_config_line);

		parse_map(n2, n2->n_map);
	}

	// XXX: Possibly go around?
}

static void
node_print_indent(const struct node *n, int indent)
{
	const struct node *n2;

	if (indent != 0) {
		log_debugx("%*.s%s\t%s\t%s # %s referenced at %s:%d", indent, "",
		    n->n_mountpoint,
		    n->n_map != NULL ? n->n_map : "",
		    n->n_options != NULL ? n->n_options : "",
		    node_is_direct_map(n) ? "direct map" : "indirect map",
		    n->n_config_file, n->n_config_line);
	}

	TAILQ_FOREACH(n2, &n->n_children, n_next)
		node_print_indent(n2, indent + 3);
}

static void
node_print(const struct node *n)
{

	node_print_indent(n, 0);
}

static bool
file_is_executable(const char *path)
{
	struct stat sb;
	int error;

	error = stat(path, &sb);
	if (error != 0)
		log_err(1, "cannot stat %s", path);
	if ((sb.st_mode & S_IXUSR) || (sb.st_mode & S_IXGRP) || (sb.st_mode & S_IXOTH))
		return (true);
	return (false);
}

static void
parse_map(struct node *parent, char *map)
{
	char *key = NULL, *options = NULL, *location = NULL;
	char *new_map = NULL;
	int error, ret;
	bool executable = false;

	assert(map != NULL);
	assert(map[0] != '\0');

	if (map[0] == '-') {
		ret = asprintf(&new_map, "%s/special_%s", AUTO_SPECIAL_PREFIX, map + 1);
		if (ret < 0)
			log_err(1, "asprintf");
		log_debugx("special map \"%s\" maps to executable \"%s\"", map, new_map);
		map = new_map;
		executable = true;
	} else if (map[0] != '/') {
		ret = asprintf(&new_map, "%s/%s", AUTO_MAP_PREFIX, map);
		if (ret < 0)
			log_err(1, "asprintf");
		log_debugx("map \"%s\" maps to \"%s\"", map, new_map);
		map = new_map;
	}

	if (!executable) {
		executable = file_is_executable(map);
		if (executable)
			log_debugx("map \"%s\" is executable", map);
	}

	if (executable) {
		yyin = popen(map, "r");
		if (yyin == NULL)
			err(1, "unable to execute \"%s\"", map);
	} else {
		yyin = fopen(map, "r");
		if (yyin == NULL)
			err(1, "unable to open \"%s\"", map);
	}

	lineno = 0;
	for (;;) {
		ret = yylex();
		if (ret == 0 || ret == NEWLINE) {
			if (key != NULL) {
				if (key[0] != '/')
					asprintf(&key, "%s/%s", parent->n_mountpoint, key);

				node_new(parent, key, location, options, map, lineno);
			}
			if (ret == 0) {
				break;
			} else {
				key = options = location = NULL;
				continue;
			}
		}
		if (key == NULL) {
			key = strdup(yytext);
		} else if (yytext[0] == '-') {
			if (options != NULL)
				errx(1, "duplicated options in %s, line %d", map, lineno);
			if (location != NULL)
				errx(1, "options out of order in %s, line %d", map, lineno);
			options = strdup(yytext);
		} else {
			if (location != NULL)
				errx(1, "too many arguments in %s, line %d", map, lineno);
			location = strdup(yytext);
		}
	}

	if (executable) {
		error = pclose(yyin);
		if (error != 0)
			log_err(1, "execution of dynamic map \"%s\" failed", map);
	} else {
		fclose(yyin);
	}
	yyin = NULL;

	node_expand_includes(parent);
	node_expand_direct_maps(parent);
}

static struct node *
parse_auto_master(void)
{
	struct node *root;
	char *mountpoint = NULL, *map = NULL, *options = NULL;
	int ret;

	root = node_new_root();

	yyin = fopen(AUTO_MASTER_PATH, "r");
	if (yyin == NULL)
		err(1, "unable to open %s", AUTO_MASTER_PATH);

	lineno = 0;
	for (;;) {
		ret = yylex();
		if (ret == 0 || ret == NEWLINE) {
			if (mountpoint != NULL)
				node_new(root, mountpoint, map, options, (char *)AUTO_MASTER_PATH, lineno);
			if (ret == 0) {
				break;
			} else {
				mountpoint = map = options = NULL;
				continue;
			}
		}
		if (mountpoint == NULL) {
			//log_debugx("trying to add \"%s\" as mountpoint", yytext);
			mountpoint = strdup(yytext);
		} else if (map == NULL) {
			//log_debugx("trying to add \"%s\" as map", yytext);
			map = strdup(yytext);
		} else if (options == NULL) {
			//log_debugx("trying to add \"%s\" as options", yytext);
			options = strdup(yytext);
		} else {
			//log_debugx("trying to add \"%s\" as whatever", yytext);
			errx(1, "too many arguments in %s, line %d", AUTO_MASTER_PATH, lineno);
		}
	}

	fclose(yyin);
	yyin = NULL;

	node_expand_includes(root);
	node_expand_direct_maps(root);

	return (root);
}

static void
do_mount(const char *map, const char *mountpoint)
{
	struct iovec *iov;
	int iovlen;
	char errmsg[255];

	// XXX: Create the mountpoint.

	log_debugx("mounting %s on %s", map, mountpoint);

	memset(errmsg, 0, sizeof(errmsg));

	build_iovec(&iov, &iovlen, "fstype", (void *)"autofs", (size_t)-1);
	build_iovec(&iov, &iovlen, "fspath", (void *)mountpoint, (size_t)-1);
	build_iovec(&iov, &iovlen, "from", (void *)map, (size_t)-1);
	build_iovec(&iov, &iovlen, "errmsg", errmsg, sizeof(errmsg));

	if (nmount(iov, iovlen, 0) == -1) {
		if (*errmsg != '\0')
			log_warn("cannot mount %s on %s: %s", map, mountpoint, errmsg);
		else
			log_warn("cannot mount %s on %s", map, mountpoint);
	}
}

static void
mount_stuff(struct node *root)
{
	struct node *n;
	const char *mountpoint, *map;

	TAILQ_FOREACH(n, &root->n_children, n_next) {
		if (!node_is_direct_map(n)) {
			mountpoint = n->n_mountpoint;
			if (node_is_direct_map(n->n_parent))
				map = n->n_parent->n_map;
			else
				map = n->n_map;
			do_mount(map, mountpoint);
		}
		mount_stuff(n);
	}
}

int
main(int argc, char **argv)
{
	struct pidfh *pidfh;
	pid_t otherpid;
	const char *pidfile_path = DEFAULT_PIDFILE;
	struct autofs_daemon_request request;
	struct node *root;
	int ch, debug = 0, error, autofs_fd, retval, saved_errno;
	bool dont_daemonize = false;

	while ((ch = getopt(argc, argv, "d")) != -1) {
		switch (ch) {
		case 'd':
			dont_daemonize = true;
			debug++;
			break;
		case '?':
		default:
			usage();
		}
	}
	argc -= optind;
	if (argc != 0)
		usage();

	log_init(debug);
	root = parse_auto_master();
	node_print(root);
	mount_stuff(root);

	pidfh = pidfile_open(pidfile_path, 0600, &otherpid);
	if (pidfh == NULL) {
		if (errno == EEXIST)
			errx(1, "daemon already running, pid: %jd.",
			    (intmax_t)otherpid);
		err(1, "cannot open or create pidfile \"%s\"",
		    pidfile_path);
	}

	autofs_fd = open(AUTOFS_PATH, O_RDWR);
	if (autofs_fd < 0 && errno == ENOENT) {
		saved_errno = errno;
		retval = kldload("autofs");
		if (retval != -1)
			autofs_fd = open(AUTOFS_PATH, O_RDWR);
		else
			errno = saved_errno;
	}
	if (autofs_fd < 0)
		err(1, "failed to open %s", AUTOFS_PATH);

	if (dont_daemonize == false) {
		if (daemon(0, 0) == -1) {
			warn("cannot daemonize");
			pidfile_remove(pidfh);
			exit(1);
		}
	}

	pidfile_write(pidfh);

	for (;;) {
		log_debugx("waiting for request from the kernel");

		memset(&request, 0, sizeof(request));
		error = ioctl(autofs_fd, AUTOFSREQUEST, &request);
		if (error != 0)
			err(1, "AUTOFSREQUEST");

		log_debugx("got mount request for \"%s\"", request.adr_path);
	}

	pidfile_close(pidfh);

	return (0);
}
