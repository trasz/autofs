%{
/*-
 * Copyright (c) 2016 Edward Tomasz Napierala <trasz@FreeBSD.org>
 * Copyright (c) 2012 The FreeBSD Foundation
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

#include <sys/queue.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <assert.h>
#include <libutil.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "hsmd.h"

extern FILE *yyin;
extern char *yytext;
extern int lineno;

static struct hsmd_conf *conf = NULL;
static struct hsmd_mount *mount = NULL;
static struct hsmd_remote *remote = NULL;

extern void	yyerror(const char *);
extern int	yylex(void);
extern void	yyrestart(FILE *);

%}

%token DEBUG MAXPROC MOUNT LOCAL PIDFILE REMOTE ARCHIVE_EXEC RELEASE_EXEC RECYCLE_EXEC STAGE_EXEC
%token OPENING_BRACKET CLOSING_BRACKET SEMICOLON

%union
{
	char *str;
}

%token <str> STR

%%

statements:
	|
	statements statement
	|
	statements statement SEMICOLON
	;

statement:
	debug
	|
	maxproc
	|
	pidfile
	|
	mount
	;

debug:		DEBUG STR
	{
		uint64_t tmp;

		if (expand_number($2, &tmp) != 0) {
			yyerror("invalid numeric value");
			free($2);
			return (1);
		}
			
		conf->hc_debug = tmp;
	}
	;

maxproc:	MAXPROC STR
	{
		uint64_t tmp;

		if (expand_number($2, &tmp) != 0) {
			yyerror("invalid numeric value");
			free($2);
			return (1);
		}

		conf->hc_maxproc = tmp;
	}
	;

pidfile:	PIDFILE STR
	{
		if (conf->hc_pidfile_path != NULL) {
			log_warnx("pidfile specified more than once");
			free($2);
			return (1);
		}
		conf->hc_pidfile_path = $2;
	}
	;

mount:	MOUNT mount_path
    OPENING_BRACKET mount_entries CLOSING_BRACKET
	{
		mount = NULL;
	}
	;

mount_path:	STR
	{
		mount = hsmd_mount_new(conf, $1);
		free($1);
		if (mount == NULL)
			return (1);
	}
	;

mount_entries:
	|
	mount_entries mount_entry
	|
	mount_entries mount_entry SEMICOLON
	;

mount_entry:
	mount_local
	|
	mount_remote
	;

mount_local:	LOCAL STR
	{
		if (mount->hm_local != NULL) {
			log_warnx("local for mount \"%s\" "
			    "specified more than once", mount->hm_path);
			return (1);
		}
		mount->hm_local = $2;
	}
	;

mount_remote:	REMOTE remote_name
    OPENING_BRACKET remote_entries CLOSING_BRACKET
	{
		remote = NULL;
	}
	;

remote_name:	STR
	{
		remote = hsmd_remote_new(mount, $1);
		free($1);
		if (remote == NULL)
			return (1);
	}
	;

remote_entries:
	|
	remote_entries remote_entry
	|
	remote_entries remote_entry SEMICOLON
	;

remote_entry:
	archive_exec
	|
	release_exec
	|
	recycle_exec
	|
	stage_exec
	;

archive_exec:	ARCHIVE_EXEC STR
	{
		if (remote->hr_archive_exec != NULL) {
			log_warnx("archive-exec for remote \"%s\" "
			    "specified more than once",
			    remote->hr_archive_exec);
			free($2);
			return (1);
		}
		remote->hr_archive_exec = $2;
	}
	;

release_exec:	RELEASE_EXEC STR
	{
		if (remote->hr_release_exec != NULL) {
			log_warnx("release-exec for remote \"%s\" "
			    "specified more than once",
			    remote->hr_release_exec);
			free($2);
			return (1);
		}
		remote->hr_release_exec = $2;
	}
	;

recycle_exec:	RECYCLE_EXEC STR
	{
		if (remote->hr_recycle_exec != NULL) {
			log_warnx("recycle-exec for remote \"%s\" "
			    "specified more than once",
			    remote->hr_recycle_exec);
			free($2);
			return (1);
		}
		remote->hr_recycle_exec = $2;
	}
	;

stage_exec:	STAGE_EXEC STR
	{
		if (remote->hr_stage_exec != NULL) {
			log_warnx("stage-exec for remote \"%s\" "
			    "specified more than once",
			    remote->hr_stage_exec);
			free($2);
			return (1);
		}
		remote->hr_stage_exec = $2;
	}
	;
%%

void
yyerror(const char *str)
{

	log_warnx("error in configuration file at line %d near '%s': %s",
	    lineno, yytext, str);
}

int
parse_conf(struct hsmd_conf *newconf, const char *path)
{
	int error;

	conf = newconf;
	yyin = fopen(path, "r");
	if (yyin == NULL) {
		log_warn("unable to open configuration file %s", path);
		return (1);
	}

	lineno = 1;
	yyrestart(yyin);
	error = yyparse();
	mount = NULL;
	remote = NULL;
	fclose(yyin);

	return (error);
}
