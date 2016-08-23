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
 * $FreeBSD$
 */

#ifndef HSMD_H
#define	HSMD_H

#include <sys/queue.h>

#define	DEFAULT_CONFIG_PATH	"/etc/hsm.conf"
#define	DEFAULT_PIDFILE		"/var/run/hsmd.pid"

struct hsmd_remote {
	TAILQ_ENTRY(hsmd_remote)	hr_next;
	char		*hr_name;
	char		*hr_archive_exec;
	//const char	*hr_exec_read;
	char		*hr_recycle_exec;
	char		*hr_release_exec;
	char		*hr_stage_exec;
};

struct hsmd_mount {
	TAILQ_ENTRY(hsmd_mount)	hm_next;
	char			*hm_path;
	char			*hm_local;
	TAILQ_HEAD(, hsmd_remote)hm_remotes;
};

struct hsmd_conf {
	char			*hc_pidfile_path;
	TAILQ_HEAD(, hsmd_mount)hc_mounts;
	struct pidfh		*hc_pidfh;
	int			hc_debug;
	int			hc_maxproc;
};

void	log_init(int level);
void	log_set_peer_name(const char *name);
void	log_set_peer_addr(const char *addr);
void	log_err(int, const char *, ...)
	    __dead2 __printf0like(2, 3);
void	log_errx(int, const char *, ...)
	    __dead2 __printf0like(2, 3);
void	log_warn(const char *, ...) __printf0like(1, 2);
void	log_warnx(const char *, ...) __printflike(1, 2);
void	log_debugx(const char *, ...) __printf0like(1, 2);

char	*checked_strdup(const char *);

int	my_own_personal_pclose(FILE *iop);
FILE	*my_own_personal_popen(const char *argv0, ...);

int			parse_conf(struct hsmd_conf *conf, const char *path);

struct hsmd_conf	*hsmd_conf_new(void);
void			hsmd_conf_delete(struct hsmd_conf *conf);
struct hsmd_mount	*hsmd_mount_new(struct hsmd_conf *conf, const char *path);
void			hsmd_mount_delete(struct hsmd_mount *mount);
struct hsmd_mount	*hsmd_mount_find(struct hsmd_conf *conf, const char *path);
struct hsmd_remote	*hsmd_remote_new(struct hsmd_mount *mount, const char *name);
void			hsmd_remote_delete(struct hsmd_remote *remote);
struct hsmd_remote	*hsmd_remote_find(struct hsmd_mount *mount, const char *path);

#endif /* !HSMD_H */
