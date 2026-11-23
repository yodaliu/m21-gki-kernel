"""Portable stdlib tests; all ELF fixtures and mocked filesystem writes are in memory.
Run: python -B -m unittest -v test_verify_all_modules
No WSL, adb, mount, compiler, nm, modinfo, external process, or real .ko is used.
"""
from __future__ import annotations

import contextlib
from dataclasses import replace
import importlib.util
import io
import json
from pathlib import Path
import struct
import sys
import unittest
from unittest.mock import patch

SPEC = importlib.util.spec_from_file_location('m21_module_verifier', Path(__file__).with_name('verify-all-modules.py'))
v = importlib.util.module_from_spec(SPEC)
sys.modules[SPEC.name] = v
SPEC.loader.exec_module(v)

CONFIG = '''CONFIG_ARM64=y
CONFIG_MODULES=y
CONFIG_MODVERSIONS=y
CONFIG_SMP=y
CONFIG_PREEMPT=y
CONFIG_MODULE_UNLOAD=y
# CONFIG_MODULE_SIG is not set
# CONFIG_MODULE_REL_CRCS is not set
# CONFIG_MODULE_ALLOW_MISSING_NAMESPACE_IMPORTS is not set
'''
FLAGS = 'SMP preempt mod_unload modversions aarch64'
MAGIC = '5.15.41-stock ' + FLAGS
LAYOUT = 0x11223344
API = 0x55667788
NS = 'VFS_internal_I_am_really_a_filesystem_and_am_NOT_a_driver'
SYMVERS = f'0x{LAYOUT:08x}\tmodule_layout\tvmlinux\tEXPORT_SYMBOL\t\n0x{API:08x}\tkernel_api\tvmlinux\tEXPORT_SYMBOL\t\n'


def module(name='demo', part='vendor_boot', **kwargs):
    defaults = dict(path=f'{part}/{name}.ko', partition=part, name=name,
                    filename=name + '.ko', sha256='hash-' + part,
                    license='GPL v2', vermagic=MAGIC,
                    versions={'module_layout': LAYOUT})
    if 'filename' in kwargs and 'path' not in kwargs:
        defaults['path'] = f'{part}/{kwargs["filename"]}'
    defaults.update(kwargs)
    return v.Module(**defaults)


def inventory(modules, loads=None, deps=None, blocked=None, excluded=None, softdeps=None, aliases=None):
    loads, deps, blocked = loads or {}, deps or {}, blocked or {}
    excluded, softdeps, aliases = excluded or {}, softdeps or {}, aliases or {}
    stages = {}
    for part in v.PARTS:
        local = [m for m in modules if m.partition == part]
        rows = {v.loader_canonical(m.filename or m.path): v.DepEntry(Path(m.path).relative_to(part).as_posix()) for m in local}
        # Fixture deployment is independent of .modinfo depends. Tests must
        # explicitly populate runtime edges; metadata never creates them.
        for key, entry in deps.get(part, {}).items():
            if entry is None:
                rows.pop(key, None)
            elif isinstance(entry, v.DepEntry):
                rows[key] = entry
            else:
                rows[key] = v.DepEntry(rows.get(key, v.DepEntry(key + '.ko')).path, tuple(entry))
        stages[part] = v.StageSpec(part, tuple(loads.get(part, [m.filename for m in local])), rows,
                                   frozenset(blocked.get(part, ())), module_dir=part,
                                   device_mappings=v.device_path_mappings(part, part),
                                   load_excluded=frozenset(excluded.get(part, ())),
                                   softdeps=tuple(softdeps.get(part, ())), aliases=tuple(aliases.get(part, ())))
    return v.Inventory(modules, stages, {p: p for p in v.PARTS})


def check(modules, exports=None, cfg=CONFIG, **stage):
    table = v.parse_symvers(SYMVERS)
    if exports:
        table.exports.update(exports)
    return v.verify_candidate(inventory(modules, **stage), table, v.parse_kernel_config(cfg))


def errors(result):
    return {f['code'] for f in result['findings'] if f['severity'] == 'error'}


