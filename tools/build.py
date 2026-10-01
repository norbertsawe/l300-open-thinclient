#!/usr/bin/env python3
"""Build only the framebuffer client and required FreeRDP libraries with ELDK."""
import concurrent.futures
import json
import os
from pathlib import Path
import subprocess
import sys

TESTS = '--tests' in sys.argv[1:]

SOURCE = Path(__file__).resolve().parents[1]
BUILD = SOURCE.parent / 'l300-build'
TC = BUILD / 'toolchain'
TARGET = TC / 'arm'
STAGE = BUILD / 'stage'
OUT = BUILD / 'out'
OBJ = BUILD / 'obj'
LOG = BUILD / 'logs'
for p in (OUT, OBJ, LOG):
    p.mkdir(parents=True, exist_ok=True)
cc = str(TC / 'usr/bin/arm-linux-gcc')
env = dict(os.environ, PATH=str(TC / 'usr/bin') + ':' + os.environ['PATH'], LC_ALL='C', CROSS_COMPILE='arm-linux')
flags = ['-Os', '-g', '-mcpu=arm926ej-s', '-marm', '-mabi=apcs-gnu',
         '-msoft-float', '-fno-strict-aliasing', '-ffunction-sections',
         '-fdata-sections', '-DHAVE_CONFIG_H', '-include', str(SOURCE / 'l300/config.h'), '-Wall', '-Wextra',
         '-Wno-unused-parameter', '-Wno-sign-compare']
for inc in [SOURCE / 'l300', STAGE / 'include', SOURCE / 'include',
            SOURCE / 'libfreerdp-core', SOURCE / 'libfreerdp-gdi',
            SOURCE / 'libfreerdp-rfx', SOURCE / 'libfreerdp-asn1', TARGET / 'usr/include']:
    flags.append('-I' + str(inc))
sources = []
for directory in ['libfreerdp-asn1', 'libfreerdp-core', 'libfreerdp-gdi', 'libfreerdp-rfx']:
    sources.extend(sorted((SOURCE / directory).glob('*.c')))
sources.append(SOURCE / 'libfreerdp-core/crypto/openssl.c')
for name in ['memory', 'unicode', 'datablob', 'hexdump', 'stopwatch', 'profiler', 'usleep']:
    sources.append(SOURCE / ('libfreerdp-utils/' + name + '.c'))
if TESTS:
    sources.extend(sorted((SOURCE / 'cunit').glob('*.c')))
else:
    sources.append(SOURCE / 'l300/fbfreerdp.c')
name = 'test_freerdp' if TESTS else 'fbfreerdp'

def compile_one(src):
    obj = OBJ / (str(src.relative_to(SOURCE)).replace('/', '_') + '.o')
    result = subprocess.run([cc, *flags, '-c', str(src), '-o', str(obj)],
                            env=env, stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
    return obj, result.returncode, result.stdout

with (LOG / (name + '-build.log')).open('wb') as log:
    with concurrent.futures.ThreadPoolExecutor(max_workers=4) as pool:
        results = list(pool.map(compile_one, sources))
    for obj, code, output in results:
        log.write(output)
        if code:
            print(output.decode(errors='replace'))
    if any(code for _, code, _ in results):
        raise SystemExit('Compilation failed; see ' + str(LOG / 'freerdp-build.log'))
    binary = OUT / (name + '.debug')
    command = [cc, '-static', '-mcpu=arm926ej-s', '-marm', '-msoft-float',
               '-B' + str(TARGET / 'lib') + '/', '-L' + str(TARGET / 'lib'),
               '-L' + str(TARGET / 'usr/lib'),
               '-Wl,-Map,' + str(OUT / (name + '.map')), '-o', str(binary),
               *[str(obj) for obj, _, _ in results], '-Wl,--start-group',
               str(STAGE / 'lib/libssl.a'), str(STAGE / 'lib/libcrypto.a'),
               str(STAGE / 'lib/libiconv.a'), '-lm', '-Wl,--end-group']
    if TESTS:
        command.append(str(STAGE / 'lib/libcunit.a'))
    result = subprocess.run(command, env=env, stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
    log.write(result.stdout)
    if result.returncode:
        print(result.stdout.decode(errors='replace'))
        raise SystemExit('Link failed')
    (OUT / (name + '-objects.json')).write_text(json.dumps([obj.name for obj, _, _ in results], indent=2) + '\n')
subprocess.run([str(TC / 'usr/bin/arm-linux-strip'), '-o', str(OUT / name),
                str(binary)], check=True, env=env)
print('Built', OUT / name, (OUT / name).stat().st_size, 'bytes')
