#!/bin/sh
# Copy the graphics headers into a destination directory.
# Usage: ./graphics.sh <destination-dir>

set -eu

HEADERS="arena.h terminal.h vec.h"
SRC_DIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)

if [ $# -ne 1 ]; then
    echo "usage: $0 <destination-dir>" >&2
    exit 1
fi

DEST=$1

for h in $HEADERS; do
    if [ ! -f "$SRC_DIR/$h" ]; then
        echo "$0: missing header $SRC_DIR/$h" >&2
        exit 1
    fi
done

mkdir -p -- "$DEST"

for h in $HEADERS; do
    cp -- "$SRC_DIR/$h" "$DEST/$h"
    echo "$h -> $DEST/$h"
done
