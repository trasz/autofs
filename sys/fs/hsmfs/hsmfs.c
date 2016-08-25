/*-
 * Copyright (c) 2016 Edward Tomasz Napierala <trasz@FreeBSD.org>
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
 */
/*-
 * Copyright (c) 1989, 1991, 1993, 1995
 *	The Regents of the University of California.  All rights reserved.
 *
 * This code is derived from software contributed to Berkeley by
 * Rick Macklem at The University of Guelph.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 * 4. Neither the name of the University nor the names of its contributors
 *    may be used to endorse or promote products derived from this software
 *    without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE REGENTS AND CONTRIBUTORS ``AS IS'' AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED.  IN NO EVENT SHALL THE REGENTS OR CONTRIBUTORS BE LIABLE
 * FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS
 * OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY
 * OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 *
 */

#include <sys/cdefs.h>
 __FBSDID("$FreeBSD$");

#include <sys/param.h>
#include <sys/systm.h>
#include <sys/buf.h>
#include <sys/conf.h>
#include <sys/dirent.h>
#include <sys/extattr.h>
#include <sys/ioccom.h>
#include <sys/kernel.h>
#include <sys/module.h>
#include <sys/mount.h>
#include <sys/refcount.h>
#include <sys/sx.h>
#include <sys/stat.h>
#include <sys/sysctl.h>
#include <sys/syscallsubr.h>
#include <sys/taskqueue.h>
#include <sys/tree.h>
#include <sys/vnode.h>
#include <machine/atomic.h>
#include <vm/uma.h>

#include <fs/hsmfs/hsmfs.h>
#include <fs/hsmfs/hsmfs_ioctl.h>
#include <fs/hsmfs/null.h>

#pragma clang optimize off

MALLOC_DEFINE(M_HSMFS, "hsmfs", "Hierarchical Storage Management filesystem");

uma_zone_t hsmfs_request_zone;

static int	hsmfs_open(struct cdev *dev, int flags, int fmt,
		    struct thread *td);
static int	hsmfs_close(struct cdev *dev, int flag, int fmt,
		    struct thread *td);
static int	hsmfs_ioctl(struct cdev *dev, u_long cmd, caddr_t arg,
		    int mode, struct thread *td);

static struct cdevsw hsmfs_cdevsw = {
     .d_version = D_VERSION,
     .d_open    = hsmfs_open,
     .d_close   = hsmfs_close,
     .d_ioctl   = hsmfs_ioctl,
     .d_name    = "hsmfs",
};

/*
 * List of signals that can interrupt an hsmfs trigger.  Might be a good
 * idea to keep it synchronised with list in sys/fs/nfs/nfs_commonkrpc.c.
 */
int hsmfs_sig_set[] = {
	SIGINT,
	SIGTERM,
	SIGHUP,
	SIGKILL,
	SIGQUIT
};

struct hsmfs_softc	*hsmfs_softc;

SYSCTL_NODE(_vfs, OID_AUTO, hsmfs, CTLFLAG_RD, 0,
    "Hierarchical Storage Management filesystem");
int hsmfs_debug = 10;
TUNABLE_INT("vfs.hsmfs.debug", &hsmfs_debug);
SYSCTL_INT(_vfs_hsmfs, OID_AUTO, debug, CTLFLAG_RWTUN,
    &hsmfs_debug, 1, "Enable debug messages");
int hsmfs_stage_on_enoent = 1;
TUNABLE_INT("vfs.hsmfs.stage_on_enoent", &hsmfs_stage_on_enoent);
SYSCTL_INT(_vfs_hsmfs, OID_AUTO, stage_on_enoent, CTLFLAG_RWTUN,
    &hsmfs_stage_on_enoent, 1,
    "Restage the directory on attempt to access file that does not exist");
int hsmfs_timeout = 30;
TUNABLE_INT("vfs.hsmfs.timeout", &hsmfs_timeout);
SYSCTL_INT(_vfs_hsmfs, OID_AUTO, timeout, CTLFLAG_RWTUN,
    &hsmfs_timeout, 30, "Number of seconds to wait for hsmd(8)");
