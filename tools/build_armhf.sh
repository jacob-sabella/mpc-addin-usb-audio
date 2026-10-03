#!/bin/sh
# Build libmpc_usb_audio.so for MPC OS (armv7 hard-float, glibc <= 2.31) in a Debian bullseye
# container, then check the result: no symbol newer than GLIBC_2.31, and no DT_NEEDED on libasound
# or libusbgx (both are resolved at run time from what MPC already loaded).
# Needs Docker with arm/v7 emulation (qemu-user binfmt). Output: build/armhf/libmpc_usb_audio.so, and build/package/
# (the .so, the default usbaudio.conf and the installer) to copy to a device.
set -eu
cd "$(dirname "$0")/.."
IMAGE=${IMAGE:-arm32v7/gcc:11-bullseye}
OUT=build/armhf
mkdir -p "$OUT"
SRCS="src/addin.c src/fwd.c src/alsa_api.c src/log.c src/config.c src/chmap.c src/fmt.c src/drift.c src/gadget.c"

docker run --rm --platform linux/arm/v7 -u "$(id -u):$(id -g)" -v "$PWD:/w" -w /w "$IMAGE" sh -c "
  set -e
  gcc -std=c11 -D_GNU_SOURCE -O2 -g0 -fPIC -shared -fvisibility=hidden \
      -march=armv7-a -mfpu=neon-vfpv4 -mfloat-abi=hard \
      -Wall -Wextra -Werror -Wl,--as-needed -Wl,-z,relro,-z,now -Wl,--no-undefined \
      $SRCS -ldl -lpthread -lm -o $OUT/libmpc_usb_audio.so
  strip --strip-unneeded $OUT/libmpc_usb_audio.so
  readelf -d $OUT/libmpc_usb_audio.so | grep NEEDED > $OUT/needed.txt
  objdump -T $OUT/libmpc_usb_audio.so | grep -o 'GLIBC_[0-9.]*' | sort -uV > $OUT/glibc.txt
  objdump -T $OUT/libmpc_usb_audio.so | awk '\$4 == \".text\" {print \$NF}' | sort > $OUT/exports.txt
"

echo "NEEDED:"; sed 's/^/  /' "$OUT/needed.txt"
if grep -Eq 'libasound|libusbgx' "$OUT/needed.txt"; then echo "FAIL: links libasound/libusbgx" >&2; exit 1; fi
max=$(tail -n1 "$OUT/glibc.txt")
echo "highest symbol version: $max"
if [ "$(printf '%s\nGLIBC_2.31\n' "$max" | sort -V | tail -n1)" != GLIBC_2.31 ]; then
  echo "FAIL: needs $max, device glibc is 2.31" >&2; exit 1
fi
echo "exports:"; sed 's/^/  /' "$OUT/exports.txt"
unexpected=$(grep -Ev '^(snd_pcm_(open|close|hw_params|writei|writen|readi|readn)|usbg_enable_gadget)$' "$OUT/exports.txt" || true)
if [ -n "$unexpected" ]; then echo "FAIL: unexpected exports: $unexpected" >&2; exit 1; fi
ls -l "$OUT/libmpc_usb_audio.so"
echo "armhf build OK"

# The package: what to copy to the device and run install.sh in (mpc-addin-installer).
P=build/package
rm -rf "$P"; mkdir -p "$P"
cp "$OUT/libmpc_usb_audio.so" addin.manifest install.sh uninstall.sh addin-lib.sh "$P/"
cp etc/usbaudio.conf.example "$P/usbaudio.conf"
echo "package: $P/"; ls "$P"
