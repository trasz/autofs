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

#include <sys/cdefs.h>
__FBSDID("$FreeBSD$");

#include <sys/types.h>
#include <sys/time.h>
#include <sys/ioctl.h>
#include <sys/param.h>
#include <sys/linker.h>
#include <sys/mount.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <sys/utsname.h>
#include <assert.h>
#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <fts.h>
#include <libgen.h>
#include <libutil.h>
#include <netdb.h>
#include <paths.h>
#include <signal.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "hsmd.h"
#include "hsmfs_ioctl.h"
#include "mntopts.h"

#define HSMD_PIDFILE	"/var/run/hsmd.pid"

static volatile bool sighup_received = false;
static volatile bool sigterm_received = false;

static int nchildren = 0;
static int hsmfs_fd;
static int request_id;

static void
usage(void)
{

	fprintf(stderr, "usage: hsmd [-dv][-f config-file]\n");
	exit(1);
}

char *
checked_strdup(const char *s)
{
	char *c;

	assert(s != NULL);

	c = strdup(s);
	if (c == NULL)
		log_err(1, "strdup");
	return (c);
}

struct hsmd_conf *
hsmd_conf_new(void)
{
	struct hsmd_conf *conf;

	conf = calloc(1, sizeof(*conf));
	if (conf == NULL)
		log_err(1, "calloc");

	TAILQ_INIT(&conf->hc_mounts);

	conf->hc_debug = 0;
	conf->hc_maxproc = 30;

	return (conf);
}

void
hsmd_conf_delete(struct hsmd_conf *conf)
{
	struct hsmd_mount *mount, *mount_tmp;

	assert(conf->hc_pidfh == NULL);

	TAILQ_FOREACH_SAFE(mount, &conf->hc_mounts, hm_next, mount_tmp)
		hsmd_mount_delete(mount);

	free(conf->hc_pidfile_path);
	free(conf);
}

struct hsmd_mount *
hsmd_mount_new(struct hsmd_conf *conf, const char *path)
{
	struct hsmd_mount *mount;

	mount = hsmd_mount_find(conf, path);
	if (mount != NULL) {
		log_warnx("duplicated mount \"%s\"", path);
		return (NULL);
	}

	mount = calloc(1, sizeof(*mount));
	if (mount == NULL)
		log_err(1, "calloc");

	mount->hm_path = checked_strdup(path);
	TAILQ_INIT(&mount->hm_remotes);
	TAILQ_INSERT_TAIL(&conf->hc_mounts, mount, hm_next);

	return (mount);
}

void
hsmd_mount_delete(struct hsmd_mount *mount)
{
	struct hsmd_remote *remote, *remote_tmp;

	TAILQ_FOREACH_SAFE(remote, &mount->hm_remotes, hr_next, remote_tmp)
		hsmd_remote_delete(remote);

	free(mount->hm_path);
	free(mount);
}

struct hsmd_mount *
hsmd_mount_find(struct hsmd_conf *conf, const char *path)
{
	struct hsmd_mount *mount;

	TAILQ_FOREACH(mount, &conf->hc_mounts, hm_next) {
		if (strcmp(mount->hm_path, path) == 0)
			return (mount);
	}

	return (NULL);
}

static struct hsmd_mount *
hsmd_mount_lookup(const struct hsmd_conf *conf, const char *path)
{
	struct hsmd_mount *mount;
	size_t len;

	TAILQ_FOREACH(mount, &conf->hc_mounts, hm_next) {
		len = strlen(mount->hm_path);
		if (strlen(path) < len)
			continue;
		if (strncmp(mount->hm_path, path, len) != 0)
			continue;
		if (path[len] != '\0' && path[len] != '/')
			continue;
		break;
	}

	return (mount);
}

static const char *
hsmd_mount_strip(const struct hsmd_mount *mount, const char *path)
{
	size_t len;
	const char *stripped_path;

	len = strlen(mount->hm_path);
	assert(len <= strlen(path));

	stripped_path = path + len;

	if (stripped_path[0] == '/')
		stripped_path++;

	if (stripped_path[0] == '\0')
		stripped_path = "/";

	log_debugx("got %s, returning %s", path, stripped_path);

	return (stripped_path);
}

