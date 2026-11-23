#!/bin/sh
# SPDX-License-Identifier: APSL-2.0
set -e

TARGET=${1:-android16-6.12}
IMAGE=docker.cnb.cool/ylarod/ddk/ddk-min:${TARGET}
ROOTDIR=$(cd "$(dirname "$0")/../.." && pwd)

docker run --rm \
	-e KDIR=/opt/ddk/kdir/${TARGET} \
	-v "$ROOTDIR":/src \
	-w /src/noGPL \
	"$IMAGE" \
	sh -c 'make clean VER="$1" 2>/dev/null; make VER="$1"' sh "$TARGET"

echo "-> ${ROOTDIR}/noGPL/out/${TARGET}"
find "$ROOTDIR/noGPL/out/${TARGET}" -name "*.ko"