int hsmfs_retry_attempts = 3;
TUNABLE_INT("vfs.hsmfs.retry_attempts", &hsmfs_retry_attempts);
SYSCTL_INT(_vfs_hsmfs, OID_AUTO, retry_attempts, CTLFLAG_RWTUN,
    &hsmfs_retry_attempts, 3, "Number of attempts before failing request");
int hsmfs_retry_delay = 1;
TUNABLE_INT("vfs.hsmfs.retry_delay", &hsmfs_retry_delay);
SYSCTL_INT(_vfs_hsmfs, OID_AUTO, retry_delay, CTLFLAG_RWTUN,
    &hsmfs_retry_delay, 1, "Number of seconds before retrying");

int
hsmfs_init(struct vfsconf *vfsp)
{
	int error;

	KASSERT(hsmfs_softc == NULL,
	    ("softc %p, should be NULL", hsmfs_softc));

	hsmfs_softc = malloc(sizeof(*hsmfs_softc), M_HSMFS,
	    M_WAITOK | M_ZERO);

	hsmfs_request_zone = uma_zcreate("hsmfs_request",
	    sizeof(struct hsmfs_request), NULL, NULL, NULL, NULL,
	    UMA_ALIGN_PTR, 0);

	TAILQ_INIT(&hsmfs_softc->sc_requests);
	cv_init(&hsmfs_softc->sc_cv, "hsmfscv");
	sx_init(&hsmfs_softc->sc_lock, "hsmfslk");

	error = make_dev_p(MAKEDEV_CHECKNAME, &hsmfs_softc->sc_cdev,
	    &hsmfs_cdevsw, NULL, UID_ROOT, GID_WHEEL, 0600, "hsmfs");
	if (error != 0) {
		HSMFS_WARN("failed to create device node, error %d", error);
		uma_zdestroy(hsmfs_request_zone);
		free(hsmfs_softc, M_HSMFS);

		return (error);
	}
	hsmfs_softc->sc_cdev->si_drv1 = hsmfs_softc;

	return (0);
}

int
hsmfs_uninit(struct vfsconf *vfsp)
{

	sx_xlock(&hsmfs_softc->sc_lock);

	if (hsmfs_softc->sc_cdev != NULL)
		destroy_dev(hsmfs_softc->sc_cdev);

	uma_zdestroy(hsmfs_request_zone);

	sx_xunlock(&hsmfs_softc->sc_lock);
	/*
	 * XXX: Race with open?
	 */
	free(hsmfs_softc, M_HSMFS);

	return (0);
}

void
hsmfs_sync(void)
{

	// XXX: notify hsmd, so it can archive pending files?
}

bool
hsmfs_ignore_thread(void)
{
	struct proc *p;

	p = curproc;

	PROC_LOCK(p);
	if (p->p_session->s_sid == hsmfs_softc->sc_hsmd_sid) {
		PROC_UNLOCK(p);
		return (true);
	}
	PROC_UNLOCK(p);

	return (false);
}

#if 0

static void
hsmfs_task(void *context, int pending)
{
	struct hsmfs_request *hr;

	hr = context;

	sx_xlock(&hsmfs_softc->sc_lock);
	HSMFS_WARN("request %d for %s timed out after %d seconds",
	    hr->hr_id, hr->hr_path, hsmfs_timeout);
	/*
	 * XXX: EIO perhaps?
	 */
	hr->hr_error = ETIMEDOUT;
	hr->hr_wildcards = true;
	hr->hr_done = true;
	hr->hr_in_progress = false;
	cv_broadcast(&hsmfs_softc->sc_cv);
	sx_xunlock(&hsmfs_softc->sc_lock);
}

#endif

/*
 * The set/restore sigmask functions are used to (temporarily) overwrite
 * the thread td_sigmask during triggering.
 */