#if 0
static void
create_directory(const char *path)
{
	char *component, *copy, *tofree, *partial, *tmp;
	int error;

	assert(path[0] == '/');

	/*
	 * +1 to skip the leading slash.
	 */
	copy = tofree = checked_strdup(path + 1);

	partial = checked_strdup("");
	for (;;) {
		component = strsep(&copy, "/");
		if (component == NULL)
			break;
		tmp = concat(partial, '/', component);
		free(partial);
		partial = tmp;
		//log_debugx("creating \"%s\"", partial);
		error = mkdir(partial, 0755);
		if (error != 0 && errno != EEXIST) {
			log_warn("cannot create %s", partial);
			return;
		}
	}

	free(tofree);
}
#endif

static int
hsmd_mount_mount(struct hsmd_mount *mount)
{
	struct iovec *iov = NULL;
	char errmsg[255];
	int error, iovlen = 0;

#if 0
	create_directory(fspath);
#endif

	log_debugx("mounting %s on %s", mount->hm_local, mount->hm_path);
	memset(errmsg, 0, sizeof(errmsg));

	build_iovec(&iov, &iovlen, "fstype",
	    __DECONST(void *, "hsmfs"), (size_t)-1);
	build_iovec(&iov, &iovlen, "fspath",
	    __DECONST(void *, mount->hm_path), (size_t)-1);
	build_iovec(&iov, &iovlen, "from",
	    __DECONST(void *, mount->hm_local), (size_t)-1);
	build_iovec(&iov, &iovlen, "errmsg",
	    errmsg, sizeof(errmsg));

	error = nmount(iov, iovlen, 0);
	if (error != 0) {
		if (*errmsg != '\0') {
			log_warn("cannot mount %s on %s: %s",
			    mount->hm_local, mount->hm_path, errmsg);
		} else {
			log_warn("cannot mount %s on %s",
			    mount->hm_local, mount->hm_path);
		}
	}

	return (error);
}

static int
hsmd_mount_unmount(struct hsmd_mount *mount)
{
	int error;

	log_debugx("unmounting %s", mount->hm_path);

	error = unmount(mount->hm_path, 0);
	if (error != 0)
		log_warn("cannot unmount %s", mount->hm_path);

	return (error);
}

struct hsmd_remote *
hsmd_remote_new(struct hsmd_mount *mount, const char *name)
{
	struct hsmd_remote *remote;

	remote = calloc(1, sizeof(*remote));
	if (remote == NULL)
		log_err(1, "calloc");

	remote->hr_name = checked_strdup(name);
	TAILQ_INSERT_TAIL(&mount->hm_remotes, remote, hr_next);

	return (remote);
}

void
hsmd_remote_delete(struct hsmd_remote *remote)
{

	free(remote->hr_name);
	free(remote);
}

struct hsmd_remote *
hsmd_remote_find(struct hsmd_mount *mount, const char *name)
{
	struct hsmd_remote *remote;

	TAILQ_FOREACH(remote, &mount->hm_remotes, hr_next) {
		if (strcmp(remote->hr_name, name) == 0)
			return (remote);
	}

	return (NULL);
}

static void
check_perms(const char *path)
{
	struct stat sb;
	int error;

	error = stat(path, &sb);
	if (error != 0) {
		log_warn("stat");
		return;
	}
	if (sb.st_mode & S_IWOTH) {
		log_warnx("%s is world-writable", path);
	} else if (sb.st_mode & S_IROTH) {
		log_warnx("%s is world-readable", path);
	} else if (sb.st_mode & S_IXOTH) {
		/*
		 * Ok, this one doesn't matter, but still do it,
		 * just for consistency.
		 */
		log_warnx("%s is world-executable", path);
	}

	/*
	 * XXX: Should we also check for owner != 0?
	 */
}

