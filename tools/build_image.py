#!/usr/bin/env python3
"""Build a separate per-device RAM-rootfs image; never write to a device."""
import argparse
import difflib
import hashlib
import ipaddress
import json
from pathlib import Path
import re
import shutil
import stat
import struct
import subprocess
import uuid
from cpio_image import read_image, write_image, replace

HERE = Path(__file__).resolve().parent
REPO = HERE.parent
ROOT = REPO.parent
BASE = REPO/'system_images'
BUILD = ROOT/'l300-appliance-build'

def sha(path): return hashlib.sha256(path.read_bytes()).hexdigest()

def parse_config(text):
    config = {}
    keys = set(re.findall(r'^([A-Z_0-9]+)=', (HERE/'overlay/etc/thinclient.conf').read_text(), re.M))
    for line in text.splitlines():
        if not line or line.startswith('#'): continue
        if '=' not in line: raise ValueError('Config requires KEY=value')
        key, value = line.split('=', 1)
        if key not in keys or key in config: raise ValueError('Unknown/duplicate key: '+key)
        config[key] = value
    if set(config) != keys: raise ValueError('Missing config keys')
    assert re.fullmatch(r'[a-zA-Z0-9][a-zA-Z0-9-]{0,14}', config['DEVICE_NAME'])
    assert re.fullmatch(r'02(:[a-fA-F0-9]{2}){5}', config['ETH_MAC'])
    address = ipaddress.IPv4Address(config['RDP_SERVER'])
    assert not (address.is_loopback or address.is_multicast or address.is_unspecified or
                config['RDP_SERVER'].split('.')[0] in ['0','255'])
    for key, low, high in [('RDP_PORT',1,65535),('RDP_WIDTH',640,1024),('RDP_HEIGHT',480,768),
                           ('CONNECT_TIMEOUT',1,300),('RETRY_SECONDS',2,60)]:
        assert re.fullmatch('[1-9][0-9]{0,4}', config[key]) and low <= int(config[key]) <= high, key
    assert re.fullmatch('[0-9a-fA-F]{1,4}', config['RDP_KEYBOARD'])
    assert config['RDP_SECURITY'] == 'tls', 'Only TLS server-side login is supported'
    assert not config['RDP_CERT_SHA256'] or re.fullmatch('[0-9a-fA-F]{64}', config['RDP_CERT_SHA256'])
    assert config['MAINTENANCE_SSH'] in ['0','1']
    return config

