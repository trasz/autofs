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

static int	autofs_trigger_vn(struct vnode *vp, const char *path,
		    int pathlen, struct vnode **newvp);

static int
autofs_access(struct vop_access_args *ap)
{

	return (0);
}

static int
autofs_getattr(struct vop_getattr_args *ap)
{
	struct vnode *vp = ap->a_vp;
	struct vnode *newvp;
	struct autofs_node *anp = vp->v_data;
	struct mount *mp = vp->v_mount;
	struct vattr *vap = ap->a_vap;
	int error;

	KASSERT(ap->a_vp->v_type == VDIR, ("!VDIR"));

	/*
	 * The reason we must do this is that some tree-walking software,
	 * namely fts(3), assumes that stat(".") results won't change
	 * between chdir("subdir") and chdir(".."), and fails with ENOENT
	 * otherwise.
	 */
	if (autofs_mount_on_stat && anp->an_trigger == true &&
	    autofs_ignore_thread(curthread) == false) {
		error = autofs_trigger_vn(vp, "", 0, &newvp);
		if (error != 0)
			return (error);

		if (newvp != NULL) {
			error = VOP_GETATTR(newvp, ap->a_vap,
			    ap->a_cred);
			vput(newvp);
			return (error);
		}
	}

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

static char *
autofs_path(struct autofs_node *anp)
{
	struct autofs_mount *amp = anp->an_mount;
	char *path, *tmp;

	path = strdup("", M_AUTOFS);
	for (; anp->an_parent != NULL; anp = anp->an_parent) {
		tmp = malloc(strlen(anp->an_name) + strlen(path) + 2, M_AUTOFS, M_WAITOK);
		strcpy(tmp, anp->an_name);
		strcat(tmp, "/");
		strcat(tmp, path);
		free(path, M_AUTOFS);
		path = tmp;
		tmp = NULL;
	}

	tmp = malloc(strlen(amp->am_mountpoint) + strlen(path) + 2, M_AUTOFS, M_WAITOK);
	strcpy(tmp, amp->am_mountpoint);
	strcat(tmp, "/");
	strcat(tmp, path);
	free(path, M_AUTOFS);
	path = tmp;
	tmp = NULL;

	//AUTOFS_DEBUG("returning \"%s\"", path);
	return (path);
}

/*
 * Send request to automountd(8) and wait for completion.
 */
static int
autofs_trigger(struct autofs_node *anp, const char *component, int componentlen)
{
	struct autofs_mount *amp = VFSTOAUTOFS(anp->an_vnode->v_mount);
	struct autofs_softc *sc = amp->am_softc;
	struct autofs_node *firstanp;
	struct autofs_request *ar;
	char *key, *path;
	int error = 0, last;

	sx_assert(&sc->sc_lock, SA_XLOCKED);

	if (anp->an_parent == NULL) {
		key = strndup(component, componentlen, M_AUTOFS);
	} else {
		for (firstanp = anp; firstanp->an_parent->an_parent != NULL;
		    firstanp = firstanp->an_parent)
			continue;
		key = strdup(firstanp->an_name, M_AUTOFS);
	}

	path = autofs_path(anp);

	//AUTOFS_DEBUG("mountpoint '%s', key '%s', path '%s'", amp->am_mountpoint, key, path);

	TAILQ_FOREACH(ar, &sc->sc_requests, ar_next) {
		if (strcmp(ar->ar_path, path) != 0)
			continue;
		if (strcmp(ar->ar_key, key) != 0)
			continue;

		KASSERT(strcmp(ar->ar_from, amp->am_from) == 0,
		    ("from changed; %s != %s", ar->ar_from, amp->am_from));
		KASSERT(strcmp(ar->ar_prefix, amp->am_prefix) == 0,
		    ("prefix changed; %s != %s", ar->ar_prefix, amp->am_prefix));
		KASSERT(strcmp(ar->ar_options, amp->am_options) == 0,
		    ("options changed; %s != %s", ar->ar_options, amp->am_options));

		break;
	}

	if (ar != NULL) {
		AUTOFS_DEBUG("found existing request for %s %s %s", ar->ar_from, ar->ar_key, ar->ar_path);
		refcount_acquire(&ar->ar_refcount);
	} else {
		ar = uma_zalloc(autofs_request_zone, M_WAITOK | M_ZERO);
		ar->ar_softc = amp->am_softc;

		ar->ar_id = ++amp->am_last_request_id;
		strlcpy(ar->ar_from, amp->am_from, sizeof(ar->ar_from));
		strlcpy(ar->ar_path, path, sizeof(ar->ar_path));
		strlcpy(ar->ar_prefix, amp->am_prefix, sizeof(ar->ar_prefix));
		strlcpy(ar->ar_key, key, sizeof(ar->ar_key));
		strlcpy(ar->ar_options, amp->am_options, sizeof(ar->ar_options));

		AUTOFS_DEBUG("new request for %s %s %s", ar->ar_from, ar->ar_key, ar->ar_path);
		refcount_init(&ar->ar_refcount, 1);
		TAILQ_INSERT_TAIL(&sc->sc_requests, ar, ar_next);
	}
	free(key, M_AUTOFS);
	free(path, M_AUTOFS);

	cv_signal(&sc->sc_cv);
	while (ar->ar_done == false) {
		error = cv_wait_sig(&sc->sc_cv, &sc->sc_lock);
		if (error != 0)
			break;
	}

	AUTOFS_DEBUG("done with %s %s %s", ar->ar_from, ar->ar_key, ar->ar_path);
	last = refcount_release(&ar->ar_refcount);
	if (last) {
		TAILQ_REMOVE(&sc->sc_requests, ar, ar_next);
		uma_zfree(autofs_request_zone, ar);
	}

	//AUTOFS_DEBUG("done");

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

	/*
	 * Release the vnode lock, so that other operations, in partcular
	 * mounting a filesystem on top of it, can proceed.
	 */
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
	if ((vp->v_iflag & VI_DOOMED) != 0) {
		AUTOFS_DEBUG("VI_DOOMED");
		return (ENOENT);
	}

	if (error != 0)
		return (error);

	if (vp->v_mountedhere == NULL) {
		*newvp = NULL;
		return (0);
	}

	error = vfs_busy(vp->v_mountedhere, 0);
	if (error) {
		AUTOFS_DEBUG("vfs_busy failed");
		*newvp = NULL;
		return (0);
	}

	error = VFS_ROOT(vp->v_mountedhere, lock_flags, newvp);
	if (error != 0) {
		vfs_unbusy(vp->v_mountedhere);
		return (error);
	}

	vfs_unbusy(vp->v_mountedhere);
	return (0);
}

static int
autofs_lookup(struct vop_lookup_args *ap)
{
	struct vnode *dvp = ap->a_dvp;
	struct vnode **vpp = ap->a_vpp;
	struct vnode *newvp;
	struct mount *mp = dvp->v_mount;
	struct autofs_mount *amp = VFSTOAUTOFS(mp);
	struct autofs_node *anp = dvp->v_data;
	struct autofs_node *child;
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
		vref(anp->an_parent->an_vnode);
		*vpp = anp->an_parent->an_vnode;
		vn_lock(dvp, lock_flags | LK_RETRY);
		vdrop(dvp);

		return (0);
	}

	if (cnp->cn_namelen == 1 && cnp->cn_nameptr[0] == '.') {
		//AUTOFS_DEBUG(".");
		vref(dvp);
		*vpp = dvp;

		return (0);
	} 

	if (anp->an_trigger == true &&
	    autofs_ignore_thread(cnp->cn_thread) == false) {
		error = autofs_trigger_vn(dvp, cnp->cn_nameptr, cnp->cn_namelen, &newvp);
		if (error != 0) {
			//AUTOFS_DEBUG("autofs_trigger_vn failed, error %d", error);
			return (error);
		}

		if (newvp != NULL) {
			//AUTOFS_DEBUG("VOP_LOOKUP");
			error = VOP_LOOKUP(newvp, ap->a_vpp, ap->a_cnp);

			/*
			 * Instead of figuring out whether our vnode should
			 * be locked or not given the error and cnp flags,
			 * just "copy" the lock status from vnode returned
			 * by mounted filesystem's VOP_LOOKUP().  Get rid
			 * of that new vnode afterwards.
			 */
			lock_flags = VOP_ISLOCKED(newvp);
			if (lock_flags == 0) {
				VOP_UNLOCK(dvp, 0);
				vrele(newvp);
			} else {
				vput(newvp);
			}
			//if (error != 0)
			//	AUTOFS_DEBUG("VOP_LOOKUP done with error %d", error);
			return (error);
		}
	}

	if (cnp->cn_nameiop == RENAME) {
		//AUTOFS_DEBUG("RENAME");
		return (EOPNOTSUPP);
	}

	AUTOFS_LOCK(amp);
	error = autofs_node_find(anp, cnp->cn_nameptr, cnp->cn_namelen, &child);
	if (error != 0) {
		if ((cnp->cn_flags & ISLASTCN) && cnp->cn_nameiop == CREATE) {
			//AUTOFS_DEBUG("JUSTRETURN");
			AUTOFS_UNLOCK(amp);
			return (EJUSTRETURN);
		}

		//AUTOFS_DEBUG("ENOENT");
		AUTOFS_UNLOCK(amp);
		return (ENOENT);
	}

	error = autofs_node_vn(child, mp, vpp);
	if (error != 0) {
		if ((cnp->cn_flags & ISLASTCN) && cnp->cn_nameiop == CREATE) {
			//AUTOFS_DEBUG("JUSTRETURN");
			AUTOFS_UNLOCK(amp);
			return (EJUSTRETURN);
		}

		//AUTOFS_DEBUG("autofs_node_vn failed with error %d", error);
		AUTOFS_UNLOCK(amp);
		return (error);
	}

	AUTOFS_UNLOCK(amp);
	return (0);
}

