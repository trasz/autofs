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
#include <unistd.h>

#include "hsmfs_ioctl.h"

static void
usage(void)
{

	fprintf(stderr, "usage: %s [-r] file ...\n", getprogname());
	exit(1);
}

int
main(int argc, char **argv)
{
	FTS *fts;
	FTSENT *entry;
	const char *cmd_name, *request_name;
	unsigned long request;
	bool recurse = false;
	int cumulated_error, ch, error, fd;

	if (argv[0] == NULL)
		errx(1, "NULL command name");

	cmd_name = getprogname();

	if (strcmp(cmd_name, "hsmarchive") == 0) {
		request = HSMARCHIVE;
		request_name = "HSMARCHIVE";
	} else if (strcmp(cmd_name, "hsmrecycle") == 0) {
		request = HSMRECYCLE;
		request_name = "HSMRECYCLE";
	} else if (strcmp(cmd_name, "hsmrelease") == 0) {
		request = HSMRELEASE;
		request_name = "HSMRELEASE";
	} else if (strcmp(cmd_name, "hsmstage") == 0) {
		request = HSMSTAGE;
		request_name = "HSMSTAGE";
	} else if (strcmp(cmd_name, "hsmunmanage") == 0) {
		request = HSMUNMANAGE;
		request_name = "HSMUNMANAGE";
	} else {
		errx(1, "binary name should be either \"hsmarchive\", "
		    "\"hsmrecycle\", \"hsmrelease\", \"hsmstage\", "
		    "or \"hsmunmanage\"");
	}

	while ((ch = getopt(argc, argv, "r")) != -1) {
		switch (ch) {
		case 'r':
			recurse = true;
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

		switch (request) {
			case HSMARCHIVE: {
				struct hsm_archive ha;

				error = ioctl(fd, request, &ha);
				break;
			}
			case HSMRECYCLE: {
				struct hsm_recycle hr;

				error = ioctl(fd, request, &hr);
				break;
			}
			case HSMRELEASE: {
				struct hsm_release hr;

				error = ioctl(fd, request, &hr);
				break;
			}
			case HSMSTAGE: {
				struct hsm_stage hs;

				error = ioctl(fd, request, &hs);
				break;
			}
			case HSMUNMANAGE: {
				struct hsm_unmanage hu;

				error = ioctl(fd, request, &hu);
				break;
			}
		}
		if (error != 0) {
			warn("%s: %s", entry->fts_path, request_name);
			cumulated_error++;
		}

		/*
		 * Don't descent into directories that are offline, unless we're
		 * actually trying to stage them.
		 */
		while (request != HSMSTAGE && entry->fts_info == FTS_D) {
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
