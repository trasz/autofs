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
 * $FreeBSD$
 */

#ifndef HSMFS_IOCTL_H
#define	HSMFS_IOCTL_H

/*
 * IOCTL interface used by hsmd(8) via /dev/hsmfs.
 */
#define	HSMFS_PATH		"/dev/hsmfs"

#define	HSMFS_TYPE_ARCHIVE	1
#define	HSMFS_TYPE_RECYCLE	3
#define	HSMFS_TYPE_RELEASE	4
#define	HSMFS_TYPE_STAGE	5
#define	HSMFS_TYPE_UNMANAGE	6

struct hsmfs_daemon_request {

	/*
	 * Request identifier.
	 */
	int		hdr_id;

	/*
	 * Request type.
	 */
	int		hdr_type;

	/*
	 * Path to the file or directory being requested.
	 */
	char		hdr_path[MAXPATHLEN];
};

struct hsmfs_daemon_done {
	/*
	 * Identifier, copied from hdr_id.
	 */
	int		hdd_id;

	/*
	 * Error number, possibly returned to userland.
	 */
	int		hdd_error;

	/*
	 * Reserved for future use.
	 */
	int		hdd_spare[7];
};

struct hsmfs_queue {
	/*
	 * Request identifier.
	 */
	int		hq_id;
	int		hq_next_id;
	int		hq_done;
	int		hq_in_progress;

	/*
	 * Request type.
	 */
	int		hq_type;

	/*
	 * Path to the file or directory being requested.
	 */
	char		hq_path[MAXPATHLEN];
};

#define	HSMFSREQUEST	_IOR('I', 0x01, struct hsmfs_daemon_request)
#define	HSMFSDONE	_IOW('I', 0x02, struct hsmfs_daemon_done)
#define	HSMFSQUEUE	_IOWR('I', 0x03, struct hsmfs_queue)

/*
 * IOCTL interface for hsmarchive(8) et all, called on individual files.
 */
struct hsm_archive {
};

struct hsm_recycle {
};

struct hsm_release {
};

struct hsm_stage {
};

struct hsm_unmanage {
};

struct hsm_state {
	int		hs_managed;
	int		hs_online;
	int		hs_modified;
	struct timeval	hs_staged_tv;
	struct timeval	hs_modified_tv;
	struct timeval	hs_archived_tv;
	struct timeval	hs_released_tv;
};

struct hsm_managed {
	int		hm_online;
	struct timespec	hm_ctime;
	nlink_t		hm_offline_nlink;
	off_t		hm_offline_size;
	u_quad_t	hm_offline_bytes;
};

#define	HSMARCHIVE	_IOW('I', 0x11, struct hsm_archive)
#define	HSMRECYCLE	_IOW('I', 0x12, struct hsm_recycle)
#define	HSMRELEASE	_IOW('I', 0x13, struct hsm_release)
#define	HSMSTAGE	_IOW('I', 0x14, struct hsm_stage)
#define	HSMUNMANAGE	_IOW('I', 0x15, struct hsm_unmanage)

#define	HSMSTATE	_IOR('I', 0x16, struct hsm_state)
#define	HSMMANAGED	_IOW('I', 0x17, struct hsm_managed)

#endif /* !HSMFS_IOCTL_H */