static int
autofs_mkdir(struct vop_mkdir_args *ap)
{
	struct vnode *vp = ap->a_dvp;
	struct autofs_node *anp = vp->v_data;
	struct autofs_mount *amp = VFSTOAUTOFS(vp->v_mount);
	struct autofs_node *child;
	int error;

	AUTOFS_LOCK(amp);
	error = autofs_node_new(anp, amp, ap->a_cnp->cn_nameptr,
	    ap->a_cnp->cn_namelen, &child);
	if (error) {
		AUTOFS_UNLOCK(amp);
		return (error);
	}

	error = autofs_node_vn(child, vp->v_mount, ap->a_vpp);
	AUTOFS_UNLOCK(amp);

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
	struct autofs_mount *amp = VFSTOAUTOFS(vp->v_mount);
	struct autofs_node *anp = vp->v_data;
	struct autofs_node *child;
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
			vput(newvp);
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
	AUTOFS_LOCK(amp);
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
		if (error != 0) {
			AUTOFS_UNLOCK(amp);
			return (error);
		}
		offset += AUTOFS_DELEN;
		resid -= AUTOFS_DELEN;
	}

	AUTOFS_UNLOCK(amp);
	return (0);
}

#if 0 /* Since automountd(8) doesn't actually use it. */
static int
autofs_rmdir(struct vop_rmdir_args *ap)
{
	struct autofs_node *anp = ap->a_vp->v_data;
	int error;

	autofs_node_delete(anp);

	return (0);
}
#endif

