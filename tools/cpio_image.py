"""Read/write newc initramfs without extraction, root, chown, or lost metadata."""
from dataclasses import dataclass
import gzip
from pathlib import PurePosixPath
import stat

@dataclass
class Entry:
    name: str
    fields: list
    data: bytes

def read_image(path):
    raw = gzip.decompress(path.read_bytes())
    entries = []
    offset = 0
    names = set()
    while True:
        assert raw[offset:offset+6] == b'070701', 'Expected newc cpio'
        fields = [int(raw[offset+6+i*8:offset+14+i*8], 16) for i in range(13)]
        offset += 110
        name_raw = raw[offset:offset+fields[11]]
        assert name_raw.endswith(b'\0')
        name = name_raw[:-1].decode()
        offset = (offset+fields[11]+3) & ~3
        data = raw[offset:offset+fields[6]]
        assert len(data) == fields[6]
        offset = (offset+fields[6]+3) & ~3
        if name == 'TRAILER!!!':
            break
        normalized = str(PurePosixPath(name))
        assert not normalized.startswith('/') and '..' not in PurePosixPath(name).parts
        assert normalized not in names, normalized
        names.add(normalized)
        entries.append(Entry(normalized, fields, data))
    return entries

def write_image(entries, path):
    out = bytearray()
    for entry in [*entries, Entry('TRAILER!!!', [0]*13, b'')]:
        name = entry.name.encode()+b'\0'
        fields = entry.fields.copy()
        fields[6], fields[11], fields[12] = len(entry.data), len(name), 0
        out += b'070701'+b''.join(f'{f:08x}'.encode() for f in fields)+name
        out += b'\0' * (-len(out) % 4)
        out += entry.data
        out += b'\0' * (-len(out) % 4)
    out += b'\0' * (-len(out) % 512)
    path.write_bytes(gzip.compress(out, compresslevel=9, mtime=0))

def replace(entries, name, data, mode=0o644, uid=None, gid=None):
    index = {entry.name: entry for entry in entries}
    parent = str(PurePosixPath(name).parent)
    if parent not in ('.', '') and parent not in index:
        replace(entries, parent, b'', stat.S_IFDIR | 0o755, uid=0, gid=0)
    if parent in index:
        assert stat.S_ISDIR(index[parent].fields[1]), 'Parent is not a directory: '+parent
    if name in index:
        entry = index[name]
        assert stat.S_ISREG(entry.fields[1]) or stat.S_ISDIR(entry.fields[1]), 'Refusing to follow '+name
        entry.data = data
        entry.fields[1] = mode if stat.S_IFMT(mode) else stat.S_IFREG | mode
        if uid is not None: entry.fields[2] = uid
        if gid is not None: entry.fields[3] = gid
    else:
        ino = max(entry.fields[0] for entry in entries)+1
        fields = [ino, mode if stat.S_IFMT(mode) else stat.S_IFREG | mode,
                  uid or 0, gid or 0, 1, 0, len(data), 0, 0, 0, 0, 0, 0]
        entries.append(Entry(name, fields, data))
