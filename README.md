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

**Status: runs on hardware.** Enumeration and the MPC-to-computer path are verified (`docs/DEVICE_TEST.md`,
steps 1-2); computer-to-MPC audio, drift over time, load and the other USB modes (steps 3-6) are not yet.
The design is backed by `docs/FEASIBILITY.md`.

**Not on the Force, MPC Live II or MPC X.** On those models the USB-B socket sits behind the internal USB hub, which
a computer gets only in Computer Mode, so in standalone mode no USB gadget on the device can reach a computer; the
device enumerates it itself. That is wiring, not firmware (Force: device report, 2026-11-08; Live II and X: their
device trees). The addin detects it, builds nothing and says why in the log. Details and the alternative under Limits.

## How it works

1. MPC builds its standalone USB gadget (USB MIDI) with libusbgx at every start. The addin hooks
   `usbg_enable_gadget` and, just before the gadget is bound, adds a `uac2` function to it with
   MPC's own libusbgx handles. MPC's normal teardown therefore removes it too.
   Where MPC builds no gadget in standalone mode, the addin builds one of its own after `own_wait`
   seconds, with only the audio function (`standalone_create`, below). On models whose device port is behind
   the internal USB hub (Limits) it builds nothing.
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
| `src/gadget.c` | UAC2 configfs attributes, IAD device class, the addin's own gadget where MPC builds none |
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

CI (`.github/workflows`): `test.yml` runs the tests and the armhf build on every push; `release.yml` (Actions > Release, with the version) builds the zip and attaches it to a draft release, to publish once it has been tried on a device.

## Install

Needs root on the device (SSH). Unzip a release (`MPC-USB-audio-addin-<version>-mpc-armv7.zip`) on the device and
run, as root, in its folder:

```sh
sh install.sh            # asks first; -y doesn't ask, -n doesn't restart MPC, -t <folder> installs elsewhere
sh /data/mpc-addins/usb-audio/uninstall.sh   # later, to remove it
```

It installs into `/data/mpc-addins/usb-audio/` and **adds** the library to `LD_PRELOAD` in MPC's systemd service;
other addins and libraries already in it stay. Reinstalling keeps your `usbaudio.conf`. Uninstalling deletes the files
it installed, `usbaudio.conf` included; the addin's own log, `usbaudio.log`, stays in the folder (delete it by hand). The scripts are the shared addin
installer from [mpc-vst-plugins](https://github.com/open-mpc/mpc-vst-plugins) (`tools/release/addin`, `docs/ADDINS.md`;
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
- **Force, MPC Live II, MPC X: no USB audio in standalone mode, by hardware.** On these models the device's only USB
  device port is wired to port 7 of the internal USB hub (an SMSC USB5537B), and a mux (`/sys/devices/platform/usb-mux`,
  states `internal`/`external`) decides who is upstream of that hub: the device's own processor in standalone mode,
  the USB-B socket in Computer Mode and Update Mode. In standalone mode a gadget on that port is therefore enumerated
  by the device itself (a Force listed "MPC USB Audio" as its own sound card, at `usb-2-1.7`), and a computer on USB-B
  hears nothing. Flipping the mux hands the whole hub to the computer: the pad/knob board, the SATA bay drive and the
  USB-A sockets go with it (the author of Force Link Audio confirmed this on a Force: the gadget enumerates, every
  control goes dark). The addin reads the mux at start, builds nothing when it says `internal`, and logs why;
  `hub_mux_check=0` builds anyway. As a backstop for a model with the same wiring but no mux node, a gadget of ours
  that turns up on the device's own USB bus is removed again. Verified on a Force (firmware 3.10, device report
  2026-11-08); the Live II and X by their device trees in the MPC firmware image (one of the two Key 61 device trees
  carries the mux too; the Key 37, One and One+ have none, their device port is wired to the USB-B socket). For audio
  to a computer from a standalone Force, the catalog's `force-link-audio` addin streams the main mix over the network
  to Ableton Link Audio receivers.
- Where MPC builds its standalone USB gadget (the USB MIDI port; the MPC Key 37), the audio function is added
  to it. Some setups build none in standalone mode: an MPC One on HAKAI 3.9.1 (issue #4). The MPC Live II on
  custom firmware (issue #1, 2026-10-09) builds none either, but for the hub reason above, which no fallback can
  help. For the MPC One the addin waits `own_wait` seconds (30) and then builds a gadget
  of its own, `own_gadget`, with only the audio function, so the computer sees a USB audio device but no MPC
  MIDI port. The log says "MPC built no gadget ... built our own". It removes that gadget as soon as MPC enables
  one, when MPC exits, and at the next MPC start (MPC restarts on a switch to controller mode).
  `standalone_create=0` turns this off; then the log says "no USB audio: after 60 s MPC has not enabled its
  gadget" with the USB controller's state. The reporter of #4 ran the same approach on an MPC One; this build
  of it has run offline only so far.
- On HAKAI, MPC is started by `/usr/bin/az01-launch-MPC`, which sets `LD_PRELOAD` itself, so the installer's
  systemd setting doesn't reach MPC and the addin isn't loaded. The installer warns when that happens. Until
  the shared installer handles that launcher, add the library to its `LD_PRELOAD` lines by hand (issue #4).
- `tap_card=auto` finds the codec by `/dev/snd/by-path/platform-sound`. Models whose audio is a USB device
  have none; there it uses the first card MPC opens and logs which. Set `tap_card=<n>` if that is wrong.

## License

MIT, see `LICENSE`.
