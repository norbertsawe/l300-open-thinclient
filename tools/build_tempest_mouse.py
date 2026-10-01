#!/usr/bin/env python3
"""Append an observation shim; change only nine verified call relocations.

This creates an experiment module on the host; it never loads a module or
accesses a device. Refuses any input other than the inspected vendor binary.
"""
import hashlib
import json
import os
from pathlib import Path
import struct
import subprocess

HERE=Path(__file__).resolve().parent
ROOT=HERE.parents[1]
TC=ROOT/'l300-build/toolchain'
ORIGINAL=ROOT/'l300-appliance-build/base-rootfs/lib/modules/tempest_usb.ko'

class ELF:
    def __init__(self,path):
        self.data=bytearray(path.read_bytes())
        assert self.data[:6]==b'\x7fELF\x01\x01'
        assert struct.unpack_from('<H',self.data,18)[0]==40
        offset=struct.unpack_from('<I',self.data,32)[0]
        size,count,names=struct.unpack_from('<HHH',self.data,46)
        self.sections=[struct.unpack_from('<10I',self.data,offset+i*size) for i in range(count)]
        strings=self.contents(self.sections[names])
        self.byname={self.string(strings,s[0]):s for s in self.sections}
        table=self.byname['.symtab']; strings=self.contents(self.sections[table[6]])
        self.symbols=[]
        for i in range(table[5]//16):
            symbol=struct.unpack_from('<IIIBBH',self.data,table[4]+i*16)
            self.symbols.append((self.string(strings,symbol[0]),symbol))
    @staticmethod
    def string(data,offset): return bytes(data[offset:]).split(b'\0',1)[0].decode()
    def contents(self,section): return self.data[section[4]:section[4]+section[5]]

def main():
    expected=(HERE/'original-module.sha256').read_text().split()[0]
    assert hashlib.sha256(ORIGINAL.read_bytes()).hexdigest()==expected,'Different vendor module'
    env=dict(os.environ,CROSS_COMPILE='arm-linux',PATH=str(TC/'usr/bin')+':'+os.environ['PATH'])
    obj=HERE/'tempest_observer.o'; merged=HERE/'tempest_usb.observed.ko'
    subprocess.run([str(TC/'usr/bin/arm-linux-gcc'),'-c','-Os','-g','-mcpu=arm926ej-s',
                    '-marm','-mabi=apcs-gnu','-msoft-float','-ffreestanding','-fno-builtin',
                    '-fno-common','-Wall','-Wextra','-o',str(obj),str(HERE/'tempest_observer.c')],env=env,check=True)
    subprocess.run([str(TC/'usr/bin/arm-linux-ld'),'-r','-o',str(merged),str(ORIGINAL),str(obj)],env=env,check=True)
    original=ELF(ORIGINAL); elf=ELF(merged)
    oldtext=original.contents(original.byname['.text'])
    assert elf.contents(elf.byname['.text'])[:len(oldtext)]==oldtext,'Original instructions changed'
    for section in ['.modinfo','.gnu.linkonce.this_module','.init.text','.exit.text','.data']:
        a=original.contents(original.byname[section]);b=elf.contents(elf.byname[section])
        assert a==b[:len(a)],section
    old_undefined={name for name,sym in original.symbols if name and sym[5]==0}
    new_undefined={name for name,sym in elf.symbols if name and sym[5]==0}
    assert old_undefined==new_undefined,'Unexpected new kernel dependency'
    symbol_ids={name:i for i,(name,sym) in enumerate(elf.symbols) if name}
    targets={0x10dc:('store_mouse_event','observe_store_mouse_event'),
             0x112c:('store_mouse_event','observe_store_mouse_event')}
    for offset in [0xe28,0xe3c,0xe50,0xe64,0xe80,0xe9c,0xeb0]:
        targets[offset]=('input_event','observe_input_event')
    changes=[]; section=elf.byname['.rel.text']
    for position in range(section[4],section[4]+section[5],8):
        offset,info=struct.unpack_from('<II',elf.data,position)
        if offset not in targets: continue
        old,new=targets[offset]
        assert info&255==1 and elf.symbols[info>>8][0]==old,(offset,old)
        struct.pack_into('<I',elf.data,position+4,(symbol_ids[new]<<8)|1)
        changes.append({'text_offset':hex(offset),'from':old,'to':new})
    assert len(changes)==9
    merged.write_bytes(elf.data)
    (HERE/'module-observation-manifest.json').write_text(json.dumps({
        'original_sha256':expected,'observed_sha256':hashlib.sha256(elf.data).hexdigest(),
        'original_text_unchanged':True,'new_kernel_dependencies':[],
        'relocations_redirected':changes,'kernel_load_tested':False},indent=2)+'\n')
    print(merged)

if __name__=='__main__': main()
