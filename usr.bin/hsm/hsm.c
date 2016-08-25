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

	fprintf(stderr, "usage: hsm [-L] [-r] [-x] [file ...]\n");
	fprintf(stderr, "       hsm -A [-r] file ...\n");
	fprintf(stderr, "       hsm -R [-r] file ...\n");
	fprintf(stderr, "       hsm -S [-r] file ...\n");
	fprintf(stderr, "       hsm -U [-r] file ...\n");
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
	FTS *fts;
	FTSENT *entry;
	int Aflag = 0, Lflag = 0, Rflag = 0, Sflag = 0, Uflag = 0;
	bool extra = false, recurse = false;
	int cumulated_error, ch, error, fd;

	if (argv[0] == NULL)
		errx(1, "NULL command name");

	while ((ch = getopt(argc, argv, "ALRSUrx")) != -1) {
		switch (ch) {
		case 'A':
			Aflag = 1;
			break;
		case 'L':
			Lflag = 1;
			break;
		case 'R':
			Rflag = 1;
			break;
		case 'S':
			Sflag = 1;
			break;
		case 'U':
			Uflag = 1;
			break;
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

	if (Aflag + Rflag + Sflag + Uflag == 0)
		Lflag = 1;
	if (Aflag + Lflag + Rflag + Sflag + Uflag > 1)
		errx(1, "at most one of -A, -L, -R, -S, or -U may be specified");
	if (extra && Lflag == 0)
		errx(1, "-x can only be used with -L");

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
				if (error != 0)
					err(1, "%s: fts_set", entry->fts_path);
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

		if (Aflag != 0) {
			struct hsm_archive ha;

			error = ioctl(fd, HSMARCHIVE, &ha);
			if (error != 0) {
				warn("%s: HSMARCHIVE", entry->fts_path);
				cumulated_error++;
			}
		} else if (Lflag != 0) {
			struct hsm_state hs;

			error = ioctl(fd, HSMSTATE, &hs);
			if (error != 0) {
				warn("%s: HSMSTATE", entry->fts_path);
				cumulated_error++;
			} else {
				if (extra)
					show_extra(entry->fts_path, &hs);
				else
					show(entry->fts_path, &hs);
			}
		} else if (Rflag != 0) {
			struct hsm_release hr;

			error = ioctl(fd, HSMRELEASE, &hr);
			if (error != 0) {
				warn("%s: HSMRELEASE", entry->fts_path);
				cumulated_error++;
			}
		} else if (Sflag != 0) {
			struct hsm_stage hs;

			error = ioctl(fd, HSMSTAGE, &hs);
			if (error != 0) {
				warn("%s: HSMSTAGE", entry->fts_path);
				cumulated_error++;
			}
		} else if (Uflag != 0) {
			struct hsm_unmanage hu;

			error = ioctl(fd, HSMUNMANAGE, &hu);
			if (error != 0) {
				warn("%s: HSMUNMANAGE", entry->fts_path);
				cumulated_error++;
			}
		}

		/*
		 * Don't descent into directories that are offline, unless we're
		 * actually trying to stage them.
		 */
		while (Sflag == 0 && entry->fts_info == FTS_D) {
			struct hsm_state hs;

			memset(&hs, 0, sizeof(hs));
			error = ioctl(fd, HSMSTATE, &hs);
			if (error != 0) {
				warn("%s: HSMSTATE", entry->fts_path);
				cumulated_error++;
				break;
			}

			if (!hs.hs_managed || hs.hs_online)
				break;

			error = fts_set(fts, entry, FTS_SKIP);
			if (error != 0) {
				warn("%s: fts_set", entry->fts_path);
				cumulated_error++;
				break;
			}

			break;
		}

		error = close(fd);
		if (error != 0)
			warn("%s: close", entry->fts_path);
	}

	if (cumulated_error)
		return (1);
	return (0);
}