static struct hsmd_conf *
hsmd_conf_new_from_kernel(void)
{
	struct hsmd_conf *conf;
	struct hsmd_mount *mount;
	struct statfs *mntbuf;
	int i, nitems;

	log_debugx("obtaining the list of mounted filesystems");

	conf = hsmd_conf_new();

	nitems = getmntinfo(&mntbuf, MNT_WAIT);
	if (nitems <= 0)
		log_err(1, "getmntinfo");

	for (i = 0; i < nitems; i++) {
		if (strcmp(mntbuf[i].f_fstypename, "hsmfs") != 0) {
#if 0
			log_debugx("skipping %s, filesystem type is not hsmfs",
			    mntbuf[i].f_mntonname);
#endif
			continue;
		}

		log_debugx("found hsmfs mounted on %s", mntbuf[i].f_mntonname);

		mount = hsmd_mount_new(conf, mntbuf[i].f_mntonname);
		if (mount == NULL) {
			hsmd_conf_delete(conf);
			return (NULL);
		}

		mount->hm_local = checked_strdup(mntbuf[i].f_mntfromname);
	}

	return (conf);
}

static struct hsmd_conf *
hsmd_conf_new_from_file(const char *path)
{
	struct hsmd_conf *conf;
	int error;

	log_debugx("obtaining configuration from %s", path);

	conf = hsmd_conf_new();

	error = parse_conf(conf, path);
	if (error != 0) {
		hsmd_conf_delete(conf);
		return (NULL);
	}

	check_perms(path);

#if 0
	error = hsmd_conf_verify(conf);
	if (error != 0) {
		hsmd_conf_delete(conf);
		return (NULL);
	}
#endif

	return (conf);
}

static int
hsmd_conf_apply(struct hsmd_conf *oldconf, struct hsmd_conf *newconf)
{
	struct hsmd_mount *oldmount, *newmount;
	pid_t otherpid;
	int cumulated_error = 0, error;

	if (oldconf->hc_debug != newconf->hc_debug) {
		log_debugx("changing debug level to %d", newconf->hc_debug);
		log_init(newconf->hc_debug);
	}

	if (oldconf->hc_pidfh != NULL) {
		assert(oldconf->hc_pidfile_path != NULL);
		if (newconf->hc_pidfile_path != NULL &&
		    strcmp(oldconf->hc_pidfile_path,
		    newconf->hc_pidfile_path) == 0) {
			newconf->hc_pidfh = oldconf->hc_pidfh;
			oldconf->hc_pidfh = NULL;
		} else {
			log_debugx("removing pidfile %s",
			    oldconf->hc_pidfile_path);
			pidfile_remove(oldconf->hc_pidfh);
			oldconf->hc_pidfh = NULL;
		}
	}

	if (newconf->hc_pidfh == NULL && newconf->hc_pidfile_path != NULL) {
		log_debugx("opening pidfile %s", newconf->hc_pidfile_path);
		newconf->hc_pidfh =
		    pidfile_open(newconf->hc_pidfile_path, 0600, &otherpid);
		if (newconf->hc_pidfh == NULL) {
			if (errno == EEXIST)
				log_errx(1, "daemon already running, pid: %jd.",
				    (intmax_t)otherpid);
			log_err(1, "cannot open or create pidfile \"%s\"",
			    newconf->hc_pidfile_path);
		}
	}

	TAILQ_FOREACH(oldmount, &oldconf->hc_mounts, hm_next) {
		newmount = hsmd_mount_find(newconf, oldmount->hm_path);
		if (newmount == NULL) {
			log_debugx("mount \"%s\" not found in new configuration; removing", oldmount->hm_path);
			error = hsmd_mount_unmount(oldmount);
			if (error != 0)
				cumulated_error++;
		}

		/*
		 * XXX: Also check hm_local.
		 */
	}

	TAILQ_FOREACH(newmount, &newconf->hc_mounts, hm_next) {
		oldmount = hsmd_mount_find(oldconf, newmount->hm_path);
		if (oldmount != NULL) {
			log_debugx("\"%s\" already mounted on \"%s\"", oldmount->hm_local, oldmount->hm_path);
			continue;
		}

		error = hsmd_mount_mount(newmount);
		if (error != 0)
			cumulated_error++;
	}

	return (cumulated_error);
}

static void
done(int request_error)
{
	struct hsmfs_daemon_done hdd;
	int error;

	memset(&hdd, 0, sizeof(hdd));
	hdd.hdd_id = request_id;
	hdd.hdd_error = request_error;

	log_debugx("completing request %d with error %d",
	    request_id, request_error);

	error = ioctl(hsmfs_fd, HSMFSDONE, &hdd);
	if (error != 0)
		log_warn("HSMFSDONE");
}

