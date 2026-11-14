#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""Extract pinned source; quarantine external vendor symlinks as metadata."""
import argparse
import hashlib
import json
from pathlib import Path, PurePosixPath
import tarfile


def main():
    p=argparse.ArgumentParser()
    p.add_argument('--archive',type=Path,required=True)
    p.add_argument('--out',type=Path,required=True)
    p.add_argument('--repo',required=True)
    p.add_argument('--commit',required=True)
    a=p.parse_args()
    if a.out.exists():raise SystemExit('output directory must be new')
    with tarfile.open(a.archive,'r:gz') as t:
        members=t.getmembers()
        roots=set()
        if len(members)>150000 or sum(m.size for m in members)>8*1024**3:
            raise SystemExit('archive exceeds limits')
        seen=set()
        for m in members:
            name=PurePosixPath(m.name)
            if name.is_absolute() or '..' in name.parts or not name.parts or m.name in seen:
                raise SystemExit('invalid/duplicate archive member')
            seen.add(m.name)
            roots.add(name.parts[0])
            if not (m.isdir() or m.isfile() or m.issym()):
                raise SystemExit('unsupported archive member type')
        if len(roots)!=1:raise SystemExit('expected one source root')
        a.out.mkdir(parents=True)
        links=[]
        # Do not materialize any symlink before extracting regular files.
        for m in members:
            if m.issym():
                links.append({'path':m.name,'target':m.linkname})
            else:
                t.extract(m,a.out,filter='data')
    source=a.out/next(iter(roots))
    identity={'schema':1,'repo':a.repo,'commit':a.commit,
              'archive_sha256':hashlib.sha256(a.archive.read_bytes()).hexdigest(),
              'quarantined_symlinks':links}
    (source/'.ctnp-source.json').write_text(json.dumps(identity,indent=2)+'\n')
    print(source)
    if (source/'Makefile').is_file():
        print('\n'.join((source/'Makefile').read_text().splitlines()[:8]))
    print('Quarantined vendor/source symlinks:',len(links))

if __name__=='__main__':main()
