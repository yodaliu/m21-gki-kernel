#!/usr/bin/env python3
"""Fail-closed, offline stock-module compatibility checks (Python stdlib only).

Scope: ELF64 little-endian AArch64 Linux 5.15 modules, CONFIG_MODVERSIONS=y,
CONFIG_MODULE_REL_CRCS=n, PREL32 kernel_symbol entries, MODULE_SIG=n. This is
not a boot/probe simulator, a full relocation checker, or a signature verifier.
Nothing is mounted, built, patched, or executed; only --json writes a report.

Usage:
  python3 wsl-verify-all-modules.py --kernel-config out/.config \
      out/vmlinux.symvers [other/Module.symvers ...] --audio-mode legacy \
      --json verification.json
One --kernel-config applies to all candidates; repeat it once per candidate
when their configs differ. Original symvers positional arguments and module
root defaults are retained. In-tree =m exports are NOT kernel providers.

Exit codes: 0 = every candidate PASS, 1 = at least one compatibility FAIL,
2 = CLI/read/parse/unsupported-input/report-write ERROR (never a silent PASS).
Stage checks follow deployed Android 13 libmodprobe modules.dep owner paths,
raw alias requests, optional modules.softdep attempts, and separate hard/load-only
blocklists. .modinfo depends/softdep NEVER manufacture runtime loader edges.
Vendor/system services run independently after first-stage; neither is assumed
ready for the other. Hard-dependency order, not a full partition export union,
is used to prove strong-provider availability. Runtime init/probe completion,
firmware, hardware, private behavior, and non-metadata ABI are not proven.
A missing SINGLE import CRC is a warning (5.15 check_version permits it), NOT
an ABI-verified reference. Entire missing/malformed version tables are rejected.
Only merged SHF_ALLOC __ksymtab / __ksymtab_gpl runtime tables are accepted.
Hard requests whose aliases change the load set are explicitly unsupported
(ERROR): a dependency graph alone cannot certify libmodprobe's process cache.

Loader semantics: android13-release system/core/libmodprobe/{libmodprobe.cpp,
libmodprobe_ext.cpp}; kernel/common commit 3540cf724bf0 kernel/module.c.
"""
from __future__ import annotations

import argparse
import collections
from dataclasses import dataclass, field
import fnmatch
import hashlib
import json
import os
from pathlib import Path, PurePosixPath
import re
import struct
import sys
from typing import NamedTuple

PARTS = ('vendor_boot', 'vendor_dlkm', 'system_dlkm')
DEFAULT_ROOTS = dict(zip(PARTS, ('/tmp/rv/rd/lib/modules', '/mnt/vdlkm', '/mnt/sdlkm')))
GPL_LICENSES = frozenset(('GPL', 'GPL v2', 'GPL and additional rights',
                          'Dual BSD/GPL', 'Dual MIT/GPL', 'Dual MPL/GPL'))
IDENTIFIER = re.compile(r'[A-Za-z_][A-Za-z0-9_]*\Z')
MODULE_NAME = re.compile(r'[A-Za-z0-9_][A-Za-z0-9_.-]*\Z')
SHN_UNDEF, SHN_ABS = 0, 0xfff1
R_AARCH64_ABS32, R_AARCH64_PREL32 = 258, 261


class InputError(ValueError):
    """Input is unreadable, malformed, ambiguous, or outside the supported scope."""


def require(condition, message):
    if not condition:
        raise InputError(message)


def digest(data):
    return hashlib.sha256(data).hexdigest()


def read_bytes(path):
    try:
        return Path(path).read_bytes()
    except OSError as exc:
        raise InputError(f'{path}: {exc}') from exc


def read_text(path):
    data = read_bytes(path)
    try:
        return data.decode('utf-8')
    except UnicodeError as exc:
        raise InputError(f'{path}: not UTF-8: {exc}') from exc


def module_token(token):
    name = PurePosixPath(token).name
    if name.endswith('.ko'):
        name = name[:-3]
    require(bool(MODULE_NAME.fullmatch(name)), f'invalid module token: {token!r}')
    return name.replace('-', '_')


@dataclass(frozen=True)
class Export:
    crc: int | None
    namespace: str = ''
    gpl_only: bool = False
    # A weak undefined __crc symbol is evaluated only after checking that it
    # cannot bind to an exported runtime symbol. Its ELF RELA addend is retained.
    weak_crc_symbol: str | None = None
    crc_addend: int = 0


@dataclass
class Module:
    path: str
    partition: str
    name: str
    filename: str = ''
    sha256: str = ''
    license: str = ''
    import_ns: frozenset[str] = frozenset()
    vermagic: str = ''
    depends: tuple[str, ...] = ()
    versions: dict[str, int] = field(default_factory=dict)
    undefined: dict[str, bool] = field(default_factory=dict)  # True = STB_WEAK
    exports: dict[str, Export] = field(default_factory=dict)
    metadata_weak_symbols: frozenset[str] = frozenset()
    softdep: tuple[str, ...] = ()


@dataclass(frozen=True)
class KernelConfig:
    values: dict[str, str]
    vermagic_flags: str
    allow_missing_namespace: bool


@dataclass
class Symvers:
    exports: dict[str, Export]
    intree_ignored: int = 0


@dataclass(frozen=True)
class DepEntry:
    path: str
    dependencies: tuple[str, ...] = ()


@dataclass(frozen=True)
class Softdep:
    owner: str  # Raw owner is compared EXACTLY by Android libmodprobe.
    kind: str   # 'pre' or 'post'; order and repeated owners are preserved.
    target: str
    line: int = 0


@dataclass
class StageSpec:
    partition: str
    load: tuple[str, ...]  # Raw requests, not normalized names (aliases match raw text).
    depfile: dict[str, DepEntry] = field(default_factory=dict)
    hard_blocked: frozenset[str] = frozenset()
    sources: tuple[str, ...] = ()
    module_dir: str = ''
    device_mappings: tuple[tuple[str, str], ...] = ()
    load_excluded: frozenset[str] = frozenset()
    softdeps: tuple[Softdep, ...] = ()
    aliases: tuple[tuple[str, str], ...] = ()
    source_hashes: dict[str, str] = field(default_factory=dict)
    hard_blocklist_sources: tuple[str, ...] = ()
    load_exclusion_sources: tuple[str, ...] = ()


@dataclass
class Inventory:
    modules: list[Module]
    stages: dict[str, StageSpec]
    roots: dict[str, str] = field(default_factory=dict)


def parse_kernel_config(text):
    values = {}
    for lineno, line in enumerate(text.splitlines(), 1):
        line = line.strip()
        unset = re.fullmatch(r'# (CONFIG_[A-Za-z0-9_]+) is not set', line)
        setting = re.fullmatch(r'(CONFIG_[A-Za-z0-9_]+)=(.+)', line)
        if unset:
            key, value = unset[1], 'n'
        elif setting:
            key, value = setting[1], setting[2]
        elif not line or line.startswith('#'):
            continue
        else:
            raise InputError(f'config line {lineno}: malformed setting')
        require(key not in values, f'config line {lineno}: duplicate {key}')
        values[key] = value
    require(values, 'empty kernel config')
    for key in ('ARM64', 'MODULES', 'MODVERSIONS'):
        require(values.get('CONFIG_' + key) == 'y', f'only CONFIG_{key}=y is supported')
    for key in ('SMP', 'PREEMPT', 'MODULE_UNLOAD', 'MODULE_SIG'):
        require(values.get('CONFIG_' + key) in ('y', 'n'), f'missing/non-boolean CONFIG_{key}')
    for key in ('CPU_BIG_ENDIAN', 'MODULE_REL_CRCS', 'GCC_PLUGIN_RANDSTRUCT',
                'RANDSTRUCT_FULL', 'RANDSTRUCT_PERFORMANCE', 'MODULE_SIG'):
        require(values.get('CONFIG_' + key, 'n') != 'y', f'CONFIG_{key}=y is outside supported verification scope')
    allow = values.get('CONFIG_MODULE_ALLOW_MISSING_NAMESPACE_IMPORTS', 'n')
    require(allow in ('y', 'n'), 'invalid CONFIG_MODULE_ALLOW_MISSING_NAMESPACE_IMPORTS')
    flags = []
    if values['CONFIG_SMP'] == 'y':
        flags.append('SMP')
    if values['CONFIG_PREEMPT'] == 'y' or values.get('CONFIG_PREEMPT_BUILD') == 'y':
        flags.append('preempt')
    elif values.get('CONFIG_PREEMPT_RT') == 'y':
        flags.append('preempt_rt')
    if values['CONFIG_MODULE_UNLOAD'] == 'y':
        flags.append('mod_unload')
    flags.extend(('modversions', 'aarch64'))
    return KernelConfig(values, ' '.join(flags), allow == 'y')


