# Feasibility: the MPC as a USB audio interface in standalone mode

Goal: while the MPC runs in normal standalone mode, a computer connected to its USB device port sees a
class-compliant USB Audio 2.0 interface next to the existing USB-MIDI ports. The computer records the
MPC's main out (and the MPC's inputs) as a multichannel recording device, and anything the computer
plays arrives at the MPC as an input.

Everything below was gathered read-only on one device (MPC Key 37, MPC OS with kernel
`6.18.26-az01-2026-04-30-rt4`, PREEMPT_RT, armv7l, glibc 2.39) and by static analysis of a copy of
`/usr/bin/MPC` and `/usr/lib/libusbgx.so.3.0.0`. Nothing on the device was changed. Addresses are
file offsets/virtual addresses in that build of `MPC` (PIE, text loaded at 0).

## Verdict

**Feasible as a pure userspace LD_PRELOAD addin. No kernel change, no root-fs change and no binary
patch are needed.** Every kernel piece is built in, MPC already creates the USB gadget through
libusbgx (which a preload can hook), MPC's audio goes through dynamically linked libasound with
`snd_pcm_writei`/`snd_pcm_readi`, and the kernel's UAC2 gadget has the pitch controls needed for
clock-drift compensation without a resampler. MPC itself already contains a complete UAC2 gadget
forwarder, but only for its controller ("computer") mode, where the standalone engine is off.

| Question | Verdict | Evidence (section) |
|---|---|---|
| USB device controller present | yes: `ff580000.usb` (DWC2, dual role, high speed) | 1 |
| configfs gadget + `f_uac2` + `u_audio` | yes, all built in (`=y`), no modules needed | 1 |
| What is on the device port today | gadget `standalone`, one function `midi.midi`, created by MPC itself | 2 |
| Can an addin add a function MPC won't wipe | yes, by hooking MPC's own `usbg_enable_gadget` call | 2, 3 |
| MPC's audio path | JUCE ALSA, `libasound.so.2` is DT_NEEDED, `hw:` card `ACVR`, S32_LE, 2 ch, 44100 Hz, period 128, RW_INTERLEAVED | 4 |
| Where to tap / inject | `snd_pcm_writei` (main out), `snd_pcm_readi` (inputs) | 5 |
| Clock drift | handled by the gadget's "Playback/Capture Pitch 1000000" controls, the same method MPC's own forwarder uses; no resampler | 6 |
| Latency / CPU | ~8 to 12 ms added each way; well under 1% CPU on the audio cores | 7 |
| "Individual outputs" | only the physical channels MPC sends to the codec (2 on this model) | 8 |
| Blockers | none hard; open risks are listed in section 9 | 9 |

### Which reading of "USB audio support"

Two readings were considered:

1. **The MPC acts as a USB audio interface for a computer** (device port). Not available in
   standalone mode today; the port only carries MIDI. This is what the addin builds.
