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
#include <sys/event.h>
#include <sys/mount.h>
#include <sys/time.h>
#include <assert.h>
#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <libutil.h>

#include "common.h"

#define AUTOUNMOUNTD_PIDFILE	"/var/run/autounmountd.pid"

struct automounted_fs {
	TAILQ_ENTRY(automounted_fs)	af_next;
	time_t				af_mount_time;
	bool				af_mark;
	fsid_t				af_fsid;
	char				af_mountpoint[MNAMELEN];
};

static TAILQ_HEAD(, automounted_fs)	automounted;

static struct automounted_fs *
automounted_find(fsid_t fsid)
{
	struct automounted_fs *af;

	TAILQ_FOREACH(af, &automounted, af_next) {
		if (af->af_fsid.val[0] == fsid.val[0] &&
		    af->af_fsid.val[1] == fsid.val[1])
			return (af);
	}

	return (NULL);
}

static struct automounted_fs *
automounted_add(fsid_t fsid, const char *mountpoint)
{
	struct automounted_fs *af;

#if 0
	log_debugx("adding filesystem with FSID:%d:%d, mounted on %s",
	    fsid.val[0], fsid.val[1], mountpoint);
#endif

	af = calloc(sizeof(*af), 1);
	if (af == NULL)
		log_err(1, "calloc");
	af->af_mount_time = time(NULL);
	af->af_fsid = fsid;
	strlcpy(af->af_mountpoint, mountpoint, sizeof(af->af_mountpoint));

	TAILQ_INSERT_TAIL(&automounted, af, af_next);

	return (af);
}

static void
automounted_remove(struct automounted_fs *af)
{

#if 0
	log_debugx("removing filesystem with FSID:%d:%d, mounted on %s",
	    af->af_fsid.val[0], af->af_fsid.val[1], af->af_mountpoint);
#endif

	TAILQ_REMOVE(&automounted, af, af_next);
	free(af);
}

static void
refresh_automounted(void)
{
	struct automounted_fs *af, *tmpaf;
	struct statfs *mntbuf;
	int i, nitems;

	nitems = getmntinfo(&mntbuf, MNT_WAIT);
	if (nitems <= 0)
		log_err(1, "getmntinfo");

	log_debugx("refreshing list of automounted filesystems");

	TAILQ_FOREACH(af, &automounted, af_next)
		af->af_mark = false;

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

		af = automounted_find(mntbuf[i].f_fsid);
		if (af == NULL) {
			log_debugx("new automounted filesystem on %s",
			    mntbuf[i].f_mntonname);
			af = automounted_add(mntbuf[i].f_fsid,
			    mntbuf[i].f_mntonname);
		} else {
			log_debugx("already known automounted filesystem on %s",
			    mntbuf[i].f_mntonname);
		}
		af->af_mark = true;
	}

	TAILQ_FOREACH_SAFE(af, &automounted, af_next, tmpaf) {
		if (af->af_mark)
			continue;
		log_debugx("lost filesystem with FSID:%d:%d, mounted on %s",
	    	    af->af_fsid.val[0], af->af_fsid.val[1], af->af_mountpoint);
		automounted_remove(af);
	}
}

static int
unmount_by_fsid(const fsid_t fsid)
{
	char *fsid_str;
	int error, ret;

	ret = asprintf(&fsid_str, "FSID:%d:%d", fsid.val[0], fsid.val[1]);
	if (ret < 0)
		log_err(1, "asprintf");

	error = unmount(fsid_str, MNT_BYFSID);
	free(fsid_str);
	if (error != 0 && errno != EBUSY)
		log_warn("unmount");

	return (error);
}

static double
expire_automounted(double expiration_time)
{
	struct automounted_fs *af, *tmpaf;
	time_t now;
	double mounted_for, mounted_max = 0;
	int error;

	now = time(NULL);

	TAILQ_FOREACH_SAFE(af, &automounted, af_next, tmpaf) {
		mounted_for = difftime(now, af->af_mount_time);

		if (mounted_for < expiration_time) {
			log_debugx("skipping %s, mounted for %.0f seconds",
			    af->af_mountpoint, mounted_for);

			if (mounted_for > mounted_max)
				mounted_max = mounted_for;

			continue;
		}

		log_debugx("filesystem with FSID:%d:%d, mounted on %s, "
		    "was mounted for %.0f seconds; unmounting",
		    af->af_fsid.val[0], af->af_fsid.val[1], af->af_mountpoint,
		    mounted_for);
		error = unmount_by_fsid(af->af_fsid);
		if (error != 0) {
			if (mounted_for > mounted_max)
				mounted_max = mounted_for;
		}
	}

	return (mounted_max);
}

static void
usage_autounmountd(void)
{

	fprintf(stderr, "usage: autounmountd [-r time][-t time][-dv]\n");
	exit(1);
}

static void
do_wait(double sleep_time)
{
	struct kevent event;
	struct timespec timeout;
	static int kq = -1;
	int error;

	if (kq == -1) {
		log_debugx("setting up EVFILT_FS");

		kq = kqueue();
		if (kq < 0)
			log_err(1, "kqueue");

		EV_SET(&event, 0, EVFILT_FS, EV_ADD | EV_CLEAR, 0, 0, NULL);
		error = kevent(kq, NULL, 0, &event, 1, &timeout);
		if (error < 0)
			log_err(1, "kevent");
	}

	assert(sleep_time > 0);
	timeout.tv_sec = sleep_time;
	timeout.tv_nsec = 0;

	/*
	 * XXX: For some reason this doesn't work - it always returns after
	 * 	a timeout, ignoring filesystem events.
	 */
	log_debugx("waiting for filesystem event for %.0f seconds", sleep_time);
	error = kevent(kq, NULL, 0, &event, 1, &timeout);
	if (error < 0)
		log_err(1, "kevent");
}

int
main_autounmountd(int argc, char **argv)
{
	struct pidfh *pidfh;
	pid_t otherpid;
	const char *pidfile_path = AUTOUNMOUNTD_PIDFILE;
	int ch, debug = 0;
	double expiration_time = 600, retry_time = 600, mounted_max, sleep_time;
	bool dont_daemonize = false;

	while ((ch = getopt(argc, argv, "dr:t:v")) != -1) {
		switch (ch) {
		case 'd':
			dont_daemonize = true;
			debug++;
			break;
		case 'r':
			retry_time = atoi(optarg);
			break;
		case 't':
			expiration_time = atoi(optarg);
			break;
		case 'v':
			debug++;
			break;
		case '?':
		default:
			usage_autounmountd();
		}
	}
	argc -= optind;
	if (argc != 0)
		usage_autounmountd();

	if (retry_time <= 0)
		log_errx(1, "retry time must be greater than zero");
	if (expiration_time <= 0)
		log_errx(1, "expiration time must be greater than zero");

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

	if (dont_daemonize == false) {
		if (daemon(0, 0) == -1) {
			log_warn("cannot daemonize");
			pidfile_remove(pidfh);
			exit(1);
		}
	}

	pidfile_write(pidfh);

	TAILQ_INIT(&automounted);

	for (;;) {
		refresh_automounted();
		mounted_max = expire_automounted(expiration_time);
		if (mounted_max < expiration_time) {
			sleep_time = difftime(expiration_time, mounted_max);
			log_debugx("filesystem expires in %.0f seconds",
			    sleep_time);
		} else {
			sleep_time = retry_time;
			log_debugx("some expired filesystems remain mounted, "
			    "will retry in %.0f seconds", sleep_time);
		}

		do_wait(sleep_time);
	}

	return (0);
}
