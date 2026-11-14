#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""Debian-only, fresh-workdir profile build; never reuses published binaries."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import shutil
import struct
import subprocess
import sys

sys.path.insert(0, str(Path(__file__).resolve().parent))
from abi import Elf, FormatError, sha256
from verify_profile import verify

ROOT=Path(__file__).resolve().parent.parent


def run(command, env=None):
    print('+', ' '.join(map(str,command)),flush=True)
    subprocess.run(list(map(str,command)),check=True,env=env)


def configure(text, key, value):
    lines=text.splitlines()
    result=[]
    for line in lines:
        if line.startswith(key+'=') or line==f'# {key} is not set':
            continue
        result.append(line)
    result.append(f'{key}={value}' if value is not None else f'# {key} is not set')
    return '\n'.join(result)+'\n'


def source_identity(src, profile):
    if (src/'.git').exists():
        commit=subprocess.check_output(['git','-C',str(src),'rev-parse','HEAD'],text=True).strip()
        if commit!=profile['source']['commit']:
            raise ValueError('source commit differs from pinned profile')
        diff=subprocess.check_output(['git','-C',str(src),'diff','HEAD','--'])
        return {'commit':commit,'tracked_diff_sha256':hashlib.sha256(diff).hexdigest(),
                'reference_not_exact_oem':profile['source']['status']!='matched'}
    record_path = src / '.ctnp-source.json'
    if not record_path.is_file():
        raise ValueError('source needs a pinned Git checkout or archive provenance')
    record = json.loads(record_path.read_text())
    expected = profile['source']
    if record.get('repo') != expected['repo'] or record.get('commit') != expected['commit']:
        raise ValueError('archive source identity differs from pinned profile')
    if record.get('archive_sha256') != expected.get('archive_sha256'):
        raise ValueError('archive source digest differs from profile')
    relink = src / '.ctnp-relink.json'
    return {'commit': record['commit'], 'archive_sha256': record['archive_sha256'],
            'relink_sha256': sha256(relink) if relink.is_file() else None,
            'reference_not_exact_oem': expected['status'] != 'matched'}