def elf_fixture(name='demo', *, undefined=None, versions=None, exports=(),
                import_ns=(), license='GPL v2', vermagic=MAGIC, depends='',
                combined=True, strings_by_section=False, comment=b'build-A',
                crc_relocation_type=v.R_AARCH64_ABS32, machine=183,
                empty_versions=False):
    """Construct real ELF64/RELA records, including merged module.lds tables.

    export dict: name, crc, namespace, gpl, mode ('literal', 'absolute', 'weak'),
    addend. Weak mode reproduces the q_check_kernel_client ABS32 S=0,A=0 case.
    strings_by_section reproduces relocations against __ksymtab_strings+addend,
    rather than assuming that named __kstrtabns symbols are relocation targets.
    """
    undefined = dict(undefined or {})
    versions = dict(versions if versions is not None else {'module_layout': LAYOUT})
    sections = [dict(name='', kind=0, flags=0, data=b'', link=0, info=0, align=0, entsize=0)]
    def section(name, data=b'', kind=1, flags=2, link=0, info=0, align=1, entsize=0):
        sections.append(dict(name=name, kind=kind, flags=flags, data=data, link=link, info=info, align=align, entsize=entsize))
        return len(sections) - 1
    text_idx = section('.text', b'\xc0\x03\x5f\xd6', flags=6, align=4)
    info = [('name', name), ('vermagic', vermagic), ('license', license), ('depends', depends)]
    info.extend(('import_ns', ns) for ns in import_ns)
    section('.modinfo', b''.join(f'{k}={value}'.encode() + b'\0' for k, value in info))
    version_data = b''.join(struct.pack('<Q', crc) + symbol.encode().ljust(56, b'\0') for symbol, crc in versions.items())
    section('__versions', b'' if empty_versions else version_data, align=8)
    string_idx = section('__ksymtab_strings', b'\0')
    section('.comment', comment + b'\0', flags=0)
    symbols = []
    def symbol(name, binding=0, kind=0, index=0, value=0, size=0):
        symbols.append(dict(name=name, binding=binding, kind=kind, index=index, value=value, size=size))
    # Section symbols intentionally share the __ksymtab_ textual prefix.
    symbol('__ksymtab_strings', kind=3, index=string_idx)
    tables = {}
    export_records = []
    string_data = bytearray(b'\0')
    for spec in sorted(exports, key=lambda x: (x.get('gpl', False), x['name'])):
        exported = spec['name']
        gpl = spec.get('gpl', False)
        suffix = '_gpl' if gpl else ''
        key = suffix if combined else suffix + '+' + exported
        if key not in tables:
            kname = '__ksymtab' + suffix if combined else '___ksymtab' + suffix + '+' + exported
            cname = '__kcrctab' + suffix if combined else '___kcrctab' + suffix + '+' + exported
            kidx = section(kname, b'', align=4)
            cidx = section(cname, b'', align=1)
            tables[key] = (kidx, cidx)
            symbol(kname, kind=3, index=kidx)
            symbol(cname, kind=3, index=cidx)
        kidx, cidx = tables[key]
        koff, coff = len(sections[kidx]['data']), len(sections[cidx]['data'])
        sections[kidx]['data'] += b'\0' * 12
        mode = spec.get('mode', 'literal')
        sections[cidx]['data'] += struct.pack('<I', spec['crc'] if mode == 'literal' else 0)
        name_offset = len(string_data)
        string_data.extend(exported.encode() + b'\0')
        ns_offset = len(string_data)
        string_data.extend(spec.get('namespace', '').encode() + b'\0')
        symbol('__kstrtab_' + exported, index=string_idx, value=name_offset)
        symbol('__kstrtabns_' + exported, index=string_idx, value=ns_offset)
        symbol('__ksymtab_' + exported, index=kidx, value=koff, size=0)
        symbol(exported, binding=1, kind=2, index=text_idx, size=4)
        symbol('__crc_' + exported, binding=2 if mode == 'weak' else 1,
               index=v.SHN_UNDEF if mode == 'weak' else v.SHN_ABS,
               value=0 if mode == 'weak' else spec['crc'])
        export_records.append((spec, kidx, cidx, koff, coff, name_offset, ns_offset))
    sections[string_idx]['data'] = bytes(string_data)
    for imported, weak in undefined.items():
        symbol(imported, binding=2 if weak else 1)
    symbols.sort(key=lambda s: s['binding'] != 0)
    symbol_indexes = {s['name']: i + 1 for i, s in enumerate(symbols)}
    strings = bytearray(b'\0')
    symdata = bytearray(b'\0' * 24)
    for sym in symbols:
        name_offset = len(strings)
        strings.extend(sym['name'].encode() + b'\0')
        symdata.extend(struct.pack('<IBBHQQ', name_offset, (sym['binding'] << 4) | sym['kind'], 0,
                                   sym['index'], sym['value'], sym['size']))
    strtab_idx = section('.strtab', bytes(strings), kind=3, flags=0)
    symtab_idx = section('.symtab', bytes(symdata), kind=2, flags=0, link=strtab_idx,
                         info=1 + sum(s['binding'] == 0 for s in symbols), align=8, entsize=24)
    for spec, kidx, cidx, koff, coff, name_offset, ns_offset in export_records:
        exported = spec['name']
        def relocation(offset, target, kind, addend=0):
            return struct.pack('<QQq', offset, (symbol_indexes[target] << 32) | kind, addend)
        if strings_by_section:
            name_target = ns_target = '__ksymtab_strings'
            name_addend, ns_addend = name_offset, ns_offset
        else:
            name_target, ns_target = '__kstrtab_' + exported, '__kstrtabns_' + exported
            name_addend = ns_addend = 0
        rels = relocation(koff, exported, v.R_AARCH64_PREL32)
        rels += relocation(koff + 4, name_target, v.R_AARCH64_PREL32, name_addend)
        rels += relocation(koff + 8, ns_target, v.R_AARCH64_PREL32, ns_addend)
        # Keep the input-section name even when sh_info points at merged tables.
        section('.rela___ksymtab+' + exported, rels, kind=4, flags=0, link=symtab_idx, info=kidx, align=8, entsize=24)
        if spec.get('mode', 'literal') != 'literal':
            rels = relocation(coff, '__crc_' + exported, crc_relocation_type, spec.get('addend', 0))
            section('.rela___kcrctab+' + exported, rels, kind=4, flags=0, link=symtab_idx, info=cidx, align=8, entsize=24)
    shstr_idx = section('.shstrtab', kind=3, flags=0)
    names = bytearray(b'\0')
    for sec in sections:
        sec['name_offset'] = len(names) if sec['name'] else 0
        if sec['name']:
            names.extend(sec['name'].encode() + b'\0')
    sections[shstr_idx]['data'] = bytes(names)
    data = bytearray(b'\0' * 64)
    headers = []
    for sec in sections:
        alignment = max(sec['align'], 1)
        data.extend(b'\0' * ((-len(data)) % alignment))
        offset = len(data) if sec['kind'] else 0
        data.extend(sec['data'])
        headers.append(struct.pack('<IIQQQQIIQQ', sec['name_offset'], sec['kind'], sec['flags'], 0,
                                   offset, len(sec['data']), sec['link'], sec['info'], sec['align'], sec['entsize']))
    data.extend(b'\0' * ((-len(data)) % 8))
    shoff = len(data)
    data.extend(b''.join(headers))
    ident = b'\x7fELF\x02\x01\x01' + b'\0' * 9
    data[:64] = struct.pack('<16sHHIQQQIHHHHHH', ident, 1, machine, 1, 0, 0, shoff, 0, 64, 0, 0, 64, len(sections), shstr_idx)
    return bytes(data)


