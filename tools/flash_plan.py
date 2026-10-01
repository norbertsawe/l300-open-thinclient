#!/usr/bin/env python3
"""Print (never execute) a board-verified U-Boot rootfs/FWD update or rollback plan."""
import argparse
import hashlib
import json
from pathlib import Path
import struct
import zlib

HERE=Path(__file__).resolve().parent
BUILD=HERE.parents[1]/'l300-appliance-build'

def main():
    p=argparse.ArgumentParser(description=__doc__)
    p.add_argument('--candidate',type=Path,default=BUILD/'candidate')
    p.add_argument('--erase-size',type=lambda s:int(s,0),required=True,help='From this board\'s nand info')
    p.add_argument('--nand-size',type=lambda s:int(s,0),required=True,help='From this board\'s nand info')
    p.add_argument('--reference-layout-confirmed',action='store_true',required=True)
    p.add_argument('--allow-unprovisioned',action='store_true')
    p.add_argument('--rollback',action='store_true')
    args=p.parse_args()
    candidate=args.candidate.resolve()
    manifest=json.loads((candidate/'manifest.json').read_text())
    if not args.rollback and manifest['status'].startswith('UNPROVISIONED') and not args.allow_unprovisioned:
        p.error('Candidate is unprovisioned; use RAM boot, or explicitly allow flashing it for development')
    backup=BUILD/'backup'
    rootname='l300_initramfs_minimal.gz' if args.rollback else 'l300_initramfs_appliance.gz'
    fwdname='l300_fwd_mod.fwd' if args.rollback else 'l300_fwd_appliance.fwd'
    directory=backup if args.rollback else candidate
    root=directory/rootname; fwd=directory/fwdname
    if not args.rollback:
        for path in [root,fwd]:
            assert hashlib.sha256(path.read_bytes()).hexdigest()==manifest['files'][path.name]['sha256']
    else:
        original=json.loads((backup/'manifest.json').read_text())
        for path in [root,fwd]: assert hashlib.sha256(path.read_bytes()).hexdigest()==original[path.name]['sha256']
    payload=fwd.read_bytes()
    offset,size=struct.unpack_from('<II',payload,0x50)
    assert offset==0x600000 and size==root.stat().st_size and len(payload)==180
    block=args.erase_size
    assert block>0 and not block&(block-1), 'Erase size must be a power of two'
    assert offset%block==0 and 0x80000%block==0
    assert 0x80000+block<=0xe0000, 'FWD erase would overlap U-Boot'
    largest=max(size,(candidate/'l300_initramfs_appliance.gz').stat().st_size,(backup/'l300_initramfs_minimal.gz').stat().st_size)
    span=((largest+block-1)//block)*block
    assert span<=0xc00000 and offset+span<=args.nand_size
    print('# MANUAL PLAN ONLY. Confirm board layout, NAND geometry, good-block capacity,')
    print('# backups, stable power and a working serial/USB recovery path first.')
    print('# Copy these files to the host TFTP directory: '+str(root)+' and '+str(fwd))
    print('# Stop on ANY transfer/NAND error or unexpected CRC. Never use saveenv.')
    print('# Use nand bad and the reference layout to confirm skipped bad blocks cannot reach other data.')
    for path,dest,erase in [(root,offset,span),(fwd,0x80000,block)]:
        n=path.stat().st_size;crc=zlib.crc32(path.read_bytes())&0xffffffff
        print(f'\ntftpboot 0x800000 {path.name}')
        print(f'# Require filesize 0x{n:x} and CRC32 {crc:08x}')
        print(f'crc32 0x800000 0x{n:x}')
        print(f'nand erase 0x{dest:x} 0x{erase:x}')
        print(f'nand write.jffs2 0x800000 0x{dest:x} 0x{n:x}')
        print(f'nand read.jffs2 0x1000000 0x{dest:x} 0x{n:x}')
        print(f'crc32 0x1000000 0x{n:x}')
        print(f'# Readback CRC32 must be {crc:08x} before continuing.')
    print('\n# After both readbacks match: reset and let the autoboot timer expire.')
    print('# The kernel, X-Loader and U-Boot are deliberately not rewritten.')

if __name__=='__main__':main()
