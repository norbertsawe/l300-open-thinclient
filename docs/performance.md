# L300 PERFORMANCE v1 — engineering investigation

> **Historical development note**
>
> This document preserves paths and commands from the original development
> workspace for reproducibility and engineering reference. Some referenced
> directories and intermediate artifacts are not included in this public
> repository. See the repository README for the current public layout.


Status: host/QEMU verified; physical board disconnected throughout this work.
No hardware FPS, latency, framebuffer bandwidth, or Windows-session performance
has been measured for this candidate. The reported approximately 2 FPS cannot be
assigned a measured percentage to each cause without the board. The following
implementation defects and redundant work were established and corrected.

## Findings and complete runtime path

1. **TLS receive dispatch can strand decrypted packets.** TCP is nonblocking.
   The frontend waits in select(), then l_rdp_check_fds() gates dispatch solely
   on tcp_can_recv(fd,0). SSL_read() can consume an entire TLS record while
   returning only the bytes requested by network_recv(). A second RDP packet in
   that record remains in SSL_pending(), with no readable kernel socket. The
   old check refuses to dispatch it even after the idle timeout expires; new
   wire traffic is needed to unstick it. Our actual ARM TLS/core test sends two
   Fast-Path Synchronize PDUs in one TLS 1.2 record and then sends nothing else.
   It proves socket-not-ready plus SSL_pending()==5 after the first PDU. The new
   readiness helper makes select nonblocking only while decrypted bytes exist,
   and the core dispatches the second PDU immediately. No busy polling at idle.
2. **Full-desktop per-pixel overhead.** Protocol updates pass through
   process_bitmap_updates()/bitmap_decompress() or drawing orders to software
   GDI's RGB565 backing store. At ui_end_update(), present() previously ran
   cursor intersection checks and a variable-width store_pixel() function call
   for each desktop pixel. Saved ARM disassembly confirms the per-pixel BL.
   At 1024x768 this means 786,432 function calls and up to 2,359,296 byte stores
   for one full redraw, plus cursor branching. RGB565 lookup conversion already
   existed; it was retained, not invented anew. A specialized RGB24 row loop
   packs four pixels into three aligned 32-bit stores (the compiler emits
   STMIA), using byte stores at unaligned edges. It does no unaligned ARM word
   access, floating point, NEON, or architecture upgrade. RGB16/32 fallback is
   retained. Cursor alpha work is restricted to its clipped intersection.
3. **Distant updates create unnecessary full-screen writes.** The old GDI
   invalid region was one bounding box. Two 20x20 rectangles at opposite corners
   caused all 786,432 pixels to be presented. The new L300-only 64-entry region
   list coalesces only when union area is no larger than the two areas combined.
   Excess fragmentation falls back to the original bounding box, preserving
   correctness and bounding memory. The old public invalid-region semantics are
   unchanged (all CUnit tests pass). The exact sparse case now writes 800 pixels
   / 2,400 background bytes instead of 786,432 pixels / 2,359,296 bytes. Completed
   damage is cleared, so duplicate end_update calls do not repaint it.
4. **Temporary bitmap allocation and redundant copy.** A decoded 16-bit bitmap
   previously went through create_bitmap: allocate image/DC/bitmap, copy the
   decoded data into the temporary bitmap, copy again into the primary buffer,
   then destroy it. For bounded RGB565 data already matching GDI, the new path
   clips using GDI's own clip function and copies rows directly to the backing
   store. Other source formats retain the original conversion path. Differential
   tests compare it with the old create/BitBlt/destroy operation, including
   nonzero source padding, destination clipping, and negative coordinates.
5. **Cursor and input scheduling costs.** Cursor movement previously repainted
   both old and new rectangles even if overlapping, hidden, or unchanged. Nearby
   rectangles now coalesce only if that avoids work; far moves restore separately.
   Restoration reads the authoritative GDI buffer, never the framebuffer. Input
   used to wait behind graphics dispatch. Ready input is now handled first in
   bounded batches (32 events/device), followed by one existing core dispatch.
   A pipe-based test of the actual event loop verifies input priority, bounded
   service to both input and network, and Ctrl+Alt+F12 exiting before graphics.
   Mouse scaling, direction, wheel, button and key translation are unchanged.

## Timing and scheduling audit

No unconditional 500 ms sleep exists in the active graphics path. The frontend's
one-second select timeout is an idle bound and returns immediately on readiness;
our event-loop test confirms that behavior. TCP/TLS waits (1/100 ms) occur on
would-block transport operations, not as per-frame throttling. A partially
received packet can still hold the single-threaded decoder until more bytes
arrive or the existing alarm terminates the session. This has not been rewritten
into an asynchronous protocol parser. Input writes now share the alarm guard.

