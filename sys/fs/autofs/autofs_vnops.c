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
#include <sys/mount.h>
#include <sys/mutex.h>
#include <sys/namei.h>
#include <sys/refcount.h>
#include <sys/signalvar.h>
#include <sys/systm.h>
#include <sys/vnode.h>
#include <vm/uma.h>

#include "autofs.h"

static int
autofs_access(struct vop_access_args *ap)
{

	return (0);
}

static int
autofs_getattr(struct vop_getattr_args *ap)
{
	struct autofs_node *anp = ap->a_vp->v_data;
	struct mount *mp = ap->a_vp->v_mount;
	struct vattr *vap = ap->a_vap;

	KASSERT(ap->a_vp->v_type == VDIR, ("!VDIR"));

	vap->va_type = VDIR;
	vap->va_mode = 0755;
	vap->va_nlink = 3; /* XXX */
	vap->va_uid = 0;
	vap->va_gid = 0;
	vap->va_rdev = NODEV;
	vap->va_fsid = mp->mnt_stat.f_fsid.val[0];
	vap->va_fileid = anp->an_fileno;
	vap->va_size = 512; /* XXX */
	vap->va_blocksize = 512;
	vap->va_mtime = anp->an_ctime;
	vap->va_atime = anp->an_ctime;
	vap->va_ctime = anp->an_ctime;
	vap->va_birthtime = anp->an_ctime;
	vap->va_gen = 0;
	vap->va_flags = 0;
	vap->va_rdev = 0;
	vap->va_bytes = 512; /* XXX */
	vap->va_filerev = 0;
	vap->va_spare = 0;

	return (0);
}

/*
 * Send request to automountd(8) and wait for completion.
 */
static int
autofs_trigger(struct autofs_node *anp, const char *path, int pathlen)
{
	struct autofs_mount *amp = VFSTOAUTOFS(anp->an_vnode->v_mount);
	struct autofs_softc *sc = amp->am_softc;
	struct autofs_request *ar;
	int error, last;

	sx_assert(&sc->sc_lock, SA_XLOCKED);

	TAILQ_FOREACH(ar, &sc->sc_requests, ar_next) {
		AUTOFS_DEBUG("looking at %s %s:%s", ar->ar_from, ar->ar_mountpoint, ar->ar_path);
		if (strcmp(ar->ar_from, amp->am_from) != 0)
			continue;
		if (strcmp(ar->ar_mountpoint, amp->am_mountpoint) != 0)
			continue;
		if (strlen(ar->ar_path) != pathlen)
			continue;
		if (strcmp(ar->ar_path, path) != 0)
			continue;

		/*
		 * XXX: Ignore options, right?
		 */
		break;
	}

	if (ar != NULL) {
		AUTOFS_DEBUG("found existing request for %s %s:%s", ar->ar_from, ar->ar_mountpoint, ar->ar_path);
		refcount_acquire(&ar->ar_refcount);
	} else {
		ar = uma_zalloc(autofs_request_zone, M_WAITOK | M_ZERO);
		ar->ar_softc = amp->am_softc;

		AUTOFS_LOCK(amp);
		ar->ar_id = ++amp->am_last_request_id;
		strlcpy(ar->ar_from, amp->am_from, sizeof(ar->ar_from));
		strlcpy(ar->ar_mountpoint, amp->am_mountpoint, sizeof(ar->ar_mountpoint));
		snprintf(ar->ar_path, sizeof(ar->ar_path), "%.*s", pathlen, path);
		strlcpy(ar->ar_options, amp->am_options, sizeof(ar->ar_options));
		AUTOFS_UNLOCK(amp);

		AUTOFS_DEBUG("new request for %s %s:%s", ar->ar_from, ar->ar_mountpoint, ar->ar_path);
		refcount_init(&ar->ar_refcount, 1);
		TAILQ_INSERT_TAIL(&sc->sc_requests, ar, ar_next);
	}

	cv_signal(&sc->sc_cv);
	while (ar->ar_done == false) {
		error = cv_wait_sig(&sc->sc_cv, &sc->sc_lock);
		if (error != 0)
			break;
	}

	last = refcount_release(&ar->ar_refcount);
	if (last) {
		TAILQ_REMOVE(&sc->sc_requests, ar, ar_next);
		uma_zfree(autofs_request_zone, ar);
	}

	AUTOFS_DEBUG("done");

	return (error);
}