static void
hsmfs_set_sigmask(sigset_t *oldset)
{
	sigset_t newset;
	int i;

	SIGFILLSET(newset);
	/* Remove the hsmfs set of signals from newset */
	PROC_LOCK(curproc);
	mtx_lock(&curproc->p_sigacts->ps_mtx);
	for (i = 0 ; i < nitems(hsmfs_sig_set); i++) {
		/*
		 * But make sure we leave the ones already masked
		 * by the process, i.e. remove the signal from the
		 * temporary signalmask only if it wasn't already
		 * in p_sigmask.
		 */
		if (!SIGISMEMBER(curthread->td_sigmask, hsmfs_sig_set[i]) &&
		    !SIGISMEMBER(curproc->p_sigacts->ps_sigignore,
		    hsmfs_sig_set[i])) {
			SIGDELSET(newset, hsmfs_sig_set[i]);
		}
	}
	mtx_unlock(&curproc->p_sigacts->ps_mtx);
	kern_sigprocmask(curthread, SIG_SETMASK, &newset, oldset,
	    SIGPROCMASK_PROC_LOCKED);
	PROC_UNLOCK(curproc);
}

static void
hsmfs_restore_sigmask(sigset_t *set)
{

	kern_sigprocmask(curthread, SIG_SETMASK, set, NULL, 0);
}

static int
hsmfs_trigger_one(struct vnode *vp, int type)
{
	sigset_t oldset;
	struct hsmfs_request *hr;
	int error = 0, request_error, last;

	sx_assert(&hsmfs_softc->sc_lock, SA_XLOCKED);

	TAILQ_FOREACH(hr, &hsmfs_softc->sc_requests, hr_next) {
		if (hr->hr_type != type)
			continue;
		if (hr->hr_vp != vp)
			continue;

		break;
	}

	if (hr != NULL) {
		refcount_acquire(&hr->hr_refcount);
	} else {
		hr = uma_zalloc(hsmfs_request_zone, M_WAITOK | M_ZERO);

		hr->hr_id =
		    atomic_fetchadd_int(&hsmfs_softc->sc_last_request_id, 1);
		hr->hr_type = type;
		hr->hr_vp = vp;

#if 0
		TIMEOUT_TASK_INIT(taskqueue_thread, &hr->hr_task, 0,
		    hsmfs_task, hr);
		error = taskqueue_enqueue_timeout(taskqueue_thread,
		    &hr->hr_task, hsmfs_timeout * hz);
		if (error != 0) {
			HSMFS_WARN("taskqueue_enqueue_timeout() failed "
			    "with error %d", error);
		}
#endif
		refcount_init(&hr->hr_refcount, 1);
		TAILQ_INSERT_TAIL(&hsmfs_softc->sc_requests, hr, hr_next);
	}

	cv_broadcast(&hsmfs_softc->sc_cv);
	while (hr->hr_done == false) {
		hsmfs_set_sigmask(&oldset);
		error = cv_wait_sig(&hsmfs_softc->sc_cv,
		    &hsmfs_softc->sc_lock);
		hsmfs_restore_sigmask(&oldset);
		if (error != 0) {
			HSMFS_WARN("cv_wait_sig failed "
			    "with error %d", error);
			break;
		}
	}

	request_error = hr->hr_error;
	if (request_error != 0) {
		HSMFS_WARN("request completed with error %d",
		    request_error);
	}

	last = refcount_release(&hr->hr_refcount);
	if (last) {
		TAILQ_REMOVE(&hsmfs_softc->sc_requests, hr, hr_next);
#if 0
		/*
		 * Unlock the sc_lock, so that hsmfs_task() can complete.
		 */
		sx_xunlock(&hsmfs_softc->sc_lock);
		taskqueue_cancel_timeout(taskqueue_thread, &hr->hr_task, NULL);
		taskqueue_drain_timeout(taskqueue_thread, &hr->hr_task);
		sx_xlock(&hsmfs_softc->sc_lock);
#endif
		uma_zfree(hsmfs_request_zone, hr);
	}

	if (error != 0)
		return (error);
	return (request_error);
}

