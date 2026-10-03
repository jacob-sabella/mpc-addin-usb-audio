# On-device test plan (proposed, needs approval)

Nothing here has been run. Every step below changes device state, so each one needs the owner's
go-ahead first. The steps are ordered so each risk from `docs/FEASIBILITY.md` section 9 is settled
before the next step depends on it. Steps 0 and 3 only read.

Conventions:

- `$SVC` is MPC's systemd unit: `acvs` on stock firmware, `inmusic-mpc` on some third-party
  firmware. Find it with `systemctl cat acvs >/dev/null 2>&1 && SVC=acvs || SVC=inmusic-mpc`.
- `G=/sys/kernel/config/usb_gadget/standalone`
- Files go up as `x.new` and are then `mv`'d into place.
- "Host" means the computer on the other end of the USB cable.

## 0. Baseline (read-only)

```sh
# device
cat $G/UDC $G/bDeviceClass; ls $G/functions $G/configs/config.1
cat /proc/asound/cards
systemctl show $SVC -p Environment -p DropInPaths -p FragmentPath
tr '\0' '\n' < /proc/$(pidof MPC)/environ | grep LD_PRELOAD
# host (Linux)
lsusb -d 09e8: -v | grep -E 'idProduct|bDeviceClass|bInterfaceClass|bInterfaceSubClass'
aplay -l; arecord -l
```

Keep the output. It is what every revert below gets checked against.

## 1. Enumeration with a hand-made UAC2 function (MPC keeps running)

This checks risk 1 (DWC2 endpoints and FIFOs) and the host's view of the composite device, with no
addin involved. It touches MPC's live gadget: while the UDC is unbound, the computer loses the
MPC's USB MIDI port for about a second. MPC's own MIDI gadget card on the device is rebuilt on
rebind, and MPC may not reopen it, so plan on step 1e (an MPC restart) to get USB MIDI back.

1a. Add the function:

```sh
G=/sys/kernel/config/usb_gadget/standalone
UDC=$(cat $G/UDC); echo "$UDC" > /tmp/uac-test-udc
echo "" > $G/UDC
mkdir $G/functions/uac2.test
F=$G/functions/uac2.test
echo 0xf   > $F/p_chmask ; echo 44100 > $F/p_srate ; echo 4 > $F/p_ssize
echo 0x3   > $F/c_chmask ; echo 44100 > $F/c_srate ; echo 4 > $F/c_ssize
echo async > $F/c_sync
echo 4 > $F/p_hs_bint ; echo 4 > $F/c_hs_bint          # skip if the files are absent
for a in p_mute_present p_volume_present c_mute_present c_volume_present; do echo 0 > $F/$a; done
echo "MPC USB Audio" > $F/function_name                 # skip if absent
echo 0xEF > $G/bDeviceClass; echo 0x02 > $G/bDeviceSubClass; echo 0x01 > $G/bDeviceProtocol
ln -s $F $G/configs/config.1/uac2.test
echo "$UDC" > $G/UDC; echo "bind: $?"
```

1b. Check, device side:

```sh
cat $G/UDC; cat /sys/class/udc/*/state /sys/class/udc/*/current_speed
dmesg | tail -n 20          # BusyBox: dmesg | tail
cat /proc/asound/cards      # a new "UAC2_Gadget" card
```

1c. Check, host side:

```sh
lsusb -d 09e8: -v | grep -E 'bDeviceClass|bInterfaceClass|bInterfaceSubClass|wMaxPacketSize|bInterval'
arecord -l; aplay -l        # the MPC's card now has a capture device (4 ch) and a playback device (2 ch)
arecord -D hw:CARD=<card>,DEV=0 -c 4 -f S32_LE -r 44100 -d 5 /tmp/x.wav   # silence expected
aplay -D hw:CARD=<card>,DEV=0 -c 2 -f S32_LE -r 44100 -d 5 /dev/zero
amidi -l                    # MIDI ports still listed
```

On macOS: Audio MIDI Setup shows a 4-in / 2-out device. On Windows 10/11: Sound settings show it.
If Windows shows only MIDI or a yellow mark, uninstall the device once in Device Manager (it may
have cached the MIDI-only descriptors) and reconnect.

1d. Check, MPC side (risk 2): MPC still plays audio and its audio settings still use the built-in
codec. `cat /proc/asound/card<gadget>/pcm0p/sub0/status` and `pcm0c` should say `closed`, so MPC did
not open the gadget PCMs.

1e. Revert, either way:

