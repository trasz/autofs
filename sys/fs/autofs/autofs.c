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
#include <sys/dirent.h>
#include <sys/ioccom.h>
#include <sys/kernel.h>
#include <sys/module.h>
#include <sys/mount.h>
#include <sys/refcount.h>
#include <sys/sx.h>
#include <sys/sysctl.h>
#include <sys/vnode.h>
#include <vm/uma.h>

#include "autofs.h"
#include "autofs_ioctl.h"

MALLOC_DEFINE(M_AUTOFS, "autofs", "Automounter filesystem");

uma_zone_t autofs_request_zone;
uma_zone_t autofs_node_zone;

static int	autofs_open(struct cdev *dev, int flags, int fmt,
		    struct thread *td);
static int	autofs_close(struct cdev *dev, int flag, int fmt,
		    struct thread *td);
static int	autofs_ioctl(struct cdev *dev, u_long cmd, caddr_t arg,
		    int mode, struct thread *td);

static struct cdevsw autofs_cdevsw = {
     .d_version = D_VERSION,
     .d_open   = autofs_open,
     .d_close   = autofs_close,
     .d_ioctl   = autofs_ioctl,
     .d_name    = "autofs",
};

struct autofs_softc	*sc;

SYSCTL_NODE(_vfs, OID_AUTO, autofs, CTLFLAG_RD, 0, "Automounter filesystem");
int autofs_debug = 2;
TUNABLE_INT("vfs.autofs.debug", &autofs_debug);
SYSCTL_INT(_vfs_autofs, OID_AUTO, autofs_debug, CTLFLAG_RWTUN,
    &autofs_debug, 2, "Enable debug messages");
int autofs_mount_on_stat = 1;
TUNABLE_INT("vfs.autofs.mount_on_stat", &autofs_mount_on_stat);
SYSCTL_INT(_vfs_autofs, OID_AUTO, autofs_mount_on_stat, CTLFLAG_RWTUN,
    &autofs_mount_on_stat, 1, "Enable debug messages");
int autofs_timeout = 10;
TUNABLE_INT("vfs.autofs.timeout", &autofs_timeout);
SYSCTL_INT(_vfs_autofs, OID_AUTO, autofs_timeout, CTLFLAG_RWTUN,
    &autofs_timeout, 10, "Number of seconds to wait for automountd(8)");

int
autofs_init(struct vfsconf *vfsp)
{
	int error;

	sc = malloc(sizeof(*sc), M_AUTOFS, M_WAITOK | M_ZERO);

	autofs_request_zone = uma_zcreate("autofs_request",
	    sizeof(struct autofs_request), NULL, NULL, NULL, NULL,
	    UMA_ALIGN_PTR, 0);
	autofs_node_zone = uma_zcreate("autofs_node",
	    sizeof(struct autofs_node), NULL, NULL, NULL, NULL,
	    UMA_ALIGN_PTR, 0);

	TAILQ_INIT(&sc->sc_requests);
	cv_init(&sc->sc_cv, "autofscv");
	sx_init(&sc->sc_lock, "autofslk");

	error = make_dev_p(MAKEDEV_CHECKNAME, &sc->sc_cdev, &autofs_cdevsw,
	    NULL, UID_ROOT, GID_WHEEL, 0600, "autofs");
	if (error != 0) {
		AUTOFS_WARN("failed to create device node, error %d", error);
		free(sc, M_AUTOFS);
		return (error);
	}
	sc->sc_cdev->si_drv1 = sc;

	return (0);
}

int
autofs_uninit(struct vfsconf *vfsp)
{

	sx_xlock(&sc->sc_lock);
	if (sc->sc_dev_opened) {
		sx_xunlock(&sc->sc_lock);
		return (EBUSY);
	}
	if (sc->sc_cdev != NULL) {
		//AUTOFS_DEBUG("removing device node");
		destroy_dev(sc->sc_cdev);
		//AUTOFS_DEBUG("device node removed");
	}

	uma_zdestroy(autofs_request_zone);
	uma_zdestroy(autofs_node_zone);

	sx_xunlock(&sc->sc_lock);
	/*
	 * XXX: Race with open?
	 */
	free(sc, M_AUTOFS);

	return (0);
}