static int
hsmfs_trigger(struct vnode *vp, int type)
{
	struct hsmfs_node *hnp;
	int error;

	hnp = VTONULL(vp);

	for (;;) {
		error = hsmfs_trigger_one(vp, type);
		if (error == 0) {
			hnp->hn_retries = 0;
			return (0);
		}
		if (error == EINTR || error == ERESTART) {
			HSMFS_DEBUG("trigger interrupted by signal, "
			    "not retrying");
			hnp->hn_retries = 0;
			return (error);
		}
		hnp->hn_retries++;
		if (hnp->hn_retries >= hsmfs_retry_attempts) {
			HSMFS_DEBUG("trigger failed %d times; returning "
			    "error %d", hnp->hn_retries, error);
			hnp->hn_retries = 0;
			return (error);

		}
		HSMFS_DEBUG("trigger failed with error %d; will retry in "
		    "%d seconds, %d attempts left", error, hsmfs_retry_delay,
		    hsmfs_retry_attempts - hnp->hn_retries);
		sx_xunlock(&hsmfs_softc->sc_lock);
		pause("hsmfs_retry", hsmfs_retry_delay * hz);
		sx_xlock(&hsmfs_softc->sc_lock);
	}
}

/*
 * Figure out whether the vnode needs hsmd(8) attention - and if it does,
 * notify them and wait until it's done.
 */
int
hsmfs_trigger_vn(struct vnode *vp, int type)
{
	struct hsmfs_node *hnp;
	int error, locked;

	hnp = VTONULL(vp);

	locked = sx_xlocked(&hsmfs_softc->sc_lock);
	if (locked) {
		/*
		 * Looks like we've come back to square one, probably from
		 * hsmfs_ioctl_queue(), called by hsmq(8).  We don't want
		 * to sleep waiting for hsmd(8), so let's not trigger it.
		 */
		return (0);
	}

	/*
	 * Release the vnode lock, so that other operations can proceed.
	 * Increase use count, to prevent the vnode from being deallocated
	 * and to prevent the filesystem from being unmounted.
	 */
	vref(vp);
	VOP_UNLOCK(vp, 0);

	sx_xlock(&hsmfs_softc->sc_lock);
	error = hsmfs_trigger(vp, type);
	sx_xunlock(&hsmfs_softc->sc_lock);

	vn_lock(vp, LK_EXCLUSIVE | LK_RETRY);
	vunref(vp);
	if ((vp->v_iflag & VI_DOOMED) != 0) {
		HSMFS_DEBUG("VI_DOOMED");
		return (ENOENT);
	}

	if (error != 0)
		return (error);

	/*
	 * XXX: Move all the below to hsmfs_ioctl_done()?
	 */

	switch (type) {
	case HSMFS_TYPE_ARCHIVE:
		/*
		 * This obviously only applies when triggered via ioctl
		 * (eg by the userspace utilities); to get triggered
		 * by usual file access, the file would have to be already
		 * marked as managed.
		 */
		hnp->hn_hm.hm_managed = true;

		if (hnp->hn_hm.hm_modified) {
			microtime(&hnp->hn_hm.hm_archived_tv);
			hnp->hn_hm.hm_modified = false;
		}
		break;

	case HSMFS_TYPE_RECYCLE:
		hnp->hn_hm.hm_managed = true;
		break;

	case HSMFS_TYPE_RELEASE:
		hnp->hn_hm.hm_managed = true;

		if (hnp->hn_hm.hm_online) {
			microtime(&hnp->hn_hm.hm_released_tv);
			hnp->hn_hm.hm_online = false;
		}
		break;

	case HSMFS_TYPE_STAGE:
		hnp->hn_hm.hm_managed = true;

		if (!hnp->hn_hm.hm_online) {
			microtime(&hnp->hn_hm.hm_staged_tv);
			hnp->hn_hm.hm_online = true;
		}
		break;

	case HSMFS_TYPE_UNMANAGE:
		memset(hnp, 0, sizeof(*hnp));
		hnp->hn_hm.hm_metadata_valid = true;
		hnp->hn_hm.hm_managed = false;
		break;
	}

	error = hsmfs_metadata_write(vp);

	return (error);
}

int
hsmfs_trigger_archive(struct vnode *vp)
{

#ifdef notyet
	return (hsmfs_trigger_vn(vp, HSMFS_TYPE_ARCHIVE));
#else
	HSMFS_DEBUG("dummy; workaround for vn_fullpath failures");

	return (0);
#endif
}

int
hsmfs_trigger_recycle(struct vnode *vp)
{

	return (hsmfs_trigger_vn(vp, HSMFS_TYPE_RECYCLE));
}

