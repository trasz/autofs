# $FreeBSD$

PACKAGE=hsm
PROG=	hsmd
SRCS=	hsmd.c
SRCS+=	getmntopts.c
SRCS+=	log.c
SRCS+=	parse.y
SRCS+=	popen.c
SRCS+=	token.l
SRCS+=	y.tab.h

CFLAGS+=-I${.CURDIR}
CFLAGS+=-I${.CURDIR}/../../sys/fs/hsmfs

MAN=	

LIBADD=	l util

YFLAGS+=	-v
CLEANFILES=	y.tab.c y.tab.h y.output

# Needed for getmntopts.c
MOUNT=  ${.CURDIR}/../../sbin/mount
CFLAGS+=-I${MOUNT}

WARNS=	6
NO_WMISSING_VARIABLE_DECLARATIONS=

.PATH:	${MOUNT}

.include <bsd.prog.mk>