```sh
# quick: an MPC restart rebuilds "standalone" from scratch (MIDI only), removing the test function
systemctl restart $SVC
# or by hand, without restarting MPC:
echo "" > $G/UDC
rm $G/configs/config.1/uac2.test
rmdir $G/functions/uac2.test
echo 0x00 > $G/bDeviceClass; echo 0x00 > $G/bDeviceSubClass; echo 0x00 > $G/bDeviceProtocol
echo "$(cat /tmp/uac-test-udc)" > $G/UDC
```

Then rerun step 0 and compare.

Stop here if the bind fails or the host does not enumerate audio. Try once more with
`p_hs_bint=1`/`c_hs_bint=1`, and with `c_chmask=0` (playback to the computer only). Record what
happened in the notes. The addin cannot work around a UDC that will not bind the function.

### Step 1 result (2026-10-02, MPC Key 37, firmware 3.9.1.2, kernel 6.18 PREEMPT_RT, Linux host)

- Every attribute in 1a exists on this kernel (no skips). The bind succeeded, the UDC came back `configured`,
  `high-speed`, and a `UAC2Gadget` card appeared on the device.
- The host enumerated a composite device: class EF/02/01, 5 interfaces (MIDI streaming, audio control, two audio
  streaming), `wMaxPacketSize` 720 bytes (4 ch x 4 B x 45 frames) out and 360 bytes (2 ch) in, `bInterval` 4.
  The MIDI ports were still listed.
- `arecord` 4 ch and `aplay` 2 ch, S32_LE 44.1 kHz, 5 s each: no errors, exactly 220500 frames recorded.
- End to end: a 1 kHz / 500 Hz tone played into `hw:CARD=UAC2Gadget` on the device (a small dlopen-libasound
  player; the device has no `aplay`) was recorded on the host at the level and frequencies sent, on all 4 channels.
- MPC did not open the gadget PCMs (`closed`) and kept playing through the codec (ACVR `RUNNING`).
- Reverted by hand (1e, no MPC restart): the gadget, the cards and the host's view matched step 0. As warned
  above, MPC's sequencer ports for `f_midi-0/1` lost their connections at the rebind, so USB MIDI to and from the
  computer stays down until MPC restarts. The addin is designed to avoid this (it adds the function inside MPC's own
  first enable, so there is no rebind); step 2 checks that.

## 2. Install the addin (one MPC restart)

Build on the workstation: `tools/build_armhf.sh`, which gives `build/armhf/libmpc_usb_audio.so`.

2a. Copy the package (MPC keeps running; nothing loads it yet):

```sh
# workstation
scp -r build/package root@<device>:/data/mpc-addins-usb-audio-pkg
```

For the first boot, set `test_tone=1` in the package's `usbaudio.conf` before installing:
the computer then records a 1 kHz tone, which proves the USB path without MPC's audio.

2b. Install, without restarting MPC yet:

```sh
# device
cd /data/mpc-addins-usb-audio-pkg && sh install.sh -n
systemctl show $SVC -p Environment -p DropInPaths
```

The installer (mpc-addin-installer) copies the files into `/data/mpc-addins/usb-audio/` and adds the library
to `LD_PRELOAD`. The unit sits on the read-only root, so the drop-in
`/etc/systemd/system/$SVC.service.d/90-mpc-addins.conf` sets it: the unit's list (recorded as `# base:`), then
the addins. Check that the printed `LD_PRELOAD` holds everything step 0 printed, with the addin last. `/etc`
is an overlay backed by `/data`, so the drop-in survives a reboot. After a firmware update, re-run
`install.sh`: it picks up the unit's new list.

2c. Restart MPC (approval needed): `systemctl restart $SVC`.

2d. Check:

```sh
cat /data/mpc-addins/usb-audio/usbaudio.log
#   expect: active ... / tapping playback PCM ... / playback: 2 ch, format 10, 44100 Hz /
#           uac2.usbaudio added to gadget 'standalone' / forwarder started / session on hw:N,0 ...
#           and every 10 s: tapped: main out <calls> <frames> fr peak <dBFS> (MPC's output as tapped)
ls $G/functions; ls $G/configs/config.1; cat $G/UDC
grep -c libmpc_usb_audio /proc/$(pidof MPC)/maps
for t in /proc/$(pidof MPC)/task/*; do grep -q usbaudio $t/comm && awk '{print "policy", $41, "rtprio", $40}' $t/stat; done
#   expect: policy 0 rtprio 0 (SCHED_OTHER)
```

Host: record the tone (`arecord -D hw:CARD=<card>,DEV=0 -c 4 -f S32_LE -r 44100 -d 10 tone.wav`)
and check it is clean. Then set `test_tone=0`, restart MPC again, and record MPC's main out while
playing a sequence. Channels 1-2 are the main out and 3-4 are MPC's inputs.

2e. Revert:

```sh
cd /data/mpc-addins-usb-audio-pkg && sh uninstall.sh   # takes only this addin out of LD_PRELOAD, restarts MPC
                                                       # (it rebuilds the MIDI-only gadget), deletes its folder
```

