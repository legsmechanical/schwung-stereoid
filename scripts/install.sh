#!/usr/bin/env bash
# install.sh — deploy dist/stereoid/ to the Move.
# Usage: scripts/install.sh              (WiFi, move.local)
#        MOVE_HOST=172.16.254.1 scripts/install.sh   (USB tether)
#
# Installs to the STOCK tree only. dbx-host's modules/audio_fx is a symlink to
# it, so dAVEBOx picks the module up from there; a second copy would be the
# same directory written twice.
set -euo pipefail

HERE="$(cd "$(dirname "$0")/.." && pwd)"
MODULE_ID=stereoid
HOST="${MOVE_HOST:-move.local}"
DEST="/data/UserData/schwung/modules/audio_fx/${MODULE_ID}"

[ -f "$HERE/dist/${MODULE_ID}/${MODULE_ID}.so" ] || { echo "run scripts/build.sh first" >&2; exit 1; }

echo "==> installing to ableton@${HOST}:${DEST}"
ssh "ableton@${HOST}" "mkdir -p '${DEST}'"
# A temp name, then mv: never write a loaded .so in place (ETXTBSY), and a
# module file may be hard-linked into a backup tree, which mv leaves alone.
for f in "${MODULE_ID}.so" module.json help.json LICENSE; do
    scp "$HERE/dist/${MODULE_ID}/$f" "ableton@${HOST}:${DEST}/.$f.new"
    ssh "ableton@${HOST}" "mv -f '${DEST}/.$f.new' '${DEST}/$f'"
done
ssh "ableton@${HOST}" "chmod -R a+rw '${DEST}'"
echo "==> installed. Swap the FX out and back in (or restart) to load the new .so."