def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--config', type=Path, help='Complete administrator config; preserves device identity')
    parser.add_argument('--device-id', help='Unique alphanumeric/hyphen name, max 15 characters')
    parser.add_argument('--server')
    parser.add_argument('--cert-sha256')
    parser.add_argument('--admin-key', type=Path, help='RSA SSH PUBLIC key; never a private key')
    parser.add_argument('--output', type=Path, default=BUILD/'candidate')
    args = parser.parse_args()
    out = args.output.resolve()
    out.mkdir(parents=True, exist_ok=True)
    assert out != BASE.resolve() and BASE.resolve() not in out.parents, 'Do not overwrite original images'
    backup = BUILD/'backup'
    backup.mkdir(parents=True, exist_ok=True)
    manifest_path = backup/'manifest.json'
    backup_manifest = json.loads(manifest_path.read_text()) if manifest_path.exists() else {}
    for path in BASE.iterdir():
        if path.is_file():
            dest = backup/path.name
            if dest.exists(): assert sha(dest) == sha(path), 'Original differs from backup: '+path.name
            else: shutil.copy2(path, dest)
            detail = {'original':str(path.relative_to(ROOT)), 'sha256':sha(dest), 'size':dest.stat().st_size}
            if path.name in backup_manifest:
                assert backup_manifest[path.name] == detail, 'Backup manifest mismatch: '+path.name
            backup_manifest[path.name] = detail
    manifest_path.write_text(json.dumps(backup_manifest,indent=2)+'\n')
    binary = ROOT/'l300-build/out/fbfreerdp'
    raw = binary.read_bytes()
    assert raw[:6] == b'\x7fELF\x01\x01' and struct.unpack_from('<HI',raw,18)[0] == 40
    assert struct.unpack_from('<I',raw,36)[0] == 0x602
    phoff = struct.unpack_from('<I',raw,28)[0]
    size, count = struct.unpack_from('<HH',raw,42)
    assert all(struct.unpack_from('<I',raw,phoff+i*size)[0] not in (2,3) for i in range(count)), 'Need static binary'
    assert b'--appliance' in raw and b'--status' in raw, 'Rebuild appliance frontend first'
    text = (args.config or HERE/'overlay/etc/thinclient.conf').read_text()
    if not args.config or args.device_id:
        device = args.device_id or 'l300-'+uuid.uuid4().hex[:6]
        mac = '02:'+':'.join(f'{x:02x}' for x in hashlib.sha256(device.encode()).digest()[:5])
        text = re.sub(r'^DEVICE_NAME=.*$', 'DEVICE_NAME='+device, text, flags=re.M)
        text = re.sub(r'^ETH_MAC=.*$', 'ETH_MAC='+mac, text, flags=re.M)
    if args.server: text = re.sub(r'^RDP_SERVER=.*$', 'RDP_SERVER='+args.server, text, flags=re.M)
    if args.cert_sha256:
        text = re.sub(r'^RDP_CERT_SHA256=.*$', 'RDP_CERT_SHA256='+args.cert_sha256.replace(':','').lower(), text, flags=re.M)
    if args.admin_key: text = re.sub(r'^MAINTENANCE_SSH=.*$', 'MAINTENANCE_SSH=1', text, flags=re.M)
    config = parse_config(text)
    if config['MAINTENANCE_SSH']=='1' and not args.admin_key:
        raise ValueError('Remote maintenance requires --admin-key with this build')
    (out/'thinclient.conf').write_text(text)
    (out/'thinclient.conf').chmod(0o600)
    original = read_image(backup/'l300_initramfs_minimal.gz')
    entries = read_image(backup/'l300_initramfs_minimal.gz')
    before = {e.name: e for e in original}
    for path in sorted((HERE/'overlay').rglob('*')):
        if not path.is_file(): continue
        name = str(path.relative_to(HERE/'overlay'))
        data = text.encode() if name=='etc/thinclient.conf' else path.read_bytes()
        mode = 0o600 if name=='etc/thinclient.conf' else (0o755 if data.startswith(b'#!') else 0o644)
        replace(entries, name, data, mode, uid=0 if name not in before or name=='etc/thinclient.conf' else None)
    replace(entries, 'usr/bin/fbfreerdp', raw, 0o755)
    # Hardware-validated L300 Tempest USB mouse compatibility module.
    # Corrects the one-byte alignment error in length-10 mouse reports.
    mouse_module = ROOT/'l300-mouse-diagnostics/rootcause-evidence/tempest_usb.observed.ko'
    assert mouse_module.exists(), 'Build the Tempest compatibility module first'
    assert sha(mouse_module) == 'f647a91327695e68a3f57a54c9a7157da3d2f5e0cc0f8814257609f8be22c8fc', \
        'Unexpected Tempest compatibility module'
    replace(entries, 'lib/modules/tempest_usb.ko', mouse_module.read_bytes(), 0o644)
    # Remove a known vendor password. No Linux login prompt is started.
    password = '!'
    if args.admin_key:
        out.chmod(0o700)
        # A random, unknown hash keeps old Dropbear's public-key account checks
        # happy; password auth is disabled with -s. This is not a Windows secret.
        password = subprocess.check_output(['openssl','passwd','-6','-stdin'], input=uuid.uuid4().hex.encode()).decode().strip()
    passwd = before['etc/passwd'].data.decode().splitlines()
    passwd[0] = 'root:'+password+':0:0:Administrator:/root:/bin/sh'
    replace(entries,'etc/passwd', ('\n'.join(passwd)+'\n').encode(),0o644)
    # No shared stock SSH private keys are used by the new service.
    entries = [e for e in entries if not (e.name.startswith('etc/dropbear/') and e.name.endswith('_host_key'))]
    if args.admin_key:
        key = args.admin_key.read_text().strip()
        assert re.fullmatch(r'ssh-rsa [A-Za-z0-9+/]+={0,3}( [^\r\n]*)?',key), 'Supply one ssh-rsa public key'
        subprocess.run(['ssh-keygen','-lf',str(args.admin_key)],check=True,stdout=subprocess.DEVNULL)
        replace(entries,'root',b'',stat.S_IFDIR|0o700,uid=0,gid=0)
        replace(entries,'root/.ssh',b'',stat.S_IFDIR|0o700,uid=0,gid=0)
        replace(entries,'root/.ssh/authorized_keys',(key+'\n').encode(),0o600,uid=0,gid=0)
        hostkey = out/'maintenance_rsa_host_key'
        if not hostkey.exists():
            rootfs = BUILD/'base-rootfs'
            if not (rootfs/'usr/bin/dropbearkey').exists():
                rootfs.mkdir(parents=True, exist_ok=True)
                # Extract only the key generator and its runtime; no device nodes.
                subprocess.run(['bsdtar','--no-same-owner','-xf',str(backup/'l300_initramfs_minimal.gz'),
                                '-C',str(rootfs),'*lib/*','*usr/bin/dropbearkey'],check=True)
            subprocess.run([str(ROOT/'l300-build/downloads/qemu-arm-static'),'-cpu','arm926','-L',str(rootfs),
                            str(rootfs/'usr/bin/dropbearkey'),'-t','rsa','-s','2048','-f',str(hostkey)],
                           check=True,stdout=subprocess.DEVNULL)
        hostkey.chmod(0o600)
        replace(entries,'etc/dropbear/thinclient_rsa_host_key',hostkey.read_bytes(),0o600,uid=0,gid=0)
    image = out/'l300_initramfs_appliance.gz'
    write_image(entries,image)
    if args.admin_key: image.chmod(0o600)
    assert image.stat().st_size < 0xC00000, 'Exceeds the documented reference update span'
    fwd = bytearray((backup/'l300_fwd_mod.fwd').read_bytes())
    assert len(fwd)==180 and struct.unpack_from('<I',fwd,0x50)[0]==0x600000
    assert struct.unpack_from('<I',fwd,0x54)[0]==(backup/'l300_initramfs_minimal.gz').stat().st_size
    struct.pack_into('<I',fwd,0x54,image.stat().st_size)
    (out/'l300_fwd_appliance.fwd').write_bytes(fwd)
    patches = []
    after = {e.name:e for e in entries}
    changed = []
    for name in sorted(set(before)|set(after)):
        old, new = before.get(name),after.get(name)
        if old and new and old.data==new.data and old.fields==new.fields: continue
        changed.append(name)
        if new and (new.data.startswith(b'#!') or name in ['etc/inittab','etc/thinclient.conf','etc/passwd']):
            old_data = old.data.decode(errors='replace').splitlines(True) if old else []
            patches += difflib.unified_diff(old_data,new.data.decode().splitlines(True),fromfile='a/'+name,tofile='b/'+name)
    (out/'rootfs.patch').write_text(''.join(patches))
    # Verify every untouched entry's UID, GID, mode, links, device metadata and data.
    reread={e.name:e for e in read_image(image)}
    for name, old in before.items():
        if name not in changed:
            new=reread[name]
            assert old.fields[:6]==new.fields[:6] and old.fields[7:11]==new.fields[7:11] and old.data==new.data,name
    report={'status':'PROVISIONED - HARDWARE TEST REQUIRED' if config['RDP_CERT_SHA256'] else 'UNPROVISIONED - NO RDP CONNECTION',
            'device':config['DEVICE_NAME'],'mac':config['ETH_MAC'],'changed_entries':changed,
            'original_entries':len(original),'output_entries':len(entries),'untouched_metadata_verified':len(before)-len(set(changed)&set(before)),
            'kernel_rebuilt':False,'new_shared_libraries':[],
            'files':{p.name:{'sha256':sha(p),'bytes':p.stat().st_size} for p in [image,out/'l300_fwd_appliance.fwd',binary]},
            'rootfs_nand_offset_from_reference_fwd':'0x600000','fwd_nand_offset_from_reference_guide':'0x80000',
            'hardware_nand_geometry_verified':False}
    (out/'manifest.json').write_text(json.dumps(report,indent=2)+'\n')
    (out/'SHA256SUMS').write_text(''.join(sha(p)+'  '+p.name+'\n' for p in [image,out/'l300_fwd_appliance.fwd',out/'thinclient.conf']))
    print(json.dumps(report,indent=2))

if __name__=='__main__': main()
