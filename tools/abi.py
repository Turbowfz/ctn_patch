# SPDX-License-Identifier: GPL-2.0-only
"""Bounded ELF64/LE and BTF readers used by CTNP baseline and build checks."""
from __future__ import annotations

import hashlib
import struct
from pathlib import Path


class FormatError(ValueError):
    pass


def sha256(path):
    h = hashlib.sha256()
    with open(path, 'rb') as stream:
        for chunk in iter(lambda: stream.read(1024 * 1024), b''):
            h.update(chunk)
    return h.hexdigest()


def cstring(data, offset):
    if not 0 <= offset < len(data):
        raise FormatError('string offset outside string table')
    end = data.find(b'\0', offset)
    if end < 0:
        raise FormatError('unterminated ELF/BTF string')
    return data[offset:end].decode('utf-8', 'strict')


def slice_bytes(data, offset, size):
    if offset < 0 or size < 0 or offset > len(data) or size > len(data) - offset:
        raise FormatError('range outside file')
    return data[offset:offset + size]


class Elf:
    def __init__(self, path):
        self.path = Path(path)
        self.data = d = self.path.read_bytes()
        if len(d) < 64 or d[:6] != b'\x7fELF\x02\x01':
            raise FormatError('expected ELF64 little-endian')
        self.machine = struct.unpack_from('<H', d, 18)[0]
        self.type = struct.unpack_from('<H', d, 16)[0]
        shoff = struct.unpack_from('<Q', d, 40)[0]
        entry_size, count, string_index = struct.unpack_from('<HHH', d, 58)
        if entry_size != 64 or not count or string_index >= count:
            raise FormatError('unsupported ELF section header table')
        slice_bytes(d, shoff, count * entry_size)
        self.sections = []
        for i in range(count):
            v = struct.unpack_from('<IIQQQQIIQQ', d, shoff + i * entry_size)
            section = dict(index=i, name_offset=v[0], type=v[1], flags=v[2], address=v[3],
                           offset=v[4], size=v[5], link=v[6], info=v[7], align=v[8], entry_size=v[9])
            if section['type'] != 8:
                slice_bytes(d, section['offset'], section['size'])
            self.sections.append(section)
        names = self.blob(self.sections[string_index])
        self.by_name = {}
        for section in self.sections:
            section['name'] = cstring(names, section['name_offset'])
            if section['name'] in self.by_name and section['name']:
                raise FormatError('duplicate ELF section name')
            self.by_name[section['name']] = section
        self.symbols = []
        self.symbol_tables = {}
        for table in self.sections:
            if table['type'] != 2:
                continue
            if table['entry_size'] != 24 or table['size'] % 24 or table['link'] >= count:
                raise FormatError('invalid symbol table')
            strings = self.blob(self.sections[table['link']])
            symbols = []
            for i in range(table['size'] // 24):
                no, info, other, section_index, value, size = struct.unpack_from('<IBBHQQ', d, table['offset'] + i * 24)
                sym = dict(index=i, name=cstring(strings, no), binding=info >> 4, type=info & 15,
                           visibility=other, section_index=section_index, value=value, size=size)
                sym['section'] = self.sections[section_index]['name'] if section_index < count else None
                symbols.append(sym)
            self.symbols.extend(symbols)
            self.symbol_tables[table['index']] = symbols

    def blob(self, section):
        if isinstance(section, str):
            if section not in self.by_name:
                raise FormatError('missing section: ' + section)
            section = self.by_name[section]
        if section['type'] == 8:
            raise FormatError('NOBITS section has no stored bytes')
        return slice_bytes(self.data, section['offset'], section['size'])

    def info(self):
        result = {}
        for item in self.blob('.modinfo').split(b'\0'):
            if not item:
                continue
            if b'=' not in item:
                raise FormatError('malformed modinfo')
            key, value = item.decode().split('=', 1)
            result.setdefault(key, []).append(value)
        return result

    def one_info(self, name):
        values = self.info().get(name, [])
        if len(values) != 1:
            raise FormatError('expected one modinfo ' + name)
        return values[0]

    def symbol(self, name):
        found = [s for s in self.symbols if s['name'] == name and s['section_index'] != 0]
        if len(found) != 1:
            raise FormatError('expected one defined symbol ' + name)
        return found[0]

    def symbol_bytes(self, symbol):
        if isinstance(symbol, str):
            symbol = self.symbol(symbol)
        if symbol['section_index'] >= len(self.sections):
            raise FormatError('symbol has no file section')
        raw = self.blob(self.sections[symbol['section_index']])
        return slice_bytes(raw, symbol['value'], symbol['size'])

    def kcfi(self, name):
        absolute = [s for s in self.symbols if s['name'] == '__kcfi_typeid_' + name and s['section_index'] == 0xfff1]
        if len(absolute) == 1:
            return absolute[0]['value'] & 0xffffffff
        sym = self.symbol(name)
        if sym['type'] != 2 or sym['value'] < 4 or sym['section_index'] >= len(self.sections):
            raise FormatError('no KCFI prefix for ' + name)
        raw = self.blob(self.sections[sym['section_index']])
        return struct.unpack('<I', slice_bytes(raw, sym['value'] - 4, 4))[0]

    def versions(self):
        result = {}
        if '__versions' in self.by_name:
            raw = self.blob('__versions')
            if len(raw) % 64:
                raise FormatError('invalid __versions size')
            for i in range(0, len(raw), 64):
                value = struct.unpack_from('<Q', raw, i)[0]
                name = cstring(raw[i + 8:i + 64], 0)
                if not name or value > 0xffffffff or (name in result and result[name] != value):
                    raise FormatError('invalid/conflicting symbol CRC')
                result[name] = value
        ext = ('__version_ext_crcs', '__version_ext_names')
        if any(n in self.by_name for n in ext):
            if not all(n in self.by_name for n in ext):
                raise FormatError('incomplete extended symbol versions')
            crcs = self.blob(ext[0])
            names = self.blob(ext[1]).split(b'\0')
            if names[-1] == b'':
                names.pop()
            if len(crcs) != 4 * len(names):
                raise FormatError('extended versions size mismatch')
            for i, rawname in enumerate(names):
                name = rawname.decode()
                value = struct.unpack_from('<I', crcs, i * 4)[0]
                if name in result and result[name] != value:
                    raise FormatError('conflicting extended CRC')
                result[name] = value
        return result

    def relocations(self, target=None):
        records = []
        for section in self.sections:
            if section['type'] != 4 or (target and self.sections[section['info']]['name'] != target):
                continue
            if section['entry_size'] != 24 or section['size'] % 24 or section['link'] not in self.symbol_tables:
                raise FormatError('invalid RELA section')
            symbols = self.symbol_tables[section['link']]
            for i in range(section['size'] // 24):
                offset, info, addend = struct.unpack_from('<QQq', self.data, section['offset'] + i * 24)
                index = info >> 32
                if index >= len(symbols):
                    raise FormatError('bad relocation symbol index')
                records.append(dict(section=self.sections[section['info']]['name'], offset=offset,
                                    type=info & 0xffffffff, symbol=symbols[index], addend=addend))
        return records

    def undefined(self):
        return {s['name'] for s in self.symbols if s['section_index'] == 0 and s['name'] and s['binding'] in (1, 2)}


class Btf:
    _extra = {0: 0, 1: 4, 2: 0, 3: 12, 7: 0, 8: 0, 9: 0, 10: 0, 11: 0,
              12: 0, 14: 4, 16: 0, 17: 4, 18: 0}
    _stride = {4: 12, 5: 12, 6: 8, 13: 8, 15: 12, 19: 12}

    def __init__(self, path):
        d = Path(path).read_bytes()
        if len(d) < 24:
            raise FormatError('short BTF header')
        magic, version, flags, h, to, tl, so, sl = struct.unpack_from('<HBBIIIII', d)
        if magic != 0xeb9f or version != 1 or flags != 0 or h < 24:
            raise FormatError('expected BTF v1 little-endian')
        raw = slice_bytes(d, h + to, tl)
        strings = slice_bytes(d, h + so, sl)
        p = 0
        self.types = [dict(kind=0, name='void', size=0)]
        while p < len(raw):
            no, info, size = struct.unpack('<III', slice_bytes(raw, p, 12))
            p += 12
            kind, count, flag = (info >> 24) & 31, info & 65535, info >> 31
            if kind in self._extra:
                n = self._extra[kind]
            elif kind in self._stride:
                n = self._stride[kind] * count
            else:
                raise FormatError('unknown BTF kind ' + str(kind))
            payload = slice_bytes(raw, p, n)
            p += n
            t = dict(id=len(self.types), name=cstring(strings, no), kind=kind, size=size, vlen=count)
            if kind in (4, 5):
                t['members'] = []
                for i in range(count):
                    mn, mt, mo = struct.unpack_from('<III', payload, i * 12)
                    t['members'].append(dict(name=cstring(strings, mn), type_id=mt,
                                             offset_bits=(mo & 0xffffff) if flag else mo,
                                             bitfield_size=(mo >> 24) if flag else 0))
            if kind == 13:
                t['params'] = [struct.unpack_from('<II', payload, i * 8)[1] for i in range(count)]
            self.types.append(t)

    def structure(self, name):
        found = [t for t in self.types if t['name'] == name and t['kind'] in (4, 5)]
        if not found:
            raise FormatError('BTF missing structure ' + name)
        layouts = [dict(size=t['size'], members={m['name']: m['offset_bits'] // 8 for m in t['members']}) for t in found]
        if any(layout != layouts[0] for layout in layouts[1:]):
            raise FormatError('BTF has conflicting layouts for ' + name)
        return layouts[0]