def parse_symvers(text):
    kernel, seen, ignored = {}, set(), 0
    for lineno, line in enumerate(text.splitlines(), 1):
        if not line.strip():
            continue
        fields = line.split('\t')
        require(len(fields) in (4, 5), f'symvers line {lineno}: expected 4/5 tab-separated columns')
        crc, name, owner, export_type = fields[:4]
        namespace = fields[4] if len(fields) == 5 else ''
        require(bool(re.fullmatch(r'0x[0-9a-fA-F]{1,8}', crc)), f'symvers line {lineno}: invalid CRC')
        require(bool(IDENTIFIER.fullmatch(name)) and bool(owner), f'symvers line {lineno}: invalid name/owner')
        require(export_type in ('EXPORT_SYMBOL', 'EXPORT_SYMBOL_GPL'), f'symvers line {lineno}: unsupported export class {export_type!r}')
        require(not any(ord(c) < 32 for c in namespace), f'symvers line {lineno}: invalid namespace')
        require((name, owner) not in seen, f'symvers line {lineno}: duplicate {owner}:{name}')
        seen.add((name, owner))
        if owner == 'vmlinux':
            kernel[name] = Export(int(crc, 16), namespace, export_type == 'EXPORT_SYMBOL_GPL')
        else:
            ignored += 1
    require(kernel, 'symvers has no vmlinux exports')
    require('module_layout' in kernel, 'MODVERSIONS kernel symvers lacks module_layout')
    return Symvers(kernel, ignored)


class Section(NamedTuple):
    name: str
    kind: int
    flags: int
    offset: int
    size: int
    link: int
    info: int
    entsize: int


class Symbol(NamedTuple):
    name: str
    binding: int
    kind: int
    section: int
    value: int
    size: int


class Relocation(NamedTuple):
    kind: int
    symbol: int
    addend: int


def cstring(data, offset, label):
    require(0 <= offset < len(data), f'{label}: string offset outside section')
    end = data.find(b'\0', offset)
    require(end >= 0, f'{label}: unterminated string')
    try:
        return data[offset:end].decode('utf-8')
    except UnicodeError as exc:
        raise InputError(f'{label}: invalid UTF-8 string') from exc


class ELF64:
    """Bounds-checked minimal ELF reader; reads metadata, never executes tools."""
    def __init__(self, data):
        self.data = data
        require(len(data) >= 64, 'ELF header truncated')
        require(data[:7] == b'\x7fELF\x02\x01\x01', 'only ELF64 little-endian ELF version 1 is supported')
        header = struct.unpack_from('<16sHHIQQQIHHHHHH', data)
        require(header[1:4] == (1, 183, 1), 'only AArch64 ET_REL modules are supported')
        shoff, ehsize, shsize, count, names_index = header[6], header[8], header[11], header[12], header[13]
        require(ehsize == 64 and shsize == 64 and count > 0, 'invalid/unsupported ELF section header layout')
        require(0 < names_index < count and shoff >= 64 and shoff + count * 64 <= len(data), 'ELF section table outside file')
        raw = [struct.unpack_from('<IIQQQQIIQQ', data, shoff + i * 64) for i in range(count)]
        for i, s in enumerate(raw):
            if s[1] not in (0, 8):  # SHT_NULL / SHT_NOBITS have no file payload.
                require(s[4] <= len(data) and s[5] <= len(data) - s[4], f'section {i} outside ELF file')
        names = raw[names_index]
        require(names[1] == 3, 'section-name table is not SHT_STRTAB')
        names_data = data[names[4]:names[4] + names[5]]
        self.sections = [Section(cstring(names_data, s[0], 'section name'), s[1], s[2], s[4], s[5], s[6], s[7], s[9]) for s in raw]
        self.by_name = {}
        for i, sec in enumerate(self.sections):
            # Distinct relocation sections may retain their pre-link names; use
            # sh_info (target), never their textual names, to apply relocations.
            if sec.name in ('.modinfo', '__versions', '.symtab') or sec.name.startswith(('__ksymtab', '___ksymtab', '__kcrctab', '___kcrctab')):
                require(sec.name not in self.by_name, f'ambiguous metadata section {sec.name}')
            self.by_name.setdefault(sec.name, i)
        require('.symtab' in self.by_name, 'missing .symtab')
        self.symtab_index = self.by_name['.symtab']
        tab = self.sections[self.symtab_index]
        require(tab.kind == 2 and tab.entsize == 24 and tab.size >= 24 and tab.size % 24 == 0, 'invalid .symtab')
        require(0 < tab.link < count and self.sections[tab.link].kind == 3, 'invalid symbol string table')
        strings = self.section_data(tab.link)
        self.symbols = []
        self.named_symbols = collections.defaultdict(list)
        for pos in range(tab.offset, tab.offset + tab.size, 24):
            name, info, other, index, value, size = struct.unpack_from('<IBBHQQ', data, pos)
            require(index < count or index == SHN_ABS, 'unsupported/reserved symbol section index')
            sym = Symbol(cstring(strings, name, 'symbol name'), info >> 4, info & 15, index, value, size)
            self.named_symbols[sym.name].append(len(self.symbols))
            self.symbols.append(sym)
        self._relocations = {}

    def section_data(self, index):
        sec = self.sections[index]
        require(sec.kind not in (0, 8), f'{sec.name}: metadata cannot be NOBITS/NULL')
        return self.data[sec.offset:sec.offset + sec.size]

    def named_data(self, name):
        require(name in self.by_name, f'missing {name}')
        return self.section_data(self.by_name[name])

    def relocations(self, target):
        if target not in self._relocations:
            result = {}
            for sec in self.sections:
                if sec.kind not in (4, 9) or sec.info != target:
                    continue
                require(sec.kind == 4, 'SHT_REL metadata relocations are unsupported; expected RELA')
                require(sec.link == self.symtab_index and sec.entsize == 24 and sec.size % 24 == 0, 'invalid metadata RELA section')
                for pos in range(sec.offset, sec.offset + sec.size, 24):
                    offset, info, addend = struct.unpack_from('<QQq', self.data, pos)
                    require(offset + 4 <= self.sections[target].size, 'metadata relocation outside target section')
                    require(info >> 32 < len(self.symbols), 'invalid relocation symbol index')
                    require(offset not in result, 'overlapping metadata relocations')
                    result[offset] = Relocation(info & 0xffffffff, info >> 32, addend)
            self._relocations[target] = result
        return self._relocations[target]

    def prel32_string(self, target, offset):
        reloc = self.relocations(target).get(offset)
        require(reloc is not None and reloc.kind == R_AARCH64_PREL32, 'missing/unsupported ksymtab string PREL32 relocation')
        sym = self.symbols[reloc.symbol]
        require(0 < sym.section < len(self.sections), 'ksymtab string refers to undefined/absolute symbol')
        require(bool(self.sections[sym.section].flags & 2), 'ksymtab string must reside in a SHF_ALLOC section')
        return cstring(self.section_data(sym.section), sym.value + reloc.addend, 'export string')