class ParsingTests(unittest.TestCase):
    def test_config_flags(self):
        cfg = v.parse_kernel_config(CONFIG)
        self.assertEqual(cfg.vermagic_flags, FLAGS)
        self.assertFalse(cfg.allow_missing_namespace)

    def test_valid_kconfig_symbols_may_contain_lowercase_characters(self):
        cfg = v.parse_kernel_config(CONFIG + 'CONFIG_FONT_8x16=y\n# CONFIG_FONT_6x11 is not set\n')
        self.assertEqual(cfg.values['CONFIG_FONT_8x16'], 'y')
        self.assertEqual(cfg.values['CONFIG_FONT_6x11'], 'n')

    def test_config_empty_or_unsupported(self):
        for text in ('', CONFIG.replace('CONFIG_ARM64=y', 'CONFIG_ARM64=n'),
                     CONFIG.replace('CONFIG_MODVERSIONS=y', 'CONFIG_MODVERSIONS=n'),
                     CONFIG.replace('# CONFIG_MODULE_REL_CRCS is not set', 'CONFIG_MODULE_REL_CRCS=y'),
                     CONFIG.replace('# CONFIG_MODULE_SIG is not set', 'CONFIG_MODULE_SIG=y'),
                     CONFIG + 'CONFIG_CPU_BIG_ENDIAN=y\n', CONFIG + 'CONFIG_GCC_PLUGIN_RANDSTRUCT=y\n',
                     CONFIG.replace('CONFIG_SMP=y\n', ''), CONFIG + 'CONFIG_SMP=y\n'):
            with self.subTest(text=text), self.assertRaises(v.InputError):
                v.parse_kernel_config(text)

    def test_symvers_namespaces_and_intree_exclusion(self):
        table = v.parse_symvers(SYMVERS + f'0x00000001\tprivate\tvmlinux\tEXPORT_SYMBOL_GPL\t{NS}\n0x00000002\tlate\tdrivers/late\tEXPORT_SYMBOL\t\n')
        self.assertEqual(table.exports['private'], v.Export(1, NS, True))
        self.assertNotIn('late', table.exports)
        self.assertEqual(table.intree_ignored, 1)

    def test_symvers_fail_closed(self):
        invalid = ('', 'garbage', '0x00\tfoo\tvmlinux\tEXPORT_SYMBOL\t\n',
                   SYMVERS + SYMVERS.splitlines()[0] + '\n',
                   SYMVERS.replace('0x11223344', '0x111223344'),
                   SYMVERS.replace('EXPORT_SYMBOL', 'UNKNOWN'),
                   SYMVERS.replace('\tvmlinux\t', '\tmodule_only\t'))
        for text in invalid:
            with self.subTest(text=text), self.assertRaises(v.InputError):
                v.parse_symvers(text)

    def test_real_elf_metadata_and_literal_crc(self):
        blob = elf_fixture(undefined={'kernel_api': False}, versions={'module_layout': LAYOUT, 'kernel_api': API},
                           exports=[{'name': 'public_api', 'crc': 77, 'namespace': NS}], import_ns=[NS])
        meta = v.parse_module(blob)
        self.assertEqual(meta.versions['kernel_api'], API)
        self.assertEqual(meta.undefined, {'kernel_api': False})
        self.assertEqual(meta.exports['public_api'], v.Export(77, NS))
        self.assertEqual(meta.import_ns, frozenset([NS]))

    def test_merged_tables_and_section_symbol_relocations(self):
        blob = elf_fixture(exports=[{'name': 'one', 'crc': 5, 'mode': 'absolute'},
                                    {'name': 'two', 'crc': 6, 'namespace': NS},
                                    {'name': 'three', 'crc': 7, 'gpl': True}],
                           combined=True, strings_by_section=True)
        exports = v.parse_module(blob).exports
        self.assertEqual(exports['one'].crc, 5)
        self.assertEqual(exports['two'].namespace, NS)
        self.assertTrue(exports['three'].gpl_only)

    def test_unmerged_export_sections_are_not_runtime_providers(self):
        blob = elf_fixture(exports=[{'name': 'api', 'crc': 1}], combined=False)
        with self.assertRaisesRegex(v.InputError, 'unmerged'):
            v.parse_module(blob)

    def test_nonallocated_runtime_export_tables_or_strings_are_rejected(self):
        original = elf_fixture(exports=[{'name': 'api', 'crc': 1, 'namespace': NS}])
        elf = v.ELF64(original)
        section_table = struct.unpack_from('<Q', original, 40)[0]
        for name in ('__ksymtab', '__kcrctab', '__ksymtab_strings'):
            with self.subTest(section=name):
                blob = bytearray(original)
                struct.pack_into('<Q', blob, section_table + elf.by_name[name] * 64 + 8, 0)
                with self.assertRaisesRegex(v.InputError, 'SHF_ALLOC'):
                    v.parse_module(bytes(blob))

    def test_gpl_classification_is_section_prefix_not_name_substring(self):
        blob = elf_fixture(exports=[{'name': 'not_a_gpl_restriction', 'crc': 9},
                                    {'name': 'restricted', 'crc': 10, 'gpl': True}])
        exports = v.parse_module(blob).exports
        self.assertFalse(exports['not_a_gpl_restriction'].gpl_only)
        self.assertTrue(exports['restricted'].gpl_only)

    def test_weak_crc_uses_actual_relocation_and_addend(self):
        for addend in (0, 7):
            with self.subTest(addend=addend):
                blob = elf_fixture(exports=[{'name': 'quectel_kernel_model_check', 'crc': 0, 'mode': 'weak', 'addend': addend}],
                                   combined=True, strings_by_section=True)
                meta = v.parse_module(blob)
                exp = meta.exports['quectel_kernel_model_check']
                self.assertIsNone(exp.crc)
                self.assertEqual(exp.weak_crc_symbol, '__crc_quectel_kernel_model_check')
                self.assertEqual(v.effective_crc(exp, {'module_layout'}), addend)
                self.assertIn(exp.weak_crc_symbol, meta.metadata_weak_symbols)
                with self.assertRaises(v.InputError):
                    v.effective_crc(exp, {exp.weak_crc_symbol})

    def test_weak_crc_not_misreported_as_unknown_symbol(self):
        meta = v.parse_module(elf_fixture(exports=[{'name': 'weak_export', 'crc': 0, 'mode': 'weak'}]), 'vendor_boot/q_check.ko')
        result = check([meta])
        self.assertEqual(result['status'], 'PASS')
        self.assertEqual(result['counts']['unresolved_optional_weak_imports'], 0)
        self.assertEqual(result['weak_crc_resolutions'][0]['effective_crc'], '0x00000000')

    def test_weak_crc_requires_undefined_weak_zero_value(self):
        original = elf_fixture(exports=[{'name': 'weak_api', 'crc': 0, 'mode': 'weak'}])
        elf = v.ELF64(original)
        index = elf.named_symbols['__crc_weak_api'][0]
        position = elf.sections[elf.symtab_index].offset + index * 24
        bad_value = bytearray(original)
        struct.pack_into('<Q', bad_value, position + 8, 99)
        strong = bytearray(original)
        strong[position + 4] = 1 << 4
        for blob in (bytes(bad_value), bytes(strong)):
            with self.subTest(blob=blob[position:position + 24]), self.assertRaisesRegex(v.InputError, 'neither absolute nor weak'):
                v.parse_module(blob)

    def test_missing_namespace_relocation_fails_closed(self):
        original = elf_fixture(exports=[{'name': 'api', 'crc': 1, 'namespace': NS}])
        elf = v.ELF64(original)
        rel = next(s for s in elf.sections if s.name == '.rela___ksymtab+api')
        corrupt = bytearray(original)
        # Change namespace relocation type from PREL32 to ABS32.
        info = struct.unpack_from('<Q', corrupt, rel.offset + 2 * 24 + 8)[0]
        struct.pack_into('<Q', corrupt, rel.offset + 2 * 24 + 8, (info & ~0xffffffff) | v.R_AARCH64_ABS32)
        with self.assertRaisesRegex(v.InputError, 'string PREL32'):
            v.parse_module(bytes(corrupt))

    def test_relative_crc_relocation_is_rejected(self):
        blob = elf_fixture(exports=[{'name': 'one', 'crc': 0, 'mode': 'weak'}], crc_relocation_type=v.R_AARCH64_PREL32)
        with self.assertRaisesRegex(v.InputError, 'CRC relocation'):
            v.parse_module(blob)

    def test_bad_elf_empty_versions_and_wrong_architecture(self):
        for blob in (b'', b'\x7fELF' + b'\0' * 60, elf_fixture(machine=62),
                     elf_fixture(empty_versions=True), elf_fixture(versions={}),
                     elf_fixture(versions={'not_layout': 7}), elf_fixture()[:100]):
            with self.subTest(length=len(blob)), self.assertRaises(v.InputError):
                v.parse_module(blob)

    def test_bad_modinfo_duplicate_identity(self):
        blob = elf_fixture(name='demo\x00name=other')
        with self.assertRaises(v.InputError):
            v.parse_module(blob)

    def test_crc_zero_is_valid_data_not_empty(self):
        meta = v.parse_module(elf_fixture(versions={'module_layout': 0}), 'vendor_boot/demo.ko')
        table = v.Symvers({'module_layout': v.Export(0)})
        result = v.verify_candidate(inventory([meta]), table, v.parse_kernel_config(CONFIG))
        self.assertEqual(result['status'], 'PASS')

    def test_load_dependency_and_blocklist_parsers(self):
        self.assertEqual(v.parse_load_list('# comment\na-b.ko\na-b.ko\n'), ('a-b.ko', 'a-b.ko'))
        self.assertEqual(v.parse_depfile('/lib/modules/alpha.ko: beta.ko gamma-delta.ko\n'),
                         {'alpha': v.DepEntry('/lib/modules/alpha.ko', ('beta.ko', 'gamma-delta.ko'))})
        self.assertEqual(v.parse_blocklist('blocklist test-mod\n# text\n'), frozenset(['test_mod']))
        for function, text in ((v.parse_load_list, ''), (v.parse_load_list, 'a.ko option=1'),
                               (v.parse_depfile, ''), (v.parse_depfile, 'not a depfile'),
                               (v.parse_depfile, 'alpha.ko:\nalpha.ko:'), (v.parse_blocklist, 'options x debug=1')):
            with self.subTest(text=text), self.assertRaises(v.InputError):
                function(text)

    def test_softdep_parser_preserves_raw_owner_repetition_and_pre_post(self):
        parsed = v.parse_softdep_file('softdep owner-name.ko pre: first-target post: second-target\nsoftdep owner-name.ko pre: third-target\n')
        self.assertEqual([(s.owner, s.kind, s.target) for s in parsed],
                         [('owner-name.ko', 'pre', 'first-target'), ('owner-name.ko', 'post', 'second-target'),
                          ('owner-name.ko', 'pre', 'third-target')])
        self.assertEqual(v.parse_softdep_file('# comment only\n'), ())
        with self.assertRaises(v.InputError):
            v.parse_softdep_file('softdep owner target before-state\n')

    def test_alias_patterns_match_case_sensitive_raw_requests(self):
        aliases = v.parse_alias_file('alias pci:device* driver_name\n')
        self.assertEqual(aliases, (('pci:device*', 'driver_name'),))
        with self.assertRaises(v.InputError):
            v.parse_alias_file('alias [[:digit:]] driver_name\n')

    def test_load_only_filters_preserve_deployed_shell_spelling(self):
        patterns = v.parse_load_exclusions('blocklist provider-name\n')
        self.assertTrue(v.excluded_load_request('provider_name.ko', patterns))
        self.assertFalse(v.excluded_load_request('provider-name.ko', patterns))
        self.assertFalse(v.excluded_load_request('another_provider_name.ko', patterns))

    def test_missing_deployed_depfile_is_an_input_error_not_metadata_fallback(self):
        def scan(root):
            return [Path(root) / 'demo.ko'], Path(root) / 'modules.load'
        def read(path):
            if str(path).endswith('.ko'):
                return elf_fixture()
            raise v.InputError('missing required modules.dep')
        with patch.object(v, 'scan_root', side_effect=scan), patch.object(v, 'read_bytes', side_effect=read):
            with self.assertRaisesRegex(v.InputError, 'modules.dep'):
                v.load_inventory({p: p for p in v.PARTS}, 'legacy')

    def test_inventory_separates_runtime_blocklists_from_shell_exclusions(self):
        data = {}
        for part in v.PARTS:
            data[f'{part}/demo.ko'] = elf_fixture(name=part + '_demo')
            data[f'{part}/modules.dep'] = b'demo.ko:\n'
            data[f'{part}/modules.load'] = b'demo.ko\n'
        data['vendor_dlkm/modules.blocklist'] = b'blocklist hard_target\n'
        data['vendor_dlkm/modules.audio.legacy.blocklist'] = b'blocklist load_only\n'
        data['vendor_dlkm/system_dlkm.modules.blocklist'] = b'blocklist system_only\n'
        data['vendor_dlkm/modules.softdep'] = b'softdep demo pre: optional_target\n'
        def key(path):
            return str(path).replace('\\', '/')
        def scan(root):
            return [Path(root) / 'demo.ko'], Path(root) / 'modules.load'
        with patch.object(v, 'scan_root', side_effect=scan), patch.object(v, 'read_bytes', side_effect=lambda p: data[key(p)]), \
             patch.object(v.Path, 'exists', lambda p: key(p) in data):
            inv = v.load_inventory({p: p for p in v.PARTS}, 'legacy')
        vendor, system = inv.stages['vendor_dlkm'], inv.stages['system_dlkm']
        self.assertEqual(vendor.hard_blocked, frozenset(['hard_target']))
        self.assertEqual(vendor.load_excluded, frozenset(['hard_target', 'load_only']))
        self.assertEqual(system.hard_blocked, frozenset())
        self.assertEqual(system.load_excluded, frozenset(['system_only']))
        self.assertEqual(len(vendor.softdeps), 1)
        self.assertTrue(any(path.endswith('modules.softdep') for path in vendor.source_hashes))

    def test_required_root_missing_or_empty(self):
        with patch.object(v.Path, 'is_dir', return_value=False), self.assertRaises(v.InputError):
            v.scan_root('absent')
        with patch.object(v.Path, 'is_dir', return_value=True), patch.object(v.os, 'walk', return_value=[('root', [], [])]), self.assertRaises(v.InputError):
            v.scan_root('root')

    def test_directory_enumeration_error_is_not_silenced(self):
        def denied(root, *, onerror, followlinks):
            onerror(PermissionError('blocked'))
        with patch.object(v.Path, 'is_dir', return_value=True), patch.object(v.os, 'walk', side_effect=denied), self.assertRaisesRegex(v.InputError, 'blocked'):
            v.scan_root('root')

    def test_compressed_module_not_silently_skipped(self):
        with patch.object(v.Path, 'is_dir', return_value=True), patch.object(v.Path, 'is_file', return_value=True), patch.object(v.os, 'walk', return_value=[('root', [], ['one.ko', 'two.ko.xz', 'modules.load'])]), self.assertRaisesRegex(v.InputError, 'compressed'):
            v.scan_root('root')

    def test_module_symlink_is_not_followed_and_fifo_is_not_read(self):
        entries = [('root', [], ['module.ko', 'modules.load'])]
        with patch.object(v.Path, 'is_dir', return_value=True), patch.object(v.Path, 'is_symlink', return_value=True), \
             patch.object(v.os, 'walk', return_value=entries), self.assertRaisesRegex(v.InputError, 'O_NOFOLLOW'):
            v.scan_root('root')
        with patch.object(v.Path, 'is_dir', return_value=True), patch.object(v.Path, 'is_symlink', return_value=False), \
             patch.object(v.Path, 'is_file', return_value=False), patch.object(v.os, 'walk', return_value=entries), \
             self.assertRaisesRegex(v.InputError, 'regular file'):
            v.scan_root('root')


