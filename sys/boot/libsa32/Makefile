# $FreeBSD$

.include <bsd.init.mk>

LIB=sa32
.if ${MACHINE_CPUARCH} == "amd64"
LIBSA_CPUARCH=i386
.else
LIBSA_CPUARCH=${MACHINE_CPUARCH}
.endif

.if ${MACHINE_ARCH} == "amd64" || ${MACHINE_ARCH} == "powerpc64"
CFLAGS+=	-m32 -I.
.endif

.PATH:	${SASRC}
.include "${SASRC}/Makefile"

.if ${MACHINE_CPUARCH} == "amd64"
CLEANFILES+= machine
beforedepend ${OBJS}: machine
machine: .NOMETA
	ln -fs ${SYSDIR}/i386/include machine
.endif