/*
 * Unlock the vnode, request automountd(8) action, and then lock it back.
 * If anything got mounted on top of the vnode, return the new filesystem's
 * root vnode in 'newvp', locked.
 */
static int
autofs_trigger_vn(struct vnode *vp, const char *path, int pathlen, struct vnode **newvp)
{
	struct autofs_node *anp = vp->v_data;
	struct autofs_mount *amp = VFSTOAUTOFS(vp->v_mount);
	struct autofs_softc *sc = amp->am_softc;
	int error, lock_flags;

	lock_flags = VOP_ISLOCKED(vp);
	vhold(vp);
	VOP_UNLOCK(vp, 0);

	sx_xlock(&sc->sc_lock);

	/*
	 * XXX: Workaround for mounting the same thing multiple times; revisit.
	 */
	if (vp->v_mountedhere != NULL) {
		error = 0;
		goto mounted;
	}

	error = autofs_trigger(anp, path, pathlen);
mounted:
	sx_xunlock(&sc->sc_lock);
	vn_lock(vp, lock_flags | LK_RETRY);
	vdrop(vp);

	if (error != 0)
		return (error);

	if (vp->v_mountedhere == NULL) {
		*newvp = NULL;
		return (0);
	}

	error = VFS_ROOT(vp->v_mountedhere, lock_flags, newvp);
	if (error != 0) {
		*newvp = NULL;
		return (error);
	}

	return (0);
}

static int
autofs_lookup(struct vop_lookup_args *ap)
{
	struct vnode *dvp = ap->a_dvp;
	struct vnode **vpp = ap->a_vpp;
	struct vnode *newvp;
	struct autofs_node *anp = dvp->v_data;
	struct componentname *cnp = ap->a_cnp;
	int error, lock_flags;

	if (cnp->cn_flags & ISDOTDOT) {
		//AUTOFS_DEBUG("..");
		KASSERT(anp->an_parent != NULL, ("NULL parent"));
		KASSERT(anp->an_parent->an_vnode != NULL, ("NULL parent vnode"));
		lock_flags = VOP_ISLOCKED(dvp);
		vhold(dvp);
		VOP_UNLOCK(dvp, 0);
		vn_lock(anp->an_parent->an_vnode, LK_EXCLUSIVE | LK_RETRY);
		VREF(anp->an_parent->an_vnode);
		*vpp = anp->an_parent->an_vnode;
		vn_lock(dvp, lock_flags | LK_RETRY);
		vdrop(dvp);

		return (0);
	}

	if (cnp->cn_namelen == 1 && cnp->cn_nameptr[0] == '.') {
		//AUTOFS_DEBUG(".");
		VREF(dvp);
		*vpp = dvp;

		return (0);
	} 

	if (anp->an_trigger == true &&
	    autofs_ignore_thread(cnp->cn_thread) == false) {
		error = autofs_trigger_vn(dvp, cnp->cn_nameptr, cnp->cn_namelen, &newvp);
		if (error != 0) {
			//AUTOFS_DEBUG("autofs_trigger_vn failed");
			return (error);
		}

		if (newvp != NULL) {
			//AUTOFS_DEBUG("VOP_LOOKUP");
			error = VOP_LOOKUP(newvp, ap->a_vpp, ap->a_cnp);

			/*
			 * Instead of figuring out whether our node should
			 * be locked or not given the error and cnp flags,
			 * just "copy" the lock status from vnode returned by
			 * mounted filesystem's VOP_LOOKUP() to our own vnode.
			 */
			lock_flags = VOP_ISLOCKED(newvp);
			if (lock_flags == 0)
				VOP_UNLOCK(dvp, 0);
			else
				VOP_UNLOCK(newvp, 0);
			//AUTOFS_DEBUG("VOP_LOOKUP done");
			return (error);
		}
	}

	if (cnp->cn_nameiop == RENAME) {
		//AUTOFS_DEBUG("RENAME");
		return (EOPNOTSUPP);
	}

	error = autofs_find_vnode(anp, cnp->cn_nameptr, cnp->cn_namelen, vpp);
	if (error != 0) {
		if ((cnp->cn_flags & ISLASTCN) && cnp->cn_nameiop == CREATE) {
			//AUTOFS_DEBUG("JUSTRETURN");
			return (EJUSTRETURN);
		}

		//AUTOFS_DEBUG("ENOENT");
		return (ENOENT);
	}

	vn_lock(*vpp, LK_EXCLUSIVE | LK_RETRY);
	VREF(*vpp);

	return (0);
}

