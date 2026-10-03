#!/usr/bin/env bash
# The release zip: the build (run tools/build_armhf.sh first) packaged with mpc-vst-plugins' shared addin installer, then checked as
# the catalog will check it. Needs mpc-vst-plugins next to this repo (or MPC_VST=/path), docs/ADDINS.md there.
# Usage: tools/release.sh <version>   ->  dist/<Name>-<version>-mpc-armv7.zip
set -euo pipefail
cd "$(dirname "$0")/.."
[ $# -eq 1 ] || { echo "usage: $0 <version>" >&2; exit 2; }
MPC_VST="${MPC_VST:-../mpc-vst-plugins}"
python3 "$MPC_VST/tools/release_addin.py" --dir build/package --version "$1" --repo jacob-sabella/mpc-addin-usb-audio --license MIT \
  --about "The MPC as a USB audio interface: its outputs and inputs as channels on the computer, next to its MIDI ports." -o dist
python3 "$MPC_VST/tools/catalog_check.py" dist/*-"$1"-mpc-armv7.zip --catalog --expect-id usb-audio --expect-repo jacob-sabella/mpc-addin-usb-audio
