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
#include <sys/socket.h>
#include <sys/capability.h>
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

#define DEFAULT_PIDFILE	"/var/run/automountd.pid"

static void
usage(void)
{

	fprintf(stderr, "usage: automountd\n");
	exit(1);
}

int
main(int argc, char **argv)
{
	int ch, error, autofs_fd, retval, saved_errno;
	bool dont_daemonize = false;
	struct pidfh *pidfh;
	pid_t otherpid;
	const char *pidfile_path = DEFAULT_PIDFILE;
	struct autofs_wait aw;

	while ((ch = getopt(argc, argv, "d")) != -1) {
		switch (ch) {
		case 'd':
			dont_daemonize = true;
			break;
		case '?':
		default:
			usage();
		}
	}
	argc -= optind;
	if (argc != 0)
		usage();

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
		printf("waiting for request from the kernel\n");

		memset(&aw, 0, sizeof(aw));
		error = ioctl(autofs_fd, AUTOFSWAIT, &aw);
		if (error != 0)
			err(1, "ISCSIDWAIT");

		printf("got \"%s\"\n", aw.aw_path);

	}

	pidfile_close(pidfh);

	return (0);
}