static void
exit_callback(void)
{

	done(EIO);
}

static void
hsmfs_mark_managed(char *path)
{
	struct hsm_managed hm;
	char *fts_argv[2];
	FTS *fts;
	FTSENT *entry;
	int error, fd;

	fts_argv[0] = path;
	fts_argv[1] = NULL;

	fts = fts_open(fts_argv, FTS_NOSTAT | FTS_PHYSICAL, NULL);
	if (fts == NULL)
		log_err(1, "fts_open");

	for (;;) {
		entry = fts_read(fts);
		if (entry == NULL) {
			if (errno != 0)
				log_err(1, "fts_read");
			break;
		}

		memset(&hm, 0, sizeof(hm));

		switch (entry->fts_info) {
		case FTS_F:
		case FTS_NSOK:
			/*
			 * Mark files online, directories offline.
			 */
			hm.hm_online = 1;
			break;
		case FTS_DP:
			hm.hm_online = 0;
			break;
		case FTS_D:
			continue;
		case FTS_DNR:
		case FTS_ERR:
		case FTS_NS:
			log_errx(1, "%s: %s", entry->fts_path, strerror(entry->fts_errno));
		}

		log_debugx("marking %s as managed, %s",
		    entry->fts_path, hm.hm_online ? "online" : "offline");

		fd = open(entry->fts_accpath, O_RDONLY);
		if (fd < 0)
			log_err(1, "%s", entry->fts_path);

		error = ioctl(fd, HSMMANAGED, &hm);
		if (error != 0)
			log_err(1, "%s: HSMMANAGED", entry->fts_path);

		error = close(fd);
		if (error != 0)
			log_err(1, "%s: close", entry->fts_path);
	}

	error = fts_close(fts);
	if (error != 0)
		log_err(1, "fts_close");
}

static const char *
type2str(int type)
{

	switch (type) {
	case HSMFS_TYPE_ARCHIVE:
		return ("ARCHIVE");
	case HSMFS_TYPE_RECYCLE:
		return ("RECYCLE");
	case HSMFS_TYPE_RELEASE:
		return ("RELEASE");
	case HSMFS_TYPE_STAGE:
		return ("STAGE");
	case HSMFS_TYPE_UNMANAGE:
		return ("UNMANAGE");
	default:
		return ("UNKNOWN");
	}
}

static void
handle_request(const struct hsmd_conf *conf, /* const */ struct hsmfs_daemon_request *hdr)
{
	FILE *f;
	struct hsmd_mount *mount;
	struct hsmd_remote *remote;
	const char *exec, *stripped_path;
	int error;

	log_debugx("got request %d: type %d (%s), path %s",
	    hdr->hdr_id, hdr->hdr_type, type2str(hdr->hdr_type), hdr->hdr_path);

	/*
	 * Try to notify the kernel about any problems.
	 */
	request_id = hdr->hdr_id;
	atexit(exit_callback);

	mount = hsmd_mount_lookup(conf, hdr->hdr_path);
	if (mount == NULL)
		log_errx(1, "got request for unknown mount %s", hdr->hdr_path);

	stripped_path = hsmd_mount_strip(mount, hdr->hdr_path);

	/*
	 * XXX: Will exit at the first failing remote.
	 * XXX: Will not exit after the first remote stage succeeding, which probably doesn't make any sense.
	 */
	TAILQ_FOREACH(remote, &mount->hm_remotes, hr_next) {
		switch (hdr->hdr_type) {
		case HSMFS_TYPE_ARCHIVE:
			exec = remote->hr_archive_exec;
			break;
		case HSMFS_TYPE_RECYCLE:
			exec = remote->hr_recycle_exec;
			break;
		case HSMFS_TYPE_RELEASE:
			exec = remote->hr_release_exec;
			break;
		case HSMFS_TYPE_STAGE:
			exec = remote->hr_stage_exec;
			break;
		case HSMFS_TYPE_UNMANAGE:
			/* XXX */
			exec = NULL;
			break;
		default:
			log_errx(1, "received request with invalid hm_type %d", hdr->hdr_type);
		}

		if (exec == NULL) {
			log_debugx("remote \"%s\" does not define any exec for request type %s",
			    remote->hr_name, type2str(hdr->hdr_type));
			continue;
		}

		f = my_own_personal_popen(exec, mount->hm_path, stripped_path, NULL);
		assert(f != NULL);
		error = my_own_personal_pclose(f);
		if (error != 0)
			log_errx(1, "request failed");
	}

	/*
	 * Newly added local files must have their extattr set.
	 */
	if (hdr->hdr_type == HSMFS_TYPE_STAGE)
		hsmfs_mark_managed(hdr->hdr_path);

	log_debugx("request done; exiting");
	done(0);

	/*
	 * Exit without calling exit_callback().
	 */
	quick_exit(0);
}

