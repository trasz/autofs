# $FreeBSD$

PACKAGE=hsm
PROG=	hsmarchive
SRCS=	hsmarchive.c

CFLAGS+=-I${.CURDIR}
CFLAGS+=-I${.CURDIR}/../../sys/fs/hsmfs

MAN=	

WARNS=	6

LINKS=	${BINDIR}/hsmarchive ${BINDIR}/hsmrelease
LINKS+=	${BINDIR}/hsmarchive ${BINDIR}/hsmrecycle
LINKS+=	${BINDIR}/hsmarchive ${BINDIR}/hsmstage
LINKS+=	${BINDIR}/hsmarchive ${BINDIR}/hsmunmanage

.PATH:	${MOUNT}

.include <bsd.prog.mk>