def main():
    ap=argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--profile',required=True,choices=('6.1.141','6.1.157'))
    ap.add_argument('--srcdir',required=True,type=Path)
    ap.add_argument('--work',required=True,type=Path)
    ap.add_argument('--clang',required=True,type=Path)
    ap.add_argument('--jobs',type=int,default=4)
    ap.add_argument('--output-root',type=Path,default=ROOT/'out',help='isolated output root; existing profile output is never overwritten')
    ap.add_argument('--prepare-only',action='store_true',help='prepare headers and compare BTF layout; do not build an installable KO')
    a=ap.parse_args()
    if 'ID=debian' not in Path('/etc/os-release').read_text().splitlines():
        raise ValueError('all builds must run on Debian')
    if a.jobs<1:raise ValueError('jobs must be positive')
    profile_path=ROOT/'profiles'/a.profile/'profile.json'
    p=json.loads(profile_path.read_text())
    if p['source']['status']=='unavailable':raise ValueError('profile source is unavailable')
    for entry in p['files'].values():
        path=profile_path.parent/entry['path']
        if Path(entry['path']).name!=entry['path'] or sha256(path)!=entry['sha256']:
            raise ValueError('profile input hash mismatch')
    src=a.srcdir.resolve()
    identity=source_identity(src,p)
    compiler=subprocess.check_output([str(a.clang),'--version'],text=True)
    if 'clang version 17.0.2' not in compiler:raise ValueError('requires captured clang 17.0.2')
    env=dict(os.environ,PATH=str(a.clang.parent)+os.pathsep+os.environ['PATH'])
    overrides = ('KCFLAGS', 'KCPPFLAGS', 'CFLAGS_KERNEL', 'CFLAGS_MODULE',
                 'KBUILD_CFLAGS', 'KBUILD_CPPFLAGS', 'MAKEFLAGS', 'MFLAGS')
    if any(env.get(key) for key in overrides):
        raise ValueError('unrecorded kernel build environment overrides are not allowed')
    for tool in ('ld.lld','llvm-strip','llvm-objcopy','pahole','gcc','make'):
        if not shutil.which(tool,path=env['PATH']):raise ValueError('missing required tool '+tool)
    work=a.work.resolve()
    if work.exists():raise ValueError('work directory must not already exist')
    work.mkdir(parents=True)
    module=work/'module';module.mkdir()
    (module/'kernel').mkdir()
    for path in (ROOT/'ctn_patch.c',ROOT/'Kbuild',*sorted((ROOT/'kernel').glob('*.h'))):
        dest=module/path.relative_to(ROOT)
        shutil.copyfile(path,dest)
    build_id=subprocess.check_output([sys.executable,'-I',str(ROOT/'tools/profile_header.py'),
        '--profile',str(profile_path),'--module',str(module),'--compiler',compiler],text=True).strip()
    out=work/'kernel-out';out.mkdir()
    cfg=(profile_path.parent/'kernel.config').read_text()
    localversion=p['vendor']['vermagic'].split(' ',1)[0][len(p['kernel_version']):]
    cfg=configure(cfg,'CONFIG_LOCALVERSION',json.dumps(localversion))
    cfg=configure(cfg,'CONFIG_LOCALVERSION_AUTO',None)
    cfg=configure(cfg,'CONFIG_UNUSED_KSYMS_WHITELIST','""')
    (out/'.config').write_text(cfg)
    common=['make','-C',src,f'O={out}','ARCH=arm64','LLVM=1',f'CC={a.clang}',
            'HOSTCC=gcc','HOSTCXX=g++',f'-j{a.jobs}']
    version=subprocess.check_output(common+['-s','kernelversion'],env=env,text=True).strip()
    if version!=p['kernel_version']:raise ValueError('real source version mismatch: '+version)
    run(common+['olddefconfig'],env)
    run(common+['modules_prepare'],env)
    shutil.copyfile(profile_path.parent/'Module.symvers',out/'Module.symvers')
    run(common+[f'M={module}','ctn_abi_probe.o'],env)
    probe=Elf(module/'ctn_abi_probe.o')
    raw=probe.symbol_bytes('ctn_abi_probe')
    labels=json.loads((module/'probe-labels.json').read_text())
    if len(raw)!=len(labels)*8:raise ValueError('invalid layout probe')
    actual=dict(zip(labels,struct.unpack('<'+'Q'*len(labels),raw)))
    expected={}
    for name,s in p['abi']['structures'].items():
        expected[name+'.sizeof']=s['size']
        expected.update({name+'.offsetof.'+key:value for key,value in s['members'].items()})
    layout_report = {'profile': p['id'], 'actual': actual, 'expected': expected,
                     'passed': actual == expected, 'source': identity}
    (work/'layout-verification.json').write_text(json.dumps(layout_report,indent=2)+'\n')
    if actual!=expected:
        raise ValueError('compiled layouts differ from captured device BTF')
    if a.prepare_only:
        print('Header layout passed; no KO built or load compatibility claimed:', work)
        return
    run(common+[f'M={module}','modules'],env)
    ko=module/'ctn_patch.ko'
    run([a.clang.parent/'llvm-strip','--strip-debug',ko],env)
    report = verify(profile_path, ko, module/'ctn_abi_probe.o',
                    module/'probe-labels.json', build_id)
    report.update(source=identity, compiler=compiler)
    (work/'verification.json').write_text(json.dumps(report,indent=2)+'\n')
    if report['static'] != 'passed':
        raise ValueError('static verification failed; no installable output published')
    dest=a.output_root.resolve()/a.profile
    if dest.exists():raise ValueError('output exists; preserve previous output and select another work cycle explicitly')
    dest.mkdir(parents=True)
    shutil.copyfile(ko,dest/'ctn_patch.ko')
    shutil.copyfile(work/'verification.json',dest/'verification.json')
    print('Static checks passed; runtime NOT TESTED:',dest)

if __name__=='__main__':
    try:main()
    except (ValueError,FormatError,OSError,subprocess.SubprocessError) as exc:
        print('BUILD BLOCKED:',exc,file=sys.stderr);sys.exit(1)
