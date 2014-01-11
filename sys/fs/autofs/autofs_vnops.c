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

#include <sys/cdefs.h>
__FBSDID("$FreeBSD$");

#include <sys/param.h>
#include <sys/kernel.h>
#include <sys/condvar.h>
#include <sys/dirent.h>
#include <sys/fcntl.h>
#include <sys/lock.h>
#include <sys/malloc.h>
#include <sys/mount.h>
#include <sys/mutex.h>
#include <sys/namei.h>
#include <sys/systm.h>
#include <sys/vnode.h>

#include "autofs.h"

int	autofs_rootvp(struct mount *mp, struct vnode **vpp);

static int
autofs_access(struct vop_access_args *ap)
{

	return (0);
}

static int
autofs_getattr(struct vop_getattr_args *ap)
{
	struct vattr *vap = ap->a_vap;

	KASSERT(ap->a_vp->v_type == VDIR, ("not a directory"));

	/*
	 * XXX: Verify if we fill out all required fields.
	 */
	vap->va_mode = 0555;
	vap->va_uid = 0;
	vap->va_gid = 0;
	vap->va_nlink = 1;
	vap->va_rdev = NODEV;
	vap->va_size = 0;
	vap->va_mtime.tv_sec = 0;
	vap->va_mtime.tv_nsec = 0;
	vap->va_atime = vap->va_mtime;
	vap->va_ctime = vap->va_mtime;
	vap->va_birthtime = vap->va_mtime;
	vap->va_flags = 0;
	vap->va_gen = 0;
	vap->va_blocksize = 512;
	vap->va_bytes = 0;
	vap->va_type = ap->a_vp->v_type;
	vap->va_filerev = 0;

	return (0);
}

static int
autofs_lookup(struct vop_lookup_args *ap)
{
	struct autofs_mount *amp;
	int error;

	amp = VFSTOAUTOFS(ap->a_dvp->v_mount);

	AUTOFS_DEBUG("looking up %s/%s", amp->am_path, ap->a_cnp->cn_nameptr);

	AUTOFS_LOCK(amp);
	amp->am_waiting = true;
	cv_signal(&amp->am_softc->sc_cv);

	while (amp->am_waiting == true) {
		error = cv_wait_sig(&amp->am_cv, &amp->am_lock);
		if (error != 0) {
			amp->am_waiting = false;
			AUTOFS_UNLOCK(amp);

			return (error);
		}
	}
	AUTOFS_UNLOCK(amp);

	return (0);
}

static int
autofs_readdir(struct vop_readdir_args *ap)
{
	AUTOFS_DEBUG("go");

	return (EDOOFUS);
}

static int
autofs_reclaim(struct vop_reclaim_args *ap)
{

	return (0);
}

static int
autofs_inactive(struct vop_inactive_args *ap)
{
	struct vnode *vp = ap->a_vp;

	vrecycle(vp);

	return (0);
}

struct vop_vector autofs_vnodeops = {
	.vop_default =		&default_vnodeops,

	.vop_access =		autofs_access,
	.vop_lookup =		autofs_lookup,
	.vop_create =		VOP_EOPNOTSUPP,
	.vop_getattr =		autofs_getattr,
	.vop_link =		VOP_EOPNOTSUPP,
	.vop_mkdir =		VOP_EOPNOTSUPP,
	.vop_mknod =		VOP_EOPNOTSUPP,
	.vop_read =		VOP_EOPNOTSUPP,
	.vop_readdir =		autofs_readdir,
	.vop_remove =		VOP_EOPNOTSUPP,
	.vop_rename =		VOP_EOPNOTSUPP,
	.vop_rmdir =		VOP_EOPNOTSUPP,
	.vop_setattr =		VOP_EOPNOTSUPP,
	.vop_symlink =		VOP_EOPNOTSUPP,
	.vop_vptocnp =		VOP_EOPNOTSUPP, /* XXX */
	.vop_write =		VOP_EOPNOTSUPP,
	.vop_reclaim =		autofs_reclaim,
	.vop_inactive =		autofs_inactive,
};

int
autofs_rootvp(struct mount *mp, struct vnode **vpp)
{
	int error;
	struct vnode *vp;

	error = getnewvnode("autofs", mp, &autofs_vnodeops, &vp);
	if (error)
		return (error);

	vp->v_type = VDIR;
	vp->v_data = NULL;
	error = insmntque(vp, mp);
	if (error != 0)
		return (error);

	*vpp = vp;
	AUTOFS_DEBUG("setting rootvp to %p", vp);
	return (0);
}