static int
autofs_mkdir(struct vop_mkdir_args *ap)
{
	struct vnode *vp = ap->a_dvp;
	struct autofs_node *anp = vp->v_data;
	int error;

	error = autofs_new_vnode(anp, ap->a_cnp->cn_nameptr,
	    ap->a_cnp->cn_namelen, vp->v_mount, ap->a_vpp);

	if (error == 0) {
		vn_lock(*ap->a_vpp, LK_EXCLUSIVE | LK_RETRY);
		VREF(*ap->a_vpp);
	}

	return (error);
}

static int
autofs_readdir_one(struct uio *uio, const char *name, int fileno)
{
	struct dirent dirent;
	int error, i;

	memset(&dirent, 0, sizeof(dirent));
	dirent.d_type = DT_DIR;
	dirent.d_reclen = AUTOFS_DELEN;
	dirent.d_fileno = fileno;
	/* PFS_DELEN was picked to fit PFS_NAMLEN */
	for (i = 0; i < AUTOFS_NAMELEN - 1 && name[i] != '\0'; ++i)
		dirent.d_name[i] = name[i];
	dirent.d_name[i] = 0;
	dirent.d_namlen = i;

	error = uiomove(&dirent, AUTOFS_DELEN, uio);
	return (error);
}

static int
autofs_readdir(struct vop_readdir_args *ap)
{
	struct vnode *vp = ap->a_vp;
	struct vnode *newvp;
	struct autofs_node *child, *anp = vp->v_data;
	struct uio *uio = ap->a_uio;
	off_t offset;
	int error, i, resid;

	KASSERT(vp->v_type == VDIR, ("!VDIR"));

	if (anp->an_trigger == true &&
	    autofs_ignore_thread(curthread) == false) {
		error = autofs_trigger_vn(vp, "", 0, &newvp);
		if (error != 0)
			return (error);

		if (newvp != NULL) {
			error = VOP_READDIR(newvp, ap->a_uio, ap->a_cred,
			    ap->a_eofflag, ap->a_ncookies, ap->a_cookies);
			VOP_UNLOCK(newvp, 0);
			return (error);
		}
	}

	/* only allow reading entire entries */
	offset = uio->uio_offset;
	resid = uio->uio_resid;
	if (offset < 0 || offset % AUTOFS_DELEN != 0 ||
	    (resid && resid < AUTOFS_DELEN))
		return (EINVAL);
	if (resid == 0)
		return (0);

	if (ap->a_eofflag != NULL)
		*ap->a_eofflag = TRUE;

	if (offset == 0 && resid >= AUTOFS_DELEN) {
		error = autofs_readdir_one(uio, ".", anp->an_fileno);
		if (error != 0)
			return (error);
		offset += AUTOFS_DELEN;
		resid -= AUTOFS_DELEN;
	}

	if (offset == AUTOFS_DELEN && resid >= AUTOFS_DELEN) {
		if (anp->an_parent == NULL)
			/*
			 * XXX: Right?
			 */
			error = autofs_readdir_one(uio, "..", anp->an_fileno);
		else
			error = autofs_readdir_one(uio, "..", anp->an_parent->an_fileno);
		if (error != 0)
			return (error);
		offset += AUTOFS_DELEN;
		resid -= AUTOFS_DELEN;
	}

	i = 2; /* Account for "." and "..". */
	TAILQ_FOREACH(child, &anp->an_children, an_next) {
		if (resid < AUTOFS_DELEN) {
			if (ap->a_eofflag != NULL)
				*ap->a_eofflag = 0;
			break;
		}

		/*
		 * Skip entries returned by previous call to getdents().
		 */
		i++;
		if (i * AUTOFS_DELEN <= offset)
			continue;

		error = autofs_readdir_one(uio, child->an_name, child->an_fileno);
		if (error != 0)
			return (error);
		offset += AUTOFS_DELEN;
		resid -= AUTOFS_DELEN;
	}

	return (0);
}