The core handles the available transport packet and any contained PDUs, then
returns to the loop. Graphics decode within that packet is not preempted. RDP
RGB565 decode, GDI raster operations, TLS crypto, and real framebuffer bus/cache
behavior can still dominate a demanding workload. Update presentation remains
at protocol callback boundaries; there is no added frame timer or artificial
frame-rate cap. Sparse region coalescing is within each update. No speculative
cross-packet buffering, resolution reduction, protocol downgrade, or codec
negotiation changes were added.

The supervisor's sleeps are independent connection/network monitoring and retry
operations, not active-client rendering delays. Status screens are transition
screens, not periodic redraws over an active session. Network/decode buffers
already grow on demand and are reused; the identified per-bitmap temporary
allocation was removed only where byte-format identity is established.

Production diagnostics were already clean: the exact proven mouse shim contains
no TMPUSB/TMPFIX strings or print calls; the supervisor does not enable
--mouse-diag. Core drawing debug macros are disabled; frame-ack logging is on a
path not enabled by these default settings. Existing error/reconnect logs remain.
The only new normal log is one PERFORMANCE v1 marker after connection. No
per-pixel, per-update, or per-event production counters/logs are added.

## Reproducible QEMU comparison

Same ELDK 4.1 GCC 4.0.0 -Os, ARM926/OABI/soft-float toolchain; actual frontend and
GDI routines. Baseline sources were snapshotted before changes and rebuilt in an
isolated directory. The known flashed baseline itself remains untouched. The
harness uses a RAM framebuffer; it cannot emulate physical VGA memory bandwidth.
Median of three sequential runs, milliseconds per operation, lower is better.
Each sample internally repeats 80 or 4,000 operations. Both variants finish
with the same framebuffer checksum. Results are relative emulation evidence,
**not hardware FPS predictions**. copy_full is a control, not an optimization.

| Workload | Before ms | After ms | Before/after |
|---|---:|---:|---:|
| full_hidden | 25.490203 | 1.804798 | 14.12× |
| full_cursor | 29.933635 | 1.805518 | 16.58× |
| dirty_32 | 0.032225 | 0.003568 | 9.03× |
| dirty_800x600 | 15.628245 | 1.139776 | 13.71× |
| cursor_move | 0.024246 | 0.007792 | 3.11× |
| sparse_callbacks | 25.432667 | 0.016797 | 1514.12× |
| bitmap_64 | 0.013375 | 0.005232 | 2.56× |
| copy_full | 0.659899 | 0.638108 | 1.03× |

The large sparse-update gain combines eliminating almost the entire unnecessary
redraw with the faster row converter; it is deliberately a worst-case bounding
box example, not a typical-session speed multiplier.

Memory: the RGB565 desktop remains 1.5 MiB, physical RGB24 framebuffer 2.25 MiB,
and lookup table 256 KiB. The bounded damage list adds 1,544 bytes to GDI_WND.
No second desktop framebuffer, new daemon, new shared library, or decoder
threads are added. The standalone ARM ELF grew by 2,420 bytes; compressed image
by 1,726 bytes. Build flags, static dependencies and crypto settings are retained.

## Tests and limitations

- 25 frontend self-tests passed under qemu-arm-static -cpu arm926, including
  existing 22 tests and three new dirty-region checks. The actual executable
  extracted from the final gzip/cpio also passed all 25.
- All 51 original CUnit tests / 198 assertions passed. All required archives
  verified ARM OABI; executable ELF flags 0x602, no PT_INTERP or PT_DYNAMIC.
- Tempest test_vendor_decoder.py passed: 19 physical report vectors, 56 synthetic
  button/wheel combinations, and 80 unchanged length-8/12 cases (155 total).
  Exact hardware-proven module SHA is retained; it was not rebuilt or patched.
- Renderer differential tests: 492 conversion/cursor cases across RGB16/24/32,
  real/headless color behavior, offsets, odd pitches and clipping; 2,000 random
  drawing orders; 65-region overflow; 80 cursor moves; 100 bitmap-copy cases.
- Actual event-loop pipe tests and actual TLS 1.2 multi-PDU buffering test pass.
- All 11 appliance fault tests and all five existing image tests passed. The
  scripts exercised by the appliance suite match the candidate byte-for-byte.
