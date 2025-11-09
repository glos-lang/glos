#!/bin/sh

BASE="$(dirname "$(dirname "$(realpath "$0")")")"

TMP_NAME="assets/main.glos"
TMP_PATH="$BASE/$TMP_NAME"

{
    echo "package main"
    echo ""
    echo "import ("

    DIR="$BASE/std"
    find "$DIR" -name "*.glos" -not -path "$DIR/builtin/*" -exec dirname {} \; \
        | sort -u \
        | sed "s|^$DIR/||;s|.*|	\"&\"|"

    echo ")"
} > "$TMP_PATH"

DOCS="$BASE/docs"
glos -d "$DOCS" "$TMP_PATH"
rm "$TMP_PATH"

rm -rf "$DOCS/$TMP_NAME"
sed -i "\|'$TMP_NAME'|d" "$DOCS/index.html"
