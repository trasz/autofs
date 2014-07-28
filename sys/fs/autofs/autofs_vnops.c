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
#include <sys/signalvar.h>
#include <sys/systm.h>
#include <sys/vnode.h>
#include <machine/atomic.h>
#include <vm/uma.h>

#include "autofs.h"

static int	autofs_trigger_vn(struct vnode *vp, const char *path,
		    int pathlen, struct vnode **newvp);

static int
autofs_access(struct vop_access_args *ap)
{

	/*
	 * Nothing to do here; the only kind of access control
	 * needed is in autofs_mkdir().
	 */

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
	if (autofs_mount_on_stat && autofs_cached(anp, NULL, 0) == false &&
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
	 * mounting a filesystem on top of it, can proceed.  Increase hold
	 * count, to prevent the vnode from being deallocated.
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
	} else {
		/*
		 * If the operation that succeeded was mount, then mark
		 * the node as non-cached.  Otherwise, if someone unmounts
		 * the filesystem before the cache times out, we'll fail
		 * to trigger.
		 */
		anp->an_cached = false;
		//AUTOFS_DEBUG("node %s covered by mount; uncaching",
		//    anp->an_name);
	}

	error = vfs_busy(vp->v_mountedhere, 0);
	if (error != 0) {
		AUTOFS_WARN("vfs_busy failed");
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
		/*
		 * Note that in this case, dvp is the child vnode, and we're
		 * looking up the parent vnode - exactly reverse from normal
		 * operation.  To preserve lock order, we unlock the child
		 * (dvp), obtain the lock on parent (*vpp) in autofs_node_vn(),
		 * then relock the child.  We use vhold()/vdrop() to prevent
		 * dvp from being freed in the meantime.
		 */
		lock_flags = VOP_ISLOCKED(dvp);
		vhold(dvp);
		VOP_UNLOCK(dvp, 0);
		error = autofs_node_vn(anp->an_parent, mp, vpp);
		if (error != 0) {
			AUTOFS_WARN("autofs_node_vn() failed with error %d",
			    error);
		}
		vn_lock(dvp, lock_flags | LK_RETRY);
		vdrop(dvp);

		return (error);
	}

	if (cnp->cn_namelen == 1 && cnp->cn_nameptr[0] == '.') {
		//AUTOFS_DEBUG(".");
		vref(dvp);
		*vpp = dvp;

		return (0);
	}

	if (autofs_cached(anp, cnp->cn_nameptr, cnp->cn_namelen) == false &&
	    autofs_ignore_thread(cnp->cn_thread) == false) {
		error = autofs_trigger_vn(dvp, cnp->cn_nameptr, cnp->cn_namelen, &newvp);
		if (error != 0) {
			//AUTOFS_DEBUG("autofs_trigger_vn failed, error %d", error);
			return (error);
		}

		if (newvp != NULL) {
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

	/*
	 * XXX: Dropping the node here is ok, because we never remove nodes.
	 */
	AUTOFS_UNLOCK(amp);

	error = autofs_node_vn(child, mp, vpp);
	if (error != 0) {
		if ((cnp->cn_flags & ISLASTCN) && cnp->cn_nameiop == CREATE) {
			//AUTOFS_DEBUG("JUSTRETURN");
			return (EJUSTRETURN);
		}

		//AUTOFS_DEBUG("autofs_node_vn failed with error %d", error);
		return (error);
	}

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

	/*
	 * Don't allow mkdir() if the calling thread is not
	 * automountd(8) descendant.
	 */
	if (autofs_ignore_thread(curthread) == false)
		return (EPERM);

	AUTOFS_LOCK(amp);
	error = autofs_node_new(anp, amp, ap->a_cnp->cn_nameptr,
	    ap->a_cnp->cn_namelen, &child);
	if (error != 0) {
		AUTOFS_UNLOCK(amp);
		return (error);
	}
	AUTOFS_UNLOCK(amp);

	error = autofs_node_vn(child, vp->v_mount, ap->a_vpp);

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

	if (autofs_cached(anp, NULL, 0) == false &&
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

static int
autofs_reclaim(struct vop_reclaim_args *ap)
{
	struct vnode *vp = ap->a_vp;
	struct autofs_node *anp = vp->v_data;

	/*
	 * We don't free autofs_node here; instead we're
	 * destroying them in autofs_node_delete().
	 */
	sx_xlock(&anp->an_vnode_lock);
	anp->an_vnode = NULL;
	vp->v_data = NULL;
	sx_xunlock(&anp->an_vnode_lock);

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
	.vop_rmdir =		VOP_EOPNOTSUPP,
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
	callout_init(&anp->an_callout, 1);
	/*
	 * The reason for SX_NOWITNESS here is that witness(4)
	 * can't tell vnodes apart, so the following perfectly
	 * valid lock order...
	 *
	 * vnode lock A -> autofsvlk B -> vnode lock B
	 *
	 * ... gets reported as a LOR.
	 */
	sx_init_flags(&anp->an_vnode_lock, "autofsvlk", SX_NOWITNESS);
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

		if (anpp != NULL)
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

	callout_drain(&anp->an_callout);

	parent = anp->an_parent;
	if (parent != NULL)
		TAILQ_REMOVE(&parent->an_children, anp, an_next);
	sx_destroy(&anp->an_vnode_lock);
	free(anp->an_name, M_AUTOFS);
	uma_zfree(autofs_node_zone, anp);
}

int
autofs_node_vn(struct autofs_node *anp, struct mount *mp, struct vnode **vpp)
{
	struct vnode *vp;
	int error;

	AUTOFS_LOCK_ASSERT_NOT(anp->an_mount);

	sx_xlock(&anp->an_vnode_lock);

	vp = anp->an_vnode;
	if (vp != NULL) {
		error = vget(vp, LK_EXCLUSIVE | LK_RETRY, curthread);
		if (error != 0) {
			AUTOFS_WARN("vget failed with error %d", error);
			sx_xunlock(&anp->an_vnode_lock);
			return (error);
		}
		if (vp->v_iflag & VI_DOOMED) {
			/*
			 * We got forcibly unmounted.
			 */
			AUTOFS_DEBUG("doomed vnode");
			sx_xunlock(&anp->an_vnode_lock);
			vput(vp);

			return (ENOENT);
		}

		*vpp = vp;
		sx_xunlock(&anp->an_vnode_lock);
		return (0);
	}

	error = getnewvnode("autofs", mp, &autofs_vnodeops, &vp);
	if (error != 0) {
		sx_xunlock(&anp->an_vnode_lock);
		return (error);
	}

	error = vn_lock(vp, LK_EXCLUSIVE | LK_RETRY);
	if (error != 0) {
		sx_xunlock(&anp->an_vnode_lock);
		vdrop(vp);
		return (error);
	}

	vp->v_type = VDIR;
	if (anp->an_parent == NULL)
		vp->v_vflag |= VV_ROOT;
	vp->v_data = anp;

	error = insmntque(vp, mp);
	if (error != 0) {
		AUTOFS_WARN("insmntque() failed with error %d", error);
		sx_xunlock(&anp->an_vnode_lock);
		return (error);
	}

	KASSERT(anp->an_vnode == NULL, ("lost race"));
	anp->an_vnode = vp;

	sx_xunlock(&anp->an_vnode_lock);

	*vpp = vp;
	return (0);
}
