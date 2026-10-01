#!/usr/bin/env python3
"""Regression tests for the L300 Tempest length-10 mouse compatibility shim."""

from pathlib import Path
import os
import random
import re
import subprocess

HERE = Path(__file__).resolve().parent
ROOT = HERE.parents[1]
TC = ROOT / 'l300-build/toolchain'
QEMU = ROOT / 'l300-build/downloads/qemu-arm-static'


def main():
    env = dict(
        os.environ,
        CROSS_COMPILE='arm-linux',
        PATH=str(TC / 'usr/bin') + ':' + os.environ['PATH']
    )

    for name, module in [
        ('vendor', ROOT / 'l300-appliance-build/base-rootfs/lib/modules/tempest_usb.ko'),
        ('observed', HERE / 'tempest_usb.observed.ko')
    ]:
        subprocess.run([
            str(TC / 'usr/bin/arm-linux-gcc'),
            '-static', '-Os',
            '-mcpu=arm926ej-s',
            '-marm',
            '-mabi=apcs-gnu',
            '-msoft-float',
            '-I' + str(TC / 'arm/usr/include'),
            '-B' + str(TC / 'arm/lib') + '/',
            '-L' + str(TC / 'arm/lib'),
            '-L' + str(TC / 'arm/usr/lib'),
            '-o', str(HERE / (name + '_decoder_harness')),
            str(HERE / 'vendor_decoder_harness.c'),
            str(module)
        ], env=env, check=True)

    def run(name, length, data):
        result = subprocess.run([
            str(QEMU), '-cpu', 'arm926',
            str(HERE / (name + '_decoder_harness')),
            str(length),
            *[f'{x:02x}' for x in data]
        ], capture_output=True, text=True, check=True)

        assert (
            'ABI long=4 timeval=8 input_event=16 '
            'type=8 code=10 value=12'
        ) in result.stdout

        event = re.search(
            r' -> x=(-?\d+) y=(-?\d+) wheel=(-?\d+) buttons=(\d+)',
            result.stdout
        )
        assert event, result.stdout
        return tuple(map(int, event.groups()))

    #
    # REAL PHYSICAL L300 REPORTS
    #
    # These came from the TMPUSB physical capture.
    # They demonstrate the one-byte displacement in the vendor length-10
    # decoder.
    #
    physical = [
        # raw hex                    corrected X,Y,W,buttons
        ('c3010000000000fe3500',      (0,  0,  0, 0)),
        ('4b0100fd4f0000fe4e00',      (-3, 4,  0, 0)),
        ('c30100ff3f0000fe2d00',      (-1, 3,  0, 0)),
        ('4b0100ff0f0000fe2200',      (-1, 0,  0, 0)),
        ('c3010000400000ffe100',      (0,  4,  0, 0)),
        ('4b010000200000ffff00',      (0,  2,  0, 0)),
        ('c3010000100000fff000',      (0,  1,  0, 0)),
        ('4b010004400000fed100',      (4,  4,  0, 0)),
        ('c301000a300000fde200',      (10, 3,  0,0)),
        ('4b010008000000fc5500',      (8,  0,  0, 0)),
        ('c3010006000000febd00',      (6,  0,  0, 0)),
        ('4b010001000000ffc900',      (1,  0,  0, 0)),

        # Reports where the vendor falsely interpreted Y data as wheel.
        ('4b0100f96fff00bf4400',       (-7, -10, 0, 0)),
        ('c30100f77fff00bc6900',       (-9, -9, 0, 0)),
        ('4b0100f65fff00bc5f00',       (-10,-11,0, 0)),
        ('c30100f9bfff00bebd00',       (-7, -5, 0, 0)),
        ('4b0100fbbfff00bf0500',       (-5, -5, 0, 0)),
        ('c30100ffffff00bfe100',       (-1, -1, 0, 0)),
        ('4b0100feefff00bfd800',       (-2, -2, 0, 0)),
    ]

    for raw, expected in physical:
        data = list(bytes.fromhex(raw))
        got = run('observed', 10, data)
        assert got == expected, (
            f'physical report {raw}: expected {expected}, got {got}'
        )
        print(f'PASS physical {raw} -> {got}')

    #
    # Explicit synthetic length-10 packets.
    #
    # Verify corrected buttons and wheel independently.
    #
    def packet10(buttons=0, x=0, y=0, wheel=0):
        # Encode signed values as 12-bit two's-complement.
        x &= 0xfff
        y &= 0xfff

        data = [0] * 10
        data[0] = 0xc3
        data[1] = 1
        data[2] = buttons & 7

        data[3] = x & 0xff
        data[4] = ((x >> 8) & 0x0f) | ((y & 0x0f) << 4)
        data[5] = (y >> 4) & 0xff
        data[6] = wheel & 0xff

        return data

    for buttons in range(8):
        for wheel in (-3, -2, -1, 0, 1, 2, 3):
            data = packet10(
                buttons=buttons,
                x=-7,
                y=5,
                wheel=wheel
            )

            expected = (-7, 5, wheel, buttons)
            got = run('observed', 10, data)

            assert got == expected, (
                f'10-byte corrected decode: '
                f'expected {expected}, got {got}'
            )

    print('PASS corrected length-10 X/Y/wheel/button decoding')

    #
    # The compatibility shim MUST NOT alter the vendor's other branches.
    #
    rng = random.Random(600)

    for length in (8, 12):
        for _ in range(40):
            data = [rng.randrange(256) for _ in range(length)]

            original = run('vendor', length, data)
            observed = run('observed', length, data)

            assert original == observed, (
                f'length-{length} regression: '
                f'vendor={original}, shim={observed}'
            )

    print('PASS length-8 and length-12 vendor behavior unchanged')

    print()
    print('ALL TEMPEST COMPATIBILITY TESTS PASSED')


if __name__ == '__main__':
    main()
