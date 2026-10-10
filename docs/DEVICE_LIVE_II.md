# MPC Live II: test results (no USB gadget in standalone mode)

Tested 2026-10-09 on an MPC Live II running Hakai firmware (kernel
`6.18.26-az01-2026-04-30-rt4`, launcher `az01-launch-MPC`, unit `acvs`). This is not the
stock 3.9.1.2 firmware the Key 37 results were taken on. Result: **the addin loads, but there is
nothing to attach to, and no USB audio reaches the computer in standalone mode.**

Everything below is what was observed. The explanation at the end is an inference from those
observations, not something verified in hardware documentation.

## Install issues seen on this firmware

1. **The drop-in was not applied after a reboot.** `install.sh` wrote
   `/etc/systemd/system/acvs.service.d/90-mpc-addins.conf` and MPC loaded the addin once
   (log pid 716). After a reboot, `systemctl show acvs -p DropInPaths -p Environment` returned
   empty values and the addin was not in MPC's `LD_PRELOAD`. `systemctl daemon-reload` followed by
   `systemctl restart acvs` loaded it. Possible cause: systemd read its units before the `/etc`
   overlay was mounted (not confirmed).
2. **`# base:` is empty** in the drop-in because the unit has no `LD_PRELOAD` of its own. The
   Hakai launcher (`/usr/bin/az01-launch-MPC`) appends with
   `LD_PRELOAD="$LD_PRELOAD /usr/lib/hakai_driver.so ..."`, so all three libraries ended up in
   MPC's environment, with the addin first. That part works.
3. **`codec card -1`.** The log line is
   `active: gadget 'standalone', 4 ch to computer, 2 from it, 44100 Hz, codec card -1`.
   `/dev/snd/by-path/` has no `platform-sound` entry. The Live II's audio is a USB audio device:

   ```
    0 [Audio ]: USB-Audio - MPC Live II Audio   (usb-ff500000.usb-1.5, high speed)
    1 [II    ]: USB-Audio - MPC Live II         (usb-ff500000.usb-1.6, full speed)
   ```

   So `tap_card=auto` cannot find a codec here. `tap_card=0` was not tried.

## No gadget in standalone mode

With MPC in standalone mode and a USB cable in the computer port:

- `ls /sys/kernel/config/usb_gadget/` is empty.
- `/sys/class/udc/ff580000.usb/state` is `not attached`.
- `usbaudio.log` never reaches the "uac2.usbaudio added to gadget" line.
- `strings /usr/bin/MPC | grep -iE 'usbg|gadget|configfs'` finds only `|UsbG-BR`, even though
  `libusbgx.so.3.0.0` is mapped into the MPC process.
- `dmesg`: `dwc2 ff580000.usb: DWC OTG Controller` registers a USB host bus (`usb1`), and no gadget
  is bound.

## Hand-made gadget test (as in DEVICE_TEST.md step 1)

A UAC2 gadget (VID/PID `1d6b:0104`, `p_chmask=0xf`, `c_chmask=0x3`, 44100 Hz, S32) was created in
configfs and bound to `ff580000.usb`:

- The bind succeeded (`bind: 0`, `dwc2: bound driver configfs-gadget.uactest`).
- The UDC state stayed `not attached`.
- The MPC's own USB host enumerated the gadget on its internal hub:
  `usb 2-1.7: New USB device found, idVendor=1d6b, idProduct=0104 ... Product: MPC UAC2 test`.
- Windows showed no new device (nothing new in Device Manager or Sound settings).

## Standalone vs. controller mode (`diff` of a snapshot taken in each mode)

```
 == udc
-not attached
+configured
 == gadgets
+smexstream
 == usb devices
-2-1: 09e8:5047 MPC Live Mk 2 Hub
-2-1.2: 0781:55a9  SanDisk 3.2Gen1
-2-1.3: 09e8:703b Card Reader
-2-1.4: 152d:0578 USB to ATA/ATAPI Bridge
-2-1.5: 09e8:2047 MPC Live II Audio
-2-1.6: 09e8:0047 MPC Live II
 == cards
- 0 [Audio ]: USB-Audio - MPC Live II Audio
- 1 [II    ]: USB-Audio - MPC Live II
+--- no soundcards ---
```

Relevant `dmesg` lines from the switch: the internal devices disconnect from the MPC's bus,
`usb5537 1-002d: switched to HUB mode` and `usb5537_probe: probed in hub mode`, register writes follow, and then
`dwc2 ff580000.usb: bound driver configfs-gadget.smexstream`. In controller mode, audio works on
the computer.

## Interpretation (unverified)

The internal hub (`09e8:5047 "MPC Live Mk 2 Hub"`, with a `usb5537` driver) appears to be switched
between two roles. In standalone mode the MPC's CPU is its host: the audio chip, the controller
and the drives hang off it, and the `dwc2` device controller's gadget appears on that hub rather
than on the computer port. In controller mode the computer becomes its host, the MPC loses its
sound cards, and `smexstream` binds to the computer. If so, the Live II cannot present a USB
audio gadget to a computer while also keeping its own audio hardware in standalone mode, and this
addin has nothing to attach to on this model.

Not yet checked: in controller mode on a Windows computer, whether Device Manager
(View > Devices by connection) shows `MPC Live II Audio` under `MPC Live Mk 2 Hub`, which
would support this reading.

## Suggested README note

> Tested on the MPC Key 37 (stock firmware 3.9.1.2). On the MPC Live II (Hakai firmware) MPC does
> not create a USB gadget in standalone mode and the addin has no effect: see
> `docs/DEVICE_LIVE_II.md`.