class CompatibilityTests(unittest.TestCase):
    def importer(self, **kwargs):
        values = dict(undefined={'kernel_api': False}, versions={'module_layout': LAYOUT, 'kernel_api': API})
        values.update(kwargs)
        return module(**values)

    def test_matching_crc(self):
        self.assertEqual(check([self.importer()])['status'], 'PASS')

    def test_crc_mismatch(self):
        result = check([self.importer(versions={'module_layout': LAYOUT, 'kernel_api': API + 1})])
        self.assertIn('crc_mismatch', errors(result))

    def test_unknown_strong_fails_but_weak_can_be_absent(self):
        for weak in (False, True):
            m = module(undefined={'optional': weak}, versions={'module_layout': LAYOUT, 'optional': 7})
            result = check([m])
            self.assertEqual(result['status'], 'PASS' if weak else 'FAIL')
            self.assertEqual(result['counts']['unresolved_optional_weak_imports'], int(weak))

    def test_strong_undefined_missing_from_versions_is_still_checked(self):
        result = check([module(undefined={'unknown_without_crc': False})])
        self.assertIn('unresolved_symbol', errors(result))
        result = check([module(undefined={'kernel_api': False})])
        self.assertEqual(result['status'], 'PASS')
        self.assertEqual(result['counts']['warning_categories'], {'missing_symbol_version': 1})
        self.assertEqual(result['counts']['crc_refs_resolved_by_kernel'], 1)  # module_layout only
        warning = next(f for f in result['findings'] if f['code'] == 'missing_symbol_version')
        self.assertFalse(warning['abi_verified'])
        self.assertEqual(warning['providers'][0]['crc'], f'0x{API:08x}')

    def test_missing_namespace_fails_present_namespace_passes(self):
        exports = {'kernel_api': v.Export(API, NS)}
        self.assertIn('namespace_missing_import', errors(check([self.importer()], exports)))
        self.assertEqual(check([self.importer(import_ns=frozenset([NS]))], exports)['status'], 'PASS')

    def test_allow_missing_namespace_is_explicit_warning(self):
        cfg = CONFIG.replace('# CONFIG_MODULE_ALLOW_MISSING_NAMESPACE_IMPORTS is not set', 'CONFIG_MODULE_ALLOW_MISSING_NAMESPACE_IMPORTS=y')
        result = check([self.importer()], {'kernel_api': v.Export(API, NS)}, cfg=cfg)
        self.assertEqual(result['status'], 'PASS')
        self.assertTrue(result['allow_missing_namespace_imports'])
        self.assertEqual(result['counts']['warning_categories'], {'namespace_missing_import': 1})

    def test_weak_namespace_import_is_still_checked_if_provider_resolves(self):
        result = check([self.importer(undefined={'kernel_api': True})], {'kernel_api': v.Export(API, NS)})
        self.assertIn('namespace_missing_import', errors(result))

    def test_gpl_only_license_violation(self):
        exports = {'kernel_api': v.Export(API, gpl_only=True)}
        self.assertIn('gpl_only_violation', errors(check([self.importer(license='Proprietary')], exports)))
        self.assertEqual(check([self.importer(license='Dual BSD/GPL')], exports)['status'], 'PASS')

    def test_weak_gpl_only_import_can_remain_unresolved(self):
        result = check([self.importer(license='Proprietary', undefined={'kernel_api': True})],
                       {'kernel_api': v.Export(API, gpl_only=True)})
        self.assertEqual(result['status'], 'PASS')
        self.assertEqual(result['counts']['warning_categories'], {'optional_weak_gpl_unavailable': 1})

    def test_proprietary_taint_through_strong_module_provider(self):
        provider = module('provider', license='Proprietary', exports={'vendor_api': v.Export(9)})
        consumer = self.importer(depends=('provider',), undefined={'vendor_api': False, 'kernel_api': False},
                                 versions={'module_layout': LAYOUT, 'vendor_api': 9, 'kernel_api': API})
        result = check([provider, consumer], {'kernel_api': v.Export(API, gpl_only=True)})
        self.assertIn('gpl_only_violation', errors(result))

    def test_vermagic_tail_mismatch_but_release_difference_allowed(self):
        self.assertEqual(check([module(vermagic='5.15.220-new-test ' + FLAGS)])['status'], 'PASS')
        result = check([module(vermagic=MAGIC.replace('SMP ', ''))])
        self.assertIn('vermagic_flags_mismatch', errors(result))
        self.assertIn('vermagic_flags_mismatch', errors(check([module(vermagic=MAGIC + ' ')])))

    def test_kernel_module_export_collision(self):
        result = check([module(exports={'kernel_api': v.Export(API)})])
        self.assertIn('kernel_module_export_conflict', errors(result))

    def test_different_module_export_collision(self):
        result = check([module('one', exports={'shared': v.Export(1)}), module('two', exports={'shared': v.Export(1)})])
        self.assertIn('module_module_export_conflict', errors(result))

    def test_same_identity_copies_are_not_independent_providers(self):
        one = module('same', exports={'shared': v.Export(1)})
        two = replace(one, path='vendor_dlkm/same.ko', partition='vendor_dlkm', sha256='different bytes')
        inv = inventory([one, two])
        result = v.verify_candidate(inv, v.parse_symvers(SYMVERS), v.parse_kernel_config(CONFIG))
        self.assertEqual(result['status'], 'PASS')
        self.assertEqual(result['counts']['module_files'], 2)
        self.assertEqual(result['counts']['module_identities'], 1)
        self.assertEqual(v.inventory_report(inv)['counts']['same_name_different_file_hashes'], 1)

    def test_every_copy_is_checked_not_only_first(self):
        one = self.importer(import_ns=frozenset([NS]))
        two = replace(one, path='vendor_dlkm/demo.ko', partition='vendor_dlkm', import_ns=frozenset())
        result = check([one, two], {'kernel_api': v.Export(API, NS)})
        self.assertIn('module_copy_metadata_conflict', errors(result))
        failures = [f for f in result['findings'] if f['code'] == 'namespace_missing_import']
        self.assertEqual([f['file'] for f in failures], [two.path])

    def test_release_only_difference_between_copies_is_not_conflict(self):
        one = self.importer()
        two = replace(one, path='vendor_dlkm/demo.ko', partition='vendor_dlkm', vermagic='5.15.99-other ' + FLAGS)
        self.assertEqual(check([one, two])['status'], 'PASS')

    def test_empty_inventory_or_version_data_rejected(self):
        for mods in ([], [module(versions={})]):
            with self.subTest(modules=mods), self.assertRaises(v.InputError):
                check(mods)

    def test_module_provider_namespace_and_gpl_class(self):
        provider = module('provider', exports={'module_api': v.Export(55, NS, True)})
        consumer = module('consumer', license='Proprietary', depends=('provider',),
                          undefined={'module_api': False}, versions={'module_layout': LAYOUT, 'module_api': 55})
        stage = {'deps': {'vendor_boot': {'consumer': ('provider',)}}}
        result = check([provider, consumer], **stage)
        self.assertIn('gpl_only_violation', errors(result))
        consumer.license = 'GPL v2'
        self.assertIn('namespace_missing_import', errors(check([provider, consumer], **stage)))

    def test_weak_gpl_before_strong_proprietary_import_must_fail(self):
        provider = module('provider', license='Proprietary', exports={'vendor_api': v.Export(9)})
        consumer = module('consumer', depends=('provider',), undefined={'kernel_api': True, 'vendor_api': False},
                          versions={'module_layout': LAYOUT, 'kernel_api': API, 'vendor_api': 9})
        result = check([provider, consumer], {'kernel_api': v.Export(API, gpl_only=True)},
                       deps={'vendor_boot': {'consumer': ('provider',)}})
        self.assertIn('gpl_only_violation', errors(result))
        violation = next(f for f in result['findings'] if f['code'] == 'gpl_only_violation')
        self.assertEqual(violation['symbol'], 'vendor_api')
        self.assertTrue(violation['using_gplonly_symbols'])

    def test_reverse_gpl_order_keeps_weak_gpl_unresolved_and_can_load(self):
        provider = module('provider', license='Proprietary', exports={'vendor_api': v.Export(9)})
        consumer = module('consumer', depends=('provider',), undefined={'vendor_api': False, 'kernel_api': True},
                          versions={'module_layout': LAYOUT, 'kernel_api': API, 'vendor_api': 9})
        result = check([provider, consumer], {'kernel_api': v.Export(API, gpl_only=True)},
                       deps={'vendor_boot': {'consumer': ('provider',)}})
        self.assertEqual(result['status'], 'PASS')
        self.assertEqual(result['counts']['unresolved_optional_weak_imports'], 1)

    def test_weak_proprietary_availability_cannot_hide_gpl_order_failure(self):
        provider = module('provider', license='Proprietary', exports={'optional_vendor': v.Export(5), 'vendor_api': v.Export(9)})
        consumer = module('consumer', undefined={'optional_vendor': True, 'kernel_api': True, 'vendor_api': False},
                          versions={'module_layout': LAYOUT, 'optional_vendor': 5, 'kernel_api': API, 'vendor_api': 9})
        result = check([provider, consumer], {'kernel_api': v.Export(API, gpl_only=True)},
                       deps={'vendor_boot': {'consumer': ('provider',)}})
        self.assertIn('weak_proprietary_provider', errors(result))


