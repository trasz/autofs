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

#include "common.h"
#include "mntopts.h"

static int
unmount_by_statfs(const struct statfs *statfs)
{
	char *fsid_str;
	int error, ret;

	ret = asprintf(&fsid_str, "FSID:%d:%d",
	    statfs->f_fsid.val[0], statfs->f_fsid.val[1]);
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

/*
 * Take two pointers to strings, concatenate the contents with "/" in the
 * middle, make the first pointer point to the result, the second pointer
 * to NULL, and free the old strings.
 *
 * Concatenate pathnames, basically.
 */
static void
concat(char **p1, char **p2)
{
	int ret;
	char *path;

	assert(p1 != NULL);
	assert(p2 != NULL);

	if (*p1 == NULL) {
		assert(*p2 != NULL);
		*p1 = *p2;
		return;
	}
	if (*p2 == NULL) {
		assert(*p1 != NULL);
		*p2 = *p1;
		return;
	}

	ret = asprintf(&path, "%s/%s", *p1, *p2);
	if (ret < 0)
		log_err(1, "asprintf");

	free(*p1);
	free(*p2);

	*p1 = path;
	*p2 = NULL;
}

static void
create_directory(const char *path)
{
	char *component, *copy, *tofree, *partial;
	int error;

	assert(path[0] == '/');

	/*
	 * +1 to skip the leading slash.
	 */
	copy = tofree = checked_strdup(path + 1);

	partial = NULL;
	for (;;) {
		component = strsep(&copy, "/");
		if (component == NULL)
			break;
		concat(&partial, &component);
		log_debugx("checking \"%s\" for existence", partial);
		error = access(partial, F_OK);
		if (error == 0)
			continue;
		if (errno != ENOENT)
			log_err(1, "cannot access %s", partial);
		log_debugx("directory \"%s\" does not exist, creating",
		    partial);
		error = mkdir(partial, 0755);
		if (error != 0)
			log_err(1, "cannot create %s", partial);
	}

	free(tofree);
}

static void
mount_autofs(const char *from, const char *fspath, const char *opts)
{
	struct iovec *iov = NULL;
	char errmsg[255], *option, *options, *tofree;
	int error, iovlen = 0;

	create_directory(fspath);

	log_debugx("mounting %s on %s", from, fspath);
	memset(errmsg, 0, sizeof(errmsg));

	build_iovec(&iov, &iovlen, "fstype", (void *)"autofs", (size_t)-1);
	build_iovec(&iov, &iovlen, "fspath", (void *)fspath, (size_t)-1);
	build_iovec(&iov, &iovlen, "from", (void *)from, (size_t)-1);
	build_iovec(&iov, &iovlen, "errmsg", errmsg, sizeof(errmsg));

	/*
	 * Append the options defined in auto_master.  The autofs
	 * will pass them to automountd(8), which will then append
	 * them to options specified in the map.
	 */
	options = tofree = checked_strdup(opts);

	while ((option = strsep(&options, ",")) != NULL) {
		build_iovec(&iov, &iovlen,
		    "master_option", option, sizeof(option));
	}

	free(tofree);

	error = nmount(iov, iovlen, 0);
	if (error != 0) {
		if (*errmsg != '\0') {
			log_err(1, "cannot mount %s on %s: %s",
			    from, fspath, errmsg);
		} else {
			log_err(1, "cannot mount %s on %s", from, fspath);
		}
	}
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
			log_debugx("skipping %s, filesystem type is not autofs",
			    mntbuf[i].f_mntonname);
			continue;
		}

		n = node_find(root, mntbuf[i].f_mntonname);
		if (n != NULL) {
			log_debugx("leaving autofs mounted on %s",
			    mntbuf[i].f_mntonname);
			continue;
		}

		log_debugx("autofs mounted on %s not found "
		    "in new configuration; unmounting", mntbuf[i].f_mntonname);
		unmount_by_statfs(&(mntbuf[i]));
	}

	log_debugx("mounting new autofs mounts");

	TAILQ_FOREACH(n, &root->n_children, n_next) {
		if (!node_is_direct_map(n)) {
			ret = asprintf(&from, "map %s", n->n_location);
			if (ret < 0)
				log_err(1, "asprintf");

			mountpoint = node_mountpoint(n);
			mount = find_statfs(mntbuf, nitems, mountpoint);
			if (mount != NULL) {
				if (strcmp(mount->f_fstypename, "autofs") != 0) {
					log_debugx("unknown filesystem mounted "
					    "on %s; mounting", mountpoint);
					/*
					 * XXX: Compare options and 'from',
					 * and update the mount if neccessary.
					 */
				} else {
					log_debugx("autofs already mounted "
					    "on %s", mountpoint);
					free(from);
					free(mountpoint);
					continue;
				}
			}

			mount_autofs(from, mountpoint, n->n_options);
			free(from);
			free(mountpoint);
			continue;
		}

		TAILQ_FOREACH(n2, &n->n_children, n_next) {
			TAILQ_FOREACH(n3, &n2->n_children, n_next) {
				ret = asprintf(&from, "map %s", n->n_location);
				if (ret < 0)
					log_err(1, "asprintf");
				/*
				 * XXX: Check if it's alrady mounted.
				 */
				mountpoint = node_mountpoint(n3);
				mount_autofs(from, mountpoint, n3->n_options);
				free(from);
				free(mountpoint);
			}
		}
	}
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
			log_debugx("skipping %s, filesystem type is autofs",
			    mntbuf[i].f_mntonname);
			continue;
		}

#if 0
		if ((mntbuf[i].f_flags & MNT_AUTOMOUNTED) == 0) {
			log_debugx("skipping %s, not automounted",
			    mntbuf[i].f_mntonname);
			continue;
		}
#endif

		unmount_by_statfs(&(mntbuf[i]));
	}
}

static void
usage_automount(void)
{

	fprintf(stderr, "usage: automount [-D name=value][-Lvu]\n");
	exit(1);
}

int
main_automount(int argc, char **argv)
{
	struct node *root;
	int ch, debug = 0;
	bool show_maps = false, unmount = false;

	/*
	 * Note that in automount(8), the only purpose of variable
	 * handling is to aid in debugging maps (automount -L).
	 */
	defined_init();

	while ((ch = getopt(argc, argv, "D:Luv")) != -1) {
		switch (ch) {
		case 'D':
			defined_parse_and_add(optarg);
			break;
		case 'L':
			show_maps = true;
			break;
		case 'u':
			unmount = true;
			break;
		case 'v':
			debug++;
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

	root = node_new_root();
	parse_master(root, AUTO_MASTER_PATH);

	if (show_maps) {
		node_print(root);
		return (0);
	}

	mount_unmount(root);

	return (0);
}