bool
autofs_ignore_thread(const struct thread *td)
{
	struct proc *p = td->td_proc;

	if (sc->sc_dev_opened == false)
		return (false);

	PROC_LOCK(p);
	if (p->p_flag2 & P2_AUTOMOUNTD) {
		AUTOFS_DEBUG("must pass pid %d (%s)", p->p_pid, p->p_comm);
		PROC_UNLOCK(p);
		return (true);
	}
	AUTOFS_DEBUG("must hold pid %d (%s)", p->p_pid, p->p_comm);
	PROC_UNLOCK(p);

	return (false);
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

static void
autofs_callout(void *context)
{
	struct autofs_request *ar = context;
	struct autofs_softc *sc = ar->ar_softc;

	sx_xlock(&sc->sc_lock);
	AUTOFS_DEBUG("timing out request %d", ar->ar_id);
	/*
	 * XXX: EIO perhaps?
	 */
	ar->ar_error = ETIMEDOUT;
	ar->ar_done = true;
	ar->ar_in_progress = false;
	cv_signal(&sc->sc_cv);
	sx_xunlock(&sc->sc_lock);
}

/*
 * Send request to automountd(8) and wait for completion.
 */
int
autofs_trigger(struct autofs_node *anp, const char *component, int componentlen)
{
	struct autofs_mount *amp = VFSTOAUTOFS(anp->an_vnode->v_mount);
	struct autofs_softc *sc = amp->am_softc;
	struct autofs_node *firstanp;
	struct autofs_request *ar;
	char *key, *path;
	int error = 0, request_error, last;

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
		callout_init(&ar->ar_callout, 1);
		callout_reset(&ar->ar_callout, autofs_timeout * hz, autofs_callout, ar);
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

	request_error = ar->ar_error;

	AUTOFS_DEBUG("done with %s %s %s", ar->ar_from, ar->ar_key, ar->ar_path);
	last = refcount_release(&ar->ar_refcount);
	if (last) {
		TAILQ_REMOVE(&sc->sc_requests, ar, ar_next);
		/*
		 * XXX
		 */
		sx_xunlock(&sc->sc_lock);
		callout_drain(&ar->ar_callout);
		sx_xlock(&sc->sc_lock);
		uma_zfree(autofs_request_zone, ar);
	}

	//AUTOFS_DEBUG("done");

	if (error != 0)
		return (error);
	return (request_error);
}

static int
autofs_ioctl_request(struct autofs_softc *sc, struct autofs_daemon_request *adr)
{
	struct autofs_request *ar;
	int error;

	AUTOFS_DEBUG("go");

	sx_xlock(&sc->sc_lock);
	for (;;) {
		TAILQ_FOREACH(ar, &sc->sc_requests, ar_next) {
			if (ar->ar_done)
				continue;
			if (ar->ar_in_progress)
				continue;

			break;
		}

		if (ar != NULL)
			break;

		error = cv_wait_sig(&sc->sc_cv, &sc->sc_lock);
		if (error != 0) {
			sx_xunlock(&sc->sc_lock);
			AUTOFS_DEBUG("failed with error %d", error);
			return (error);
		}
	}

	ar->ar_in_progress = true;
	sx_xunlock(&sc->sc_lock);

	adr->adr_id = ar->ar_id;
	strlcpy(adr->adr_from, ar->ar_from, sizeof(adr->adr_from));
	strlcpy(adr->adr_path, ar->ar_path, sizeof(adr->adr_path));
	strlcpy(adr->adr_prefix, ar->ar_prefix, sizeof(adr->adr_prefix));
	strlcpy(adr->adr_key, ar->ar_key, sizeof(adr->adr_key));
	strlcpy(adr->adr_options, ar->ar_options, sizeof(adr->adr_options));

	PROC_LOCK(curproc);
	curproc->p_flag2 |= P2_AUTOMOUNTD;
	PROC_UNLOCK(curproc);

	AUTOFS_DEBUG("done");

	return (0);
}

static int
autofs_ioctl_done(struct autofs_softc *sc, struct autofs_daemon_done *add)
{
	struct autofs_request *ar;

	AUTOFS_DEBUG("request %d, error %d", add->add_id, add->add_error);

	sx_xlock(&sc->sc_lock);
	TAILQ_FOREACH(ar, &sc->sc_requests, ar_next) {
		if (ar->ar_id == add->add_id)
			break;
	}

	if (ar == NULL) {
		sx_xunlock(&sc->sc_lock);
		AUTOFS_DEBUG("id %d not found", add->add_id);
		return (ESRCH);
	}

	ar->ar_error = add->add_error;
	ar->ar_done = true;
	ar->ar_in_progress = false;
	cv_signal(&sc->sc_cv);

	sx_xunlock(&sc->sc_lock);

	AUTOFS_DEBUG("done");

	return (0);
}

static int
autofs_open(struct cdev *dev, int flags, int fmt, struct thread *td)
{

	sx_xlock(&sc->sc_lock);
	if (sc->sc_dev_opened) {
		sx_xunlock(&sc->sc_lock);
		return (EBUSY);
	}

	sc->sc_dev_opened = true;
	sx_xunlock(&sc->sc_lock);

	return (0);
}

static int
autofs_close(struct cdev *dev, int flag, int fmt, struct thread *td)
{

	sx_xlock(&sc->sc_lock);
	KASSERT(sc->sc_dev_opened, ("not opened?"));
	sc->sc_dev_opened = false;
	sx_xunlock(&sc->sc_lock);

	return (0);
}

static int
autofs_ioctl(struct cdev *dev, u_long cmd, caddr_t arg, int mode,
    struct thread *td)
{
	//struct autofs_softc *sc = dev->si_drv1;

	KASSERT(sc->sc_dev_opened, ("not opened?"));

	switch (cmd) {
	case AUTOFSREQUEST:
		return (autofs_ioctl_request(sc,
		    (struct autofs_daemon_request *)arg));
	case AUTOFSDONE:
		return (autofs_ioctl_done(sc,
		    (struct autofs_daemon_done *)arg));
	default:
		AUTOFS_DEBUG("invalid cmd %lx", cmd);
		return (EINVAL);
	}
}