class StageTests(unittest.TestCase):
    def pair(self, consumer_part='vendor_boot', provider_part='vendor_dlkm'):
        provider = module('provider', provider_part, exports={'late_api': v.Export(7)})
        consumer = module('consumer', consumer_part, depends=('provider',), undefined={'late_api': False},
                          versions={'module_layout': LAYOUT, 'late_api': 7})
        return consumer, provider

    def test_firststage_cannot_depend_on_late_partition(self):
        result = check(list(self.pair()), deps={'vendor_boot': {'consumer': ('provider',)}})
        self.assertTrue({'stage_late_dependency', 'stage_symbol_unavailable'} <= errors(result))

    def test_parallel_vendor_does_not_assume_system_ready(self):
        result = check(list(self.pair('vendor_dlkm', 'system_dlkm')))
        self.assertIn('stage_symbol_unavailable', errors(result))

    def test_parallel_system_does_not_assume_vendor_ready(self):
        result = check(list(self.pair('system_dlkm', 'vendor_dlkm')))
        self.assertIn('stage_symbol_unavailable', errors(result))

    def test_firststage_provider_available_to_both_later_stages(self):
        consumer, provider = self.pair('vendor_dlkm', 'vendor_boot')
        self.assertEqual(check([consumer, provider])['status'], 'PASS')

    def test_modinfo_depends_does_not_auto_load_an_unlisted_provider(self):
        consumer, provider = self.pair('vendor_boot', 'vendor_boot')
        result = check([consumer, provider], loads={'vendor_boot': ['consumer']})
        self.assertIn('stage_modinfo_dependency_not_deployed', errors(result))
        self.assertIn('stage_symbol_unavailable', errors(result))
        self.assertEqual(result['stages']['vendor_boot']['selected_count'], 1)

    def test_missing_dep_row_is_not_replaced_by_scanned_file(self):
        result = check([module()], deps={'vendor_boot': {'demo': None}})
        self.assertIn('stage_module_missing', errors(result))

    def test_modules_dep_adds_dependency_without_modinfo_depends(self):
        consumer, provider = self.pair('vendor_boot', 'vendor_boot')
        consumer.depends = ()
        result = check([consumer, provider], loads={'vendor_boot': ['consumer']}, deps={'vendor_boot': {'consumer': ('provider',)}})
        self.assertEqual(result['status'], 'PASS')
        self.assertEqual(result['stages']['vendor_boot']['selected_count'], 2)

    def test_owner_path_is_not_repaired_by_basename_search(self):
        consumer, provider = self.pair('vendor_boot', 'vendor_boot')
        result = check([consumer, provider], loads={'vendor_boot': ['consumer']},
                       deps={'vendor_boot': {'consumer': ('provider',), 'provider': v.DepEntry('wrong-directory/provider.ko')}})
        self.assertIn('stage_owner_path_missing', errors(result))

    def test_rhs_stale_path_uses_canonical_lookup_and_valid_owner_row(self):
        consumer, provider = self.pair('vendor_boot', 'vendor_boot')
        provider.path = 'vendor_boot/correct-directory/provider.ko'
        result = check([consumer, provider], loads={'vendor_boot': ['consumer']},
                       deps={'vendor_boot': {'consumer': ('/stale/unused/provider.ko',)}})
        self.assertEqual(result['status'], 'PASS')

    def test_absolute_vendor_aliases_map_to_the_same_preserved_path(self):
        consumer, provider = self.pair('vendor_dlkm', 'vendor_dlkm')
        for prefix in ('/vendor/lib/modules', '/vendor_dlkm/lib/modules'):
            with self.subTest(prefix=prefix):
                result = check([consumer, provider], loads={'vendor_dlkm': ['consumer']},
                               deps={'vendor_dlkm': {'consumer': v.DepEntry(prefix + '/consumer.ko', (prefix + '/provider.ko',)),
                                                    'provider': v.DepEntry(prefix + '/provider.ko')}})
                self.assertEqual(result['status'], 'PASS')

    def test_unknown_absolute_owner_prefix_is_not_guessed(self):
        result = check([module()], deps={'vendor_boot': {'demo': v.DepEntry('/wrong-root/demo.ko')}})
        self.assertIn('stage_owner_path_missing', errors(result))

    def test_same_stage_union_is_not_load_order_proof(self):
        consumer, provider = self.pair('vendor_boot', 'vendor_boot')
        result = check([consumer, provider], loads={'vendor_boot': ['consumer', 'provider']})
        self.assertIn('stage_symbol_unavailable', errors(result))

    def test_parallel_siblings_are_not_assumed_loaded(self):
        consumer, provider = self.pair('vendor_dlkm', 'vendor_dlkm')
        first = module('first', 'vendor_dlkm')
        result = check([first, consumer, provider], loads={'vendor_dlkm': ['first', 'consumer', 'provider']})
        self.assertIn('stage_symbol_unavailable', errors(result))

    def test_completed_first_vendor_request_can_supply_later_siblings(self):
        consumer, provider = self.pair('vendor_dlkm', 'vendor_dlkm')
        result = check([consumer, provider], loads={'vendor_dlkm': ['provider', 'consumer']})
        self.assertEqual(result['status'], 'PASS')

    def test_blocklist_excludes_top_level_but_blocks_required_dependency(self):
        normal, blocked = module('normal'), module('test_module')
        result = check([normal, blocked], blocked={'vendor_boot': ['test_module']})
        self.assertEqual(result['status'], 'PASS')
        self.assertEqual(result['stages']['vendor_boot']['selected_modules'], ['normal'])
        result = check([normal, blocked], blocked={'vendor_boot': ['test_module']},
                       deps={'vendor_boot': {'normal': ('test_module',)}})
        self.assertIn('stage_dependency_blocklisted', errors(result))

    def test_load_only_exclusion_does_not_block_hard_dependency(self):
        consumer, provider = self.pair('vendor_boot', 'vendor_boot')
        result = check([consumer, provider], excluded={'vendor_boot': ['provider']},
                       deps={'vendor_boot': {'consumer': ('provider',)}})
        self.assertEqual(result['status'], 'PASS')
        self.assertEqual(result['stages']['vendor_boot']['selected_count'], 2)
        self.assertEqual(result['stages']['vendor_boot']['excluded_load_entries'], ['provider.ko'])
        self.assertEqual(result['stages']['vendor_boot']['declared_blocklist'], [])

    def test_real_first_vendor_request_is_not_replaced_after_hard_block(self):
        first = module('foo_bar', 'vendor_dlkm', filename='foo-bar.ko')
        second = module('valid_module', 'vendor_dlkm')
        result = check([first, second], blocked={'vendor_dlkm': ['foo_bar']},
                       excluded={'vendor_dlkm': ['foo_bar']})
        self.assertIn('stage_first_module_unloadable', errors(result))
        stage = result['stages']['vendor_dlkm']
        self.assertEqual(stage['first_shell_request'], 'foo-bar.ko')
        self.assertIsNone(stage['first_shell_request_row'])

    def test_system_first_local_hard_block_is_not_shell_filtered(self):
        result = check([module('first_module', 'system_dlkm'), module('later_module', 'system_dlkm')],
                       blocked={'system_dlkm': ['first_module']})
        self.assertIn('stage_first_module_unloadable', errors(result))

    def test_empty_post_filter_shell_load_list_is_not_pass(self):
        result = check([module('excluded_module', 'vendor_dlkm')], excluded={'vendor_dlkm': ['excluded_module']})
        self.assertIn('stage_empty_filtered_load_list', errors(result))

    def test_dependency_cycle_and_missing_load_target(self):
        result = check([module('alpha'), module('beta')], deps={'vendor_boot': {'alpha': ('beta',), 'beta': ('alpha',)}})
        self.assertIn('stage_dependency_cycle', errors(result))
        self.assertIn('stage_module_missing', errors(check([module()], loads={'vendor_boot': ['does_not_exist']})))

    def test_firststage_residency_does_not_replace_new_process_dep_row(self):
        base = module('actual_name', filename='file-alias.ko', exports={'early_api': v.Export(1)})
        late = module('late', 'vendor_dlkm', depends=('actual_name',), undefined={'early_api': False},
                      versions={'module_layout': LAYOUT, 'early_api': 1})
        self.assertEqual(check([base, late])['status'], 'PASS')  # Symbol already in the kernel.
        result = check([base, late], deps={'vendor_dlkm': {'late': ('file-alias.ko',)}})
        self.assertIn('stage_late_dependency', errors(result))  # Explicit loader request still needs a row/file.

    def test_firststage_residency_does_not_bypass_wrong_local_owner_path(self):
        base = module('provider', exports={'early_api': v.Export(1)})
        copied = replace(base, partition='vendor_dlkm', path='vendor_dlkm/provider.ko')
        late = module('late', 'vendor_dlkm')
        result = check([base, copied, late], deps={'vendor_dlkm': {'late': ('provider',),
                                                                'provider': v.DepEntry('absent/provider.ko')}})
        self.assertIn('stage_owner_path_missing', errors(result))

    def test_duplicate_load_entries_do_not_inflate_module_count(self):
        result = check([module()], loads={'vendor_boot': ['demo', 'demo.ko', 'demo']})
        self.assertEqual(result['stages']['vendor_boot']['selected_count'], 1)

    def test_modinfo_softdep_does_not_create_runtime_edges(self):
        result = check([module(softdep=('pre: unknown',))])
        self.assertEqual(result['status'], 'PASS')
        self.assertEqual(result['stages']['vendor_boot']['optional_attempts'], [])

    def test_missing_or_blocked_optional_targets_do_not_fail_owner(self):
        for target, blocked in [('absent_module', []), ('provider', ['provider'])]:
            with self.subTest(target=target):
                result = check([module(), module('provider')], loads={'vendor_boot': ['demo']},
                               blocked={'vendor_boot': blocked}, softdeps={'vendor_boot': [v.Softdep('demo', 'pre', target)]})
                self.assertEqual(result['status'], 'PASS')
                attempt = result['stages']['vendor_boot']['optional_attempts'][0]
                self.assertFalse(attempt['result_required'])
                self.assertEqual(attempt['severity'], 'info')

    def test_optional_attempt_can_be_unresolvable_locally_but_provider_in_baseline(self):
        base, late = module('early'), module('late', 'vendor_dlkm')
        result = check([base, late], softdeps={'vendor_dlkm': [v.Softdep('late', 'pre', 'early')]})
        self.assertEqual(result['status'], 'PASS')
        attempt = result['stages']['vendor_dlkm']['optional_attempts'][0]
        self.assertTrue(attempt['baseline_provider_present'])
        self.assertEqual(attempt['local_candidates'], [])

    def test_softdep_pre_or_post_does_not_hide_a_missing_hard_provider(self):
        consumer, provider = self.pair('vendor_boot', 'vendor_boot')
        consumer.depends = ()
        for kind in ('pre', 'post'):
            with self.subTest(kind=kind):
                result = check([consumer, provider], loads={'vendor_boot': ['consumer']},
                               softdeps={'vendor_boot': [v.Softdep('consumer', kind, 'provider')]})
                self.assertIn('stage_symbol_unavailable', errors(result))
                self.assertEqual(result['stages']['vendor_boot']['selected_modules'], ['consumer'])

    def test_raw_softdep_owner_is_not_canonicalized(self):
        m = module('owner_name', filename='owner-name.ko')
        result = check([m], softdeps={'vendor_boot': [v.Softdep('owner-name.ko', 'pre', 'missing')]})
        self.assertEqual(result['status'], 'PASS')
        self.assertEqual(result['stages']['vendor_boot']['optional_attempts'], [])

    def test_optional_pre_recursion_fails_but_post_self_reference_is_cached(self):
        result = check([module()], softdeps={'vendor_boot': [v.Softdep('demo', 'pre', 'demo')]})
        self.assertIn('stage_softdep_recursion', errors(result))
        result = check([module()], softdeps={'vendor_boot': [v.Softdep('demo', 'post', 'demo')]})
        self.assertEqual(result['status'], 'PASS')

    def test_reachable_hard_pre_cycle_is_not_ignored_as_optional(self):
        result = check([module('alpha'), module('beta')], loads={'vendor_boot': ['alpha']},
                       deps={'vendor_boot': {'beta': ('alpha',)}},
                       softdeps={'vendor_boot': [v.Softdep('alpha', 'pre', 'beta')]})
        self.assertIn('stage_softdep_recursion', errors(result))

    def test_post_target_recursion_is_checked_even_outside_hard_closure(self):
        result = check([module('alpha'), module('beta'), module('gamma')], loads={'vendor_boot': ['alpha']},
                       softdeps={'vendor_boot': [v.Softdep('alpha', 'post', 'beta'),
                                                v.Softdep('beta', 'pre', 'gamma'), v.Softdep('gamma', 'pre', 'beta')]})
        self.assertIn('stage_softdep_recursion', errors(result))

    def test_noncanonical_hard_alias_resolution_is_explicitly_unsupported(self):
        consumer, provider = self.pair('vendor_boot', 'vendor_boot')
        with self.assertRaisesRegex(v.InputError, 'hard alias resolution'):
            check([consumer, provider], loads={'vendor_boot': ['consumer']},
                  deps={'vendor_boot': {'consumer': ('virtual-provider',)}},
                  aliases={'vendor_boot': [('virtual-provider', 'provider')]})

    def test_identity_alias_does_not_change_the_load_set(self):
        result = check([module()], aliases={'vendor_boot': [('demo.ko', 'demo')]})
        self.assertEqual(result['status'], 'PASS')

    def test_multi_provider_alias_is_explicitly_unsupported_not_pass(self):
        with self.assertRaisesRegex(v.InputError, 'hard alias resolution'):
            check([module('owner'), module('alpha'), module('beta')],
                  deps={'vendor_boot': {'owner': ('virtual',)}},
                  aliases={'vendor_boot': [('virtual', 'alpha'), ('virtual', 'beta')]})

    def test_firmware_shaped_missing_soft_targets_are_not_fuzzily_renamed(self):
        modules = [module('pinctrl_msm'), module('arm_smmu'), module('msm_video', 'vendor_dlkm'),
                   module('msm_kgsl', 'vendor_dlkm'), module('ipam', 'vendor_dlkm')]
        soft = {'vendor_boot': v.parse_softdep_file('softdep pinctrl_msm pre: qcom-pdc\n'),
                'vendor_dlkm': v.parse_softdep_file('softdep msm_video pre: subsys-pil-tz\nsoftdep msm_kgsl pre: qcom-arm-smmu-mod\nsoftdep ipam pre: subsys-pil-tz\nsoftdep ipam pre: qcom-arm-smmu-mod\n')}
        result = check(modules, softdeps=soft)
        self.assertEqual(result['status'], 'PASS')
        attempts = [a for stage in result['stages'].values() for a in stage['optional_attempts']]
        self.assertEqual(len(attempts), 5)
        self.assertTrue(all(a['status'] == 'missing_optional' and not a['local_candidates'] for a in attempts))


