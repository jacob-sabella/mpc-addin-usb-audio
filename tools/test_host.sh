#!/bin/sh
# Offline x86 host tests, all under ASan + UBSan. Run from anywhere; nothing touches a device.
#   tools/test_host.sh          unit tests + add-in integration test
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

# Integration: fake libasound/libusbgx, the add-in preloaded, a test binary named MPC.
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
echo "--- add-in log"
cat "$tmp/usbaudio.log"
echo "all host tests passed"
