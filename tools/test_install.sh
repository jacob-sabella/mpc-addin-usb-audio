#!/usr/bin/env bash
# The installer (vendored from mpc-addin-installer, whose own tests cover its rules) with this addin's manifest:
# install into a scratch systemd tree next to another preloaded library, settings kept on reinstall, uninstall
# restores the line. If mpc-addin-installer is checked out next to this repo (or at MPC_ADDIN_INSTALLER), the copy
# here must match it. BUSYBOX=/path/to/busybox runs it in the device's shell.
set -euo pipefail
cd "$(dirname "$0")/.."
INST="${MPC_ADDIN_INSTALLER:-../mpc-addin-installer}"
if [ -x "$INST/sync.sh" ]; then "$INST/sync.sh" --check . && echo "ok   installer: same as mpc-addin-installer"
else echo "skip installer copy check: no mpc-addin-installer at $INST"; fi
SH="sh"; BB="${BUSYBOX:-$(command -v busybox || true)}"; [ -n "$BB" ] && SH="$BB sh"
T=$(mktemp -d); trap 'rm -rf "$T"' EXIT
mkdir -p "$T/pkg" "$T/root/usr/lib/systemd/system"
cp install.sh uninstall.sh addin-lib.sh addin.manifest "$T/pkg/"
cp etc/usbaudio.conf.example "$T/pkg/usbaudio.conf"; echo so > "$T/pkg/libmpc_usb_audio.so"
U="$T/root/usr/lib/systemd/system/acvs.service"
printf '[Service]\nEnvironment=LD_PRELOAD=/usr/lib/x.so\nExecStart=/usr/bin/az01-launch-MPC\n' > "$U"
run() { ADDIN_INSTALL_TEST=1 SYSTEMD_ROOT="$T/root" $SH "$T/pkg/$1" -y -t "$T/addin" > "$T/out" 2>&1 || { cat "$T/out"; exit 1; }; }
fail() { echo "FAIL installer: $1"; exit 1; }
run install.sh
grep -qx "Environment=LD_PRELOAD=/usr/lib/x.so:$T/addin/libmpc_usb_audio.so" "$U" || fail "LD_PRELOAD: $(grep Env "$U")"
grep -q "^test_tone=0" "$T/addin/usbaudio.conf" || fail "settings file"
sed -i 's/^test_tone=0/test_tone=1/' "$T/addin/usbaudio.conf"; run install.sh
grep -q "^test_tone=1" "$T/addin/usbaudio.conf" || fail "settings not kept"
run uninstall.sh
grep -qx "Environment=LD_PRELOAD=/usr/lib/x.so" "$U" && [ ! -e "$T/addin" ] || fail "uninstall"
echo "ok   installer: install, reinstall keeps settings, uninstall"