2. **Better support for class-compliant interfaces plugged into the MPC** (host ports). The kernel
   already has `CONFIG_SND_USB_AUDIO=y`, and MPC already enumerates ALSA cards and has format and
   access-mode handling for external devices (strings at `MPC.strings` near "Error: Read/Write Stereo
   Interleaved access may not be supported"). An addin has little to add there, and the user
   explicitly described the device-port behaviour.

Reading 1 is both the requested feature and the feasible one, so the goal is unchanged.

## 1. Kernel and controller

```
$ ls -la /sys/class/udc/
ff580000.usb -> ../../devices/platform/ff580000.usb/udc/ff580000.usb
$ cat /sys/class/udc/ff580000.usb/maximum_speed
high-speed
$ zcat /proc/config.gz | grep -E 'USB_CONFIGFS|U_AUDIO|F_UAC2|DWC2'
CONFIG_USB_DWC2=y
CONFIG_USB_DWC2_DUAL_ROLE=y
CONFIG_USB_U_AUDIO=y
CONFIG_USB_F_UAC2=y
CONFIG_USB_CONFIGFS=y
CONFIG_USB_CONFIGFS_F_UAC2=y
# CONFIG_USB_CONFIGFS_F_UAC1 is not set
# CONFIG_USB_CONFIGFS_F_UAC2_AZ01 is not set
CONFIG_USB_CONFIGFS_F_MIDI=y
CONFIG_USB_CONFIGFS_F_FS=y
CONFIG_USB_CONFIGFS_MASS_STORAGE=y
```

- Device tree (`/proc/device-tree/usb@ff580000`): `compatible = rockchip,rk3288-usb, snps,dwc2`,
  `dr_mode = peripheral`, `g-rx-fifo-size = 0x113`, `g-np-tx-fifo-size = 0x10`,
  `g-tx-fifo-size = 0x100 0x80 0x80 0x40 0x40 0x20` (words).
- Boot log: `dwc2 ff580000.usb: EPs: 10, dedicated fifos, 972 entries in SPRAM`.
- The kernel tree has a vendor variant of f_uac2 (`CONFIG_USB_CONFIGFS_F_UAC2_AZ01`), which is
  **off** in this build; the upstream f_uac2 is what is available. Other firmware builds might
  enable the variant instead. The addin treats every attribute beyond channel mask, rate and sample
  size as optional, so it works with either.
- `/lib/modules` has nothing relevant to load; everything needed is `=y`.

Endpoint budget: MIDI uses 1 IN + 1 OUT, UAC2 uses 1 IN data + 1 OUT data + 1 IN feedback. That is 5
of the 9 non-control endpoints, and 3 of the 6 TX FIFOs. A high-speed isochronous IN packet for
4 ch x 4 bytes at 44.1 kHz with a 1 ms interval (`p_hs_bint=4`) is at most ~46 frames x 16 B = 736 B.
Only the single 1 KiB FIFO (0x100 words) fits that; the MIDI bulk IN needs 512 B and the feedback
endpoint 4 B. At the default 125 us interval (`p_hs_bint=1`) it is 7 x 16 = 112 B, which fits even
the smallest FIFO (0x20 words = 128 B). So `hs_bint=4` is fine up to 5 channels to the computer at
4 bytes per sample; layouts wider than that should use `hs_bint=1`, which costs more interrupts
(section 7). DWC2 picks a FIFO per endpoint at bind time, so the first device test checks this (see
`DEVICE_TEST.md`).

**The same controller already does UAC2:** controller/computer mode (USB PID `0x1057`) enumerates on a
computer as a USB Audio class device. This was observed earlier on this unit (see the interop
project notes), and that mode's gadget is built from the strings in section 3.

## 2. What is on the device port today

```
$ ls /sys/kernel/config/usb_gadget/
standalone
$ cat .../standalone/{UDC,idVendor,idProduct,bcdDevice,bDeviceClass}
ff580000.usb  0x09e8  0x0057  0x3091  0x00
$ ls .../standalone/functions .../standalone/configs/config.1
midi.midi        (functions)
MaxPower bmAttributes midi.midi strings   (config.1; bmAttributes 0xc0, MaxPower 2)
midi.midi: in_ports=2 out_ports=2 buflen=512 qlen=32 id="MPC Key 37"
```

With a cable to a Linux computer (connected by the user during this study):

```
device: /sys/class/udc/ff580000.usb/state = configured, current_speed = high-speed
computer: Bus 003 Device 018: ID 09e8:0057 ... MPC Key 37   (480 Mb/s)
          bNumInterfaces 2: Audio/Control Device, Audio/MIDI Streaming
          ALSA card "M37 ... MPC Key 37 ... high speed" (MIDI only, no PCM)
```

**MPC itself owns this gadget.** `MPC` imports `usbg_init`, `usbg_create_gadget`,
`usbg_create_function`, `usbg_add_config_function`, `usbg_enable_gadget`, `usbg_rm_gadget`, and also
`usbg_to_uac2_function` / `usbg_f_uac2_set_attr_val` (`readelf --dyn-syms MPC`). The kernel log shows
`bound driver configfs-gadget.standalone` at every MPC start (13:14, 13:16, 14:47, ... 22:56 on the
study day, matching acvs restarts), and the interop notes record that MPC re-creates the gadget as
MIDI-only on every restart, which wiped an Ethernet function that had been added by hand.

The creation sequence, from disassembly of the function at `0x1f63800..0x1f64130`:

```
usbg_init                         0x1f63854
usbg_get_first_udc                0x1f63960
usbg_get_udc_gadget / disable     0x1f63970 / 0x1f6397c
usbg_get_gadget("standalone")     0x1f6398c   (string ref at 0x1f63988)
  -> usbg_rm_gadget(g, 1)         0x1f639a8   (1 = USBG_RM_RECURSE)
usbg_create_gadget("standalone")  0x1f639c8
usbg_set_gadget_vendor_id / product_id / device_bcd_device
usbg_create_config(g, 1, ...)     0x1f63ad0
usbg_set_config_bm_attrs(c, 0xc0) 0x1f63ae4
usbg_create_function(g, 11, ...)  0x1f63b04   (11 = USBG_F_MIDI)
usbg_f_midi_set_attr_val x5       0x1f63b34..0x1f63b9c
(open/write .../functions/midi.midi/strings/0x0409/interface)
usbg_add_config_function(c, NULL, f)  0x1f6410c
usbg_enable_gadget(g, udc)        0x1f64120
```

Teardown at `0x1f644fc..0x1f64520`: `usbg_disable_gadget`, `usbg_rm_config(c, 1)`,
`usbg_rm_gadget(g, 1)`, both recursive.

## 3. MPC's own UAC2 path (controller mode only)

The binary contains a second gadget, `smexstream`, with UAC2 + MIDI + mass storage + FunctionFS
("Remote Screen"), and a forwarder between the gadget's ALSA card and the codec:

```
/sys/kernel/config/usb_gadget/smexstream/functions/uac2.audio
.../uac2.audio/{function_name,if_ctrl_name,named_channels,shared_clock,req_number,
                c_hs_bint,p_hs_bint,fb_max,explicit_feedback}
failed to create UAC2 function / failed to add UAC2 function to config
UAC2Gadget                      (ALSA card id it looks for)
/dev/snd/by-path/platform-sound (the codec card it forwards to)
No audio device to forward
Playback Pitch 1000000 / Capture Pitch 1000000
update play rate: pitch=%ld += %d
gadgetPlayback write running=%d captureFrames=%lu soundAvail=%ld avail=%ld
soundCapture rewound by %ld / soundPlayback forwarded by %ld
classes: UsbGadgetAudioForwarding, UsbGadgetVolumeControl, UsbGadgetMidiForwarding,
         UsbGadgetBlockDetection, SmexControlService
setting: USBAudioIOConfiguration (MPC.settings value "24"); UI string "USB I/O Settings Changed"
```

(`strings MPC`, around string index 25828..25990.) Code references: `smexstream` at
`0x1f0d414`, `0x1f0f728`, `0x1f106b0`; `UAC2Gadget` at `0x1f0fe04`; "No audio device to forward" at
`0x1f0fe2c`; "Playback Pitch 1000000" at `0x1f11118`, `0x1f11454`, `0x1f11a24`.

The UI text says controller mode "will disable the standalone functionality". So the firmware can
already be a USB audio interface, but only as a front end for desktop software, with the standalone
engine off. The addin brings the same kind of gadget to standalone mode. Notes on that design:

- Some attribute names MPC writes (`named_channels`, `shared_clock`, `explicit_feedback`) are not in
  upstream f_uac2. They belong to the vendor variant that is off in this kernel, so those writes
  presumably fail there. The addin only writes upstream attributes.
- MPC's audio-device enumeration skips the gadget card: at `0x1f822ec..0x1f82368` it reads each
  card's longname (`snd_ctl_card_info_get_longname`), compares it against "UAC2_Gadget" (string ref
  at `0x1f82324`) and only adds non-matching cards to its device map. So a UAC2 gadget card that
  appears in standalone mode should not show up as an MPC audio device. **Inferred from the
  disassembly; DEVICE_TEST step 3 confirms it.**