int
hsmfs_trigger_stage(struct vnode *vp)
{
	int error;

	//vn_rangelock_ignore(vp, 1);
	error = hsmfs_trigger_vn(vp, HSMFS_TYPE_STAGE);
	//vn_rangelock_ignore(vp, 0);

	return (error);
}

static void
hsmfs_request_done(struct hsmfs_request *hr, int error)
{

	hr->hr_error = error;
	hr->hr_done = true;
	hr->hr_in_progress = false;
	cv_broadcast(&hsmfs_softc->sc_cv);
}

static int
hsmfs_ioctl_request(struct hsmfs_daemon_request *hdr)
{
	struct hsmfs_request *hr;
	char *retbuf, *freebuf;
	int error;

	sx_xlock(&hsmfs_softc->sc_lock);
	for (;;) {
		TAILQ_FOREACH(hr, &hsmfs_softc->sc_requests, hr_next) {
			if (hr->hr_done)
				continue;
			if (hr->hr_in_progress)
				continue;

			break;
		}

		if (hr != NULL)
			break;

		error = cv_wait_sig(&hsmfs_softc->sc_cv,
		    &hsmfs_softc->sc_lock);
		if (error != 0) {
			sx_xunlock(&hsmfs_softc->sc_lock);
			return (error);
		}
	}

	hr->hr_in_progress = true;
	sx_xunlock(&hsmfs_softc->sc_lock);

	KASSERT(hsmfs_softc->sc_hsmd_sid == curproc->p_session->s_sid,
	    ("sid %d != hsmd_sid %d",
	     curproc->p_session->s_sid, hsmfs_softc->sc_hsmd_sid));

	error = vn_fullpath(curthread, hr->hr_vp, &retbuf, &freebuf);
	if (error != 0) {
		HSMFS_WARN("vn_fullpath() failed with error %d", error);
		sx_xlock(&hsmfs_softc->sc_lock);
		hsmfs_request_done(hr, error);
		sx_xunlock(&hsmfs_softc->sc_lock);
		return (error);
	}

	hdr->hdr_id = hr->hr_id;
	hdr->hdr_type = hr->hr_type;
	strlcpy(hdr->hdr_path, retbuf, sizeof(hdr->hdr_path));
	free(freebuf, M_TEMP);

	return (0);
}

static int
hsmfs_ioctl_done(struct hsmfs_daemon_done *hdd)
{
	struct hsmfs_request *hr;

	sx_xlock(&hsmfs_softc->sc_lock);
	TAILQ_FOREACH(hr, &hsmfs_softc->sc_requests, hr_next) {
		if (hr->hr_id == hdd->hdd_id)
			break;
	}

	if (hr == NULL) {
		sx_xunlock(&hsmfs_softc->sc_lock);
		HSMFS_DEBUG("id %d not found", hdd->hdd_id);
		return (ESRCH);
	}

	hsmfs_request_done(hr, hdd->hdd_error);
	sx_xunlock(&hsmfs_softc->sc_lock);

	return (0);
}

static int
hsmfs_ioctl_queue(struct hsmfs_queue *hq)
{
	struct hsmfs_request *hr;
	char *retbuf, *freebuf;
	int error;

	/*
	 * Needs to be exclusive because of sx_xlocked() elsewhere.
	 */
	sx_xlock(&hsmfs_softc->sc_lock);

	TAILQ_FOREACH(hr, &hsmfs_softc->sc_requests, hr_next) {
		if (hr->hr_id < hq->hq_next_id)
			continue;
		break;
	}

	/*
	 * No more requests.
	 */
	if (hr == NULL) {
		sx_xunlock(&hsmfs_softc->sc_lock);
		hq->hq_next_id = 0;
		return (0);
	}

	hq->hq_id = hr->hr_id;
	hq->hq_next_id = hr->hr_id + 1;
	hq->hq_done = hr->hr_done;
	hq->hq_in_progress = hr->hr_in_progress;
	hq->hq_type = hr->hr_type;

	error = vn_fullpath(curthread, hr->hr_vp, &retbuf, &freebuf);
	if (error != 0) {
		sx_xunlock(&hsmfs_softc->sc_lock);
		HSMFS_WARN("vn_fullpath() failed with error %d", error);
		return (error);
	}
	strlcpy(hq->hq_path, retbuf, sizeof(hq->hq_path));
	free(freebuf, M_TEMP);

	sx_xunlock(&hsmfs_softc->sc_lock);

	return (0);
}

