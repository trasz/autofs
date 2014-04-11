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
#include <err.h>
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

#include "automountd.h"
#include "mntopts.h"

#define DEFAULT_PIDFILE	"/var/run/automountd.pid"

extern FILE *yyin;
extern char *yytext;
extern int lineno;
extern int yylex(void);

static int nchildren = 0;

static TAILQ_HEAD(, defined_value)	defined_values;

static void	parse_map(struct node *parent, const char *map);

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
		log_err(1, "calloc");
	// XXX
	n->n_key = checked_strdup("/");

	TAILQ_INIT(&n->n_children);

	return (n);
}

static struct node *
node_new(struct node *parent, char *key, char *options, char *location, const char *config_file, int config_line)
{
	struct node *n;

	n = calloc(1, sizeof(*n));
	if (n == NULL)
		log_err(1, "calloc");

	TAILQ_INIT(&n->n_children);
	n->n_key = key;
	n->n_options = options;
	n->n_location = location;
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
	if (n->n_key == NULL)
		return (false);
	if (n->n_key[0] != '+')
		return (false);
	return (true);
}

static void
node_expand_includes(struct node *n)
{
	struct node *n2, *tmp, *root;
	bool expanded;
	char *include = NULL;
	int error, ret;

	for (;;) {
		expanded = false;

		TAILQ_FOREACH_SAFE(n2, &n->n_children, n_next, tmp) {
			if (node_is_include(n2) == false)
				continue;

			/*
			 * "+1" to skip leading "+".
			 */
			ret = asprintf(&include, "%s %s", AUTO_INCLUDE_PATH, n2->n_key + 1);
			if (ret < 0)
				log_err(1, "asprintf");
			log_debugx("include \"%s\" maps to \"%s\"", n2->n_key, include);

			yyin = popen(include, "r");
			if (yyin == NULL)
				log_err(1, "unable to execute \"%s\"", include);

			/*
			 * We need to put the included entries at the same place
			 * the include entry was, preserving the ordering
			 */

			root = node_new_root();

			/*
			 * XXX
			 */

			expanded = true;
			log_warnx("cannot expand %s (%s:%d); not supported yet", n2->n_key, n2->n_config_file, n2->n_config_line);
			node_delete(n2);

			error = pclose(yyin);
			yyin = NULL;
			if (error != 0)
				log_errx(1, "execution of \"%s\" failed", include);
		}

		/*
		 * XXX: Is this the right behaviour?
		 */
		if (expanded) {
			log_debugx("expanded some includes, go around");
			continue;
		}

		break;
	}
}

static bool
node_is_direct_map(const struct node *n)
{

	for (;;) {
		assert(n->n_parent != NULL);
		if (n->n_parent->n_parent == NULL)
			break;
		n = n->n_parent;
	}

	assert(n->n_key != NULL);
	if (strcmp(n->n_key, "/-") != 0)
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

		/*
		 * This is the first-level map node; the one that contains
		 * the key and subnodes with mountpoints and actual map names.
		 */
		if (n2->n_location == NULL)
			continue;

		log_debugx("map \"%s\" is a direct map, parsing", n2->n_location);
		parse_map(n2, n2->n_location);
	}
}

static void
node_expand_defined(struct node *n)
{
	struct node *n2, *tmp;

	TAILQ_FOREACH_SAFE(n2, &n->n_children, n_next, tmp) {
		/*
		 * XXX: Remember to only expand variables in the location field.
		 */
	}
}

static char *
node_mountpoint_x(const struct node *n, char *x)
{
	char *path;
	int ret;

	if (n->n_parent == NULL)
		return (x);

	if (n->n_parent->n_parent == NULL && strcmp(n->n_key, "/-") == 0)
		return (x);

	ret = asprintf(&path, "%s%s", n->n_key, x);
	if (ret < 0)
		log_err(1, "asprintf");
	free(x);

	return (node_mountpoint_x(n->n_parent, path));
}

static char *
node_mountpoint(const struct node *n)
{

	return (node_mountpoint_x(n, checked_strdup("")));
}

#if 0
static char *
node_mountpoint(const struct node *n)
{
	char *path;
	int ret;

	path = checked_strdup(n->n_key);
	for (;;) {
		if (n->n_parent == NULL)
			break;
		if (n->n_parent->n_key == NULL)
			break;
		/*
		 * XXX: Make it clearer.
		 */
		if (n->n_parent->n_parent->n_parent == NULL && strcmp(n->n_parent->n_key, "/-") == 0)
			break;
		n = n->n_parent;
		ret = asprintf(&path, "%s%s", n->n_key, path);
		if (ret < 0)
			log_err(1, "asprintf");
	}

	return (path);
}
#endif