static int
autofs_rmdir(struct vop_rmdir_args *ap)
{
	struct autofs_node *anp = ap->a_dvp->v_data;
	int error;

	error = autofs_delete_vnode(anp, ap->a_vp);

	return (error);
}

static int
autofs_reclaim(struct vop_reclaim_args *ap)
{

	AUTOFS_DEBUG("go");

	return (0);
}

static int
autofs_inactive(struct vop_inactive_args *ap)
{
	struct vnode *vp = ap->a_vp;
	struct autofs_node *anp = vp->v_data;

	//AUTOFS_DEBUG("go");

	KASSERT(TAILQ_EMPTY(&anp->an_children), ("have children"));
	free(anp->an_name, M_AUTOFS);
	uma_zfree(autofs_node_zone, anp);
	vp->v_data = NULL;
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
	.vop_mkdir =		autofs_mkdir,
	.vop_mknod =		VOP_EOPNOTSUPP,
	.vop_read =		VOP_EOPNOTSUPP,
	.vop_readdir =		autofs_readdir,
	.vop_remove =		VOP_EOPNOTSUPP,
	.vop_rename =		VOP_EOPNOTSUPP,
	.vop_rmdir =		autofs_rmdir,
	.vop_setattr =		VOP_EOPNOTSUPP,
	.vop_symlink =		VOP_EOPNOTSUPP,
	.vop_write =		VOP_EOPNOTSUPP,
	.vop_reclaim =		autofs_reclaim,
	.vop_inactive =		autofs_inactive,
};

int
autofs_new_vnode(struct autofs_node *parent, const char *name, int namelen,
    struct mount *mp, struct vnode **vpp)
{
	struct autofs_mount *amp = VFSTOAUTOFS(mp);
	struct autofs_node *anp;
	struct vnode *vp;
	int error;

	error = getnewvnode("autofsvn", mp, &autofs_vnodeops, &vp);
	if (error)
		return (error);

	anp = uma_zalloc(autofs_node_zone, M_WAITOK | M_ZERO);
	if (namelen >= 0)
		anp->an_name = strndup(name, namelen, M_AUTOFS);
	else
		anp->an_name = strdup(name, M_AUTOFS);
	anp->an_fileno = atomic_fetchadd_int(&amp->am_last_fileno, 1);
	anp->an_trigger = true;
	getnanotime(&anp->an_ctime);
	anp->an_parent = parent;
	if (parent != NULL) {
#if 0
		/*
		 * XXX: Release blocked processes?
		 */
		parent->an_trigger = false;
#endif
		TAILQ_INSERT_TAIL(&parent->an_children, anp, an_next);
	}
	TAILQ_INIT(&anp->an_children);

	vn_lock(vp, LK_EXCLUSIVE | LK_RETRY);
	vp->v_type = VDIR;
	if (parent == NULL)
		vp->v_vflag |= VV_ROOT;
	vp->v_data = anp;
	anp->an_vnode = vp;

	error = insmntque(vp, mp);
	if (error != 0) {
		/*
		 * XXX
		 */
		VOP_UNLOCK(vp, 0);
		vrecycle(vp);
		return (error);
	}
	VOP_UNLOCK(vp, 0);

	if (vpp != NULL)
		*vpp = vp;

	return (0);
}

int
autofs_find_vnode(struct autofs_node *parent, const char *name,
    int namelen, struct vnode **vpp)
{
	struct autofs_node *anp;

	TAILQ_FOREACH(anp, &parent->an_children, an_next) {
		if (namelen >= 0) {
			if (strncmp(anp->an_name, name, namelen) != 0)
				continue;
		} else {
			if (strcmp(anp->an_name, name) != 0)
				continue;
		}

		*vpp = anp->an_vnode;
		return (0);
	}

	return (ENOENT);
}

int
autofs_delete_vnode(struct autofs_node *parent, struct vnode *vp)
{
	struct autofs_node *anp, *tmp;

	TAILQ_FOREACH_SAFE(anp, &parent->an_children, an_next, tmp) {
		if (anp->an_vnode != vp)
			continue;

		TAILQ_REMOVE(&parent->an_children, anp, an_next);

#if 0
		if (TAILQ_EMPTY(&parent->an_children))
			parent->an_trigger = true;
#endif

		return (0);
	}

	return (ENOENT);
}
