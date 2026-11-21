#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""Generate compile-time profile gates and a layout probe from captured ABI."""
import argparse
import hashlib
import json
from pathlib import Path


def gates(profile):
    p=profile
    a=p['abi']
    s=a['structures']
    values={'CTN_PROFILE_ID':p['id'],'CTN_RUNTIME_RELEASE':p['runtime_releases'][0],
            'CTN_MODULE_VERMAGIC':p['vendor']['vermagic'],
            'CTN_NAME_READERS_RCU_VERIFIED':int(a['name_readers_rcu_verified']),
            'CTN_PID_SOURCE_VERIFIED':int(a['pid_source_verified']),
            'CTN_EXPECT_MODULE_SIZE':s['module']['size'],
            'CTN_EXPECT_TASK_COMM_OFFSET':s['task_struct']['members']['comm'],
            'CTN_EXPECT_TASK_PID_OFFSET':s['task_struct']['members']['pid'],
            'CTN_EXPECT_TASK_TGID_OFFSET':s['task_struct']['members']['tgid'],
            'CTN_EXPECT_RRT_SIZE':24,'CTN_EXPECT_RT_LOCK_SIZE':a['vendor_symbols']['rt_info_rwlock']['size'],
            'CTN_EXPECT_CRITICAL_TASK_BYTES':a['vendor_symbols']['critical_task']['size'],
            'CTN_EXPECT_DEFAULT0':'UnityMain','CTN_EXPECT_DEFAULT1':'UnityGfxDevice'}
    for structname,prefix in (('module','CTN_EXPECT_MODULE_'),('module_layout','CTN_EXPECT_MODULE_LAYOUT_'),('mod_kallsyms','CTN_EXPECT_MOD_KALLSYMS_')):
        values[prefix+'SIZE']=s[structname]['size']
        for field,offset in s[structname]['members'].items():
            values[prefix+field.upper()+'_OFFSET']=offset
    chain = a.get('ctn_chain')
    if chain:
        if chain.get('private_layout_verified') is not True:
            raise ValueError('缺少已验证的 CTN 链路布局证据')
        structures = chain.get('structures', {})
        required = ('tracepoint', 'tracepoint_func', 'thread_info')
        if any(name not in structures for name in required):
            raise ValueError('CTN 链路结构证据不完整')
        values['CTN_CHAIN_VERIFIED'] = 1
        values['CTN_EXPECT_CHB_LOCK_SIZE'] = chain['raw_spinlock_size']
        values['CTN_EXPECT_TRACE_FUNC_SIZE'] = chain['tracepoint_func_size']
        values['CTN_EXPECT_TRACE_PRIORITY_OFFSET'] = chain['tracepoint_func_priority_offset']
        values['CTN_EXPECT_TASK_CPU_OFFSET'] = chain['task_cpu_offset']
        values['CTN_EXPECT_TASK_USAGE_OFFSET'] = chain['task_usage_offset']
        values['CTN_EXPECT_REFCOUNT_SIZE'] = chain['refcount_size']
        values['CTN_CHAIN_PRIORITY'] = chain['tracepoint_priority']
        tp = structures['tracepoint']
        values['CTN_EXPECT_TRACEPOINT_SIZE'] = tp['size']
        values['CTN_EXPECT_TRACEPOINT_FUNCS_OFFSET'] = tp['members']['funcs']
        func = structures['tracepoint_func']
        values['CTN_EXPECT_TRACE_FUNC_PTR_OFFSET'] = func['members']['func']
        values['CTN_EXPECT_TRACE_FUNC_DATA_OFFSET'] = func['members']['data']
        sched = chain['functions']['sched']
        values['CTN_CHAIN_SCHED_SIZE'] = sched['size']
        values['CTN_CHAIN_SCHED_KCFI'] = int(sched['kcfi'], 16)
    return values


def probe_source(profile):
    values=[]
    labels=[]
    for name,structure in profile['abi']['structures'].items():
        values.append(f'sizeof(struct {name})');labels.append(f'{name}.sizeof')
        for field in structure['members']:
            values.append(f'offsetof(struct {name}, {field})');labels.append(f'{name}.offsetof.{field}')
    source='#include <linux/module.h>\n#include <linux/sched.h>\n#include <linux/stddef.h>\n'
    source+='const unsigned long long ctn_abi_probe[] __attribute__((used)) = {\n'
    source+=',\n'.join('    '+v for v in values)+'\n};\n'
    return source,labels


def main():
    ap=argparse.ArgumentParser()
    ap.add_argument('--profile',type=Path,required=True)
    ap.add_argument('--module',type=Path,required=True)
    ap.add_argument('--compiler',required=True)
    a=ap.parse_args()
    p=json.loads(a.profile.read_text())
    values=gates(p)
    h=hashlib.sha256(a.profile.read_bytes()+a.compiler.encode())
    inputs = [a.module / 'Kbuild', a.module / 'ctn_patch.c']
    inputs += sorted(f for f in (a.module / 'kernel').glob('*.h')
                     if f.name != 'ctn_profile.h')
    for f in inputs:
        if not f.is_file() or f.is_symlink():
            raise ValueError('missing/unsafe build identity input: ' + str(f))
        h.update(str(f.relative_to(a.module)).encode())
        h.update(b'\0')
        h.update(f.read_bytes())
        h.update(b'\0')
    values['CTN_BUILD_ID']=h.hexdigest()
    lines=['/* Generated from captured ABI; do not edit. */','#ifndef CTN_PROFILE_H','#define CTN_PROFILE_H']
    lines.extend('#define '+k+' '+(json.dumps(v) if isinstance(v,str) else str(v)) for k,v in values.items())
    lines.append('#endif')
    (a.module/'kernel').mkdir(exist_ok=True)
    (a.module/'kernel/ctn_profile.h').write_text('\n'.join(lines)+'\n')
    source,labels=probe_source(p)
    (a.module/'ctn_abi_probe.c').write_text(source)
    (a.module/'probe-labels.json').write_text(json.dumps(labels)+'\n')
    print(values['CTN_BUILD_ID'])

if __name__=='__main__':main()
