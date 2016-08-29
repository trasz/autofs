#!/bin/sh

if [ $# -ne 2 ]; then
	echo "usage: $0 prefix path"
	exit 1
fi

RSYNC="/usr/local/bin/rsync"

SOURCE="/net"
PREFIX="${1}"
PATH="${2}"
SPATH="${SOURCE}/${PATH}"
if [ "${PATH}" = "/" ]; then
	DPATH="${PREFIX}/${PATH}"
else
	DPATH=`/usr/bin/dirname "${PREFIX}/${PATH}"`
fi

${RSYNC} -vaAX --max-size=30m --inplace -r --exclude='/*/*/*' "${SPATH}" "${DPATH}"