static void
node_print_indent(const struct node *n, int indent)
{
	const struct node *n2;

	printf("%*.s%s\t%s\t%s # %s referenced at %s:%d\n", indent, "",
	    node_mountpoint(n),
	    n->n_options != NULL ? n->n_options : "",
	    n->n_location != NULL ? n->n_location : "",
	    node_is_direct_map(n) ? "direct map" : "indirect map",
	    n->n_config_file, n->n_config_line);

	TAILQ_FOREACH(n2, &n->n_children, n_next)
		node_print_indent(n2, indent + 3);
}

static void
node_print(const struct node *n)
{
	const struct node *n2;

	TAILQ_FOREACH(n2, &n->n_children, n_next)
		node_print_indent(n2, 0);
}

static struct node *
node_find(struct node *root, const char *mountpoint)
{
	struct node *n, *n2;
	char *tmp;

	tmp = node_mountpoint(root);
	if (strcmp(tmp, mountpoint) == 0) {
		free(tmp);
		return (root);
	}
	free(tmp);

	TAILQ_FOREACH(n, &root->n_children, n_next) {
		n2 = node_find(n, mountpoint);
		if (n2 != NULL)
			return (n2);
	}

	return (NULL);
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

/*
 * Canonical form of a map entry looks like this:
 *
 * key [-options] [ [/mountpoint] [-options2] location ... ]
 *
 * We parse it in such a way that a map always has two levels - first
 * for key, and the second, for the mountpoint.
 */
static void
parse_map_yyin(struct node *parent, const char *map)
{
	char *key = NULL, *options = NULL, *mountpoint = NULL, *options2 = NULL, *location = NULL;
	int ret;
	struct node *node;

	for (;;) {
		ret = yylex();
		if (ret == 0 || ret == NEWLINE) {
			if (key != NULL || options != NULL)
				log_errx(1, "truncated x entry in %s, line %d", map, lineno);
			if (ret == 0) {
				/*
				 * End of file.
				 */
				break;
			} else {
				key = options = NULL;
				continue;
			}
		}
		if (key == NULL) {
			key = checked_strdup(yytext);
			continue;
		} else if (yytext[0] == '-') {
			if (options != NULL)
				log_errx(1, "duplicated options in %s, line %d", map, lineno);
			options = checked_strdup(yytext);
			continue;
		}

		log_debugx("adding map node, %s", key);
		node = node_new(parent, key, options, NULL, map, lineno);
		key = options = NULL;

		for (;;) {
			if (yytext[0] == '/') {
				if (mountpoint != NULL)
					log_errx(1, "duplicated mountpoint in %s, line %d", map, lineno);
				if (options2 != NULL || location != NULL)
					log_errx(1, "mountpoint out of order in %s, line %d", map, lineno);
				mountpoint = checked_strdup(yytext);
				goto again;
			}

			if (yytext[0] == '-') {
				if (options2 != NULL)
					log_errx(1, "duplicated options in %s, line %d", map, lineno);
				if (location != NULL)
					log_errx(1, "options out of order in %s, line %d", map, lineno);
				options2 = checked_strdup(yytext);
				goto again;
			}

			if (location != NULL)
				log_errx(1, "too many arguments in %s, line %d", map, lineno);
			location = checked_strdup(yytext);

			if (mountpoint == NULL)
				mountpoint = checked_strdup("/");
			if (options2 == NULL)
				options2 = checked_strdup("");

			log_debugx("adding map node, %s %s %s", mountpoint, options2, location);
			node_new(node, mountpoint, options2, location, map, lineno);
			mountpoint = options2 = location = NULL;
again:
			ret = yylex();
			if (ret == 0 || ret == NEWLINE) {
				if (mountpoint != NULL || options2 != NULL || location != NULL)
					log_errx(1, "truncated entry in %s, line %d", map, lineno);
				break;
			}
		}
	}
}

static void
parse_map(struct node *parent, const char *map)
{
	char *new_map = NULL;
	int error, ret;
	bool executable = false;

	assert(map != NULL);
	assert(map[0] != '\0');

	log_debugx("parsing map \"%s\"", map);

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
			log_err(1, "unable to execute \"%s\"", map);
	} else {
		yyin = fopen(map, "r");
		if (yyin == NULL)
			log_err(1, "unable to open \"%s\"", map);
	}

	/*
	 * XXX: Here it's 1, below it's 0, and both work correctly; investigate.
	 */
	lineno = 1;

	parse_map_yyin(parent, map);

	if (executable) {
		error = pclose(yyin);
		if (error != 0)
			log_err(1, "execution of dynamic map \"%s\" failed", map);
	} else {
		fclose(yyin);
	}
	yyin = NULL;

	log_debugx("done parsing map \"%s\"", map);

	node_expand_includes(parent);
	node_expand_direct_maps(parent);
	node_expand_defined(parent);
}

