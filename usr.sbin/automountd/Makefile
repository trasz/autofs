# $FreeBSD$

PROG=		automountd
SRCS=		automountd.c getmntopts.c log.c token.l
CFLAGS+=	-I${.CURDIR}
CFLAGS+=	-I${.CURDIR}/../../sys/fs/autofs
MAN=		

DPADD=		${LIBUTIL}
LDADD=		-lutil

# Needed for getmntopts.c
MOUNT=  	${.CURDIR}/../../sbin/mount
CFLAGS+=	-I${MOUNT}

CFLAGS+=	-O0 -ggdb

WARNS=		6

.PATH:		${MOUNT}

.include <bsd.prog.mk>
