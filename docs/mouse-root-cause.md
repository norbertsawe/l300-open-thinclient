# Tempest mouse investigation and the next RAM-only test

> **Historical development note**
>
> This document preserves paths and commands from the original development
> workspace for reproducibility and engineering reference. Some referenced
> directories and intermediate artifacts are not included in this public
> repository. See the repository README for the current public layout.


## Result so far

The evidence places the fault below FreeRDP. The vendor module constructs
the extreme X/Y values and wheel events before they reach evdev. The evdev
ABI is correct, and the RDP negative-wheel encoding is correct.

**The exact mouse report format is still missing. There is no justified mouse
decoding fix yet.** Applying a byte shift, sensitivity divisor, or wheel
suppression would be a guess. The new image captures the missing USB bytes
and their decoded results directly at the vendor report decoder.

No NAND operation, permanent boot change or Windows setting change was made.
The original TFTP files and the hardware-tested framebuffer color function
are unchanged. This document replaces the earlier flash-install workflow for
this experiment: use RAM boot only.

## What was found below FreeRDP

No Tempest source was found in either firmware checkout, the extracted rootfs,
or the available kernel source tree. The ELDK tree is not the complete vendor
BSP. However, `tempest_usb.ko` is unstripped and contains useful DWARF debug
information. It identifies the missing source as
`drivers/tempest/usb/tempest_usb.c`, with `store_mouse_event()` starting around
source line 446. The module's vermagic is `2.6.19.2-dirty mod_unload ARMv5`.

The recovered path is:

```text
Tempest controller receive FIFO
  → per-port recv_buffer and recv_length
  → tasklet_procedure0 / tasklet_procedure1
  → store_mouse_event(port, recv_buffer)
  → input_event(EV_REL / EV_KEY / EV_SYN)
  → evdev
  → fbfreerdp
```

The driver receives into a shared 64-byte buffer at port offset 40. Length
is a 32-bit field at offset 104. The tasklets pass that buffer directly to
`store_mouse_event`. Its decoding branches are selected **only by controller
receive length**. These lengths are not assumed to equal HID payload length.

Let `b[n]` be the raw receive-buffer byte at index n; `sN` means signed N-bit
interpretation. The actual ARM instructions implement:

| Receive length | X | Y | Wheel | Buttons |
| --- | --- | --- | --- | --- |
| 8 | `s8(b[2])` | `s8(b[3])` | `s8(b[4])` | `b[1] & 7` |
| 10 | `s12(b[2] \| ((b[3]&15)<<8))` | `s12((b[3]>>4) \| (b[4]<<4))` | `s8(b[5])` | `b[1] & 7` |
| 12 | `s16(b[3] \| (b[4]<<8))` | `s16(b[5] \| (b[6]<<8))` | `s8(b[7])` | `b[2] & 7` |

The corresponding decoder branches start at `.text+0xd18`, `0xd60` and
`0xdc0`. Calls at `0xe28`, `0xe3c`, and `0xe50` submit X, Y, and wheel.
The next three calls submit left/right/middle state, and the final call
submits SYN_REPORT. Sign extension in the recognized branches is consistent
with these formulas; the large values are not introduced by FreeRDP.

The function has no HID descriptor parser. The static USB setup packet named
`setup_mousecfg_pkt` is SET_CONFIGURATION (request 9), not a request that
establishes a generic mouse report layout. Consequently, packet length alone
cannot establish the correct format for an arbitrary mouse.

Another defect is visible: unrecognized lengths fall through without fully
initializing decoded axis registers. This is documented, but not changed as
an unrelated speculative fix; the next capture will identify the active branch.

The left-button observation also remains ambiguous. The driver takes its
button bits from b[1] or b[2], depending on the branch. A misplaced report ID
could resemble a held button, but an actual held button could produce the
same evdev state. The current information does not distinguish them.

## Why the supplied numbers do not uniquely identify a repair

I linked the **original module's actual ARM decoder and tasklet instructions**
into an isolated userspace harness. QEMU executes only those report-processing
functions; hardware initialization, interrupts and module entry points are
never called. Unexpected kernel calls terminate the harness.

For example, these two deliberately synthesized receive buffers both produce
`x=1537, y=0, wheel=-1, left=down` in that original decoder:

```text
length 10: c3 01 01 06 00 ff 00 00 00 00
length 12: c3 00 01 01 06 00 00 ff 00 00 00 00
```

Those are demonstrations of ambiguity, **not claims that either buffer was
captured from your mouse**. All eight requested values, including `0x0601`,
`0xfffffa01`, and `0xfffff901`, can be reproduced through either the length-10
or length-12 branch with different underlying bytes. It would be incorrect
to assign them an expected physical X/Y meaning without the original reports.

