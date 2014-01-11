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
#include <sys/sx.h>
#include <sys/sysctl.h>
#include <sys/vnode.h>

#include "autofs.h"
#include "autofs_ioctl.h"

MALLOC_DEFINE(M_AUTOFS, "autofs", "Automounter filesystem");

static const char *autofs_opts[] = {
	"from", NULL
};

static int	autofs_ioctl(struct cdev *dev, u_long cmd, caddr_t arg,
		    int mode, struct thread *td);

static struct cdevsw autofs_cdevsw = {
     .d_version = D_VERSION,
     .d_ioctl   = autofs_ioctl,
     .d_name    = "autofs",
};

static struct autofs_softc	*sc;

SYSCTL_NODE(_vfs, OID_AUTO, autofs, CTLFLAG_RD, 0, "Automounter filesystem");
int autofs_debug = 2;
TUNABLE_INT("vfs.autofs.debug", &autofs_debug);
SYSCTL_INT(_vfs_autofs, OID_AUTO, autofs_debug, CTLFLAG_RW,
    &autofs_debug, 2, "Enable debug messages");

int	autofs_rootvp(struct mount *mp, struct vnode **vpp);

static int
autofs_mount(struct mount *mp)
{
	struct autofs_mount *amp;
	char *from, *fspath;
	int error;

	if (vfs_filteropt(mp->mnt_optnew, autofs_opts))
		return (EINVAL);

	if (mp->mnt_flag & MNT_UPDATE)
		return (0);

	if (vfs_getopt(mp->mnt_optnew, "from", (void **)&from, NULL))
		return (EINVAL);

	if (vfs_getopt(mp->mnt_optnew, "fspath", (void **)&fspath, NULL))
		return (EINVAL);

	amp = malloc(sizeof(*amp), M_AUTOFS, M_WAITOK | M_ZERO);
	mp->mnt_data = amp;
	amp->am_softc = sc;
	amp->am_path = strdup(fspath, M_AUTOFS);
	cv_init(&amp->am_cv, "autofs_cv");
	mtx_init(&amp->am_lock, "autofs_lock", NULL, MTX_DEF);

	error = autofs_rootvp(mp, &amp->am_rootvp);
	if (error != 0) {
		/* XXX */
		return (error);
	}

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

static int
autofs_init(struct vfsconf *vfsp)
{
	int error;

	sc = malloc(sizeof(*sc), M_AUTOFS, M_WAITOK | M_ZERO);

	error = make_dev_p(MAKEDEV_CHECKNAME, &sc->sc_cdev, &autofs_cdevsw,
	    NULL, UID_ROOT, GID_WHEEL, 0600, "autofs");
	if (error != 0) {
		AUTOFS_WARN("failed to create device node, error %d", error);
		return (error);
	}
	sc->sc_cdev->si_drv1 = sc;

	TAILQ_INIT(&sc->sc_mounts);
	TAILQ_INIT(&sc->sc_requests);
	cv_init(&sc->sc_cv, "autofs_cv");
	sx_init(&sc->sc_lock, "autofs_lock");

	return (0);
}

static int
autofs_uninit(struct vfsconf *vfsp)
{

	if (sc->sc_cdev != NULL) {
		AUTOFS_DEBUG("removing device node");
		destroy_dev(sc->sc_cdev);
		AUTOFS_DEBUG("device node removed");
	}

	free(sc, M_AUTOFS);
	return (0);
}

static int
autofs_ioctl_wait(struct autofs_softc *sc, struct autofs_wait *aw)
{
	struct autofs_request *ar;
	int error;

	sx_slock(&sc->sc_lock);
	for (;;) {
		ar = TAILQ_FIRST(&sc->sc_requests);
		if (ar == NULL) {
			error = cv_wait_sig(&sc->sc_cv, &sc->sc_lock);
			if (error != 0) {
				sx_sunlock(&sc->sc_lock);

				return (error);
			}
			continue;
		}
		TAILQ_REMOVE(&sc->sc_requests, ar, ar_next);

		strlcpy(aw->aw_path, ar->ar_path, sizeof(aw->aw_path));
		sx_sunlock(&sc->sc_lock);

		return (0);
	}
}

static int
autofs_ioctl(struct cdev *dev, u_long cmd, caddr_t arg, int mode,
    struct thread *td)
{
	struct autofs_softc *sc;

	sc = dev->si_drv1;

	switch (cmd) {
	case AUTOFSWAIT:
		return (autofs_ioctl_wait(sc, (struct autofs_wait *)arg));
	default:
		return (EINVAL);
	}
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
