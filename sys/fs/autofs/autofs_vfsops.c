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

static const char *autofs_opts[] = {
	"export", "from", NULL
};

static int
autofs_cmount(struct mntarg *ma, void *data, uint64_t flags)
{

	return (EDOOFUS);
}

static int
autofs_mount(struct mount *mp)
{

	return (EDOOFUS);
}

static int
autofs_unmount(struct mount *mp, int mntflags)
{

	return (EDOOFUS);
}

static int
autofs_root(struct mount *mp, int flags, struct vnode **vpp)
{

	return (EDOOFUS);
}

static int
autofs_statfs(struct mount *mp, struct statfs *sbp)
{

	return (EDOOFUS);
}

static struct vfsops autofs_vfsops = {
	.vfs_fhtovp =		NULL, /* XXX */
	.vfs_mount =		autofs_mount,
	.vfs_cmount =		autofs_cmount,
	.vfs_root =		autofs_root,
	.vfs_statfs =		autofs_statfs,
	.vfs_unmount =		autofs_unmount,
};

VFS_SET(autofs_vfsops, autofs, 0);
MODULE_VERSION(autofs, 1);
