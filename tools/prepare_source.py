#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""Re-link quarantined vendor paths only inside a dedicated build source tree."""
import argparse
import json
from pathlib import Path, PurePosixPath


def main():
    p=argparse.ArgumentParser()
    p.add_argument('--source',type=Path,required=True)
    p.add_argument('--modules',type=Path,required=True)
    a=p.parse_args()
    source=a.source.resolve(); modules=a.modules.resolve()
    record=json.loads((source/'.ctnp-source.json').read_text())
    links=record.get('quarantined_symlinks',[])
    if not (modules/'vendor').is_dir():raise ValueError('modules source missing vendor directory')
    changes=[]
    for link in links:
        relative=PurePosixPath(link['path'])
        # Archive member names include one source-root component.
        relative=Path(*relative.parts[1:])
        path=source/relative
        if path.exists() or path.is_symlink():raise ValueError('refusing to replace existing path: '+str(relative))
        raw=link['target']
        pos=raw.rfind('vendor/')
        target=(modules/Path(raw[pos:])).resolve() if pos>=0 else (path.parent/raw).resolve()
        base=modules if pos>=0 else source
        if not target.is_relative_to(base) or not target.exists():
            changes.append({'path':str(relative),'target':raw,'status':'missing'})
            continue
        path.parent.mkdir(parents=True,exist_ok=True)
        path.symlink_to(target,target_is_directory=target.is_dir())
        changes.append({'path':str(relative),'target':str(target),'status':'linked'})
    # These source references were missing in both captured OEM source trees.
    # Do not silently remove Kconfig statements or fabricate other features.
    stubs=('mm/mm_osvelte/Kconfig','drivers/base/kernelFwUpdate/Kconfig',
           'drivers/base/touchpanel_notify/Kconfig','drivers/misc/vibrator/aw8697_haptic/Kconfig',
           'drivers/misc/vibrator/si_haptic/Kconfig','drivers/misc/vibrator/oplus_haptic/Kconfig',
           'kernel/oplus_cpu/uad/Kconfig','drivers/oplus_inject/Kconfig')
    for name in stubs:
        path=source/name
        if path.is_file():continue
        # Do not write through a vendor-directory link into the pinned modules tree.
        if any(parent.is_symlink() for parent in path.parents if parent!=source.parent):
            continue
        path.parent.mkdir(parents=True,exist_ok=True)
        path.write_text('# Missing OEM auxiliary Kconfig; no symbols declared.\n')
        changes.append({'path':name,'status':'empty_auxiliary_kconfig'})
    (source/'.ctnp-relink.json').write_text(json.dumps(changes,indent=2)+'\n')
    print('linked',sum(x['status']=='linked' for x in changes),
          'missing',sum(x['status']=='missing' for x in changes))

if __name__=='__main__':main()
