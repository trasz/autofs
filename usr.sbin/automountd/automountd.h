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

#ifndef AUTOMOUNTD_H
#define	AUTOMOUNTD_H

#include <sys/queue.h>

#define	AUTO_MASTER_PATH	"/etc/auto_master"
#define	AUTO_MAP_PREFIX		"/etc"
#define	AUTO_SPECIAL_PREFIX	"/etc/automountd"
#define	AUTO_INCLUDE_PATH	AUTO_SPECIAL_PREFIX "/include"

struct node {
	TAILQ_ENTRY(node)	n_next;
	TAILQ_HEAD(, node)	n_children;
	struct node		*n_parent;
	char			*n_key;
	char			*n_options;
	char			*n_location;
	const char		*n_config_file;
	int			n_config_line;
};

struct defined_value {
	TAILQ_ENTRY(defined_value)	d_next;
	char				*d_name;
	char				*d_value;
};

void			log_init(int level);
void			log_set_peer_name(const char *name);
void			log_set_peer_addr(const char *addr);
void			log_err(int, const char *, ...)
			    __dead2 __printf0like(2, 3);
void			log_errx(int, const char *, ...)
			    __dead2 __printf0like(2, 3);
void			log_warn(const char *, ...) __printf0like(1, 2);
void			log_warnx(const char *, ...) __printflike(1, 2);
void			log_debugx(const char *, ...) __printf0like(1, 2);

char			*checked_strdup(const char *);

/*
 * lex(1) stuff.
 */
extern int lineno;

#define	STR	1
#define	NEWLINE	2

#endif /* !AUTOMOUNTD_H */
