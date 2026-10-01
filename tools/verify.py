#!/usr/bin/env python3
"""Check the shipped ABI and run both ARM test executables."""
import hashlib
from pathlib import Path
import struct
import subprocess

B = Path(__file__).resolve().parents[2] / 'l300-build'
log = B/'logs/verification.log'
messages = []
for name in ['fbfreerdp', 'test_freerdp']:
    binary = B/'out'/name
    data = binary.read_bytes()
    assert data[:7] == b'\x7fELF\x01\x01\x01', 'Need ELF32 little endian'
    assert struct.unpack_from('<H', data, 18)[0] == 40, 'Need ARM machine'
    flags = struct.unpack_from('<I', data, 36)[0]
    assert flags == 0x602, hex(flags)
    phoff = struct.unpack_from('<I', data, 28)[0]
    entsize, count = struct.unpack_from('<HH', data, 42)
    kinds = [struct.unpack_from('<I', data, phoff+i*entsize)[0] for i in range(count)]
    assert 2 not in kinds and 3 not in kinds, 'Unexpected dynamic loader/dependencies'
    messages.append(f'PASS: {name}: ARM ELF32 OABI 0x602; static; {len(data)} bytes; SHA256 {hashlib.sha256(data).hexdigest()}')
libraries = [B/'stage/lib'/name for name in ['libssl.a', 'libcrypto.a', 'libiconv.a', 'libcunit.a']]
libraries += [B/'toolchain/arm/lib'/name for name in ['libc.a', 'libm.a']]
libraries += [B/'toolchain/usr/lib/gcc/arm-linux/4.0.0/libgcc.a']
for path in libraries:
    library = path.name
    headers = subprocess.check_output(['readelf','-h',str(path)],text=True)
    machines = [line.split(':',1)[1].strip() for line in headers.splitlines() if 'Machine:' in line]
    assert machines and all(m == 'ARM' for m in machines), library
    flags = [int(line.split(':',1)[1].strip().split(',')[0],16) for line in headers.splitlines() if 'Flags:' in line]
    assert all(f >> 24 == 0 for f in flags), 'EABI objects in ' + library
    messages.append(f'PASS: {library}: all {len(machines)} archive members ARM/OABI')
for name, options in [('fbfreerdp',['--version']), ('fbfreerdp',['--self-test']), ('test_freerdp',[])]:
    result = subprocess.run([str(B/'downloads/qemu-arm-static'), '-cpu', 'arm926',
                             str(B/'out'/name), *options],text=True,
                            stdout=subprocess.PIPE,stderr=subprocess.STDOUT,timeout=60)
    messages.append(result.stdout)
    if result.returncode:
        log.write_text('\n'.join(messages))
        raise SystemExit(f'{name} exited {result.returncode}; see {log}')
log.write_text('\n'.join(messages))
print('\n'.join(messages[:9]))
print('PASS: version, frontend self-test and all original CUnit suites; see', log)