The strongest current explanation is a mismatch between the mouse's report
layout and the Tempest driver's length-based assumptions. A shared receive
buffer race is another possibility worth checking. The next probe snapshots
the bytes and flags changes noticed during decoding; `race=0` cannot rule out
every possible race, but `race=1` is useful evidence.

## ABI verification

The original vendor module's DWARF and the cross-compiled ARM harness agree:

| Item | Vendor debug information | Executed ARM userspace |
| --- | ---: | ---: |
| `sizeof(long)` | 4 | 4 |
| `sizeof(struct timeval)` | 8 | 8 |
| `sizeof(struct input_event)` | 16 | 16 |
| `offsetof(type)` / field size | 8 / 2 | 8 / 2 |
| `offsetof(code)` / field size | 10 / 2 | 10 / 2 |
| `offsetof(value)` / field size | 12 / 4 | 12 / 4 |

`value` is signed 32-bit in both. The frontend now has a regression check for
these field offsets and sizes, in addition to its existing size check.

## Exact changes in this iteration

| File | Change and purpose |
| --- | --- |
| `FreeRDP-old-master/l300/fbfreerdp.c` | Adds `<stddef.h>` and one ABI regression check; no input/color behavior changes |
| `NCOMPUTING_L300_SPEAR600/appliance/tests/test_appliance.py` | Makes the test certificate pin synthetic even when the developer's candidate is provisioned |
| `rootcause-evidence/tempest_observer.c` | Records raw USB bytes and the actual arguments passed to input_event; forwards every call unchanged |
| `rootcause-evidence/build_observer.py` | Appends the observer to a copy of the exact vendor module and redirects nine checked call relocations |
| `rootcause-evidence/vendor_decoder_harness.c` | Executes the original decoder/tasklet with synthetic reports under ARM926 QEMU |
| `rootcause-evidence/test_vendor_decoder.py` | Tests captured-number reproduction, observation equivalence, wheel/buttons and logging limits |
| `build_mouse_usb.py` | Builds a separate image from MOUSEOBSERVE_v1 without generating a FWD or flash plan |
| `analyze_usb_reports.py` | Parses complete report lines and compares them with the recovered driver formulas |

The experimental module retains all original `.text` instructions and module
metadata. Two tasklet call relocations lead through a report observer; seven
input_event call relocations in the mouse decoder lead through a pass-through
observer. Keyboard call sites are untouched. The linked module needs no new
kernel imports. This is a diagnostic addition to a copied binary, not a claim
that the missing vendor driver source has been rebuilt.

Only four payloads change in the new initramfs:

1. `/lib/modules/tempest_usb.ko`: the observation copy of the vendor module.
2. `/usr/bin/fbfreerdp`: the ABI-tested client.
3. `/etc/rc.sh`: enables notice-level kernel console output and reports probe
   load success/failure on serial.
4. `/usr/lib/thinclient/supervisor`: keeps frontend diagnostics in its RAM log
   but stops mirroring its verbose per-event lines to serial.

All 521 other entries and existing entry metadata are preserved. No change
is made to the working server address, pin, MAC, keyboard, GDI, or color fix.

The observer prints **one line per report, at most 80 report lines per boot**.
Each line contains the raw bytes and all decoded values from the same call.
Kernel printk provides the serial output instead of the previous partial
nonblocking user-space writes. Other kernel notices may appear; keep the
`TMPUSB` lines. Kernel console logging can affect timing, so this image is for
report diagnosis, not a performance benchmark.

## Test evidence

- 22 frontend self-tests pass: all previous 21 plus the exact input ABI check.
- 51 original FreeRDP unit tests / 198 assertions and static dependency checks pass.
- Local TLS 1.2 xrdp login rendering, wrong/missing pin rejection and timeout tests pass.
- Optional Dropbear public-key authentication and password rejection tests pass.
- Five image regression checks pass.
- The appliance suite initially exposed a test-fixture dependency on an empty
  candidate pin. Only the fixture was corrected; the 11-case suite was rerun.
- 192 vendor-decoder/observer cases pass under ARM926 emulation, including
  all eight supplied integer examples, randomized reports for all three
  recognized lengths, wheel directions and all three button bits.
- A 100-report test confirms the 80-line cap while all 700 input_event calls
  still reach the receiver.

The new module has **not been loaded into the physical kernel yet**. ARM
userspace tests establish decoder equivalence, not kernel-load compatibility.
No corrected physical mouse behavior is claimed. Logs and disassembly are in
`rootcause-evidence/`; image hashes and exact sizes are in `mouse-usb-v2/`.

## Next physical test — no source inspection needed

### 1. Stage the new file on CachyOS

The file is:

```text
l300-mouse-diagnostics/mouse-usb-v2/l300_initramfs_MOUSEUSB_v2.gz
```