def export_section_class(name):
    # Linux 5.15 reads these exact, allocated runtime tables. A pre-link
    # ___ksymtab+symbol input section is NOT an alternative runtime provider.
    # Also never classify an export by a "gpl" substring in its symbol name.
    if name == '__ksymtab_gpl':
        return True, '__kcrctab_gpl'
    if name == '__ksymtab':
        return False, '__kcrctab'
    raise InputError(f'unsupported/unmerged export section (not a 5.15 runtime table): {name}')


def parse_export_crc(elf, section_index, offset, export_name):
    table = elf.section_data(section_index)
    require(offset >= 0 and offset + 4 <= len(table), 'missing kcrctab entry')
    relocation = elf.relocations(section_index).get(offset)
    expected = '__crc_' + export_name
    if relocation is None:
        value = struct.unpack_from('<I', table, offset)[0]
        for index in elf.named_symbols.get(expected, ()):
            sym = elf.symbols[index]
            if sym.section == SHN_ABS:
                require(sym.value == value, f'{expected}: literal kcrctab disagrees with absolute CRC symbol')
        return value, None, 0
    require(relocation.kind == R_AARCH64_ABS32, 'relative/unsupported module CRC relocation; MODULE_REL_CRCS must be n')
    sym = elf.symbols[relocation.symbol]
    require(sym.name == expected, f'kcrctab references {sym.name!r}, expected {expected!r}')
    if sym.section == SHN_ABS:
        value = sym.value + relocation.addend
        require(0 <= sym.value <= 0xffffffff and -0x80000000 <= value <= 0xffffffff, 'absolute CRC relocation overflow')
        return value & 0xffffffff, None, 0
    require(sym.section == SHN_UNDEF and sym.binding == 2 and sym.value == 0, 'CRC relocation is neither absolute nor weak-undefined-zero')
    require(-0x80000000 <= relocation.addend <= 0xffffffff, 'weak CRC relocation addend overflow')
    # A weak undefined symbol is NOT automatically an exported CRC of zero:
    # retain S + A until runtime-provider lookup can establish that S stays zero.
    return None, sym.name, relocation.addend


