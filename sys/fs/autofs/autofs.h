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

#define VFSTOAUTOFS(mp)    ((struct autofs_mount *)((mp)->mnt_data))

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

struct autofs_mount {
	TAILQ_ENTRY(autofs_mount)	am_next;
	struct autofs_softc		*am_softc;
	struct vnode			*am_rootvp;
	struct mtx			am_lock;
	struct cv			am_cv;
	char				*am_path;
	bool				am_waiting;
};

struct autofs_softc {
	device_t			sc_dev;
	struct cdev			*sc_cdev;
	struct cv			sc_cv;
	TAILQ_HEAD(, autofs_mount)	sc_mounts;
};


