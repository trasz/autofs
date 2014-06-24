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

#ifndef AUTOFS_H
#define	AUTOFS_H

#include <sys/cdefs.h>
__FBSDID("$FreeBSD$");

#define VFSTOAUTOFS(mp)    ((struct autofs_mount *)((mp)->mnt_data))

MALLOC_DECLARE(M_AUTOFS);

extern uma_zone_t autofs_request_zone;
extern uma_zone_t autofs_node_zone;

extern int autofs_debug;

#define	AUTOFS_DEBUG(X, ...)					\
	if (autofs_debug > 1) {					\
		printf("%s: " X "\n", __func__, ## __VA_ARGS__);\
	} while (0)

#define	AUTOFS_WARN(X, ...)					\
	if (autofs_debug > 0) {					\
		printf("WARNING: %s: " X "\n",			\
		    __func__, ## __VA_ARGS__);			\
	} while (0)

#define AUTOFS_LOCK(X)		mtx_lock(&X->am_lock)
#define AUTOFS_UNLOCK(X)	mtx_unlock(&X->am_lock)
#define AUTOFS_LOCK_ASSERT(X)	mtx_assert(&X->am_lock, MA_OWNED)

struct autofs_node {
	TAILQ_ENTRY(autofs_node)	an_next;
	char				*an_name;
	int				an_fileno;
	struct autofs_node		*an_parent;
	TAILQ_HEAD(, autofs_node)	an_children;
	struct autofs_mount		*an_mount;
	struct vnode			*an_vnode;
	bool				an_trigger;
	struct timespec			an_ctime;
};

struct autofs_mount {
	TAILQ_ENTRY(autofs_mount)	am_next;
	struct autofs_softc		*am_softc;
	struct vnode			*am_rootvp;
	struct mtx			am_lock;
	char				am_from[MAXPATHLEN];
	char				am_mountpoint[MAXPATHLEN];
	char				am_options[MAXPATHLEN];
	char				am_prefix[MAXPATHLEN];
	int				am_last_fileno;
	int				am_last_request_id;
};

struct autofs_request {
	TAILQ_ENTRY(autofs_request)	ar_next;
	struct autofs_softc		*ar_softc;
	int				ar_id;
	bool				ar_done;
	bool				ar_in_progress;
	char				ar_from[MAXPATHLEN];
	char				ar_mountpoint[MAXPATHLEN];
	char				ar_key[MAXPATHLEN];
	char				ar_path[MAXPATHLEN];
	char				ar_options[MAXPATHLEN];
	char				ar_prefix[MAXPATHLEN];
	volatile u_int			ar_refcount;
};

struct autofs_softc {
	device_t			sc_dev;
	struct cdev			*sc_cdev;
	struct cv			sc_cv;
	struct sx			sc_lock;
	TAILQ_HEAD(, autofs_mount)	sc_mounts;
	TAILQ_HEAD(, autofs_request)	sc_requests;
	pid_t				sc_dev_pid;
	bool				sc_dev_opened;
};

/*
 * Limits and constants
 */
#define AUTOFS_NAMELEN		24
#define AUTOFS_FSNAMELEN	16	/* equal to MFSNAMELEN */
#define AUTOFS_DELEN		(8 + AUTOFS_NAMELEN)

bool	autofs_ignore_thread(const struct thread *td);
int	autofs_init(struct vfsconf *vfsp);
int	autofs_uninit(struct vfsconf *vfsp);
int	autofs_new_vnode(struct autofs_node *parent, const char *name,
	    int namelen, struct mount *mp, struct vnode **vpp);
int	autofs_find_vnode(struct autofs_node *parent, const char *name,
	    int namelen, struct vnode **vpp);
int	autofs_delete_vnode(struct autofs_node *parent, struct vnode *vp);

#endif /* !AUTOFS_H */
