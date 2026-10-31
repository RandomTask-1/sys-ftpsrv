#!/bin/sh
set -e

cd "$(dirname "$0")"

cmake --preset Release
cmake --build --preset Release

rm -f sys-ftpsrv.zip
(cd build/Release/out && cmake -E tar cf ../../../sys-ftpsrv.zip --format=zip atmosphere config switch)
echo "created $(pwd)/sys-ftpsrv.zip"
