# L300 Open Thin Client

Open thin-client software and engineering documentation for the NComputing L300.

This project investigates running a standalone Linux-based RDP client directly
on the L300 hardware, including framebuffer graphics, keyboard and mouse input,
network booting, NAND deployment, and performance optimization.

<p align="center">
  <img src="docs/images/ncomputing-l300-mainboard-top.jpeg"
       alt="NComputing L300 mainboard"
       width="650">
</p>

## Stage 1

Stage 1 has been tested on physical NComputing L300 hardware.

The current implementation provides:

- Linux boot on the L300 ARM platform
- direct framebuffer RDP client
- 1024x768 display operation
- Windows Remote Desktop connectivity
- keyboard input
- corrected USB mouse operation
- corrected framebuffer color handling
- optimized framebuffer update paths
- NAND boot support
- reproducible verification and regression tests

Normal Windows desktop interaction is substantially improved by the
PERFORMANCE-v1 rendering work.

Video playback remains considerably more demanding than ordinary desktop
rendering and is not considered solved by Stage 1.

## Hardware

The tested platform is an NComputing L300 using:

- ARM926-class processor
- 128 MiB NAND
- 128 KiB NAND erase blocks
- Linux 2.6.19.2 vendor environment
- framebuffer display output
- Tempest USB/input hardware

This is an unusually old embedded Linux environment. Modern ARM Linux binaries
cannot simply be copied to the device.

### Development setup

Development and hardware validation were performed directly on physical
NComputing L300 hardware using serial access for bootloader and kernel
diagnostics.

![NComputing L300 development and debug setup](docs/images/ncomputing-l300-debug-setup.jpeg)


## Mouse compatibility

Hardware investigation identified an alignment problem affecting 10-byte
Tempest mouse reports.

The working interpretation is:

```text
buttons = data[2] & 7
X       = signed12(data[3] | ((data[4] & 0x0f) << 8))
Y       = signed12((data[4] >> 4) | (data[5] << 4))
wheel   = signed8(data[6])
```

The compatibility implementation preserves the other observed decoder paths
while correcting the affected mouse reports.

See:

- `docs/mouse-root-cause.md`
- `src/tempest/tempest_mouse_fix.c`
- `tests/test_vendor_decoder.py`

## Rendering performance

PERFORMANCE-v1 reduces unnecessary framebuffer work and improves several
graphics paths used by the RDP client.

The work includes:

- bounded framebuffer damage updates
- packed RGB24 conversion
- reduced per-pixel overhead
- reduced redundant bitmap copies
- cursor update coalescing
- improved input/graphics scheduling
- handling already-buffered TLS data without unnecessarily waiting for socket readiness

The included benchmark results are engineering measurements from the test
environment and should not be interpreted as predicted physical-device FPS.

See `docs/performance.md` for the investigation and methodology.

## Repository structure

```text
src/l300/       L300 framebuffer RDP frontend
src/tempest/    Tempest input compatibility work
patches/        Source patches
tests/          Renderer, transport, input and decoder tests
tools/          Build, image and verification utilities
docs/           Testing, performance and deployment documentation
release/        Release checksums
```

## Firmware

This repository currently focuses on source code, patches, tests, documentation
and build tools.

Some components required by the original L300 environment originate from
NComputing firmware and are therefore not automatically redistributed with
this project.

## NAND warning

Writing incorrect data to NAND can make the device unbootable.

Before flashing:

1. Verify the exact board and NAND geometry.
2. Inspect the NAND bad-block table.
3. Keep backups of the original firmware.
4. Verify transferred data with checksums.
5. Maintain a tested serial or USB recovery method.
6. Test experimental firmware from RAM whenever practical.

## Development status

Stage 1 establishes the framebuffer RDP, input and rendering-performance
baseline.

Further development is focused on a newer RDP stack and additional thin-client
functionality, including audio and storage redirection.

Those features are not part of the Stage 1 hardware-proven feature set.

## Documentation

Start with:

- `docs/mouse-root-cause.md`
- `docs/performance.md`
- `docs/testing.md`
- `docs/RAM-TEST.txt`
- `docs/FLASH-AFTER-VALIDATION.txt`
- `docs/ROLLBACK.txt`

## Disclaimer

This is an independent community engineering project and is not affiliated
with or endorsed by NComputing.

Hardware modification and firmware replacement are performed at your own risk.
