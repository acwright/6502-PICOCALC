#!/usr/bin/env bash
# Regenerates src/rom/bios_rom.c from a 6502-BIOS BIOS.bin.
#
# Usage: ./embed-rom.sh [options]
#   ./embed-rom.sh                          # re-embed the v1.6 tag
#   ./embed-rom.sh --tag v1.6               # the same, said out loud
#   ./embed-rom.sh --rom /path/BIOS.bin --version v1.6
#   ./embed-rom.sh --check                  # regenerate and fail on any difference
#
# Options:
#   --tag <tag>        6502-BIOS tag to read BIOS.bin from (default: v1.6)
#   --bios-repo <dir>  where 6502-BIOS is checked out (default: $BIOS_REPO, or
#                      ../../Assembly/6502-BIOS beside this repo)
#   --rom <file>       use this image instead of a tag; needs --version
#   --version <label>  the version the header comment names (default: the tag)
#   --sha256 <hex>     require the image to hash to this
#   --check            leave the file alone and exit non-zero if it would change
#
# The image is the tag's committed BIOS.bin, which is what `make build` in
# 6502-BIOS produces; no assembler is needed here. The PicoCalc is a TMS9918A
# machine and stays on the 1.x line, so the tag is normally v1.6.
#
# Two things in this repo depend on where code sits in the image, and the
# script checks both against what is already embedded: the character set at
# $B800, which the launcher borrows to draw with, and the Kernal jump table at
# $FF00. BASIC's workspace pointers (BAS_* in src/machine/machine.c) are zero
# page and RAM addresses from BIOS.inc, so check those by hand when the BIOS
# minor version changes.

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
OUTPUT="$SCRIPT_DIR/src/rom/bios_rom.c"

TAG="v1.6"
BIOS_REPO="${BIOS_REPO:-$SCRIPT_DIR/../../Assembly/6502-BIOS}"
ROM=""
VERSION=""
WANT_SHA=""
CHECK=0

die() { echo "embed-rom: $*" >&2; exit 1; }

while [ $# -gt 0 ]; do
    case "$1" in
        --tag)       TAG="${2:?--tag needs a value}"; shift 2 ;;
        --bios-repo) BIOS_REPO="${2:?--bios-repo needs a value}"; shift 2 ;;
        --rom)       ROM="${2:?--rom needs a value}"; shift 2 ;;
        --version)   VERSION="${2:?--version needs a value}"; shift 2 ;;
        --sha256)    WANT_SHA="${2:?--sha256 needs a value}"; shift 2 ;;
        --check)     CHECK=1; shift ;;
        -h|--help)   sed -n '2,29p' "$0" | sed 's|^# \{0,1\}||'; exit 0 ;;
        *)           die "unknown option $1" ;;
    esac
done

WORK="$(mktemp -d)"
trap 'rm -rf "$WORK"' EXIT

IMAGE="$WORK/BIOS.bin"
if [ -n "$ROM" ]; then
    [ -f "$ROM" ] || die "no ROM at $ROM"
    [ -n "$VERSION" ] || die "--rom needs --version"
    cp "$ROM" "$IMAGE"
    SOURCE="$ROM"
else
    [ -d "$BIOS_REPO/.git" ] || die "no 6502-BIOS checkout at $BIOS_REPO; pass --bios-repo or --rom"
    git -C "$BIOS_REPO" rev-parse --verify --quiet "$TAG^{commit}" >/dev/null \
        || die "$BIOS_REPO has no tag $TAG; fetch it first"
    git -C "$BIOS_REPO" show "$TAG:BIOS.bin" > "$IMAGE" \
        || die "$TAG has no BIOS.bin"
    VERSION="${VERSION:-$TAG}"
    SOURCE="$BIOS_REPO $TAG ($(git -C "$BIOS_REPO" rev-parse "$TAG^{commit}"))"
fi

SIZE=$(wc -c < "$IMAGE" | tr -d ' ')
[ "$SIZE" = "32768" ] || die "the image is $SIZE bytes; it must be 32768"

SHA=$(shasum -a 256 "$IMAGE" | cut -d' ' -f1)
if [ -n "$WANT_SHA" ] && [ "$SHA" != "$WANT_SHA" ]; then
    die "the image has sha256 $SHA, not $WANT_SHA"
fi

# The two regions this repo reads by address. Warn rather than refuse: a BIOS
# that moves them is legitimate, it just needs the dependants looked at.
if [ -f "$OUTPUT" ]; then
    python3 - "$OUTPUT" "$IMAGE" <<'PY' || true
import re, sys
old = bytes(int(b, 16) for b in re.findall(r'0x([0-9a-fA-F]{2}),', open(sys.argv[1]).read()))
new = open(sys.argv[2], 'rb').read()
if len(old) == len(new):
    for lo, length, what in [
        (0x3800, 2048, 'the character set at $B800 (src/launcher/launcher.c)'),
        (0x7f00, 256, 'the Kernal jump table at $FF00'),
    ]:
        if old[lo:lo + length] != new[lo:lo + length]:
            print(f'embed-rom: warning: {what} changed; re-check what depends on it', file=sys.stderr)
    changed = [i for i in range(len(old)) if old[i] != new[i]]
    if changed:
        print(f'embed-rom: {len(changed)} bytes change, '
              f'${0x8000 + changed[0]:04X}..${0x8000 + changed[-1]:04X}', file=sys.stderr)
PY
fi

{
    cat <<EOF
// AC6502 BIOS ROM (BIOS.bin from acwright/6502-BIOS), embedded verbatim.
// 32KB, mapped at machine addresses \$8000-\$FFFF; only \$A000-\$FFFF is ever
// read as ROM (see src/machine/machine.c) since \$8000-\$9FFF is shadowed by
// the I/O slot window.
//
// Build: $VERSION, SHA-256 $SHA.
// Regenerate with ./embed-rom.sh; do not hand-edit the bytes. Nothing here
// depends on where anything in the image sits, with two exceptions the script
// checks and which are worth re-reading when they move: the character set at
// \$B800, which the launcher borrows to draw with (src/launcher/launcher.c),
// and BASIC's workspace pointers, which the program loader writes (BAS_* in
// src/machine/machine.c).
#include "bios_rom.h"

const uint8_t bios_rom[BIOS_ROM_SIZE] = {
EOF
    xxd -p -c 16 "$IMAGE" | sed -e 's/\(..\)/0x\1,/g' -e 's/^/    /'
    echo "};"
} > "$WORK/bios_rom.c"

if [ "$CHECK" = "1" ]; then
    if diff -q "$OUTPUT" "$WORK/bios_rom.c" >/dev/null 2>&1; then
        echo "embed-rom: check passed; src/rom/bios_rom.c is $VERSION ($SHA)"
    else
        diff "$OUTPUT" "$WORK/bios_rom.c" | head -20 >&2
        die "src/rom/bios_rom.c is not $VERSION; run ./embed-rom.sh"
    fi
else
    cp "$WORK/bios_rom.c" "$OUTPUT"
    echo "embed-rom: wrote src/rom/bios_rom.c from $SOURCE"
    echo "embed-rom:   $VERSION, sha256 $SHA"
fi