## 4. MPC's audio path

```
$ cat /proc/asound/cards
 0 [M37 ]: USB-Audio - MPC Key 37        (internal control surface, MIDI only)
 1 [ACVR]: ACVR - ACVR                   (the codec; /dev/snd/by-path/platform-sound -> controlC1)
 2 [MPCKey37]: MIDI Gadget - f_midi
$ cat /proc/asound/card1/pcm0p/sub0/hw_params
access: RW_INTERLEAVED  format: S32_LE  channels: 2  rate: 44100  period_size: 128  buffer_size: 384
$ cat /proc/asound/card1/pcm0c/sub0/hw_params
access: RW_INTERLEAVED  format: S32_LE  channels: 2  rate: 44100  period_size: 128  buffer_size: 256
$ cat /proc/asound/card1/pcm0p/sub0/status    -> state: RUNNING, owner_pid = MPC
sw_params (playback): avail_min 128, start_threshold = boundary (started explicitly), stop_threshold 384
```

- `readelf -d MPC`: `NEEDED libasound.so.2` (dynamic, not dlopen'd), and `/proc/<pid>/maps` shows
  `/usr/lib/libasound.so.2.0.0`. No JACK and no PulseAudio. MPC imports `snd_pcm_writei`,
  `snd_pcm_readi`, `snd_pcm_writen`, `snd_pcm_readn`, `snd_pcm_link`, `snd_pcm_hw_params*`, and no
  `snd_pcm_mmap_*` symbols. So the audio is copied through `writei`/`readi` (RW_INTERLEAVED), not
  mmap.
- One period is 128 frames, 2.90 ms at 44.1 kHz. The playback buffer is 3 periods (8.7 ms).
- `isolcpus=2-3` is on the kernel command line. MPC's `AudioWorker0..3` and "Audio Processing"
  threads run SCHED_FIFO (rt priority in `/proc/<pid>/task/*/stat`), and MPC's main affinity mask is
  CPU 0. The DWC2 device IRQ (`irq/51-ff580000.usb`) is a threaded SCHED_FIFO IRQ pinned to CPU 1.
- Board: RK3288, 4 x Cortex-A17 at 1.6 GHz. Load during the study: ~88% idle, MPC ~7%.

## 5. Tap and inject points

All hooks are ordinary LD_PRELOAD interposition with `dlsym(RTLD_NEXT, ...)`. The existing preload
shims (`libforce_cursor.so`, `mpc_midi_inject.so`) already work this way in the same process.

| Hook | Thread | Purpose |
|---|---|---|
| `usbg_enable_gadget(g, udc)` | MPC setup thread | if `usbg_get_gadget_name(g)` is `standalone`, create `uac2.<inst>` with MPC's own libusbgx handle, write its attributes, `usbg_add_config_function` it into MPC's config, then call the real function. MPC's recursive teardown (section 2) removes it with the rest. |
| `snd_pcm_open` | setup | remember the PCM if it is on the codec card (`/dev/snd/by-path/platform-sound`, the same rule MPC's own forwarder uses) |
| `snd_pcm_hw_params` | setup | record format, channels, rate and access of the tapped PCMs |
| `snd_pcm_writei` / `writen` | **audio** | after the real call, copy the frames it accepted into a lock-free ring (main out to the computer) |
| `snd_pcm_readi` / `readn` | **audio** | after the real call, copy the inputs into a second ring (MPC inputs to the computer), then sum or replace computer playback into the buffer MPC gets back |
| `snd_pcm_close` | setup | forget the PCM |

The audio-thread work is a format conversion of 128 x 2 samples into or out of a single-producer,
single-consumer ring: no locks, no allocation, no syscalls. The gadget's ALSA PCMs are serviced by
the addin's own forwarder thread (SCHED_OTHER, all signals blocked, `O_CLOEXEC`/libasound defaults).
It is started lazily from the gadget hook, never from the constructor.

The ALSA card is used, not FunctionFS or raw endpoints. f_uac2 does the isochronous scheduling and
the feedback endpoint in the kernel, and exposes a plain ALSA card ("UAC2_Gadget") with one playback
PCM (to the computer) and one capture PCM (from the computer).

## 6. Clock and drift

There are two clocks: the codec's I2S clock, which paces MPC's `writei`/`readi`, and the USB
host's SOF clock, which paces f_uac2. They differ by tens to a few hundred ppm and wander with
temperature, so a fixed ring would slowly overflow or underflow (100 ppm is 4.4 frames/s, which uses
up a 256-frame margin in about a minute).

u_audio (the f_uac2 back end) exposes two ALSA controls on the gadget card:

- **"Playback Pitch 1000000"**: scales how many samples per USB interval the gadget sends to the
  computer (1000000 = nominal, 1 unit = 1 ppm).
- **"Capture Pitch 1000000"**: sets the value reported on the asynchronous feedback endpoint, so the
  computer sends faster or slower (`c_sync=async`, which is the default).

The range is roughly -25% / +`fb_max` per mille. That is far wider than any crystal error.

The addin measures the queued frames on each side (ring fill plus gadget PCM delay), smooths them,
and a PI controller steers the matching pitch control so the queue holds its target. Codec-side
timing is never touched, and nothing is resampled. MPC's own controller-mode forwarder uses this
method ("update play rate: pitch=%ld += %d", plus the rewind/forward strings for coarse
corrections), which is strong evidence that it works on this hardware. The controller is tested
offline against a simulated +/-300 ppm clock offset (`tests/test_drift.c`).

An asynchronous resampler would only be needed on a kernel without these controls (pre-5.15 for
capture feedback). Since MPC's own firmware depends on "Playback Pitch", any kernel that runs
controller-mode audio has them. No resampler is planned.

## 7. Latency and CPU budget

Added latency, MPC to computer (about the same in the other direction):

| Stage | Frames | ms @ 44.1 kHz |
|---|---|---|
| tap granularity (one MPC period) | 128 | 2.9 |
| ring target (absorbs SCHED_OTHER wake-up jitter; config `target_frames`) | 256 | 5.8 |
| gadget PCM buffer (period 64 x 4, about half full on average) | ~128 | 2.9 |
| USB interval (`hs_bint=4`, 1 ms) | ~44 | 1.0 |
| total device side | | **~8 to 12 ms** |

The computer's own driver buffer comes on top of this. UAC2 cannot report the extra device-side
delay, so a DAW recording the MPC needs a manual recording offset. That is fine for printing stems
and for tracking with monitoring on the MPC.

CPU:
- Audio thread: two conversion loops over 256 samples per 2.9 ms period. That is well under 1 us on
  an A17, a negligible share of the period.
- Forwarder thread: about 700 wakeups/s (64-frame gadget period), each a few ALSA ioctls. Estimated
  1 to 2% of one non-isolated core (CPU 0/1).
- USB interrupts: with `p_hs_bint=c_hs_bint=4` (1 ms), roughly 3 x 1000 isochronous completions/s
  on the threaded DWC2 IRQ (CPU 1). With the kernel default of 1 (125 us) it would be about
  3 x 8000/s, which is why the addin sets 4. To be measured (DEVICE_TEST step 5).
- Memory: three static rings of 256 KiB (MPC runs mlocked).

## 8. Channel layout

- The tap sees only what MPC writes to the codec PCM: 2 channels on this model. MPC's
  per-track/per-pad output routing beyond the physical outputs never reaches ALSA, so
  "individual outputs" means the outputs the hardware has (more on models whose codec PCM has more
  channels; the addin uses whatever `hw_params` reports).
- The default USB layout to the computer is 4 channels: main out L/R and MPC inputs L/R. From the
  computer it is 2 channels, summed into MPC inputs 1/2. Both are configurable (`to_host`,
  `to_mpc_in`, `input_mode = sum | replace | off`).
- On this model MPC runs at 44100 Hz, so the gadget advertises exactly one rate (config `rate`).
  If MPC opens the codec at another rate, the tap stays idle and logs it, and the computer just
  receives silence.

## 9. Risks and unknowns (to be settled on hardware)

1. **Bind order and FIFOs.** Adding a third and fourth IN endpoint to a DWC2 with fixed per-endpoint
   TX FIFO sizes. Controller mode proves UAC2 works on this UDC, but not this exact combination.
   DEVICE_TEST step 1 checks enumeration before the addin is involved.
2. **MPC's reaction to a new gadget ALSA card in standalone mode.** The longname filter (section 3)
   suggests it is ignored. Needs to be confirmed: MPC must keep the codec, and must not open the
   gadget PCMs itself (`/proc/asound/cardN/pcm*/sub0/status` owner_pid).
3. **Composite descriptors on Windows.** UAC2 uses an IAD, so the addin sets the device class to
   EF/02/01 (`iad_class=1`). Windows may have cached the MIDI-only descriptor set for 09e8:0057 and
   may need the device removed once in Device Manager. macOS and Linux should simply re-enumerate.
4. **Forwarder at SCHED_OTHER.** The house rule keeps addin threads non-realtime. The 256-frame
   ring target is sized for that. If the USB side underruns under heavy UI load, the trade-off is a
   larger `target_frames` (more latency), never a glitch on MPC's own outputs (MPC's thread never
   waits on the forwarder).
5. **Drive mode and controller mode.** MPC may rebuild `standalone` with other functions (USB drive
   mode), and controller mode uses `smexstream`. The addin only adds to a gadget named `standalone`,
   and the forwarder only runs while its own `uac2.<inst>` function exists. Both modes must be
   exercised once.
6. **Firmware versions.** Section 2 offsets are for this build. The addin uses symbol hooks only
   (libusbgx and libasound exports), no offsets. It needs only the call order "create gadget, then
   `usbg_enable_gadget`", which any libusbgx user has.
