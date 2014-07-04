# $FreeBSD$

BIN2=	include_ldap special_hosts special_null

distribution:
	${INSTALL} -o ${BINOWN} -g ${BINGRP} -m 755 \
	    ${BIN2} ${DESTDIR}/etc/autofs

.include <bsd.prog.mk>
