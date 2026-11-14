#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""Report only relevant vendor functions and relocations, without executing inputs."""
import argparse
import hashlib
import json
import pathlib
import subprocess
import sys

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent))
from abi import Elf

parser=argparse.ArgumentParser()
parser.add_argument('vendor')
parser.add_argument('--objdump', required=True)
parser.add_argument('--out', type=pathlib.Path, required=True)
a=parser.parse_args()
a.out.mkdir(parents=True,exist_ok=False)
e=Elf(a.vendor)
relocs=e.relocations('.text')
records={}
for s in e.symbols:
    name=s['name']
    if name not in ('update_critical_task_time','get_critical_task_state','sched_switch_hook','rt_info_proc_write') or s['type']!=2:
        continue
    key=f'{name}-{s["value"]:x}'
    data=e.symbol_bytes(s)
    used=[dict(offset=r['offset']-s['value'],type=r['type'],symbol=r['symbol']['name'],addend=r['addend'])
          for r in relocs if s['value']<=r['offset']<s['value']+s['size']]
    command=[a.objdump,'-d','--no-show-raw-insn',f'--start-address={s["value"]}',f'--stop-address={s["value"]+s["size"]}',a.vendor]
    text=subprocess.run(command,capture_output=True,text=True,check=True).stdout
    (a.out/(key+'.txt')).write_text(text,encoding='utf-8')
    records[key]={'name':name,'size':s['size'],'sha256':hashlib.sha256(data).hexdigest(),'section':s['section'],'offset':s['value'],'relocations':used}
    print(name, 'size',s['size'],'hash',records[key]['sha256'])
    for line in text.splitlines():
        if any(k in line for k in ('#0x848','#2120','#0x18','#24', '#0x630', '#1584')):
            print(line)
    print('reloc refs',sorted(set(r['symbol'] for r in used)))
(a.out/'functions.json').write_text(json.dumps(records,indent=2)+'\n',encoding='utf-8')
critical=e.symbol('critical_task')
refs=[]
for r in e.relocations('.text'):
    target=r['symbol']
    if target['name']=='critical_task' or target['section']==critical['section'] and target['value']+r['addend']==critical['value']:
        funcs=[s['name'] for s in e.symbols if s['type']==2 and s['section']=='.text' and s['value']<=r['offset']<s['value']+s['size']]
        refs.extend(funcs)
print('critical_task code references', sorted(set(refs)))