Emergency off switch without touching systemd: put `enabled=0` in `usbaudio.conf` and restart MPC.
The library still loads, but every hook is a pass-through.

## 3. MPC keeps the codec (read-only, with the addin running)

```sh
for s in /proc/asound/card*/pcm*/sub0/status; do echo "$s"; grep -E 'state|owner_pid' "$s"; done
```

The codec PCMs are owned by MPC's pid. The gadget card's PCMs are owned by the same pid too, but
opened by the addin's forwarder; check that the MPC audio preferences still show the built-in
interface. Also check MPC's audio-device menu does not list "UAC2_Gadget".

## 4. Computer to MPC, and drift (no device changes)

- Play audio from the computer into the MPC playback device. With `input_mode=sum` it is heard
  wherever MPC routes its inputs (input monitoring on a track, or sampling). Check sampling records
  it.
- Soak test: record for 30 minutes on the computer while MPC plays and the computer plays. Then
  `grep 'to computer' /data/mpc-addins/usb-audio/usbaudio.log`. The pitch values should settle near
  1000000 (typically within a few hundred ppm) and stay put, and the overflow/underrun counters should not grow
  after the first minute. A pitch pinned at `max_ppm` means the drift direction is wrong for that
  side. That would be a code fix (the sign in `fwd.c`), not a setting.
- Repeat the record and playback check on macOS and Windows.

## 5. Load (no device changes)

```sh
grep ff580000 /proc/interrupts; sleep 10; grep ff580000 /proc/interrupts   # IRQ rate with audio
top -b -n 1 | head -n 15        # BusyBox top: top -b -n1
```

Compare with step 0 (MIDI only). Expect a few thousand more IRQs per second at `hs_bint=4` (each
isochronous endpoint completes once per millisecond), and well under 1% CPU for the forwarder. Then load MPC
heavily (many plugins or tracks) and check the log for overflow and underrun counters.

## 6. Other USB modes (approval needed: switching modes)

- Controller (computer) mode: MPC switches to its own `smexstream` gadget. The log should say
  `gadget 'smexstream' enabled: not ours, left alone`, and MPC's own USB audio must work as before.
- USB drive mode: check what gadget MPC builds (`ls $G/functions`). If the addin's function is
  present, check the drive still mounts on the computer.
- Coming back to standalone mode, the addin's function reappears (log: `uac2.usbaudio added`).

Record every result, with the date, in the project notes.

## Results: step 2 (2026-10-02, MPC Key 37, firmware 3.9.1.2)

Installed with `sh install.sh -n` from the package, then MPC restarted. **Passed.**

- The unit (`/usr/lib/systemd/system/acvs.service`) is on the read-only root. The installer wrote
  `90-mpc-addins.conf`, which holds the unit's two libraries and then the addin, and `systemctl show`
  gave that list.
- The log showed: settings read from `/data/mpc-addins/usb-audio/usbaudio.conf`; `uac2.usbaudio` added to gadget
  'standalone'; both codec PCMs tapped (2 ch, S32_LE, 44.1 kHz, interleaved); a session on the gadget card.
  The first time MPC started, the session came up 5 s in; another time, 16 s in, because MPC opened its
  audio later.
- The gadget has `midi.midi` and `uac2.usbaudio`, on the same UDC.
- **No rebind:** `/proc/asound/seq/clients` matched the copy taken before the restart: `f_midi` was still
  connected to MPC's client both ways, on both ports. USB MIDI kept working after every restart.
- The forwarder thread `usbaudio-fwd` is SCHED_OTHER (policy 0, rtprio 0).
- On the computer, the device enumerated as USB audio (4 ch in, 2 out).
- `test_tone=1`: recorded 10 s, 4 ch at 44.1 kHz. Every channel held a 1 kHz tone at rms 0.177, with no
  energy outside the peak and no sample steps above the tone's own (no dropouts).
- `test_tone=0`, after a restart: notes were sent into the active drum track (a seq client playing notes
  36-67) while the computer recorded. Channels 1-2 (main out) peaked at -11.0 / -10.3 dBFS, the same as
  the addin's own `tapped:` line (-11.0 dBFS). Channels 3-4 (MPC's inputs, nothing plugged in) were at the
  noise floor, about -98 dBFS. No overflows or underruns. The tap saw about 345 calls/s on each stream,
  i.e. 44,100 frames/s at period 128.
- The first `test_tone=0` recording was silent: the notes then (48-72) mostly landed on empty pads, and the
  log at that point had no tap counters to tell. Hence the `tapped:` line.

The addin is left installed on the device. Remove it with `sh uninstall.sh` from the package folder.
