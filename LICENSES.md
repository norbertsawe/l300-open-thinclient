# Licensing and provenance

This repository contains work originating from multiple sources and therefore
is not distributed under a single blanket license.

## FreeRDP-derived components

The L300 RDP frontend and associated FreeRDP modifications are derived from
FreeRDP.

The applicable upstream license text is preserved in:

`licenses/FreeRDP-COPYING`

Individual source files and patches retain their applicable upstream licensing
where present.

## Tempest mouse compatibility code

`src/tempest/tempest_mouse_fix.c` is marked:

`GPL-2.0-only`

The corresponding source is provided in this repository.

## Status font

The status font has its own licensing information preserved in:

`src/l300/status-font-LICENSE`

## NComputing components

NComputing firmware, kernel binaries, proprietary modules and other vendor
binary components are not licensed by this repository.

Their use during development does not imply permission to redistribute them.

This repository therefore focuses on source code, patches, tests,
documentation and tooling that can be distributed independently of the
original vendor firmware.

## Third-party code

Third-party components remain subject to their respective upstream licenses.
Nothing in this repository changes or supersedes those licenses.
