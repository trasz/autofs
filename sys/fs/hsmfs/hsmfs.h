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

#ifndef HSMFS_H
#define	HSMFS_H

#define VFSTOHSMFS(mp)    ((struct hsmfs_mount *)((mp)->mnt_data))
#define hsmfs_node null_node

MALLOC_DECLARE(M_HSMFS);

extern uma_zone_t hsmfs_request_zone;
extern uma_zone_t hsmfs_node_zone;

extern int hsmfs_debug;
extern int hsmfs_stage_on_enoent;

#define	HSMFS_DEBUG(X, ...)						\
	do {								\
		if (hsmfs_debug > 1)					\
			printf("%s: " X "\n", __func__, ## __VA_ARGS__);\
	} while (0)

#define	HSMFS_WARN(X, ...)						\
	do {								\
		if (hsmfs_debug > 0) {					\
			printf("WARNING: %s: " X "\n",			\
		    	    __func__, ## __VA_ARGS__);			\
		}							\
	} while (0)

#define HSMFS_SLOCK(X)		sx_slock(&X->am_lock)
#define HSMFS_XLOCK(X)		sx_xlock(&X->am_lock)
#define HSMFS_SUNLOCK(X)	sx_sunlock(&X->am_lock)
#define HSMFS_XUNLOCK(X)	sx_xunlock(&X->am_lock)
#define HSMFS_ASSERT_LOCKED(X)	sx_assert(&X->am_lock, SA_LOCKED)
#define HSMFS_ASSERT_XLOCKED(X)	sx_assert(&X->am_lock, SA_XLOCKED)
#define HSMFS_ASSERT_UNLOCKED(X)	sx_assert(&X->am_lock, SA_UNLOCKED)

struct hsmfs_request {
	TAILQ_ENTRY(hsmfs_request)	hr_next;
	struct hsmfs_mount		*hr_mount;
	int				hr_id;
	int				hr_type;
	struct vnode			*hr_vp;
	bool				hr_done;
	int				hr_error;
	bool				hr_in_progress;
	volatile u_int			hr_refcount;
};

#define	HSMFS_EXTATTR_NAMESPACE	EXTATTR_NAMESPACE_SYSTEM
#define	HSMFS_EXTATTR_NAME	"hsmfs.meta"

/*
 * This is the hsmfs metadata, stored in "hsmfs.meta" extended
 * attribute for each file and directory.
 */
struct hsmfs_metadata {
	/*
	 * XXX
	 */
	bool				hm_metadata_valid;

	/*
	 * XXX
	 */
	bool				hm_managed;

	/*
	 * Means we have a complete copy of the file; it was either
	 * here from the beginning, or successfully staged.
	 */
	bool				hm_online;

	/*
	 * Means hsmd(8) will have to archive this file.
	 */
	bool				hm_modified;

	struct timeval			hm_staged_tv;
	struct timeval			hm_modified_tv;
	struct timeval			hm_archived_tv;
	struct timeval			hm_released_tv;

	/*
	 * The hsmd has no way to set ctime for staged files;
	 * we store it here instead and fake it on stat(2).
	 */
	struct timespec			hm_ctime;

	/*
	 * Those three replace the zeroes that would be returned
	 * by stat(2) on offline files - ones that hadn't been
	 * staged yet.
	 */
	nlink_t				hm_offline_nlink;
	off_t				hm_offline_size;
	u_quad_t			hm_offline_bytes;

};

int	hsmfs_metadata_read(struct vnode *vp);
int	hsmfs_metadata_write(struct vnode *vp);

struct hsmfs_softc {
	device_t			sc_dev;
	struct cdev			*sc_cdev;
	struct cv			sc_cv;
	struct sx			sc_lock;
	TAILQ_HEAD(, hsmfs_request)	sc_requests;
	pid_t				sc_hsmd_sid;
	int				sc_last_request_id;
};

int	hsmfs_init(struct vfsconf *vfsp);
int	hsmfs_uninit(struct vfsconf *vfsp);
bool	hsmfs_ignore_thread(void);
void	hsmfs_sync(void);

int	hsmfs_trigger_archive(struct vnode *vp);
int	hsmfs_trigger_recycle(struct vnode *vp);
int	hsmfs_trigger_stage(struct vnode *vp);
int	hsmfs_trigger_vn(struct vnode *vp, int type);

#endif /* !HSMFS_H */
