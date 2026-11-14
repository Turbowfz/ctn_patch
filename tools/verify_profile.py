#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""Verify a profile-built KO without loading it; runtime is always separate."""
import argparse
import json
from pathlib import Path
import struct
import sys

sys.path.insert(0, str(Path(__file__).resolve().parent))
from abi import Elf, FormatError, sha256


def verify(profile_path, module_path, probe_path, labels_path, build_id):
    spec = json.loads(profile_path.read_text(encoding='utf-8'))
    for record in spec['files'].values():
        if Path(record['path']).name != record['path']:
            raise FormatError('profile input must have a local filename')
        if sha256(profile_path.parent / record['path']) != record['sha256']:
            raise FormatError('profile input digest mismatch')
    ko = Elf(module_path)
    labels = json.loads(labels_path.read_text(encoding='utf-8'))
    probe = Elf(probe_path).symbol_bytes('ctn_abi_probe')
    if len(labels) != len(set(labels)) or len(probe) != len(labels) * 8:
        raise FormatError('duplicate labels or invalid ABI probe')
    actual = dict(zip(labels, struct.unpack('<' + 'Q' * len(labels), probe)))
    expected = {}
    for name, layout in spec['abi']['structures'].items():
        expected[name + '.sizeof'] = layout['size']
        expected.update({name + '.offsetof.' + key: value for key, value in layout['members'].items()})
    baseline = {}
    for line in (profile_path.parent / spec['files']['symvers']['path']).read_text().splitlines():
        parts = line.split()
        if len(parts) < 4:
            raise FormatError('invalid symbol baseline')
        if parts[1] in baseline:
            raise FormatError('duplicate baseline symbol')
        baseline[parts[1]] = int(parts[0], 16)
    versions = ko.versions()
    imports = ko.undefined()
    crcs_ok = bool(versions) and all(baseline.get(k) == v for k, v in versions.items())
    required = set(spec['abi']['required_sections']) | {'__ex_table'}
    checks = {
        'vermagic': ko.one_info('vermagic') == spec['vendor']['vermagic'],
        'symbol_crcs': crcs_ok,
        'undefined_symbols': imports <= versions.keys() and imports <= baseline.keys(),
        'module_layout': actual == expected and ko.by_name.get('.gnu.linkonce.this_module', {}).get('size') == expected['module.sizeof'],
        'elf_sections': ko.machine == 183 and ko.type == 1 and required <= ko.by_name.keys(),
        'identity': ko.one_info('ctn_profile') == spec['id'] and ko.one_info('ctn_build_id') == build_id,
        'license': ko.one_info('license') == 'GPL',
        'safety_evidence': spec['abi'].get('name_readers_rcu_verified') is True,
    }
    callbacks = {'ctn_read': 'ct_enable_proc_read', 'ctn_write': 'ct_enable_proc_write',
                 'init_module': 'init_module', 'cleanup_module': 'cleanup_module'}
    types = {}
    for function, reference in callbacks.items():
        try:
            types[function] = {'actual': f'0x{ko.kcfi(function):08x}', 'expected': spec['abi']['kcfi'][reference]}
        except FormatError as exc:
            types[function] = {'error': str(exc), 'expected': spec['abi']['kcfi'][reference]}
    checks['kcfi'] = all(t.get('actual') == t['expected'] for t in types.values())
    for fn in ('ctn_lookup_stub', 'ctn_find_module_stub', 'ctn_nofault_stub'):
        try:
            types[fn] = {'local_type': f'0x{ko.kcfi(fn):08x}', 'live_check': 'required_before_indirect_call'}
        except FormatError as exc:
            types[fn] = {'error': str(exc)}
    checks['dynamic_kcfi_bootstrap'] = all('local_type' in types[fn] for fn in ('ctn_lookup_stub', 'ctn_find_module_stub', 'ctn_nofault_stub'))
    return {
        'schema': 1, 'profile_id': spec['id'], 'profile_sha256': sha256(profile_path),
        'module_sha256': sha256(module_path), 'build_id': build_id,
        'static': 'passed' if all(checks.values()) else 'failed', 'runtime': 'not_tested',
        'checks': [{'name': name, 'passed': bool(value)} for name, value in checks.items()],
        'layout': actual, 'kcfi': types,
        'imports': {name: f'0x{versions[name]:08x}' if name in versions else None for name in sorted(imports)},
        'module_layout_crc': f'0x{versions["module_layout"]:08x}' if 'module_layout' in versions else None,
        'note': 'Static checks do not establish successful insmod, node operation or unload on a real device.',
    }


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--profile', type=Path, required=True)
    p.add_argument('--module', type=Path, required=True)
    p.add_argument('--probe', type=Path, required=True)
    p.add_argument('--labels', type=Path, required=True)
    p.add_argument('--build-id', required=True)
    p.add_argument('--out', type=Path, required=True)
    a = p.parse_args()
    report = verify(a.profile, a.module, a.probe, a.labels, a.build_id)
    a.out.write_text(json.dumps(report, ensure_ascii=False, indent=2) + '\n', encoding='utf-8')
    for check in report['checks']:
        print(('PASS ' if check['passed'] else 'FAIL ') + check['name'])
    print('runtime: not_tested')
    return 0 if report['static'] == 'passed' else 1


if __name__ == '__main__':
    try:
        raise SystemExit(main())
    except (FormatError, ValueError, KeyError, OSError) as exc:
        print('VERIFICATION FAILED:', exc, file=sys.stderr)
        raise SystemExit(1)