static struct node *
parse_master(const char *path)
{
	struct node *root;
	char *mountpoint = NULL, *map = NULL, *options = NULL;
	int ret;

	log_debugx("parsing auto_master file at \"%s\"", path);

	root = node_new_root();

	yyin = fopen(path, "r");
	if (yyin == NULL)
		err(1, "unable to open %s", path);

	lineno = 0;
	for (;;) {
		ret = yylex();
		if (ret == 0 || ret == NEWLINE) {
			if (mountpoint != NULL) {
				log_debugx("adding map for %s", mountpoint);
				node_new(root, mountpoint, options, map, path, lineno);
			}
			if (ret == 0) {
				break;
			} else {
				mountpoint = map = options = NULL;
				continue;
			}
		}
		if (mountpoint == NULL) {
			mountpoint = checked_strdup(yytext);
		} else if (map == NULL) {
			map = checked_strdup(yytext);
		} else if (options == NULL) {
			options = checked_strdup(yytext);
		} else {
			log_errx(1, "too many arguments in %s, line %d", path, lineno);
		}
	}

	fclose(yyin);
	yyin = NULL;

	log_debugx("done parsing \"%s\"", path);

	node_expand_includes(root);
	node_expand_direct_maps(root);

	return (root);
}

/*
 * XXX: Make it look like written by sane person.
 */
static void
create_directory(const char *path)
{
	char *component, *path2, *tofree, *partial;
	int error;

	tofree = path2 = checked_strdup(path + 1);

	partial = checked_strdup("");
	for (;;) {
		component = strsep(&path2, "/");
		if (component == NULL)
			break;
		asprintf(&partial, "%s/%s", partial, component);
		//log_debugx("checking \"%s\" for existence", partial);
		error = access(partial, F_OK);
		if (error == 0)
			continue;
		if (errno == ENOENT) {
			log_debugx("directory \"%s\" does not exist, creating", partial);
			error = mkdir(partial, 0755);
			if (error != 0)
				log_err(1, "cannot create %s", partial);
			continue;
		}
		log_err(1, "cannot access %s", partial);
	}

	free(tofree);
}

static void
mount_autofs(const char *from, const char *fspath)
{
	struct iovec *iov = NULL;
	int iovlen;
	char errmsg[255];

	create_directory(fspath);

	log_debugx("mounting %s on %s", from, fspath);
	memset(errmsg, 0, sizeof(errmsg));

	build_iovec(&iov, &iovlen, "fstype", (void *)"autofs", (size_t)-1);
	build_iovec(&iov, &iovlen, "fspath", (void *)fspath, (size_t)-1);
	build_iovec(&iov, &iovlen, "from", (void *)from, (size_t)-1);
	build_iovec(&iov, &iovlen, "from", (void *)from, (size_t)-1);
	build_iovec(&iov, &iovlen, "errmsg", errmsg, sizeof(errmsg));

	if (nmount(iov, iovlen, 0) == -1) {
		if (*errmsg != '\0')
			log_warn("cannot mount %s on %s: %s", from, fspath, errmsg);
		else
			log_warn("cannot mount %s on %s", from, fspath);
	}
}

static int
unmount_by_statfs(const struct statfs *statfs)
{
	char *fsid_str;
	int error, ret;

	ret = asprintf(&fsid_str, "FSID:%d:%d", statfs->f_fsid.val[0], statfs->f_fsid.val[1]);
	if (ret < 0)
		log_err(1, "asprintf");

	log_debugx("unmounting %s using %s", statfs->f_mntonname, fsid_str);

	error = unmount(fsid_str, MNT_BYFSID);
	free(fsid_str);
	if (error != 0)
		log_warn("cannot unmount %s", statfs->f_mntonname);

	return (error);
}