static int
wait_for_children(bool block)
{
	pid_t pid;
	int status;
	int num = 0;

	for (;;) {
		/*
		 * If "block" is true, wait for at least one process.
		 */
		if (block && num == 0)
			pid = wait4(-1, &status, 0, NULL);
		else
			pid = wait4(-1, &status, WNOHANG, NULL);
		if (pid <= 0)
			break;
		if (WIFSIGNALED(status)) {
			log_warnx("child process %d terminated with signal %d",
			    pid, WTERMSIG(status));
		} else if (WEXITSTATUS(status) != 0) {
			log_debugx("child process %d terminated with exit status %d",
			    pid, WEXITSTATUS(status));
		} else {
			log_debugx("child process %d terminated gracefully", pid);
		}
		num++;
	}

	return (num);
}

/*
 * Two things daemon(3) does, that we actually also want to do
 * when running in foreground, is closing the stdin and chdiring
 * to "/".  This is what we do here.
 */
static void
lesser_daemon(void)
{
	int error, fd;

	error = chdir("/");
	if (error != 0)
		log_warn("chdir");

	fd = open(_PATH_DEVNULL, O_RDWR, 0);
	if (fd < 0) {
		log_warn("cannot open %s", _PATH_DEVNULL);
		return;
	}

	error = dup2(fd, STDIN_FILENO);
	if (error != 0)
		log_warn("dup2");

	error = close(fd);
	if (error != 0) {
		/* Bloody hell. */
		log_warn("close");
	}
}

static void
sighup_handler(int dummy __unused)
{

	sighup_received = true;
}

static void
sigterm_handler(int dummy __unused)
{

	sigterm_received = true;
}

static void
sigchld_handler(int dummy __unused)
{

	/*
	 * The only purpose of this handler is to make SIGCHLD
	 * interrupt the ISCSIDWAIT ioctl(2), so we can call
	 * wait_for_children().
	 */
}

static void
register_signals(void)
{
	struct sigaction sa;
	int error;

	bzero(&sa, sizeof(sa));
	sa.sa_handler = sighup_handler;
	sigfillset(&sa.sa_mask);
	error = sigaction(SIGHUP, &sa, NULL);
	if (error != 0)
		log_err(1, "sigaction");

	sa.sa_handler = sigterm_handler;
	error = sigaction(SIGTERM, &sa, NULL);
	if (error != 0)
		log_err(1, "sigaction");

	sa.sa_handler = sigterm_handler;
	error = sigaction(SIGINT, &sa, NULL);
	if (error != 0)
		log_err(1, "sigaction");

	sa.sa_handler = sigchld_handler;
	error = sigaction(SIGCHLD, &sa, NULL);
	if (error != 0)
		log_err(1, "sigaction");
}