- Loopback xrdp TLS 1.2 rendered 1024x768 with 1,644 colors; correct pin accepted,
  wrong/missing pins rejected, stalled connection exits 124, clean shutdown.
  This verifies the existing RDP stack locally; it is not a Windows Server test.
- Original Dropbear optional SSH authentication, pinned host key, and rejection
  of password authentication pass. Its host-side key-inspection invocation
  prints an existing ld.so.cache-format warning when QEMU sees the host cache;
  authentication succeeds. Production maintenance SSH remains disabled and no
  runtime loader/library changed. Historical compiler warnings remain confined
  to unchanged legacy sources; no new harness/changed-source warnings remain.
- Reopened final archive: all 525 entries and every header field verified;
  only fbfreerdp payload and its filesize field differ. All other 524 payloads,
  UID/GID, modes, timestamps, links, device metadata, and entry order match.
  Packaged rc.sh/supervisor pass original ARM BusyBox sh -n. FWD differs only in
  the rootfs byte-size field at 0x54. Component hashes are in manifest.json.
- Production image/FWD and every existing TFTP file still match the snapshot.
  No NAND command, saveenv, device connection, or permanent boot change occurred.

The target remains 1024x768. The physical R/B correction is byte-for-byte
unchanged in fb_colour(), and the optimized physical-color path is covered by
differential tests. handle_input() is also byte-for-byte unchanged. Mouse module,
keyboard setup, supervisor, TLS/pin configuration, networking and device identity
are identical to final-v1. Real framebuffer word-store behavior, cache/bus limits,
Windows workload FPS, visual artifacts, responsiveness under sustained decode,
and long-run operation await the first physical RAM test. A responsive modern
video desktop cannot be guaranteed on ARM926 from QEMU measurements.

## Exact changed production sources

All paths below are relative to FreeRDP-old-master:

- l300/fbfreerdp.c — packed converter, bounded cursor drawing/restoration,
  damage-list presentation, input-first scheduling, TLS-aware select, startup
  marker and three additional self-tests; original color/input logic retained.
- libfreerdp-gdi/gdi.h — fixed-size damage-list fields for L300 builds.
- libfreerdp-gdi/gdi_region.c — damage collection/coalescing while preserving the
  existing invalid-region behavior and bounded overflow fallback.
- libfreerdp-gdi/gdi.c — direct clipped RGB565 bitmap copy.
- libfreerdp-core/crypto/openssl.c — safe SSL_pending wrapper.
- libfreerdp-core/tls.h — declaration of that wrapper.
- libfreerdp-core/freerdp.c — TLS-buffer-aware readiness and packet dispatch.
- include/freerdp/freerdp.h — frontend readiness helper declaration.

No other production source or configuration was changed. source-changes.patch
contains the complete reversible patch. No Tempest source/module modification.

New development/test sources in l300-performance:
bench.c, benchmark.py, run_arm.py, reference-present.h (original renderer test
oracle), test_render.c, test_event.c, test_transport.c, test_transport.py and
package.py. Baseline snapshots, test executables and logs stay outside firmware.

## Reproduce / operate

Use only FreeRDP-old-master/l300/build.py, never the top-level build system.

    cd FreeRDP-old-master
    python3 l300/build.py
    python3 l300/build.py --tests
    python3 l300/verify.py
    python3 l300/test-integration.py
    cd l300-mouse-diagnostics/rootcause-evidence
    python3 test_vendor_decoder.py
    cd <workspace>
    python3 NCOMPUTING_L300_SPEAR600/appliance/tests/test_appliance.py
    python3 NCOMPUTING_L300_SPEAR600/appliance/tests/test_image.py
    python3 NCOMPUTING_L300_SPEAR600/appliance/tests/test_ssh.py
    python3 l300-performance/run_arm.py l300-performance/test_render.c
    python3 l300-performance/run_arm.py l300-performance/test_event.c
    python3 l300-performance/test_transport.py
    python3 l300-performance/run_arm.py l300-performance/bench.c --baseline
    python3 l300-performance/run_arm.py l300-performance/bench.c
    python3 l300-performance/benchmark.py

package.py creates performance-v1 once and refuses an existing output directory.
Do not delete/overwrite the delivered candidate merely to rerun packaging.
The unmodified known production image and FWD are under final-v1; RAM-TEST.txt
and FLASH-AFTER-VALIDATION.txt contain staging, exact byte counts/CRC checks,
first physical acceptance tests, conditional later flashing, and rollback.
No files were staged into /srv/tftp by this session. Use the provided copy command.
