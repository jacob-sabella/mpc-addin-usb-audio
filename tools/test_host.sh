#!/bin/sh
# Offline x86 host tests, all under ASan + UBSan. Run from anywhere; nothing touches a device.
#   tools/test_host.sh          unit tests + addin integration test
#   TSAN=1 tools/test_host.sh   also run the ring stress test under ThreadSanitizer
set -eu
cd "$(dirname "$0")/.."
CC=${CC:-gcc}
B=build/host
mkdir -p "$B"
CF="-std=c11 -D_GNU_SOURCE -Wall -Wextra -Werror -g -O1 -fno-omit-frame-pointer"
SAN="-fsanitize=address,undefined -fno-sanitize-recover=undefined"
CORE="src/config.c src/chmap.c src/fmt.c src/drift.c src/gadget.c"
export ASAN_OPTIONS=detect_leaks=1:abort_on_error=1 UBSAN_OPTIONS=print_stacktrace=1

for t in ring fmt chmap drift config gadget; do
  $CC $CF $SAN -pthread "tests/test_$t.c" $CORE -lm -o "$B/t_$t"
  "$B/t_$t"
done
if [ "${TSAN:-0}" = 1 ]; then
  $CC $CF -fsanitize=thread -pthread tests/test_ring.c -o "$B/t_ring_tsan"
  "$B/t_ring_tsan"
fi

# Integration: fake libasound/libusbgx, the addin preloaded, a test binary named MPC.
$CC $CF $SAN -fPIC -shared tests/fake_libs.c -o "$B/libfake.so"
$CC $CF $SAN -fPIC -shared -fvisibility=hidden -DMPCUA_TEST_HOOKS -pthread \
  src/addin.c src/fwd.c src/alsa_api.c src/log.c $CORE -ldl -lm -o "$B/libmpc_usb_audio.so"
mkdir -p "$B/bin"
$CC $CF $SAN tests/fake_mpc.c -L"$B" -lfake -Wl,-rpath,"$PWD/$B" -ldl -o "$B/bin/MPC"
cp "$B/bin/MPC" "$B/bin/not-mpc"

tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT
mkdir -p "$tmp/cfs"
cat > "$tmp/usbaudio.conf" <<CONF
configfs=$tmp/cfs
tap_card=0
log=$tmp/usbaudio.log
CONF
# ASan's runtime must come first in the preload list.
ASANLIB=$($CC -print-file-name=libasan.so)
run() {
  MPC_USB_AUDIO_CONF="$tmp/usbaudio.conf" FAKE_CONFIGFS="$tmp/cfs" \
    LD_PRELOAD="$ASANLIB:$PWD/$B/libmpc_usb_audio.so" "$@"
}
run "$B/bin/MPC" "$tmp/cfs" || { cat "$tmp/usbaudio.log" 2>/dev/null; exit 1; }
run "$B/bin/not-mpc" "$tmp/cfs" inert
# Preloaded into an unrelated program, the library must stay out of the way.
run /bin/true
echo "--- addin log"
cat "$tmp/usbaudio.log"
grep -q "no USB audio" "$tmp/usbaudio.log" && { echo "FAIL: gadget-missing warning although the gadget came"; exit 1; }
# tap_card=auto without a platform codec (audio on a USB card), and MPC never enabling its gadget.
printf 'configfs=%s\ntap_card=auto\nlog=%s\n' "$tmp/cfs" "$tmp/auto.log" > "$tmp/auto.conf"
MPC_USB_AUDIO_CONF="$tmp/auto.conf" MPC_USB_AUDIO_GADGET_WAIT=1 FAKE_CONFIGFS="$tmp/cfs" \
  LD_PRELOAD="$ASANLIB:$PWD/$B/libmpc_usb_audio.so" "$B/bin/MPC" "$tmp/cfs" auto ||
  { cat "$tmp/auto.log" 2>/dev/null; exit 1; }
if [ ! -e /dev/snd/by-path/platform-sound ]; then
  grep -q "codec card 3: the first card MPC opened" "$tmp/auto.log" && grep -q "no USB audio: after 1 s" "$tmp/auto.log" ||
    { echo "FAIL: auto card / gadget warning"; cat "$tmp/auto.log"; exit 1; }
  echo "ok   auto card and missing-gadget warning logged"
fi
# Installed layout: no MPC_USB_AUDIO_CONF; usbaudio.conf is read from the .so's folder and log=auto writes there.
mkdir -p "$tmp/addin" "$tmp/cfs2"
cp "$B/libmpc_usb_audio.so" "$tmp/addin/"
printf 'configfs=%s\ntap_card=0\n' "$tmp/cfs2" > "$tmp/addin/usbaudio.conf"
FAKE_CONFIGFS="$tmp/cfs2" LD_PRELOAD="$ASANLIB:$tmp/addin/libmpc_usb_audio.so" "$B/bin/MPC" "$tmp/cfs2" ||
  { cat "$tmp/addin/usbaudio.log" 2>/dev/null; exit 1; }
grep -q "config $tmp/addin/usbaudio.conf" "$tmp/addin/usbaudio.log" && ! grep -q "unreadable\|problem" "$tmp/addin/usbaudio.log" ||
  { echo "FAIL: settings and log next to the .so"; cat "$tmp/addin/usbaudio.log" 2>/dev/null; exit 1; }
echo "ok   settings and log next to the .so"
tools/test_install.sh
echo "all host tests passed"