class CLITests(unittest.TestCase):
    def run_cli(self, candidates=None, *, inv=None, config_texts=None, extra=(), report_write_error=None):
        candidates = candidates or {'candidate.symvers': SYMVERS}
        configs = config_texts or {'candidate.config': CONFIG}
        inv = inv or inventory([module(undefined={'kernel_api': False}, versions={'module_layout': LAYOUT, 'kernel_api': API})])
        data = {**candidates, **configs}
        def fake_read(path):
            value = data[str(path)]
            if isinstance(value, Exception):
                raise value
            return value.encode()
        args = list(candidates)
        for name in configs:
            args.extend(['--kernel-config', name])
        args.extend(extra)
        captured = []
        with patch.object(v, 'load_inventory', return_value=inv), patch.object(v, 'read_bytes', side_effect=fake_read), \
             patch.object(v, 'emit_report', side_effect=lambda report: captured.append(report)), \
             patch.object(v.Path, 'write_text', side_effect=report_write_error) as output:
            code = v.main(args)
        return code, captured[0], output

    def test_pass_exit_and_json_contains_input_hashes(self):
        code, report, output = self.run_cli(extra=('--json', 'verification.json'))
        self.assertEqual(code, 0)
        saved = json.loads(output.call_args.args[0])
        candidate = saved['candidates'][0]
        self.assertEqual(saved['status'], 'PASS')
        self.assertEqual(candidate['symvers'], 'candidate.symvers')
        self.assertEqual(candidate['kernel_config'], 'candidate.config')
        self.assertEqual(candidate['symvers_sha256'], v.digest(SYMVERS.encode()))
        self.assertEqual(candidate['config_sha256'], v.digest(CONFIG.encode()))

    def test_fail_returns_nonzero_and_multi_candidate_does_not_hide_failure(self):
        bad = SYMVERS.replace(f'0x{API:08x}', '0x00000001')
        for candidates in ({'bad.symvers': bad, 'good.symvers': SYMVERS},
                           {'good.symvers': SYMVERS, 'bad.symvers': bad}):
            with self.subTest(order=list(candidates)):
                code, report, _ = self.run_cli(candidates)
                self.assertEqual(code, 1)
                self.assertEqual(report['status'], 'FAIL')
                self.assertEqual({c['status'] for c in report['candidates']}, {'PASS', 'FAIL'})
                self.assertEqual(len(report['candidates']), 2)

    def test_read_or_parse_error_is_nonzero_and_other_candidate_is_evaluated(self):
        for bad in (v.InputError('permission denied'), '', 'not a symvers file'):
            with self.subTest(bad=bad):
                code, report, _ = self.run_cli({'bad.symvers': bad, 'good.symvers': SYMVERS})
                self.assertEqual(code, 2)
                self.assertEqual([c['status'] for c in report['candidates']], ['ERROR', 'PASS'])

    def test_paired_configs_apply_to_correct_candidates(self):
        namespaced = SYMVERS.replace('\tkernel_api\tvmlinux\tEXPORT_SYMBOL\t\n', f'\tkernel_api\tvmlinux\tEXPORT_SYMBOL\t{NS}\n')
        allow = CONFIG.replace('# CONFIG_MODULE_ALLOW_MISSING_NAMESPACE_IMPORTS is not set', 'CONFIG_MODULE_ALLOW_MISSING_NAMESPACE_IMPORTS=y')
        code, report, _ = self.run_cli({'one.symvers': namespaced, 'two.symvers': namespaced}, config_texts={'strict.config': CONFIG, 'allow.config': allow})
        self.assertEqual(code, 1)
        self.assertEqual([c['status'] for c in report['candidates']], ['FAIL', 'PASS'])
        self.assertEqual(report['candidates'][1]['counts']['warnings'], 1)

    def test_json_write_error_is_nonzero(self):
        code, report, _ = self.run_cli(extra=('--json', 'locked.json'), report_write_error=PermissionError('read-only destination'))
        self.assertEqual(code, 2)
        self.assertEqual(report['input_errors'][0]['code'], 'report_write_error')

    def test_json_cannot_overwrite_input(self):
        code, report, output = self.run_cli(extra=('--json', 'candidate.config'))
        self.assertEqual(code, 2)
        output.assert_not_called()

    def test_inventory_read_error_is_nonzero(self):
        with patch.object(v, 'load_inventory', side_effect=v.InputError('empty root')), patch.object(v, 'emit_report') as emit:
            self.assertEqual(v.main(['candidate.symvers', '--kernel-config', 'candidate.config']), 2)
        self.assertEqual(emit.call_args.args[0]['input_errors'][0]['code'], 'input_error')

    def test_unexpected_internal_exception_never_becomes_pass(self):
        with patch.object(v, 'load_inventory', side_effect=RuntimeError('unexpected')), patch.object(v, 'emit_report') as emit:
            self.assertEqual(v.main(['candidate.symvers', '--kernel-config', 'candidate.config']), 2)
        self.assertEqual(emit.call_args.args[0]['input_errors'][0]['code'], 'internal_error')

    def test_command_line_errors_missing_config_bad_option_bad_mode(self):
        for args in (['candidate.symvers'], ['candidate.symvers', '--kernel-config', 'x', '--wrong-option'],
                     ['candidate.symvers', '--kernel-config', 'x', '--audio-mode', 'invalid']):
            with self.subTest(args=args), contextlib.redirect_stderr(io.StringIO()):
                self.assertEqual(v.main(args), 2)

    def test_help_does_not_load_files(self):
        with contextlib.redirect_stdout(io.StringIO()), patch.object(v, 'load_inventory') as load:
            self.assertEqual(v.main(['--help']), 0)
        load.assert_not_called()


if __name__ == '__main__':
    unittest.main()