static const struct statfs *
find_statfs(const struct statfs *mntbuf, int nitems, const char *mountpoint)
{
	int i;

	for (i = 0; i < nitems; i++) {
		if (strcmp(mntbuf[i].f_mntonname, mountpoint) == 0)
			return (mntbuf + i);
	}

	return (NULL);
}

static void
mount_unmount(struct node *root)
{
	struct statfs *mntbuf;
	const struct statfs *mount;
	struct node *n, *n2, *n3;
	char *mountpoint;
	char *from;
	int i, nitems, ret;

	nitems = getmntinfo(&mntbuf, MNT_WAIT);
	if (nitems <= 0)
		log_err(1, "getmntinfo");

	log_debugx("unmounting stale autofs mounts");

	for (i = 0; i < nitems; i++) {
		if (strcmp(mntbuf[i].f_fstypename, "autofs") != 0) {
			log_debugx("skipping %s, filesystem type is not autofs", mntbuf[i].f_mntonname);
			continue;
		}

		n = node_find(root, mntbuf[i].f_mntonname);
		if (n != NULL) {
			log_debugx("leaving autofs mounted on %s", mntbuf[i].f_mntonname);
			continue;
		}

		log_debugx("autofs mounted on %s not found in new configuration; unmounting", mntbuf[i].f_mntonname);
		unmount_by_statfs(&(mntbuf[i]));
	}

	log_debugx("mounting new autofs mounts");

	TAILQ_FOREACH(n, &root->n_children, n_next) {
		if (!node_is_direct_map(n)) {
			mountpoint = node_mountpoint(n);
			ret = asprintf(&from, "map %s", n->n_location);
			if (ret < 0)
				log_err(1, "asprintf");

			mount = find_statfs(mntbuf, nitems, mountpoint);
			if (mount != NULL) {
				if (strcmp(mount->f_fstypename, "autofs") != 0) {
					log_debugx("unknown filesystem mounted on %s; mounting", mountpoint);
					/*
					 * XXX: Compare options and 'from', and update the mount if neccessary.
					 */
				} else {
					log_debugx("autofs already mounted on %s", mountpoint);
					free(from);
					free(mountpoint);
					continue;
				}
			}

			mount_autofs(from, mountpoint);
			free(from);
			free(mountpoint);
			continue;
		}

		TAILQ_FOREACH(n2, &n->n_children, n_next) {
			TAILQ_FOREACH(n3, &n2->n_children, n_next) {
				mountpoint = node_mountpoint(n3);
				ret = asprintf(&from, "map %s", n->n_location);
				if (ret < 0)
					log_err(1, "asprintf");
				/*
				 * XXX: Check if it's alrady mounted.
				 */
				mount_autofs(from, mountpoint);
				free(from);
				free(mountpoint);
			}
		}
	}
}

static void
done(int autofs_fd, const char *mountpoint)
{
	struct autofs_daemon_done done;
	int error;

	memset(&done, 0, sizeof(done));
	strlcpy(done.add_mountpoint, mountpoint, sizeof(done.add_mountpoint));

	error = ioctl(autofs_fd, AUTOFSDONE, &done);
	if (error != 0)
		log_err(1, "AUTOFSDONE");
}

static void
handle_mount(int autofs_fd, const char *from, const char *mountpoint, const char *fspath)
{
	const char *map;
	struct node *root, *node;
	FILE *f;
	char *mount_cmd;
	int error, ret;

	log_debugx("got mount request for %s on %s/%s", from, mountpoint, fspath);

	if (strncmp(from, "map ", 4) != 0)
		log_errx(1, "invalid mountfrom \"%s\"; failing mount", from);

	map = from + 4; /* 4 for strlen("map "); */
	root = node_new_root();
	parse_map(root, map);

	log_debugx("searching for key for \"%s\" in map %s", mountpoint, map);
	node = node_find(root, mountpoint);
	if (node == NULL)
		log_errx(1, "map %s does not contain key for \"%s\"; failing mount", map, mountpoint);

	/*
	 * XXX: Filesystem type.
	 */
	ret = asprintf(&mount_cmd, "%s %s %s", "mount_nfs", node->n_location, mountpoint);
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

	log_debugx("mount done");
	done(autofs_fd, mountpoint);

	log_debugx("nothing more to do; exiting");
	exit(0);
}

static const char *
defined_find(const char *name)
{
	struct defined_value *d;

	TAILQ_FOREACH(d, &defined_values, d_next) {
		if (strcmp(d->d_name, name) == 0)
			return (d->d_value);
	}

	return (NULL);
}

