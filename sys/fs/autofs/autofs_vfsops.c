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
#include <sys/kernel.h>
#include <sys/module.h>
#include <sys/mount.h>
#include <sys/vnode.h>

static const char *autofs_opts[] = {
	"export", "from", NULL
};

static struct vnode *rootvp;

int	autofs_rootvp(struct mount *mp, struct vnode **vpp);

static int
autofs_mount(struct mount *mp)
{
	int error;
	char *from;

	if (vfs_filteropt(mp->mnt_optnew, autofs_opts))
		return (EINVAL);

	if (mp->mnt_flag & MNT_UPDATE)
		return (0);

	if (vfs_getopt(mp->mnt_optnew, "from", (void **)&from, NULL))
		return (EINVAL);

	error = autofs_rootvp(mp, &rootvp);
	if (error != 0)
		return (error);

	printf("rootvp is %p\n", rootvp);

	vfs_mountedfrom(mp, from);

	return (0);
}

static int
autofs_unmount(struct mount *mp, int mntflags)
{
	int error, flags;

	vrele(rootvp);

	flags = 0;
	if (mntflags & MNT_FORCE)
		flags |= FORCECLOSE;
	error = vflush(mp, 0, flags, curthread);
	if (error != 0)
		return (error);

#if 0
	amp = VFSTOAUTOFS(mp);
	lockdestroy(&pmp->pm_fatlock);
	free(pmp, M_MSDOSFSMNT);
#endif
	mp->mnt_data = NULL;
	MNT_ILOCK(mp);
	mp->mnt_flag &= ~MNT_LOCAL;
	MNT_IUNLOCK(mp);

	return (error);
}

static int
autofs_root(struct mount *mp, int flags, struct vnode **vpp)
{
	struct vnode *vp;

	vp = rootvp;
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
	.vfs_root =		autofs_root,
	.vfs_statfs =		autofs_statfs,
	.vfs_unmount =		autofs_unmount,
};

VFS_SET(autofs_vfsops, autofs, VFCF_SYNTHETIC | VFCF_NETWORK);
MODULE_VERSION(autofs, 1);
