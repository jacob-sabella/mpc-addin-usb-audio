#!/bin/sh
# Remove an MPC addin (the one addin.manifest describes): take its .so out of LD_PRELOAD (other addins stay),
# restart MPC, delete its folder.   sh uninstall.sh [-y] [-n] [-t <folder>]
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
        *) die "usage: sh uninstall.sh [-y] [-n] [-t <folder>]" ;;
    esac
done
. ./addin-lib.sh
load_manifest
check_dir
SO="$DIR/$ADDIN_SO"
[ -n "$ADDIN_INSTALL_TEST" ] || [ "$(id -u)" = 0 ] || die "run as root"
SVC=$(mpc_service)
if [ $YES = 0 ]; then
    printf "Remove %s%s? [y/N] " "$ADDIN_NAME" "$([ $RESTART = 1 ] && echo ' and restart MPC')"; read -r ok
    case "$ok" in y|Y|yes) ;; *) echo "cancelled"; exit 1 ;; esac
fi
preload_remove "$SVC" "$SO"
svc daemon-reload
if [ $RESTART = 1 ]; then svc restart "$SVC"; fi
rm -rf "$DIR"   # after the restart: MPC no longer has the .so mapped (with -n it stays mapped until MPC restarts)
sync
echo "Removed $ADDIN_NAME."
