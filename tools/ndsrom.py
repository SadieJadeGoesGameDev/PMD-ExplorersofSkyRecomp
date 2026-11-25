"""Minimal NDS ROM reader/writer (own code) for patching PMD:EoS."""
import struct


def u32(b, o):
    return struct.unpack_from('<I', b, o)[0]


def crc16(data):
    crc = 0xFFFF
    for byte in data:
        crc ^= byte
        for _ in range(8):
            crc = (crc >> 1) ^ 0xA001 if crc & 1 else crc >> 1
    return crc


class Folder:
    def __init__(self):
        self.firstID = 0
        self.files = []      # names
        self.folders = []    # (name, Folder)

    def subfolder(self, path):
        f = self
        for part in path.split('/'):
            f = dict(f.folders)[part]
        return f


class Rom:
    def __init__(self, path):
        d = open(path, 'rb').read()
        self.raw = d
        self.header = bytearray(d[:0x4000])
        a9o, a9s = u32(d, 0x20), u32(d, 0x2C)
        self.arm9 = bytearray(d[a9o:a9o + a9s])
        # nitrocode footer (12 bytes) after arm9
        self.arm9post = d[a9o + a9s:a9o + a9s + 12] if d[a9o + a9s:a9o + a9s + 4] == bytes.fromhex('2106c0de') else b''
        a7o, a7s = u32(d, 0x30), u32(d, 0x3C)
        self.arm7 = d[a7o:a7o + a7s]
        y9o, y9s = u32(d, 0x50), u32(d, 0x54)
        self.y9 = bytearray(d[y9o:y9o + y9s])
        y7o, y7s = u32(d, 0x58), u32(d, 0x5C)
        self.y7 = d[y7o:y7o + y7s]
        bo = u32(d, 0x68)
        ver = struct.unpack_from('<H', d, bo)[0]
        bsize = {1: 0x840, 2: 0x940, 3: 0xA40, 0x103: 0x23C0}.get(ver, 0x840)
        self.banner = d[bo:bo + bsize]
        fnt, fat, fats = u32(d, 0x40), u32(d, 0x48), u32(d, 0x4C)
        self.files = []
        for i in range(fats // 8):
            s, e = struct.unpack_from('<II', d, fat + i * 8)
            self.files.append(bytearray(d[s:e]))
        self.root = self._read_folder(d, fnt, 0xF000)

    def _read_folder(self, d, fnt, did):
        o = fnt + (did & 0xFFF) * 8
        sub, first, _ = struct.unpack_from('<IHH', d, o)
        f = Folder()
        f.firstID = first
        p = fnt + sub
        while True:
            b = d[p]; p += 1
            if b == 0:
                break
            n = d[p:p + (b & 0x7F)].decode('latin1'); p += b & 0x7F
            if b & 0x80:
                c = struct.unpack_from('<H', d, p)[0]; p += 2
                f.folders.append((n, self._read_folder(d, fnt, c)))
            else:
                f.files.append(n)
        return f

    # ---- file helpers
    def file_id(self, path):
        parts = path.split('/')
        f = self.root.subfolder('/'.join(parts[:-1])) if len(parts) > 1 else self.root
        return f.firstID + f.files.index(parts[-1])

    def get_file(self, path):
        return self.files[self.file_id(path)]

    def set_file(self, path, data):
        self.files[self.file_id(path)] = bytearray(data)

    def _shift(self, folder, new_idx, skip):
        if folder is not skip and folder.firstID >= new_idx:
            folder.firstID += 1
        for _, s in folder.folders:
            self._shift(s, new_idx, skip)

    def create_file(self, path, data):
        import bisect
        parts = path.split('/')
        folder = self.root.subfolder('/'.join(parts[:-1]))
        idx = bisect.bisect(folder.files, parts[-1])
        folder.files.insert(idx, parts[-1])
        self._shift(self.root, folder.firstID, folder)
        self.files.insert(folder.firstID + idx, bytearray(data))
        self._fix_overlay_ids(folder.firstID + idx)

    def insert_raw_file(self, idx, data, shift_from):
        """Insert unnamed file (overlay) at idx; folders with firstID > shift_from move up."""
        self.files.insert(idx, bytearray(data))

        def rec(f):
            if f.firstID > shift_from:
                f.firstID += 1
            for _, s in f.folders:
                rec(s)
        rec(self.root)

    def _fix_overlay_ids(self, new_idx):
        for k in range(len(self.y9) // 32):
            fid = u32(self.y9, k * 32 + 24)
            if fid >= new_idx:
                raise RuntimeError('inserted file before an overlay')

    def overlay(self, n):
        return self.files[u32(self.y9, n * 32 + 24)]

    def set_overlay(self, n, data):
        self.files[u32(self.y9, n * 32 + 24)] = bytearray(data)

    # ---- writing
    def _build_fnt(self):
        folders = []

        def collect(f):
            folders.append(f)
            for _, s in f.folders:
                collect(s)
        collect(self.root)
        ids = {id(f): 0xF000 + i for i, f in enumerate(folders)}
        parent = {id(self.root): len(folders)}
        for f in folders:
            for _, s in f.folders:
                parent[id(s)] = ids[id(f)]
        subs = []
        for f in folders:
            b = bytearray()
            for n in f.files:
                nb = n.encode('latin1'); b += bytes([len(nb)]) + nb
            for n, s in f.folders:
                nb = n.encode('latin1'); b += bytes([0x80 | len(nb)]) + nb + struct.pack('<H', ids[id(s)])
            b += b'\0'
            subs.append(b)
        main = bytearray()
        off = len(folders) * 8
        for f, s in zip(folders, subs):
            main += struct.pack('<IHH', off, f.firstID, parent[id(f)])
            off += len(s)
        return bytes(main + b''.join(subs))

    def save(self, path, pad_to=None):
        out = bytearray(self.header)

        def align(n=0x200):
            while len(out) % n:
                out.append(0xFF)

        h = out
        def put(o, v):
            struct.pack_into('<I', h, o, v)
        align()
        put(0x20, len(out)); put(0x2C, len(self.arm9))
        out += self.arm9 + self.arm9post
        align()
        put(0x50, len(out)); put(0x54, len(self.y9))
        out += self.y9
        align()
        # file data for overlays will be in the main file area
        fnt = self._build_fnt()
        put(0x30, len(out)); put(0x3C, len(self.arm7))
        out += self.arm7
        align()
        put(0x58, len(out) if self.y7 else 0); put(0x5C, len(self.y7))
        out += self.y7
        align()
        put(0x40, len(out)); put(0x44, len(fnt))
        out += fnt
        align()
        fat_off = len(out)
        put(0x48, fat_off); put(0x4C, len(self.files) * 8)
        out += bytes(len(self.files) * 8)
        align()
        put(0x68, len(out))
        out += self.banner
        align()
        for i, f in enumerate(self.files):
            s = len(out)
            out += f
            struct.pack_into('<II', out, fat_off + i * 8, s, s + len(f))
            align()
        put(0x80, len(out))
        struct.pack_into('<H', out, 0x15E, crc16(out[:0x15E]))
        if pad_to:
            out += b'\xff' * (pad_to - len(out))
        open(path, 'wb').write(out)