static void
defined_add(const char *name, const char *value)
{
	struct defined_value *d;
	const char *found;

	found = defined_find(name);
	if (found != NULL)
		log_errx(1, "variable %s already defined", name);

	log_debugx("defining variable %s=%s", name, value);

	d = calloc(sizeof(*d), 1);
	if (d == NULL)
		log_err(1, "calloc");
	d->d_name = checked_strdup(name);
	d->d_value = checked_strdup(value);

	TAILQ_INSERT_TAIL(&defined_values, d, d_next);
}

static void
defined_parse_and_add(char *def)
{
	char *name, *value;

	value = def;
	name = strsep(&value, "=");

	if (value == NULL || value[0] == '\0')
		log_errx(1, "missing variable value");
	if (name == NULL || name[0] == '\0')
		log_errx(1, "missing variable name");

	defined_add(name, value);
}

static void
defined_init(void)
{
	struct utsname name;
	int error;

	TAILQ_INIT(&defined_values);

	error = uname(&name);
	if (error != 0)
		log_err(1, "uname");

	defined_add("ARCH", name.sysname); // XXX: What should go here?
	defined_add("CPU", name.machine);
	defined_add("HOST", name.nodename);
	defined_add("OSNAME", name.sysname);
	defined_add("OSREL", name.release);
	defined_add("OSVERS", name.version);
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

	fprintf(stderr, "usage: automountd [-D name=value][-m maxproc][-Tdv]\n");
	exit(1);
}

static void
usage_automount(void)
{

	fprintf(stderr, "usage: automount [-Lvu]\n");
	exit(1);
}

static int
main_automountd(int argc, char **argv)
{
	struct pidfh *pidfh;
	pid_t pid, otherpid;
	const char *pidfile_path = DEFAULT_PIDFILE;
	struct autofs_daemon_request request;
	int ch, debug = 0, error, autofs_fd, maxproc = 30, retval, saved_errno;
	bool dont_daemonize = false;

	defined_init();

	while ((ch = getopt(argc, argv, "D:Tdm:v")) != -1) {
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

	log_init(10);

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
		log_err(1, "failed to open %s", AUTOFS_PATH);

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
		handle_mount(autofs_fd, request.adr_from, request.adr_mountpoint, request.adr_path);
	}

	pidfile_close(pidfh);

	return (0);
}

static void
unmount_automounted(void)
{
	struct statfs *mntbuf;
	int i, nitems;

	nitems = getmntinfo(&mntbuf, MNT_WAIT);
	if (nitems <= 0)
		log_err(1, "getmntinfo");

	log_debugx("unmounting automounted filesystems");

	for (i = 0; i < nitems; i++) {
		if (strcmp(mntbuf[i].f_fstypename, "autofs") == 0) {
			log_debugx("skipping %s, filesystem type is autofs", mntbuf[i].f_mntonname);
			continue;
		}

#if 0
		if ((mntbuf[i].f_flags & MNT_AUTOMOUNTED) == 0) {
			log_debugx("skipping %s, not automounted", mntbuf[i].f_mntonname);
			continue;
		}
#endif

		unmount_by_statfs(&(mntbuf[i]));
	}
}

static int
main_automount(int argc, char **argv)
{
	struct node *root;
	int ch, debug = 0;
	bool show_maps = false, unmount = false;

	while ((ch = getopt(argc, argv, "Lvu")) != -1) {
		switch (ch) {
		case 'L':
			show_maps = true;
			break;
		case 'v':
			debug++;
			break;
		case 'u':
			unmount = true;
			break;
		case '?':
		default:
			usage_automount();
		}
	}
	argc -= optind;
	if (argc != 0)
		usage_automount();

	log_init(debug);

	if (unmount) {
		unmount_automounted();
		return (0);
	}

	root = parse_master(AUTO_MASTER_PATH);

	if (show_maps) {
		node_print(root);
		return (0);
	}

	mount_unmount(root);

	return (0);
}

int
main(int argc, char **argv)
{
	char *cmdname;

	if (argv[0] == NULL)
		log_errx(1, "NULL command name");

	cmdname = basename(argv[0]);

	if (strcmp(cmdname, "automountd") == 0)
		return (main_automountd(argc, argv));
	else if (strcmp(cmdname, "automount") == 0)
		return (main_automount(argc, argv));
	else
		log_errx(1, "binary name should be either \"automount\", or \"automountd\"");
}