Size: **4,756,445 bytes**, hexadecimal **0x4893dd**.
SHA256: `389e45ace4d5e5c038ca64d3c1dcd16ff6d4d470850a3c55ebb719e2156f1c73`.

Copy to a new TFTP filename if it has not already been staged:

```sh
cd <workspace>
sudo cp --no-clobber \
  l300-mouse-diagnostics/mouse-usb-v2/l300_initramfs_MOUSEUSB_v2.gz \
  /srv/tftp/l300_initramfs_MOUSEUSB_v2.gz
sha256sum /srv/tftp/l300_initramfs_MOUSEUSB_v2.gz
```

Stop if a preexisting file has a different hash. Do not overwrite any previous
test image. Keep the already working `uImage` in the TFTP directory.

### 2. RAM-boot at the L300 serial U-Boot prompt

Start a serial log on the host first, at the existing 115200 8N1 settings.
Keep the mouse attached but untouched. Interrupt autoboot and run these lines
one at a time; stop on transfer errors or unexpected CRCs:

```text
setenv serverip 192.168.1.10
setenv ipaddr 192.168.1.200
setenv ethaddr 02:0e:ea:0b:3f:3b
tftpboot 0x800000 uImage
crc32 0x800000 0x1913e0
tftpboot 0x1000000 l300_initramfs_MOUSEUSB_v2.gz
crc32 0x1000000 0x4893dd
setenv bootargs console=ttyS0,115200 mem=108M initrd=0x1000000,0x4893dd
bootm 0x800000
```

Expected kernel CRC32: **a2ee241a**. Expected initramfs CRC32: **749d29de**.
These RAM addresses and arguments follow your already working board sequence.
The same commands are in [`mouse-usb-v2/RAM-BOOT.txt`](mouse-usb-v2/RAM-BOOT.txt).

### 3. Check load status

Expect:

```text
TMPUSB PROBE LOADED - RAM-only, maximum 80 report lines
```

If you see `PROBE LOAD FAILED`, a kernel error, or lose previously working
input, stop. Preserve that serial log and RAM-boot the known-good COLORFIX or
MOUSEOBSERVE_v1 image using its own size. A power cycle leaves the experiment
and returns to the firmware already in NAND; if your working session also
required RAM boot, load that working image again.

### 4. Make small, separate actions

The mouse is expected to remain faulty during this diagnostic. Do not test
large sweeps or change Windows sensitivity.

1. Leave it untouched for five seconds. Note any unsolicited scrolling.
2. Move it a few millimetres right, then stop; then left, then stop.
3. Move it a few millimetres down, then stop; then up, then stop.
4. Without moving the mouse, turn the wheel one notch up and one notch down.
5. Press and release left, right and middle separately, without moving it.

Write down where each action starts in the serial capture. If `TMPUSB LIMIT`
appears, RAM-reboot and capture the remaining actions in a new log. Restarting
FreeRDP alone does not reset the kernel probe's counter. A short separate
RAM-boot capture for motion, wheel and buttons is preferable to one long log.

### 5. Send back exactly this

- The probe load message and any kernel/module error.
- The complete `TMPUSB n=...` lines from the short captures, with actions labeled
  `idle`, `right`, `left`, `down`, `up`, `wheel up/down`, and `button press/release`.
- Whether you touched/clicked anything before the first report.
- Whether `race=1` or `TMPUSB LIMIT` appeared.
- Mouse make/model if known, and whether RDP, keyboard and colors still worked.

No Windows terminal commands are needed for this capture. No ordinary keyboard
keys or passwords are included in the probe records.

Optional analysis on CachyOS:

```sh
python3 l300-mouse-diagnostics/analyze_usb_reports.py /path/to/serial.log
```

`MATCH` in this report means the observed result agrees with the vendor parser;
it does not mean that parser chose the right format for the mouse. The original
bytes and labeled physical actions are what the next fix must be based on.

## Reproduction and backups

`rootcause-baseline-20260928-164909/manifest.json` records the snapshot taken
before this iteration, including all TFTP files, the working config, source,
binary and original module. No source tree was reset.

The original FreeRDP tree is not a Git repository. Exact changes are supplied
as `rootcause-changes.patch`; the probe's relocation changes are recorded in
`rootcause-evidence/module-observation-manifest.json`.

Rebuild only on the development machine, into a fresh directory:

```sh
cd FreeRDP-old-master
python3 l300/build.py
cd <workspace>
python3 l300-mouse-diagnostics/rootcause-evidence/build_observer.py
python3 l300-mouse-diagnostics/rootcause-evidence/test_vendor_decoder.py
python3 l300-mouse-diagnostics/build_mouse_usb.py \
  --output l300-mouse-diagnostics/mouse-usb-repeat
```

Use the regenerated manifest and RAM-BOOT file for a rebuilt image. Do not
assume this document's byte count applies after further source changes.