static void
main_loop(struct hsmd_conf *conf, bool dont_fork)
{
	struct hsmfs_daemon_request request;
	pid_t pid;
	int error;

	pidfile_write(conf->hc_pidfh);

	for (;;) {
		if (sighup_received || sigterm_received)
			return;

		log_debugx("waiting for request from the kernel");

		memset(&request, 0, sizeof(request));
		error = ioctl(hsmfs_fd, HSMFSREQUEST, &request);
		if (error != 0) {
			if (errno == EINTR) {
				nchildren -= wait_for_children(false);
				assert(nchildren >= 0);
				continue;
			}

			log_err(1, "HSMFSREQUEST");
		}

		if (dont_fork) {
			log_debugx("not forking due to -d flag; "
			    "will exit after servicing a single request");
		} else {
			nchildren -= wait_for_children(false);
			assert(nchildren >= 0);

			while (conf->hc_maxproc > 0 && nchildren >= conf->hc_maxproc) {
				log_debugx("maxproc limit of %d child processes hit; "
				    "waiting for child process to exit", conf->hc_maxproc);
				nchildren -= wait_for_children(true);
				assert(nchildren >= 0);
			}
			log_debugx("got request; forking child process #%d",
			    nchildren);
			nchildren++;

			pid = fork();
			if (pid < 0)
				log_err(1, "fork");
			if (pid > 0)
				continue;
		}

		pidfile_close(conf->hc_pidfh);
		handle_request(conf, &request);
	}
}

int
main(int argc, char **argv)
{
	struct hsmd_conf *oldconf, *newconf, *tmpconf;
	const char *config_path = DEFAULT_CONFIG_PATH;
	int ch, debug = 0, error, retval, saved_errno;
	bool dont_daemonize = false;

	while ((ch = getopt(argc, argv, "dv")) != -1) {
		switch (ch) {
		case 'd':
			dont_daemonize = true;
			debug++;
			break;
		case 'f':
			config_path = optarg;
			break;
		case 'v':
			debug++;
			break;
		case '?':
		default:
			usage();
		}
	}
	argc -= optind;
	if (argc != 0)
		usage();

	log_init(debug);

	oldconf = hsmd_conf_new_from_kernel();
	newconf = hsmd_conf_new_from_file(config_path);

	if (newconf == NULL)
		log_errx(1, "configuration error; exiting");
	if (debug > 0) {
		oldconf->hc_debug = debug;
		newconf->hc_debug = debug;
	}

	error = hsmd_conf_apply(oldconf, newconf);
	if (error != 0)
		log_errx(1, "failed to apply configuration; exiting");

	hsmd_conf_delete(oldconf);
	oldconf = NULL;

	register_signals();

	if (dont_daemonize == false) {
		if (daemon(0, 0) == -1) {
			log_warn("cannot daemonize");
			pidfile_remove(newconf->hc_pidfh);
			exit(1);
		}
	} else {
		lesser_daemon();
	}

	hsmfs_fd = open(HSMFS_PATH, O_RDWR | O_CLOEXEC);
	if (hsmfs_fd < 0 && errno == ENOENT) {
		saved_errno = errno;
		retval = kldload("hsmfs");
		if (retval != -1)
			hsmfs_fd = open(HSMFS_PATH, O_RDWR | O_CLOEXEC);
		else
			errno = saved_errno;
	}
	if (hsmfs_fd < 0)
		log_err(1, "failed to open %s", HSMFS_PATH);

	for (;;) {
		main_loop(newconf, dont_daemonize);
		if (sighup_received) {
			sighup_received = false;
			log_debugx("received SIGHUP, reloading configuration");
			tmpconf = hsmd_conf_new_from_file(config_path);

			if (tmpconf == NULL) {
				log_warnx("configuration error, "
				    "continuing with old configuration");
			} else {
				if (debug > 0)
					tmpconf->hc_debug = debug;
				oldconf = newconf;
				newconf = tmpconf;
				error = hsmd_conf_apply(oldconf, newconf);
				if (error != 0)
					log_warnx("failed to reload "
					    "configuration");
				hsmd_conf_delete(oldconf);
				oldconf = NULL;
			}
		} else if (sigterm_received) {
			log_debugx("exiting on signal; "
			    "reloading empty configuration");

			oldconf = newconf;
			newconf = hsmd_conf_new();
			if (debug > 0)
				newconf->hc_debug = debug;
			error = hsmd_conf_apply(oldconf, newconf);
			if (error != 0)
				log_warnx("failed to apply configuration");
			hsmd_conf_delete(oldconf);
			oldconf = NULL;

			log_warnx("exiting on signal");
			exit(0);
		} else {
			nchildren -= wait_for_children(false);
			assert(nchildren >= 0);
		}
	}
	/* NOTREACHED */
}
