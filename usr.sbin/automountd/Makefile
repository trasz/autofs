# $FreeBSD$

PROG=		automountd
SRCS=		automountd.c
CFLAGS+=	-I${.CURDIR}
CFLAGS+=	-I${.CURDIR}/../../sys/fs/autofs
MAN=		

DPADD=		${LIBUTIL}
LDADD=		-lutil

WARNS=		6

.include <bsd.prog.mk>
