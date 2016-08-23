%{
/*-
 * Copyright (c) 2016 Edward Tomasz Napierala <trasz@FreeBSD.org>
 * All rights reserved.
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

#include <stdio.h>
#include <stdint.h>
#include <string.h>

#include "y.tab.h"

int lineno;

#define	YY_DECL int yylex(void)
extern int	yylex(void);

%}

%option noinput
%option nounput

%%
debug			{ return DEBUG; }
maxproc			{ return MAXPROC; }
pidfile			{ return PIDFILE; }
mount			{ return MOUNT; }
local			{ return LOCAL; }
remote			{ return REMOTE; }
archive-exec		{ return ARCHIVE_EXEC; }
release-exec		{ return RELEASE_EXEC; }
recycle-exec		{ return RECYCLE_EXEC; }
stage-exec		{ return STAGE_EXEC; }
\"[^"]+\"		{ yylval.str = strndup(yytext + 1,
			    strlen(yytext) - 2); return STR; }
[a-zA-Z0-9\.\-@_/\:\[\]]+ { yylval.str = strdup(yytext); return STR; }
\{			{ return OPENING_BRACKET; }
\}			{ return CLOSING_BRACKET; }
#.*$			/* ignore comments */;
\r\n			{ lineno++; }
\n			{ lineno++; }
;			{ return SEMICOLON; }
[ \t]+			/* ignore whitespace */;
.			{ yylval.str = strdup(yytext); return STR; }
%%
