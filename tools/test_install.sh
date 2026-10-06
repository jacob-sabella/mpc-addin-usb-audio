#!/usr/bin/env bash
# The shared addin installer (mpc-vst-plugins' tools/release/addin, whose own tests cover its rules) with this addin's
# manifest: install into a scratch systemd tree next to another preloaded library, settings kept on reinstall,
# uninstall restores the line. Needs mpc-vst-plugins next to this repo (or MPC_VST=/path). BUSYBOX=/path/to/busybox
# runs it in the device's shell.
set -euo pipefail
cd "$(dirname "$0")/.."
MPC_VST="${MPC_VST:-../mpc-vst-plugins}"; INST="$MPC_VST/tools/release/addin"
[ -f "$INST/addin-lib.sh" ] || { echo "FAIL installer: no mpc-vst-plugins at $MPC_VST (set MPC_VST)"; exit 1; }
SH="sh"; BB="${BUSYBOX:-$(command -v busybox || true)}"; [ -n "$BB" ] && SH="$BB sh"
T=$(mktemp -d); trap 'rm -rf "$T"' EXIT
mkdir -p "$T/pkg" "$T/root/usr/lib/systemd/system"
cp "$INST/install.sh" "$INST/uninstall.sh" "$INST/addin-lib.sh" addin.manifest "$T/pkg/"
cp etc/usbaudio.conf.example "$T/pkg/usbaudio.conf"; printf '\177ELF\001\001\001\000\000\000\000\000\000\000\000\000\003\000\050\000so' > "$T/pkg/libmpc_usb_audio.so"   # the .so has an ARM shared object's header: install.sh checks it
U="$T/root/usr/lib/systemd/system/acvs.service"
printf '[Service]\nEnvironment=LD_PRELOAD=/usr/lib/x.so\nExecStart=/usr/bin/az01-launch-MPC\n' > "$U"
run() { ADDIN_INSTALL_TEST=1 SYSTEMD_ROOT="$T/root" $SH "$T/pkg/$1" -y -t "$T/usb-audio" > "$T/out" 2>&1 || { cat "$T/out"; exit 1; }; }
fail() { echo "FAIL installer: $1"; exit 1; }
run install.sh
grep -qx "Environment=LD_PRELOAD=/usr/lib/x.so:$T/usb-audio/libmpc_usb_audio.so" "$U" || fail "LD_PRELOAD: $(grep Env "$U")"
grep -q "^test_tone=0" "$T/usb-audio/usbaudio.conf" || fail "settings file"
sed -i 's/^test_tone=0/test_tone=1/' "$T/usb-audio/usbaudio.conf"; run install.sh
grep -q "^test_tone=1" "$T/usb-audio/usbaudio.conf" || fail "settings not kept"
ADDIN_INSTALL_TEST=1 SYSTEMD_ROOT="$T/root" $SH "$T/usb-audio/uninstall.sh" -y -t "$T/usb-audio" > "$T/out" 2>&1 || { cat "$T/out"; exit 1; }
grep -qx "Environment=LD_PRELOAD=/usr/lib/x.so" "$U" && [ ! -e "$T/usb-audio" ] || fail "uninstall"
echo "ok   installer: install, reinstall keeps settings, uninstall from the installed folder"
