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

#include <sys/param.h>
#include <sys/systm.h>
#include <sys/buf.h>
#include <sys/conf.h>
#include <sys/ioccom.h>
#include <sys/kernel.h>
#include <sys/module.h>
#include <sys/mount.h>
#include <sys/sx.h>
#include <sys/sysctl.h>
#include <sys/vnode.h>

#include "autofs.h"

static const char *autofs_opts[] = {
	"from", "master_options", NULL
};

extern struct autofs_softc	*sc;

static int
autofs_mount(struct mount *mp)
{
	struct autofs_mount *amp;
	char *from, *fspath, *options;
	int error;

	if (vfs_filteropt(mp->mnt_optnew, autofs_opts))
		return (EINVAL);

	if (mp->mnt_flag & MNT_UPDATE)
		return (0);

	if (vfs_getopt(mp->mnt_optnew, "from", (void **)&from, NULL))
		return (EINVAL);

	if (vfs_getopt(mp->mnt_optnew, "fspath", (void **)&fspath, NULL))
		return (EINVAL);

	if (vfs_getopt(mp->mnt_optnew, "master_options", (void **)&options, NULL))
		options = NULL;

	amp = malloc(sizeof(*amp), M_AUTOFS, M_WAITOK | M_ZERO);
	mp->mnt_data = amp;
	amp->am_softc = sc;
	strlcpy(amp->am_from, from, sizeof(amp->am_from));
	strlcpy(amp->am_mountpoint, fspath, sizeof(amp->am_mountpoint));
	if (options != NULL)
		strlcpy(amp->am_options, options, sizeof(amp->am_options));
	else
		amp->am_options[0] = '\0';
	mtx_init(&amp->am_lock, "autofs_mtx", NULL, MTX_DEF);
	amp->am_last_fileno = 1;

	vfs_getnewfsid(mp);

	error = autofs_new_vnode(NULL, ".", -1, mp, &amp->am_rootvp);
	if (error != 0) {
		/* XXX */
		return (error);
	}
	VOP_UNLOCK(amp->am_rootvp, 0);

	TAILQ_INSERT_TAIL(&sc->sc_mounts, amp, am_next);

	vfs_mountedfrom(mp, from);

	return (0);
}

static int
autofs_unmount(struct mount *mp, int mntflags)
{
	struct autofs_mount *amp;
	int error, flags;

	amp = VFSTOAUTOFS(mp);
	vrele(amp->am_rootvp);

	flags = 0;
	if (mntflags & MNT_FORCE)
		flags |= FORCECLOSE;
	error = vflush(mp, 0, flags, curthread);
	if (error != 0)
		return (error);

	// XXX: Locking.
	TAILQ_REMOVE(&sc->sc_mounts, amp, am_next);
	free(amp, M_AUTOFS);
	mp->mnt_data = NULL;

#if 0
	MNT_ILOCK(mp);
	mp->mnt_flag &= ~MNT_LOCAL;
	MNT_IUNLOCK(mp);
#endif

	return (error);
}

static int
autofs_root(struct mount *mp, int flags, struct vnode **vpp)
{
	struct autofs_mount *amp;
	struct vnode *vp;

	amp = VFSTOAUTOFS(mp);

	vp = amp->am_rootvp;
	VREF(vp);
	vn_lock(vp, flags | LK_RETRY);
	*vpp = vp;

	return (0);
}

static int
autofs_statfs(struct mount *mp, struct statfs *sbp)
{

	sbp->f_bsize = 512;
	sbp->f_iosize = 0;
	sbp->f_blocks = 0;
	sbp->f_bfree = 0;
	sbp->f_bavail = 0;
	sbp->f_files = 0;
	sbp->f_ffree = 0;

	return (0);
}

static struct vfsops autofs_vfsops = {
	.vfs_fhtovp =		NULL, /* XXX */
	.vfs_mount =		autofs_mount,
	.vfs_unmount =		autofs_unmount,
	.vfs_root =		autofs_root,
	.vfs_statfs =		autofs_statfs,
	.vfs_init =		autofs_init,
	.vfs_uninit =		autofs_uninit,
};

VFS_SET(autofs_vfsops, autofs, VFCF_SYNTHETIC | VFCF_NETWORK);
MODULE_VERSION(autofs, 1);