static int
autofs_reclaim(struct vop_reclaim_args *ap)
{
	struct vnode *vp = ap->a_vp;
	struct autofs_node *anp = vp->v_data;
	struct autofs_mount *amp = anp->an_mount;

	/*
	 * We don't free autofs_node here; instead we're
	 * destroying them in autofs_node_delete().
	 */
	AUTOFS_LOCK(amp);
	anp->an_vnode = NULL;
	vp->v_data = NULL;
	AUTOFS_UNLOCK(amp);

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
#if 0
	.vop_rmdir =		autofs_rmdir,
#else
	.vop_rmdir =		VOP_EOPNOTSUPP,
#endif
	.vop_setattr =		VOP_EOPNOTSUPP,
	.vop_symlink =		VOP_EOPNOTSUPP,
	.vop_write =		VOP_EOPNOTSUPP,
	.vop_reclaim =		autofs_reclaim,
};

int
autofs_node_new(struct autofs_node *parent, struct autofs_mount *amp,
    const char *name, int namelen, struct autofs_node **anpp)
{
	struct autofs_node *anp;

	if (parent != NULL)
		AUTOFS_LOCK_ASSERT(parent->an_mount);

	anp = uma_zalloc(autofs_node_zone, M_WAITOK | M_ZERO);
	if (namelen >= 0)
		anp->an_name = strndup(name, namelen, M_AUTOFS);
	else
		anp->an_name = strdup(name, M_AUTOFS);
	anp->an_fileno = atomic_fetchadd_int(&amp->am_last_fileno, 1);
	anp->an_trigger = true;
	getnanotime(&anp->an_ctime);
	anp->an_parent = parent;
	anp->an_mount = amp;
	if (parent != NULL)
		TAILQ_INSERT_TAIL(&parent->an_children, anp, an_next);
	TAILQ_INIT(&anp->an_children);

	*anpp = anp;
	return (0);
}

