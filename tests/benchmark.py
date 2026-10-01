#!/usr/bin/env python3
"""Sequential median-of-three ARM926 QEMU comparison; no hardware FPS claims."""
import json,re,statistics,subprocess
from pathlib import Path
H=Path(__file__).resolve().parent; R=H.parent
q=R/'l300-build/downloads/qemu-arm-static'
results={}; raw={}; checksums=set()
for variant,binary in [('before',H/'bench.baseline.arm'),('after',H/'bench.arm')]:
    samples={}; raw[variant]=[]
    for trial in range(3):
        text=subprocess.check_output([str(q),'-cpu','arm926',str(binary)],text=True,stderr=subprocess.STDOUT)
        raw[variant].append(text)
        checksums.add(re.search(r'CHECKSUM (\w+)',text)[1])
        for name,ms in re.findall(r'BENCH (\w+) iterations=\d+ ms_per_op=([0-9.]+)',text):
            samples.setdefault(name,[]).append(float(ms))
    results[variant]={k:statistics.median(v) for k,v in samples.items()}
assert len(checksums)==1,checksums
report={'method':'Same ELDK GCC4.0.0 -Os; QEMU -cpu arm926; RAM framebuffer; median of 3 sequential runs; milliseconds per operation; NOT hardware timing',
        'results':results,'raw_runs':raw,'final_frame_checksum':checksums.pop()}
(H/'benchmark-results.json').write_text(json.dumps(report,indent=2)+'\n')
for name,before in results['before'].items():
    after=results['after'][name]
    print(f'{name:22} {before:10.6f} -> {after:10.6f} ms  {before/after:7.2f}x')
