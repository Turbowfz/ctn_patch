#!/usr/bin/env python3
"""Generate a profile from local device evidence without executing it."""
import argparse
import gzip
import json
import pathlib
import sys

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent))
from abi import Btf, Elf, FormatError, sha256


def main():
    p=argparse.ArgumentParser()
    p.add_argument('--id', required=True, choices=('6.1.141','6.1.157'))
    p.add_argument('--vendor', type=pathlib.Path, required=True)
    p.add_argument('--modules', type=pathlib.Path, required=True)
    p.add_argument('--btf', type=pathlib.Path, required=True)
    p.add_argument('--config', type=pathlib.Path, required=True)
    p.add_argument('--release', required=True)
    p.add_argument('--firmware', required=True)
    p.add_argument('--out', type=pathlib.Path, required=True)
    p.add_argument('--source-commit')
    p.add_argument('--source-status', choices=('reference','matched','unavailable'), default='unavailable')
    a=p.parse_args()
    vendor=Elf(a.vendor)
    if vendor.machine!=183 or vendor.one_info('name')!='oplus_bsp_game_opt':
        raise SystemExit('wrong vendor ELF')
    vm=vendor.one_info('vermagic')
    if not vm.startswith(a.id+'-') or not a.release.startswith(a.id+'-'):
        raise SystemExit('kernel/vendor profile mismatch')
    btf=Btf(a.btf)
    required={'module':['state','list','name','init','core_layout','init_layout','kallsyms','core_kallsyms','btf_data','exit','refcnt'],
              'module_layout':['base','size','text_size','ro_size','ro_after_init_size'],
              'mod_kallsyms':['symtab','num_symtab','strtab','typetab'],
              'task_struct':['comm','pid','tgid']}
    structures={}
    for name, fields in required.items():
        v=btf.structure(name)
        structures[name]={'size':v['size'], 'members':{k:v['members'][k] for k in fields}}
    module_size=vendor.by_name['.gnu.linkonce.this_module']['size']
    if structures['module']['size']!=module_size:
        raise SystemExit('vendor/BTF struct module mismatch')
    crc={}; crc_sources={}; compatible=[]; skipped=[]
    for path in sorted(a.modules.rglob('*.ko')):
        e=Elf(path)
        if e.one_info('vermagic')!=vm:
            skipped.append(str(path.relative_to(a.modules))); continue
        compatible.append(path)
        for symbol, value in e.versions().items():
            if symbol in crc and crc[symbol]!=value:
                raise SystemExit(f'CRC conflict {symbol}: {path}')
            crc[symbol]=value
            crc_sources.setdefault(symbol,[]).append(path.name)
    if not compatible:
        raise SystemExit('no matching baseline modules')
    records={}
    for name in ('critical_task','critical_heavy_boost_dir','related_threads','total_num','have_valid_render_pid','rt_info_rwlock'):
        s=vendor.symbol(name)
        records[name]={'size':s['size'],'section':s['section']}
    names=('ct_enable_proc_read','ct_enable_proc_write','init_module','cleanup_module')
    kcfi={name:f'0x{vendor.kcfi(name):08x}' for name in names}
    cfg=a.config.read_bytes()
    if cfg[:2]==b'\x1f\x8b': cfg=gzip.decompress(cfg)
    text=cfg.decode('utf-8').replace('\r\n','\n')
    if f'{a.id} Kernel Configuration' not in text:
        raise SystemExit('wrong config version')
    if a.out.exists() and any(a.out.iterdir()):
        raise SystemExit('profile output must be new and empty')
    a.out.mkdir(parents=True, exist_ok=True)
    (a.out/'kernel.config').write_text(text,encoding='utf-8',newline='\n')
    rows=[f'0x{value:08x}\t{name}\tvmlinux\tEXPORT_SYMBOL\t\n' for name,value in sorted(crc.items())]
    (a.out/'Module.symvers').write_text(''.join(rows),encoding='utf-8',newline='\n')
    (a.out/'crc-evidence.json').write_text(json.dumps(crc_sources,ensure_ascii=False,indent=2)+'\n',encoding='utf-8')
    baseline={'schema':1,'id':a.id,'kernel_version':a.id,'arch':'aarch64','page_size':4096,
              'runtime_releases':[a.release],'vendor':{'name':'oplus_bsp_game_opt',
              'paths':['/vendor/lib/modules/oplus_bsp_game_opt.ko','/vendor_dlkm/lib/modules/oplus_bsp_game_opt.ko'],
              'sha256':[sha256(a.vendor)],'vermagic':vm},'native_node':False,
              'device':{'model':'PJX110','firmware':a.firmware},
              'abi':{'structures':structures,'vendor_symbols':records,'kcfi':kcfi,'module_layout_crc':f'0x{crc["module_layout"]:08x}',
                     'required_sections':[n for n in ('.modinfo','.gnu.linkonce.this_module','__versions','.init.eh_frame','.note.Linux','__version_ext_crcs','__version_ext_names') if n in vendor.by_name],
                     'pid_source_verified':False,'name_readers_rcu_verified':False},
              'source':{'status':a.source_status,'repo':'https://github.com/OnePlusOSS/android_kernel_oneplus_sm8650','commit':a.source_commit,
                        'modules_repo':'https://github.com/OnePlusOSS/android_kernel_modules_and_devicetree_oneplus_sm8650',
                        'modules_commit':'a7ab73a78d71452a1f3bd4c04e2f8aa9ea4ea014' if a.id=='6.1.141' else None},
              'files':{'config':{'path':'kernel.config','sha256':sha256(a.out/'kernel.config')},'symvers':{'path':'Module.symvers','sha256':sha256(a.out/'Module.symvers')}},
              'evidence':{'vendor_sha256':sha256(a.vendor),'btf_sha256':sha256(a.btf),'config_source_sha256':sha256(a.config),
                          'module_count':len(compatible),'crc_count':len(crc),'skipped_module_count':len(skipped)},
              'validation':{'baseline':'captured','build':'not_tested','runtime':'not_tested'}}
    (a.out/'profile.json').write_text(json.dumps(baseline,ensure_ascii=False,indent=2)+'\n',encoding='utf-8',newline='\n')
    print(a.id, 'module_size',module_size,'CRC',len(crc),'vendor',sha256(a.vendor))
    print('find_task_by_vpid',f'0x{crc["find_task_by_vpid"]:08x}' if 'find_task_by_vpid' in crc else 'not in baseline exports')

if __name__=='__main__': main()
