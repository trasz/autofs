/*-
 * Copyright (c) 2016 Edward Tomasz Napierala <trasz@FreeBSD.org>
 * All rights reserved.
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
 */

#include <sys/cdefs.h>
__FBSDID("$FreeBSD$");

#include <sys/ioctl.h>
#include <sys/param.h>
#include <err.h>
#include <errno.h>
#include <fcntl.h>
#include <fts.h>
#include <libgen.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "hsmfs_ioctl.h"

static void
usage(void)
{

	fprintf(stderr, "usage: hsmstat [-rx] file ...\n");
	exit(1);
}

static void
show(const char *path, const struct hsm_state *hs)
{
	if (!hs->hs_managed && !hs->hs_online && !hs->hs_modified) {
		printf("unmanaged -       -          %s\n", path);
		return;
	}
	printf("%s ", hs->hs_managed ? "managed  " : "unmanaged");
	if (!hs->hs_online && !hs->hs_modified) {
		printf("offline -          %s\n", path);
		return;
	}
	printf("%s ", hs->hs_online ? "online " : "offline");
	printf("%s ", hs->hs_modified ? "modified  " : "unmodified");
	printf("%s\n", path);
}

static void
show_time(const char *name, const struct timeval *tv)
{
	char buf[256];
	struct tm *tm;

	if (tv->tv_sec == 0) {
		printf("%s: Never\n", name);
		return;
	}

	tm = localtime(&tv->tv_sec);
	strftime(buf, sizeof(buf), "%c", tm);
	printf("%s: %s\n", name, buf);
}

static void
show_extra(const char *path, const struct hsm_state *hs)
{

	printf("    File: \"%s\"\n", path);
	printf(" Managed: %s, Online: %s, Modified: %s\n",
	    hs->hs_managed ? "Yes" : "No",
	    hs->hs_online ? "Yes" : "No",
	    hs->hs_modified ? "Yes" : "No");

	show_time("  Staged", &hs->hs_staged_tv);
	show_time("Modified", &hs->hs_modified_tv);
	show_time("Archived", &hs->hs_archived_tv);
	show_time("Released", &hs->hs_released_tv);
}

int
main(int argc, char **argv)
{
	struct hsm_state hs;
	FTS *fts;
	FTSENT *entry;
	bool extra = false, recurse = false;
	int cumulated_error, ch, error, fd;

	while ((ch = getopt(argc, argv, "rx")) != -1) {
		switch (ch) {
		case 'r':
			recurse = true;
			break;
		case 'x':
			extra = true;
			break;
		case '?':
		default:
			usage();
		}
	}

	argc -= optind;
	argv += optind;
	if (argc < 1)
		usage();

	fts = fts_open(argv, FTS_NOSTAT | FTS_PHYSICAL, NULL);
	if (fts == NULL)
		err(1, "fts_open");

	cumulated_error = 0;
	for (;;) {
		entry = fts_read(fts);
		if (entry == NULL) {
			if (errno != 0)
				warn("fts_read");
			break;
		}

		switch (entry->fts_info) {
		case FTS_D:
			if (!recurse) {
				error = fts_set(fts, entry, FTS_SKIP);
				if (error != 0) {
					warn("%s: fts_set", entry->fts_path);
					cumulated_error++;
				}
			}
			break;
		case FTS_DP:
			/*
			 * No point in visiting directories twice.
			 */
			continue;
		case FTS_DNR:
		case FTS_ERR:
		case FTS_NS:
			warnx("%s: %s", entry->fts_path, strerror(entry->fts_errno));
			cumulated_error++;
			continue;
		}

		fd = open(entry->fts_accpath, O_RDONLY);
		if (fd < 0) {
			warn("%s", entry->fts_path);
			cumulated_error++;
			continue;
		}

		memset(&hs, 0, sizeof(hs));
		error = ioctl(fd, HSMSTATE, &hs);
		if (error != 0) {
			warn("%s: HSMSTATE", entry->fts_path);
			cumulated_error++;
			goto out;
		}

		if (extra)
			show_extra(entry->fts_path, &hs);
		else
			show(entry->fts_path, &hs);

		if (hs.hs_managed && !hs.hs_online) {
			error = fts_set(fts, entry, FTS_SKIP);
			if (error != 0) {
				warn("%s: fts_set", entry->fts_path);
				cumulated_error++;
			}
		}

out:
		error = close(fd);
		if (error != 0)
			warn("%s: close", entry->fts_path);
	}

	if (cumulated_error)
		return (1);
	return (0);
}
