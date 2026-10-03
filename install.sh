#!/bin/sh
# Install an MPC addin (the one addin.manifest describes) on an MPC OS device. Run on the device as root, from
# the unpacked folder:   sh install.sh [-y] [-n] [-t <folder>]
# Copies the addin's files into <folder> (default /data/mpc-addins/<id>), adds its .so to the LD_PRELOAD of MPC's
# systemd service, and restarts MPC. LD_PRELOAD is extended, never replaced: other addins already in it stay. Where
# the service already sets it, the line that takes effect is edited in place (a backup is kept); a second
# Environment=LD_PRELOAD= would replace the whole list. Otherwise one drop-in shared by every addin sets it.
#   -y  don't ask   -n  don't restart MPC (the addin starts with MPC's next start)
# mpc-addin-installer: identical in every addin.
set -e
cd "$(dirname "$0")"
YES=0; RESTART=1; DIR=""
die() { echo "error: $*" >&2; exit 1; }
while [ $# -gt 0 ]; do
    case "$1" in
        -y) YES=1; shift ;;
        -n) RESTART=0; shift ;;
        -t) [ -n "$2" ] || die "-t needs a folder"; DIR="$2"; shift 2 ;;
        *) die "usage: sh install.sh [-y] [-n] [-t <folder>]" ;;
    esac
done
. ./addin-lib.sh
load_manifest
check_dir
SO="$DIR/$ADDIN_SO"
if [ -z "$ADDIN_INSTALL_TEST" ]; then
    [ "$(id -u)" = 0 ] || die "run as root"
    case "$(uname -m)" in armv7*) ;; *) die "this build is for 32-bit ARM MPC OS devices; this one is $(uname -m)" ;; esac
fi
for f in "$ADDIN_SO" $ADDIN_CONF $ADDIN_FILES; do [ -f "$f" ] || die "$f is missing next to install.sh"; done

SVC=$(mpc_service)
UNIT=$(unit_with_preload "$SVC")
echo "Installing $ADDIN_NAME:"
echo "  $DIR/"
if [ -n "$UNIT" ]; then echo "  LD_PRELOAD in $UNIT gains $ADDIN_SO (a backup is kept)"
else echo "  a drop-in sets LD_PRELOAD for $SVC"; fi
if [ $RESTART = 1 ]; then echo "  then MPC restarts: save your project first"; fi
if [ $YES = 0 ]; then
    printf "Continue? [y/N] "; read -r ok
    case "$ok" in y|Y|yes) ;; *) echo "cancelled"; exit 1 ;; esac
fi

mkdir -p "$DIR"
for f in "$ADDIN_SO" $ADDIN_FILES; do   # staged, then renamed: a running MPC keeps the old file mapped
    cp "$f" "$DIR/$f.new" && chmod 644 "$DIR/$f.new" && mv "$DIR/$f.new" "$DIR/$f"
done
for f in $ADDIN_CONF; do [ -f "$DIR/$f" ] || cp "$f" "$DIR/$f"; done
preload_add "$SVC" "$UNIT" "$SO"
svc daemon-reload
sync
if [ $RESTART = 1 ]; then svc restart "$SVC"; echo "Done. MPC restarted."
else echo "Done. The addin starts with MPC's next start."; fi
[ -z "$ADDIN_DONE" ] || echo "$ADDIN_DONE"
