# mpc-addin-usb-audio

An `LD_PRELOAD` addin for Akai MPC OS standalone devices that makes the MPC act as a
class-compliant USB audio interface on its USB device port, alongside its existing USB MIDI port:

- The computer gets a **recording device** carrying MPC's main out, plus MPC's inputs (4 channels by
  default, configurable up to 16).
- The computer gets a **playback device** whose audio arrives at MPC's inputs (summed with, or
  replacing, the physical inputs). It can then be sampled, monitored or recorded on the MPC.

It works with the drivers built into Linux, macOS and Windows 10/11 (USB Audio Class 2.0). The
kernel's UAC2 gadget function does the USB work. No kernel, root filesystem or MPC binary changes
are needed.

**Status: not yet run on a device.** The design is backed by `docs/FEASIBILITY.md`, and the code
passes the offline tests. `docs/DEVICE_TEST.md` is the proposed hardware test plan.

## How it works

1. MPC builds its standalone USB gadget (USB MIDI) with libusbgx at every start. The addin hooks
   `usbg_enable_gadget` and, just before the gadget is bound, adds a `uac2` function to it with
   MPC's own libusbgx handles. MPC's normal teardown therefore removes it too.
2. It hooks `snd_pcm_open`/`hw_params`/`close` to recognise MPC's playback and capture streams on
   the built-in codec.
3. In MPC's audio thread, the `snd_pcm_writei`/`readi` hooks (and the non-interleaved variants)
   copy main out and inputs into lock-free rings, and mix computer audio into what MPC reads. This
   code takes no locks, allocates nothing and makes no syscalls.
4. A forwarder thread (`usbaudio-fwd`, SCHED_OTHER, all signals blocked) moves the rings to and from
   the gadget's ALSA card. It steers the gadget's "Playback/Capture Pitch" controls with a PI loop,
   so the USB clock follows MPC's codec clock without resampling.

The addin does nothing unless the process is `/usr/bin/MPC`. In any other process (the launcher
scripts that share the environment, for example) every hook is a plain pass-through. The worst case
on the USB side is a dropout heard on the computer. MPC's audio thread never waits on the addin.

Latency is about 8-12 ms each way at the default `target_frames=256` and 44.1 kHz.

## Layout

| Path | What |
|---|---|
| `src/addin.c` | constructor, gadget hook, PCM hooks, audio-thread tap and inject |
| `src/fwd.c` | forwarder thread: gadget PCMs, drift loop, stats |
| `src/ring.h` | SPSC ring (C11 atomics) |
| `src/fmt.c`, `src/chmap.c` | sample formats, saturating mix, channel maps |
| `src/drift.c` | PI drift controller |
| `src/config.c` | `usbaudio.conf` parser |
| `src/gadget.c` | UAC2 configfs attributes, IAD device class |
| `src/alsa_api.c` | hand-written libasound ABI table, resolved with `dlsym(RTLD_NEXT)` |
| `tests/` | unit tests, plus an integration test (fake libasound and libusbgx, fake `MPC` binary) |
| `tools/test_host.sh` | runs all offline tests under ASan and UBSan (`TSAN=1` adds a TSan ring stress) |
| `tools/build_armhf.sh` | device build in `arm32v7/gcc:11-bullseye`; checks GLIBC <= 2.31, DT_NEEDED and exports |
| `etc/usbaudio.conf.example` | every setting, with defaults |

## Build and test

```sh
tools/test_host.sh       # x86, ASan + UBSan, no device needed
tools/build_armhf.sh     # needs Docker with arm/v7 emulation -> build/armhf/libmpc_usb_audio.so
```

The library links only libc, libdl, libpthread and libm. libasound and libusbgx are looked up at run
time from what MPC has already loaded.

## Install

Needs root on the device (SSH). Unzip a release (`MPC-USB-audio-addin-<version>-mpc-armv7.zip`) on the device and
run, as root, in its folder:

```sh
sh install.sh            # asks first; -y doesn't ask, -n doesn't restart MPC, -t <folder> installs elsewhere
sh /data/mpc-addins/usb-audio/uninstall.sh   # later, to remove it
```

It installs into `/data/mpc-addins/usb-audio/` and **adds** the library to `LD_PRELOAD` in MPC's systemd service;
other addins and libraries already in it stay. Reinstalling keeps your `usbaudio.conf`. The scripts are the shared addin
installer from [mpc-vst-plugins](https://github.com/sd88me/mpc-vst-plugins) (`tools/release/addin`, `docs/ADDINS.md`;
identical in every addin, `addin.manifest` describes this one). The catalog's `mpc-store.sh` and desktop app install
it too.

To build a release: `tools/build_armhf.sh`, then `tools/release.sh <version>` (needs mpc-vst-plugins next to this
repo, or `MPC_VST=/path`): `dist/MPC-USB-audio-addin-<version>-mpc-armv7.zip`, checked as the catalog checks it. For a
quick off switch without uninstalling, put `enabled=0` in `usbaudio.conf` and restart MPC.

## Configuration

Read from `usbaudio.conf` in the addin's folder (or `$MPC_USB_AUDIO_CONF`) when MPC starts. See
`etc/usbaudio.conf.example`. The settings changed most often:

- `to_host=out1,out2,in1,in2`: what the computer records.
- `host_channels`, `to_mpc_in`, `input_mode`: what the computer plays, and how it reaches MPC's
  inputs.
- `target_frames`: latency against robustness.
- `test_tone=1`: a 1 kHz tone to the computer, to check the USB path.

The log is `usbaudio.log` in the addin's folder (`log=` moves or disables it), with a stats line every 10 s.

## Limits

- The USB rate is fixed when MPC starts and must equal MPC's codec rate (44.1 kHz by default).
  When MPC runs at another rate, the addin logs it and stays idle.
- The computer sees MPC's main out as it goes to the codec. Per-track outputs exist only as far as
  MPC routes them to codec outputs (`out3`... on devices with more than two outputs).
- Not active in MPC's controller (computer) mode. MPC provides its own USB audio there.

## License

MIT, see `LICENSE`.