int
autofs_node_find(struct autofs_node *parent, const char *name,
    int namelen, struct autofs_node **anpp)
{
	struct autofs_node *anp;

	AUTOFS_LOCK_ASSERT(parent->an_mount);

	TAILQ_FOREACH(anp, &parent->an_children, an_next) {
		if (namelen >= 0) {
			if (strncmp(anp->an_name, name, namelen) != 0)
				continue;
		} else {
			if (strcmp(anp->an_name, name) != 0)
				continue;
		}

		*anpp = anp;
		return (0);
	}

	return (ENOENT);
}

void
autofs_node_delete(struct autofs_node *anp)
{
	struct autofs_node *parent;

	AUTOFS_LOCK_ASSERT(anp->an_mount);
	KASSERT(TAILQ_EMPTY(&anp->an_children), ("have children"));

	parent = anp->an_parent;
	if (parent != NULL)
		TAILQ_REMOVE(&parent->an_children, anp, an_next);
	free(anp->an_name, M_AUTOFS);
	uma_zfree(autofs_node_zone, anp);
}

int
autofs_node_vn(struct autofs_node *anp, struct mount *mp, struct vnode **vpp)
{
	struct vnode *vp;
	int error;

	AUTOFS_LOCK_ASSERT(anp->an_mount);

	if (anp->an_vnode) {
		vp = anp->an_vnode;
		vn_lock(vp, LK_EXCLUSIVE | LK_RETRY);
		vref(vp);

		*vpp = vp;
		return (0);
	}

	error = getnewvnode("autofs", mp, &autofs_vnodeops, &vp);
	if (error)
		return (error);

	vn_lock(vp, LK_EXCLUSIVE | LK_RETRY);
	vhold(vp);

	vp->v_type = VDIR;
	if (anp->an_parent == NULL)
		vp->v_vflag |= VV_ROOT;
	vp->v_data = anp;
	anp->an_vnode = vp;

	error = insmntque(vp, mp);
	if (error != 0) {
		anp->an_vnode = NULL;
		vdrop(vp);
		return (error);
	}

	*vpp = vp;
	return (0);
}