def parse_module(data, path='<memory>', partition='vendor_boot'):
    try:
        require(partition in PARTS, f'unknown partition {partition}')
        elf = ELF64(data)
        for sec in elf.sections:
            require(not sec.name.startswith(('___ksymtab+', '___ksymtab_gpl+', '___kcrctab+', '___kcrctab_gpl+')),
                    f'{sec.name}: unmerged export input section is not consumed by the Linux 5.15 module loader')
            if sec.name in ('__ksymtab', '__ksymtab_gpl', '__kcrctab', '__kcrctab_gpl'):
                require(sec.kind == 1 and bool(sec.flags & 2), f'{sec.name}: runtime export/CRC table must be PROGBITS with SHF_ALLOC')
        info_data = elf.named_data('.modinfo')
        require(info_data and info_data[-1] == 0, 'empty/unterminated .modinfo')
        info = collections.defaultdict(list)
        for item in info_data.split(b'\0'):
            if not item:
                continue
            require(b'=' in item, 'malformed .modinfo entry')
            key, value = item.decode('utf-8').split('=', 1)
            info[key].append(value)
        for key in ('name', 'vermagic'):
            require(len(info[key]) == 1 and bool(info[key][0]), f'missing/duplicate .modinfo {key}')
        name = info['name'][0]
        require(bool(MODULE_NAME.fullmatch(name)), 'invalid module name')
        name = name.replace('-', '_')
        require(len(info['depends']) <= 1, 'multiple depends fields are ambiguous')
        depends = tuple(module_token(x.strip()) for x in info.get('depends', [''])[0].split(',') if x.strip())
        version_data = elf.named_data('__versions')
        require(version_data and len(version_data) % 64 == 0, 'empty/malformed ARM64 __versions')
        versions = {}
        for offset in range(0, len(version_data), 64):
            record = version_data[offset:offset + 64]
            crc = struct.unpack_from('<Q', record)[0]
            symbol = cstring(record, 8, '__versions')
            require(bool(IDENTIFIER.fullmatch(symbol)) and crc <= 0xffffffff, 'invalid __versions symbol/CRC')
            require(symbol not in versions, f'duplicate version reference: {symbol}')
            versions[symbol] = crc
        require('module_layout' in versions, 'MODVERSIONS module lacks module_layout')
        undefined = {}
        for sym in elf.symbols:
            if sym.section == SHN_UNDEF and sym.name and sym.binding in (1, 2):
                require(sym.name not in undefined, f'ambiguous undefined symbol {sym.name}')
                undefined[sym.name] = sym.binding == 2
        exports, weak_metadata = {}, set()
        export_offsets = collections.defaultdict(set)
        for sym in elf.symbols:
            if not sym.name.startswith('__ksymtab_') or sym.section in (SHN_UNDEF, SHN_ABS) or sym.kind == 3:
                continue
            symbol = sym.name[len('__ksymtab_'):]
            require(bool(IDENTIFIER.fullmatch(symbol)), 'invalid export symbol name')
            sec = elf.sections[sym.section]
            gpl_only, crc_section = export_section_class(sec.name)
            require(sym.value % 12 == 0 and sym.size in (0, 12) and sym.value + 12 <= sec.size, 'unsupported kernel_symbol layout (expected three PREL32 fields)')
            value_reloc = elf.relocations(sym.section).get(sym.value)
            require(value_reloc is not None and value_reloc.kind == R_AARCH64_PREL32, 'missing/unsupported export value relocation')
            actual_name = elf.prel32_string(sym.section, sym.value + 4)
            namespace = elf.prel32_string(sym.section, sym.value + 8)
            require(actual_name == symbol, f'ksymtab name mismatch: {symbol} vs {actual_name}')
            require(symbol not in exports, f'duplicate module export {symbol}')
            require(crc_section in elf.by_name, f'missing {crc_section}')
            crc_index = elf.by_name[crc_section]
            require(elf.sections[crc_index].size == sec.size // 12 * 4 and sec.size % 12 == 0, 'ksymtab/kcrctab entry counts disagree')
            crc, weak, addend = parse_export_crc(elf, crc_index, sym.value // 12 * 4, symbol)
            exports[symbol] = Export(crc, namespace, gpl_only, weak, addend)
            export_offsets[sym.section].add(sym.value)
            if weak:
                weak_metadata.add(weak)
        for index, sec in enumerate(elf.sections):
            if sec.name in ('__ksymtab', '__ksymtab_gpl') or sec.name.startswith(('___ksymtab+', '___ksymtab_gpl+')):
                require(sec.size % 12 == 0 and export_offsets[index] == set(range(0, sec.size, 12)), 'export table has undecoded/missing entries')
        result = Module(str(path), partition, name, Path(path).name, digest(data),
                        info.get('license', [''])[0], frozenset(info.get('import_ns', [])),
                        info['vermagic'][0], depends, versions, undefined, exports,
                        frozenset(weak_metadata), tuple(info.get('softdep', [])))
        validate_module(result)
        return result
    except (InputError, UnicodeError, struct.error, IndexError) as exc:
        raise InputError(f'{path}: {exc}') from exc


def vermagic_tail(vermagic):
    release, separator, tail = vermagic.partition(' ')
    require(release and separator and tail and not any(c in vermagic for c in '\r\n\0'), 'invalid vermagic')
    return tail


def validate_module(module):
    require(module.partition in PARTS and bool(MODULE_NAME.fullmatch(module.name)), 'invalid module identity/partition')
    require(module.versions and 'module_layout' in module.versions, f'{module.path}: missing effective version data/module_layout')
    require(all(isinstance(v, int) and 0 <= v <= 0xffffffff for v in module.versions.values()), f'{module.path}: invalid CRC value')
    vermagic_tail(module.vermagic)


def parse_load_list(text):
    result = []
    for lineno, line in enumerate(text.splitlines(), 1):
        words = line.split('#', 1)[0].split()
        if not words:
            continue
        require(len(words) == 1, f'modules.load line {lineno}: expected one module, no options')
        loader_canonical(words[0])  # Validate, but retain the raw alias/path request.
        result.append(words[0])
    require(result, 'empty modules.load')
    return tuple(result)


def parse_depfile(text):
    result = {}
    for lineno, line in enumerate(text.splitlines(), 1):
        line = line.split('#', 1)[0].strip()
        if not line:
            continue
        owner, colon, rest = line.partition(':')
        require(colon and len(owner.split()) == 1, f'modules.dep line {lineno}: malformed record')
        key = loader_canonical(owner)
        require(bool(MODULE_NAME.fullmatch(key)), f'modules.dep line {lineno}: unsupported owner name')
        require(key not in result, f'modules.dep line {lineno}: duplicate/ambiguous module {key}')
        dependencies = tuple(rest.split())
        for token in (owner, *dependencies):
            require('\\' not in token and '\0' not in token and '..' not in PurePosixPath(token).parts,
                    f'modules.dep line {lineno}: unsupported path {token!r}')
            loader_canonical(token)
        require(owner.endswith('.ko'), f'modules.dep line {lineno}: owner must identify a .ko file')
        # Android canonicalizes RHS tokens and looks up each target's OWN row;
        # only that row's owner path is stat/opened. Keep both pieces of evidence.
        result[key] = DepEntry(owner, dependencies)
    require(result, 'empty modules.dep; .modinfo does not replace the deployed loader graph')
    return result


def parse_blocklist(text):
    result = set()
    for lineno, line in enumerate(text.splitlines(), 1):
        words = line.split('#', 1)[0].split()
        if not words:
            continue
        require(len(words) == 2 and words[0] in ('blocklist', 'blacklist'), f'blocklist line {lineno}: unsupported directive')
        result.add(module_token(words[1]))
    return frozenset(result)


def loader_canonical(token):
    """Android MakeCanonical: filename spelling, not .modinfo's internal name."""
    require(isinstance(token, str) and token and '\0' not in token, 'empty/invalid loader request')
    name = token.rsplit('/', 1)[-1]
    if name.endswith('.ko'):
        name = name[:-3]
    require(len(name) > 1, f'Android libmodprobe rejects this module request: {token!r}')
    return name.replace('-', '_')


def parse_softdep_file(text):
    entries = []
    for lineno, line in enumerate(text.splitlines(), 1):
        words = line.split('#', 1)[0].split()
        if not words:
            continue
        require(len(words) >= 4 and words[0] == 'softdep', f'modules.softdep line {lineno}: malformed directive')
        owner, state = words[1], None
        for word in words[2:]:
            if word in ('pre:', 'post:'):
                state = word[:-1]
            else:
                require(state is not None, f'modules.softdep line {lineno}: target before pre:/post:')
                loader_canonical(word)
                entries.append(Softdep(owner, state, word, lineno))
    return tuple(entries)


def parse_alias_file(text):
    aliases = []
    for lineno, line in enumerate(text.splitlines(), 1):
        words = line.split('#', 1)[0].split()
        if not words:
            continue
        require(len(words) == 3 and words[0] == 'alias', f'modules.alias line {lineno}: malformed directive')
        pattern, target = words[1:]
        require('\\' not in pattern and '[[:' not in pattern and '[.' not in pattern and '[=' not in pattern and '[^' not in pattern,
                f'modules.alias line {lineno}: unsupported POSIX fnmatch pattern')
        aliases.append((pattern, target))  # Alias targets also remain raw in libmodprobe.
    return tuple(aliases)


def parse_load_exclusions(text):
    patterns = []
    for lineno, line in enumerate(text.splitlines(), 1):
        words = line.split('#', 1)[0].split()
        if not words:
            continue
        require(len(words) == 2 and words[0] == 'blocklist', f'load-exclusion line {lineno}: unsupported directive')
        require(bool(MODULE_NAME.fullmatch(words[1])), f'load-exclusion line {lineno}: unsupported grep expression')
        # The deployed shell scripts transform '-' to '_' in the pattern, not
        # in modules.load. Preserve raw load tokens so grep -w is not invented.
        patterns.append(words[1].replace('-', '_'))
    return frozenset(patterns)


def excluded_load_request(token, patterns):
    return any(re.search(r'(?<![A-Za-z0-9_])' + p + r'(?![A-Za-z0-9_])', token) for p in patterns)


def device_path_mappings(partition, module_dir):
    directory = Path(module_dir)
    if partition == 'vendor_boot':
        return (('/lib/modules', str(directory)),)
    if partition == 'vendor_dlkm':
        # Firmware loader paths are explicit aliases of this same deployed
        # partition, not a basename-only search through arbitrary directories.
        return (('/vendor/lib/modules', str(directory)), ('/vendor_dlkm/lib/modules', str(directory)))
    for parent in (directory, *directory.parents):
        if parent.name == 'modules' and parent.parent.name == 'lib':
            return (('/system_dlkm/lib/modules', str(parent)),)
    # Relative system modules.dep paths need no mapping. An absolute path from
    # a renamed/non-standard capture is rejected rather than guessed by basename.
    return ()


def scan_root(root):
    root = Path(root)
    require(root.is_dir(), f'module root is missing/not a directory: {root}')
    kos, loads = [], []
    def onerror(exc):
        raise InputError(f'cannot enumerate {root}: {exc}')
    for directory, dirs, files in os.walk(root, onerror=onerror, followlinks=False):
        dirs.sort()
        for name in dirs:
            require(not (Path(directory) / name).is_symlink(), f'unscanned directory symlink under module root: {Path(directory) / name}')
        for name in sorted(files):
            path = Path(directory) / name
            require(not name.endswith(('.ko.xz', '.ko.gz', '.ko.zst')), f'compressed modules unsupported: {path}')
            if name.endswith('.ko'):
                require(not path.is_symlink(), f'Android finit_module opens with O_NOFOLLOW; symlink .ko unsupported: {path}')
                require(path.is_file(), f'module input must be a regular file, not a FIFO/device/directory: {path}')
                kos.append(path)
            elif name == 'modules.load':
                loads.append(path)
    require(kos, f'no .ko files in required root: {root}')
    require(len(loads) == 1, f'{root}: expected exactly one modules.load, found {len(loads)}')
    return kos, loads[0]


def load_inventory(roots, audio_mode):
    scans = {part: scan_root(roots[part]) for part in PARTS}
    modules = [parse_module(read_bytes(path), str(path), part)
               for part in PARTS for path in scans[part][0]]
    stages = {}
    vendor_dir = scans['vendor_dlkm'][1].parent
    for part in PARTS:
        load_file = scans[part][1]
        source_hashes = {}
        def text_source(path):
            data = read_bytes(path)
            source_hashes[str(path)] = digest(data)
            return data.decode('utf-8')
        # This file is runtime input, not an optimization. Never manufacture
        # loader dependencies from .modinfo when it is missing or incomplete.
        dep_file = load_file.parent / 'modules.dep'
        deps = parse_depfile(text_source(dep_file))
        loads = parse_load_list(text_source(load_file))
        hard_sources, exclusion_sources = [], []
        hard_blocked, load_excluded = set(), set()
        block_file = load_file.parent / 'modules.blocklist'
        if block_file.exists():
            block_text = text_source(block_file)
            hard_blocked.update(parse_blocklist(block_text))
            hard_sources.append(str(block_file))
            if part == 'vendor_dlkm':
                load_excluded.update(parse_load_exclusions(block_text))
                exclusion_sources.append(str(block_file))
        extra_files = []
        if part == 'vendor_dlkm':
            audio = 'ar' if audio_mode == 'audioreach' else 'legacy'
            extra_files.append(load_file.parent / f'modules.audio.{audio}.blocklist')
        if part == 'system_dlkm':
            extra_files.append(vendor_dir / 'system_dlkm.modules.blocklist')
        for path in extra_files:
            if path.exists():
                load_excluded.update(parse_load_exclusions(text_source(path)))
                exclusion_sources.append(str(path))
        soft_file, alias_file = load_file.parent / 'modules.softdep', load_file.parent / 'modules.alias'
        softdeps = parse_softdep_file(text_source(soft_file)) if soft_file.exists() else ()
        aliases = parse_alias_file(text_source(alias_file)) if alias_file.exists() else ()
        stages[part] = StageSpec(part, loads, deps, frozenset(hard_blocked), tuple(source_hashes),
                                 str(load_file.parent), device_path_mappings(part, load_file.parent),
                                 frozenset(load_excluded), softdeps, aliases, source_hashes,
                                 tuple(hard_sources), tuple(exclusion_sources))
    return Inventory(modules, stages, {k: str(v) for k, v in roots.items()})


def finding(code, message, severity='error', **context):
    return dict(severity=severity, code=code, message=message, **context)


def module_context(module):
    return {'module': module.name, 'file': module.path, 'partition': module.partition}


def metadata_signature(module):
    # Release text and ELF/debug/build bytes are not a substitute for load
    # metadata; with valid MODVERSIONS, the loader ignores release text.
    return (module.license, tuple(sorted(module.import_ns)), vermagic_tail(module.vermagic),
            tuple(sorted(module.depends)), tuple(sorted(module.versions.items())),
            tuple(sorted(module.undefined.items())), tuple(sorted(module.exports.items())),
            tuple(sorted(module.softdep)))


def group_modules(modules):
    groups = collections.defaultdict(list)
    for module in modules:
        validate_module(module)
        groups[module.name].append(module)
    return dict(groups)


def effective_crc(export, all_exported_names):
    if export.weak_crc_symbol is None:
        require(export.crc is not None and 0 <= export.crc <= 0xffffffff, 'export lacks a valid CRC')
        return export.crc
    require(export.crc is None, 'ambiguous weak CRC expression')
    require(export.weak_crc_symbol not in all_exported_names,
            f'{export.weak_crc_symbol} could bind at load time; cannot infer its runtime address from symvers')
    # ELF SHN_UNDEF/STB_WEAK/st_value=0 was verified by the parser; no runtime
    # provider exists, so simplify_symbols leaves S=0 and ABS32 writes S + A.
    return export.crc_addend & 0xffffffff


def host_path_key(path):
    # Do not resolve device paths against the host filesystem or search by
    # basename. This key is only for paths of files actually scanned/read.
    return str(Path(path).absolute())


def owner_host_path(spec, owner):
    path = PurePosixPath(owner)
    if not path.is_absolute():
        return str(Path(spec.module_dir).joinpath(*path.parts))
    for device, host in sorted(spec.device_mappings, key=lambda pair: -len(pair[0])):
        try:
            relative = path.relative_to(PurePosixPath(device))
        except ValueError:
            continue
        return str(Path(host).joinpath(*relative.parts))
    return None


def resolve_stage_request(spec, token, files):
    canonical = loader_canonical(token)
    keys = {canonical}
    aliases = [(pattern, target) for pattern, target in spec.aliases if fnmatch.fnmatchcase(token, pattern)]
    keys.update(target for _, target in aliases)  # AOSP keeps alias targets raw.
    available, failures = [], []
    for key in sorted(keys):
        if key in spec.hard_blocked:
            failures.append({'name': key, 'reason': 'hard_blocklisted'})
            continue
        row = spec.depfile.get(key)
        if row is None:
            failures.append({'name': key, 'reason': 'no_deployed_dep_row'})
            continue
        host = owner_host_path(spec, row.path)
        module = files.get(host_path_key(host)) if host is not None else None
        if module is None:
            failures.append({'name': key, 'reason': 'owner_path_not_deployed',
                             'owner_path': row.path, 'mapped_host_path': host})
            continue
        available.append((key, module))
    return available, failures, aliases


def cyclic_nodes(nodes, edges):
    pending = {name: set(edges.get(name, ())) & set(nodes) for name in nodes}
    reverse = collections.defaultdict(set)
    ready = collections.deque(name for name, deps in pending.items() if not deps)
    for name, deps in pending.items():
        for dep in deps:
            reverse[dep].add(name)
    removed = set()
    while ready:
        name = ready.popleft()
        if name in removed:
            continue
        removed.add(name)
        for consumer in reverse[name]:
            pending[consumer].discard(name)
            if not pending[consumer]:
                ready.append(consumer)
    return set(nodes) - removed


def dependency_closure(name, edges):
    found, queue = set(), collections.deque(edges.get(name, ()))
    while queue:
        child = queue.popleft()
        if child in found:
            continue
        found.add(child)
        queue.extend(edges.get(child, ()))
    return found - {name}


def select_stage(spec, files, all_modules):
    runtime, edges, findings = {}, collections.defaultdict(set), []
    skipped_hard, skipped_extra, roots = set(), set(), []
    queue = collections.deque()
    filtered = []
    for token in spec.load:
        if excluded_load_request(token, spec.load_excluded):
            skipped_extra.add(token)
        else:
            filtered.append(token)
    first_request = filtered[0] if filtered and spec.partition != 'vendor_boot' else None
    first_row = None
    if spec.load and not filtered and spec.partition != 'vendor_boot':
        findings.append(finding('stage_empty_filtered_load_list',
                                'shell filtering leaves no first module; toolbox with an empty module list fails', stage=spec.partition))
    queue.extend((token, None, index == 0 and first_request is not None) for index, token in enumerate(filtered))
    while queue:
        token, requester, is_first = queue.popleft()
        choices, failures, aliases = resolve_stage_request(spec, token, files)
        require(not any(key != loader_canonical(token) for key, _ in choices),
                f'{spec.partition}: hard alias resolution for {token!r} requires process-cache/branch modeling; not certified')
        if not choices:
            canonical = loader_canonical(token)
            blocked = any(item['reason'] == 'hard_blocklisted' for item in failures)
            if requester is None and blocked and not is_first:
                # First-stage LoadListedModules skips blocklisted entries. A
                # later blocked shell child does not replace/fail the first
                # synchronous invocation. The TRUE first request must succeed.
                skipped_hard.add(canonical)
                continue
            elsewhere = sorted({m.partition for m in all_modules
                                if m.name == canonical or loader_canonical(m.filename or m.path) == canonical})
            blocked = any(item['reason'] == 'hard_blocklisted' for item in failures)
            code = 'stage_first_module_unloadable' if is_first else 'stage_dependency_blocklisted' if requester and blocked else 'stage_owner_path_missing' if any(item['reason'] == 'owner_path_not_deployed' for item in failures) else 'stage_late_dependency' if requester and elsewhere else 'stage_module_missing'
            findings.append(finding(code, 'runtime request has no loadable deployed modules.dep row/owner path; first-stage residency is not a new process cache',
                                    stage=spec.partition, module=runtime[requester].name if requester else None,
                                    requested=token, failures=failures, present_in=elsewhere))
            continue
        require(len(choices) == 1, f'{spec.partition}: multi-provider alias request {token!r} needs a dedicated success/failure model')
        key, module = choices[0]
        if requester is None:
            roots.append(key)
            if is_first:
                first_row = key
        else:
            edges[requester].add(key)
        if key in runtime:
            continue
        runtime[key] = module
        # InsmodWithDeps iterates RHS in reverse, resolves each RHS by canonical
        # name, and then stat/opens the target's own row path. Never substitute
        # .modinfo depends or the RHS textual path for that lookup.
        queue.extend((dep, key, False) for dep in reversed(spec.depfile[key].dependencies))
    cycles = cyclic_nodes(runtime, edges)
    if cycles:
        findings.append(finding('stage_dependency_cycle', 'deployed hard-dependency graph cannot complete',
                                stage=spec.partition, modules=sorted(cycles)))
    return runtime, edges, findings, skipped_hard, skipped_extra, roots, first_request, first_row


def inspect_softdeps(spec, runtime, hard_edges, files, base):
    """Optional attempts are observations, NEVER proof of a required provider.

    Android calls LoadWithAliases(..., false) and ignores both pre/post return
    values. Missing or blocked targets do not fail the owner. However reachable
    hard/pre recursion can prevent returning at all, so it is checked separately.
    A post edge does not create a recursion edge: its owner is already cached.
    """
    attempts, nodes, before_edges = [], dict(runtime), collections.defaultdict(set)
    for owner, children in hard_edges.items():
        before_edges[owner].update(children)
    base_aliases = {alias for m in base.values() for alias in (m.name, loader_canonical(m.filename or m.path))}
    queue, seen = collections.deque(runtime), set()
    while queue:
        owner = queue.popleft()
        if owner in seen:
            continue
        seen.add(owner)
        for token in spec.depfile[owner].dependencies:
            choices, _, _ = resolve_stage_request(spec, token, files)
            for child, module in choices:
                before_edges[owner].add(child)
                nodes[child] = module
                queue.append(child)
        for entry in spec.softdeps:
            if entry.owner != owner:  # Exact raw owner matching, like AOSP.
                continue
            choices, failures, aliases = resolve_stage_request(spec, entry.target, files)
            baseline_present = loader_canonical(entry.target) in base_aliases
            if choices:
                status = 'present_optional_not_used_as_hard_evidence'
            elif baseline_present:
                status = 'baseline_provider_present_but_local_attempt_unresolvable'
            elif any(f['reason'] == 'hard_blocklisted' for f in failures):
                status = 'blocked_optional'
            else:
                status = 'missing_optional'
            attempts.append({'owner': owner, 'owner_module': nodes[owner].name,
                             'owner_in_hard_closure': owner in runtime, 'kind': entry.kind,
                             'target': entry.target, 'source_line': entry.line,
                             'status': status, 'severity': 'info', 'result_required': False,
                             'baseline_provider_present': baseline_present,
                             'local_candidates': [{'name': key, 'file': m.path} for key, m in choices],
                             'lookup_failures': failures, 'matched_aliases': aliases})
            for child, module in choices:
                if entry.kind == 'pre':
                    before_edges[owner].add(child)
                nodes[child] = module
                queue.append(child)
    cycles = cyclic_nodes(nodes, before_edges)
    findings = []
    if cycles:
        findings.append(finding('stage_softdep_recursion', 'reachable deployed hard/pre-softdep recursion may never return; optional errors do not make recursion harmless',
                                stage=spec.partition, modules=sorted(cycles)))
    return attempts, findings


def check_stages(inventory, kernel):
    results, findings, first = {}, [], {}
    for part in PARTS:
        require(part in inventory.stages, f'missing stage specification: {part}')
        spec = inventory.stages[part]
        base = {} if part == 'vendor_boot' else first
        files = {host_path_key(m.path): m for m in inventory.modules if m.partition == part}
        runtime, edges, errors, skipped_hard, skipped_extra, roots, first_request, first_row = select_stage(spec, files, inventory.modules)
        selected = {m.name: m for m in runtime.values() if m.name not in base}
        closures = {key: dependency_closure(key, edges) for key in runtime}
        # The vendor/system shell scripts wait for the first top-level modprobe,
        # then start the rest independently. Neither service waits for the other.
        synchronous_first = ({first_row} | closures[first_row]) if first_row is not None else set()
        checks = []
        for key, module in runtime.items():
            if module.name in base:
                # Its row/path/hard requests were STILL checked above: EEXIST
                # helps only after the new process successfully opens the file.
                continue
            guaranteed = dict(base)
            prior_rows = set(closures[key])
            if key not in synchronous_first:
                prior_rows.update(synchronous_first)
            for dependency in prior_rows:
                guaranteed[runtime[dependency].name] = runtime[dependency]
            guaranteed.pop(module.name, None)
            available = {symbol for provider in guaranteed.values() for symbol in provider.exports}
            for dependency in module.depends:
                if dependency not in guaranteed:
                    errors.append(finding('stage_modinfo_dependency_not_deployed',
                                          '.modinfo dependency is not guaranteed by deployed hard dependencies/baseline; it was NOT auto-loaded',
                                          stage=part, dependency=dependency, **module_context(module)))
            for symbol, weak in module.undefined.items():
                if weak or symbol in module.metadata_weak_symbols or symbol in kernel:
                    continue
                if symbol not in available:
                    errors.append(finding('stage_symbol_unavailable',
                                          'strong import has no guaranteed provider before this insmod; other parallel loads and optional softdeps are not proof',
                                          stage=part, symbol=symbol, **module_context(module)))
            checks.append({'module': module.name, 'file': module.path, 'loader_key': key,
                           'owner_path': spec.depfile[key].path,
                           'hard_dependency_rows': sorted(closures[key]),
                           'guaranteed_provider_modules': sorted(guaranteed)})
        attempts, soft_errors = inspect_softdeps(spec, runtime, edges, files, base)
        errors.extend(soft_errors)
        results[part] = {'selected_count': len(selected), 'selected_modules': sorted(selected),
                         'baseline_modules': sorted(base), 'runtime_request_rows': sorted(runtime),
                         'first_shell_request': first_request, 'first_shell_request_row': first_row,
                         'blocked_load_entries': sorted(skipped_hard), 'excluded_load_entries': sorted(skipped_extra),
                         'declared_blocklist': sorted(spec.hard_blocked), 'load_exclusion_patterns': sorted(spec.load_excluded),
                         'hard_blocklist_sources': list(spec.hard_blocklist_sources),
                         'load_exclusion_sources': list(spec.load_exclusion_sources),
                         'module_directory': spec.module_dir, 'device_path_mappings': list(spec.device_mappings),
                         'hard_dependency_checks': checks, 'optional_attempts': attempts,
                         'optional_attempt_categories': dict(collections.Counter(item['status'] for item in attempts)),
                         'softdep_policy': 'deployed file only; pre/post attempts optional; not used as hard-provider evidence; recursion rejected',
                         'findings': errors, 'sources': list(spec.sources), 'source_sha256': spec.source_hashes,
                         'scope': 'deployed path/dependency/order guarantees; not runtime probe/init simulation'}
        findings.extend(errors)
        if part == 'vendor_boot':
            first = selected
    return results, findings


def symbol_choices(module, symbol, kernel, providers):
    if symbol in kernel:
        return [('vmlinux', kernel[symbol])]
    return [(owner, export) for owner, export in providers.get(symbol, ()) if owner != module.name]


def gpl_resolution(module, kernel, providers, provider_taint):
    """5.15 resolve_symbol/inherit_taint in ELF .symtab undefined order.

    Do not start a consumer with a globally precomputed inherited taint: a weak
    GPL-only import can set using_gplonly_symbols BEFORE a later strong import
    tries to inherit proprietary taint. That later lookup must then fail.
    """
    proprietary = module.license not in GPL_LICENSES
    using_gplonly = False
    outcomes, findings = {}, []
    for index, (symbol, weak) in enumerate(module.undefined.items()):
        if symbol in module.metadata_weak_symbols:
            continue
        choices = symbol_choices(module, symbol, kernel, providers)
        if not choices:
            outcomes[symbol] = 'optional_absent' if weak else 'missing'
            continue
        # Ambiguous providers already cause export/copy conflict errors. The
        # licensing simulation is meaningful only for an unambiguous provider.
        if len(choices) != 1:
            outcomes[symbol] = 'ambiguous'
            continue
        owner, export = choices[0]
        reason = None
        if export.gpl_only and proprietary:
            reason = 'GPL-only lookup is unavailable after proprietary taint'
        else:
            # The loader sets this flag before inherit_taint, even if the
            # subsequent inheritance check rejects a weak symbol's provider.
            if export.gpl_only:
                using_gplonly = True
            if owner != 'vmlinux' and owner in provider_taint:
                if using_gplonly:
                    reason = 'cannot inherit proprietary provider taint after using a GPL-only symbol'
                else:
                    if weak and not proprietary:
                        findings.append(finding('weak_proprietary_provider',
                                                'weak module-provider availability can change inherited taint and later GPL lookup order; global export presence is not a readiness proof',
                                                symbol=symbol, provider=owner, elf_undefined_index=index,
                                                **module_context(module)))
                    proprietary = True
        if reason:
            outcomes[symbol] = 'optional_denied' if weak else 'denied'
            findings.append(finding('optional_weak_gpl_unavailable' if weak else 'gpl_only_violation',
                                    reason + ('; weak lookup can remain zero' if weak else ''),
                                    'warning' if weak else 'error', symbol=symbol, provider=owner,
                                    elf_undefined_index=index, using_gplonly_symbols=using_gplonly,
                                    effective_proprietary_at_lookup=proprietary, license=module.license,
                                    **module_context(module)))
        else:
            outcomes[symbol] = 'resolved'
    return proprietary, outcomes, findings


def infer_provider_taint(groups, kernel, providers):
    tainted = {name for name, copies in groups.items() if any(m.license not in GPL_LICENSES for m in copies)}
    # Monotone conservative fixed point. A different effective taint between
    # copies/order variants is not silently allowed to make a consumer PASS.
    for _ in range(len(groups) + 1):
        inferred = {name for name, copies in groups.items()
                    if any(gpl_resolution(m, kernel, providers, tainted)[0] for m in copies)}
        require(tainted <= inferred, 'GPL provider taint is order-dependent/non-monotone; static resolution requires review')
        if inferred == tainted:
            return tainted
        tainted = inferred
    raise InputError('GPL provider-taint analysis did not converge')


def verify_candidate(inventory, symvers, config):
    require(inventory.modules, 'empty module inventory')
    require(symvers.exports and 'module_layout' in symvers.exports, 'empty/incomplete kernel export table')
    groups = group_modules(inventory.modules)
    kernel = symvers.exports
    findings = []
    for name, copies in groups.items():
        if len({metadata_signature(m) for m in copies}) != 1:
            findings.append(finding('module_copy_metadata_conflict', 'same module identity has different load metadata; hashes alone are not used',
                                    module=name, files=[m.path for m in copies]))
    providers = collections.defaultdict(list)
    # Identity, not file hash/path, determines an independently loadable module.
    for name, copies in groups.items():
        for symbol in sorted({s for m in copies for s in m.exports}):
            alternatives = {m.exports[symbol] for m in copies if symbol in m.exports}
            for export in sorted(alternatives, key=repr):
                providers[symbol].append((name, export))
            if symbol in kernel:
                findings.append(finding('kernel_module_export_conflict', 'module export duplicates a built-in kernel export',
                                        module=name, files=[m.path for m in copies if symbol in m.exports], symbol=symbol))
    for symbol, owners in providers.items():
        identities = sorted({name for name, _ in owners})
        if len(identities) > 1:
            findings.append(finding('module_module_export_conflict', 'different module identities export the same symbol',
                                    symbol=symbol, modules=identities))
    all_exported_names = set(kernel) | set(providers)
    tainted = infer_provider_taint(groups, kernel, providers)
    resolved_kernel = resolved_module = optional_weak = 0
    weak_crc_resolutions = []
    for module in inventory.modules:  # EVERY file copy, not one per identity.
        context = module_context(module)
        if vermagic_tail(module.vermagic) != config.vermagic_flags:
            findings.append(finding('vermagic_flags_mismatch', 'vermagic tail differs from the explicit kernel config',
                                    expected=config.vermagic_flags, actual=vermagic_tail(module.vermagic), **context))
        for symbol, export in module.exports.items():
            if export.weak_crc_symbol:
                value = effective_crc(export, all_exported_names)
                weak_crc_resolutions.append(dict(**context, export=symbol, weak_symbol=export.weak_crc_symbol,
                                                 relocation='R_AARCH64_ABS32', addend=export.crc_addend,
                                                 effective_crc=f'0x{value:08x}', reason='no exported provider; unresolved weak S=0, CRC=S+A'))
        _, policy_outcomes, policy_findings = gpl_resolution(module, kernel, providers, tainted)
        findings.extend(policy_findings)
        symbols = set(module.versions) | set(module.undefined)
        for symbol in sorted(symbols):
            if symbol in module.metadata_weak_symbols:
                continue
            weak = module.undefined.get(symbol, False)
            imported = symbol in module.undefined
            choices = symbol_choices(module, symbol, kernel, providers)
            if imported and policy_outcomes.get(symbol) in ('optional_absent', 'optional_denied'):
                optional_weak += 1
                continue
            if imported and policy_outcomes.get(symbol) == 'denied':
                continue
            if not choices:
                if weak:
                    optional_weak += 1
                    continue
                findings.append(finding('unresolved_symbol', 'no provider for strong import/version reference', symbol=symbol, **context))
                continue
            for owner, export in choices:
                if imported and export.namespace and export.namespace not in module.import_ns:
                    findings.append(finding('namespace_missing_import', 'export namespace is not imported' + ('; kernel config explicitly permits warning-only loading' if config.allow_missing_namespace else ''),
                                            'warning' if config.allow_missing_namespace else 'error',
                                            symbol=symbol, provider=owner, required_namespace=export.namespace,
                                            import_ns=sorted(module.import_ns), **context))
            if symbol not in module.versions:
                findings.append(finding('missing_symbol_version',
                                        'single resolved import lacks a version record: Linux 5.15 warns and allows it, but its ABI is NOT CRC-verified',
                                        'warning', symbol=symbol, abi_verified=False, consumer_sha256=module.sha256,
                                        providers=[{'name': owner, 'crc': f'0x{effective_crc(export, all_exported_names):08x}',
                                                    'namespace': export.namespace, 'gpl_only': export.gpl_only,
                                                    'files': [{'file': m.path, 'sha256': m.sha256} for m in groups.get(owner, ())]}
                                                   for owner, export in choices], **context))
                continue
            expected_crc = module.versions[symbol]
            actual_crcs = [effective_crc(export, all_exported_names) for _, export in choices]
            if any(crc != expected_crc for crc in actual_crcs):
                findings.append(finding('crc_mismatch', 'module version CRC differs from a provider CRC',
                                        symbol=symbol, expected=f'0x{expected_crc:08x}',
                                        providers=[{'name': owner, 'crc': f'0x{crc:08x}'} for (owner, _), crc in zip(choices, actual_crcs)], **context))
            elif symbol in kernel:
                resolved_kernel += 1
            else:
                resolved_module += 1
    stages, stage_findings = check_stages(inventory, kernel)
    findings.extend(stage_findings)
    errors = [f for f in findings if f['severity'] == 'error']
    warnings = [f for f in findings if f['severity'] == 'warning']
    return {'status': 'FAIL' if errors else 'PASS', 'findings': findings,
            'counts': {'module_files': len(inventory.modules), 'module_identities': len(groups),
                       'version_references': sum(len(m.versions) for m in inventory.modules),
                       'vmlinux_exports': len(kernel), 'intree_exports_ignored': symvers.intree_ignored,
                       'crc_refs_resolved_by_kernel': resolved_kernel, 'crc_refs_resolved_by_modules': resolved_module,
                       'unresolved_optional_weak_imports': optional_weak,
                       'imports_without_crc_verification': sum(f['code'] == 'missing_symbol_version' for f in warnings),
                       'errors': len(errors), 'warnings': len(warnings),
                       'error_categories': dict(collections.Counter(f['code'] for f in errors)),
                       'warning_categories': dict(collections.Counter(f['code'] for f in warnings))},
            'expected_vermagic_flags': config.vermagic_flags,
            'allow_missing_namespace_imports': config.allow_missing_namespace,
            'gpl_resolution_policy': {'order': 'ELF .symtab undefined order', 'effective_proprietary_providers': sorted(tainted)},
            'weak_crc_resolutions': weak_crc_resolutions, 'stages': stages}


def inventory_report(inventory):
    groups = group_modules(inventory.modules)
    return {'roots': inventory.roots,
            'counts': {'files_by_partition': dict(collections.Counter(m.partition for m in inventory.modules)),
                       'module_files': len(inventory.modules), 'module_identities': len(groups),
                       'same_name_multiple_copies': sum(len(copies) > 1 for copies in groups.values()),
                       'same_name_different_file_hashes': sum(len({m.sha256 for m in copies}) > 1 for copies in groups.values())},
            'modules': [dict(**module_context(m), sha256=m.sha256, license=m.license,
                             import_ns=sorted(m.import_ns), vermagic=m.vermagic, depends=list(m.depends),
                             modinfo_softdep=list(m.softdep),
                             version_references=len(m.versions), exports=len(m.exports),
                             strong_undefined=sum(not weak for weak in m.undefined.values()),
                             weak_undefined=sum(m.undefined.values())) for m in inventory.modules]}


def build_parser():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument('symvers', nargs='+', help='one or more vmlinux.symvers / Module.symvers candidates')
    parser.add_argument('--kernel-config', action='append', required=True, metavar='PATH',
                        help='one shared config, or repeat once per positional candidate')
    for part in PARTS:
        parser.add_argument('--' + part.replace('_', '-') + '-root', default=DEFAULT_ROOTS[part], metavar='DIR')
    parser.add_argument('--audio-mode', choices=('legacy', 'audioreach'), default='legacy')
    parser.add_argument('--json', metavar='PATH', help='write structured report; input files may not be overwritten')
    return parser


def emit_report(report):
    if 'inventory' in report:
        for part, count in report['inventory']['counts']['files_by_partition'].items():
            print(f'{part:12s}: {count:4d} .ko')
        print('module identities:', report['inventory']['counts']['module_identities'])
    for error in report['input_errors']:
        print('ERROR:', error['message'], file=sys.stderr)
    for candidate in report['candidates']:
        print('\n=====', candidate['symvers'], '=====')
        if 'counts' in candidate:
            print('counts:', json.dumps(candidate['counts'], sort_keys=True))
            print('stages:', ', '.join(f'{p}={s["selected_count"]}' for p, s in candidate['stages'].items()))
        for item in candidate['findings'][:24]:
            print(f'  {item["severity"].upper()} {item["code"]}: {item.get("file", item.get("module", ""))} {item.get("symbol", "")} {item["message"]}')
        if len(candidate['findings']) > 24:
            print(f'  ... {len(candidate["findings"]) - 24} further findings (see JSON report)')
        print('RESULT:', candidate['status'])
    print('\nOVERALL:', report['status'], f'(exit {report["exit_code"]})')
    print('Scope: static ABI/metadata and stage availability; NOT a boot/probe or hardware test.')


def main(argv=None):
    parser = build_parser()
    try:
        args = parser.parse_args(argv)
    except SystemExit as exc:
        return int(exc.code or 0)
    report = {'schema_version': 2, 'status': 'ERROR', 'exit_code': 2,
              'scope': {'architecture': 'AArch64 ELF64 LE Linux 5.15 MODVERSIONS, absolute CRCs',
                        'runtime_not_simulated': True, 'parallel_vendor_system_assume_other_ready': False,
                        'signature_policy_supported': 'MODULE_SIG=n only', 'audio_mode': args.audio_mode},
              'input_errors': [], 'candidates': []}
    protected_inputs = set()
    try:
        protected_inputs.update(str(Path(p).resolve()) for p in args.symvers + args.kernel_config)
        protected_inputs.add(str(Path(__file__).resolve()))
        require(len(args.kernel_config) in (1, len(args.symvers)), 'provide one shared --kernel-config or one per candidate')
        roots = {part: getattr(args, part + '_root') for part in PARTS}
        inventory = load_inventory(roots, args.audio_mode)
        report['inventory'] = inventory_report(inventory)
        protected_inputs.update(str(Path(m.path).resolve()) for m in inventory.modules)
        protected_inputs.update(str(Path(p).resolve()) for stage in inventory.stages.values() for p in stage.sources)
        configs = args.kernel_config * len(args.symvers) if len(args.kernel_config) == 1 else args.kernel_config
        for symvers_path, config_path in zip(args.symvers, configs):
            candidate = {'symvers': symvers_path, 'kernel_config': config_path}
            try:
                config_bytes, symvers_bytes = read_bytes(config_path), read_bytes(symvers_path)
                candidate.update(config_sha256=digest(config_bytes), symvers_sha256=digest(symvers_bytes))
                config = parse_kernel_config(config_bytes.decode('utf-8'))
                table = parse_symvers(symvers_bytes.decode('utf-8'))
                candidate.update(verify_candidate(inventory, table, config))
            except (InputError, OSError, UnicodeError) as exc:
                candidate.update(status='ERROR', findings=[finding('input_error', str(exc))])
            report['candidates'].append(candidate)
        statuses = {c['status'] for c in report['candidates']}
        report['status'] = 'ERROR' if 'ERROR' in statuses else 'FAIL' if 'FAIL' in statuses else 'PASS'
        report['exit_code'] = {'PASS': 0, 'FAIL': 1, 'ERROR': 2}[report['status']]
    except (InputError, OSError, UnicodeError) as exc:
        report['input_errors'].append(finding('input_error', str(exc)))
    except Exception as exc:
        # Never turn an implementation exception into a successful verification.
        report['input_errors'].append(finding('internal_error', f'{type(exc).__name__}: {exc}'))
        report['status'], report['exit_code'] = 'ERROR', 2
    if args.json:
        try:
            target = Path(args.json)
            require(str(target.resolve()) not in protected_inputs, '--json must not overwrite an input/script file')
            target.write_text(json.dumps(report, ensure_ascii=False, indent=2) + '\n', encoding='utf-8')
        except (InputError, OSError) as exc:
            report['input_errors'].append(finding('report_write_error', str(exc)))
            report['status'], report['exit_code'] = 'ERROR', 2
    emit_report(report)
    return report['exit_code']


if __name__ == '__main__':
    sys.exit(main())