/*
 * Handler for ioctls issued on /dev/hsmfs.
 */
static int
hsmfs_ioctl(struct cdev *dev, u_long cmd, caddr_t arg, int mode,
    struct thread *td)
{

	switch (cmd) {
	case HSMFSREQUEST:
		if (!hsmfs_ignore_thread())
			return (EBUSY);

		return (hsmfs_ioctl_request((struct hsmfs_daemon_request *)arg));
	case HSMFSDONE:
		if (!hsmfs_ignore_thread())
			return (EBUSY);

		return (hsmfs_ioctl_done((struct hsmfs_daemon_done *)arg));
	case HSMFSQUEUE:
		return (hsmfs_ioctl_queue((struct hsmfs_queue *)arg));
	default:
		HSMFS_DEBUG("invalid cmd %lx", cmd);
		return (EINVAL);
	}
}

static int
hsmfs_open(struct cdev *dev, int flags, int fmt, struct thread *td)
{
	sx_xlock(&hsmfs_softc->sc_lock);

	/*
	 * We must never block hsmd(8) and its descendants, and we use
	 * session ID to determine that: we store session id of the process
	 * that opened the device, and then compare it with session ids
	 * of triggering processes.  This means running a second hsmd(8)
	 * instance would break the previous one.  The check below prevents
	 * it from happening.
	 */
	if (hsmfs_softc->sc_hsmd_sid == 0) {
		PROC_LOCK(curproc);
		hsmfs_softc->sc_hsmd_sid = curproc->p_session->s_sid;
		PROC_UNLOCK(curproc);
	}
	sx_xunlock(&hsmfs_softc->sc_lock);

	return (0);
}

static int
hsmfs_close(struct cdev *dev, int flag, int fmt, struct thread *td)
{
	sx_xlock(&hsmfs_softc->sc_lock);

	PROC_LOCK(curproc);
	if (hsmfs_softc->sc_hsmd_sid == curproc->p_session->s_sid)
		hsmfs_softc->sc_hsmd_sid = 0;
	PROC_UNLOCK(curproc);

	sx_xunlock(&hsmfs_softc->sc_lock);

	return (0);
}

int
hsmfs_metadata_read(struct vnode *vp)
{
	struct hsmfs_metadata *hmp;
	int error, len;

	hmp = VTOHM(vp);

	if (hmp->hm_metadata_valid)
		return (0);

	len = sizeof(*hmp);
	memset(hmp, 0, len);
	error = vn_extattr_get(vp, IO_NODELOCKED, HSMFS_EXTATTR_NAMESPACE,
	    HSMFS_EXTATTR_NAME, &len, (char *)hmp, curthread);
	if (error == ENOATTR) {
		//HSMFS_DEBUG("vn_extattr_get() failed with error %d", error);
		hmp->hm_metadata_valid = true;
	} else if (error != 0) {
		HSMFS_WARN("vn_extattr_get() failed with error %d", error);
		hmp->hm_metadata_valid = false; // XXX?
		return (error);
	} else if (len != sizeof(*hmp)) {
		HSMFS_DEBUG("invalid metadata extattr size, got %zd, should be %zd", len, sizeof(*hmp));
		hmp->hm_metadata_valid = false; // XXX?
		return (EIO);
	}

	return (0);
}

int
hsmfs_metadata_write(struct vnode *vp)
{
	struct hsmfs_metadata *hmp;
	int error;

	hmp = VTOHM(vp);

	KASSERT(hmp->hm_metadata_valid, ("metadata invalid"));

	error = vn_extattr_set(vp, IO_NODELOCKED, HSMFS_EXTATTR_NAMESPACE,
	    HSMFS_EXTATTR_NAME, sizeof(*hmp), (char *)hmp, curthread);
	if (error != 0) {
		HSMFS_DEBUG("vn_extattr_set() failed with error %d", error);
		return (error);
	}

	return (0);
}
