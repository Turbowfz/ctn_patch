#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""Fetch checksum-pinned OnePlusOSS archives into separate new directories."""
import argparse
import json
from pathlib import Path
import subprocess
import sys
import urllib.request

sys.path.insert(0,str(Path(__file__).resolve().parent))
from abi import sha256

ROOT=Path(__file__).resolve().parent.parent


def download(repo,commit,digest,directory):
    if not repo.startswith('https://github.com/OnePlusOSS/') or len(commit)!=40:
        raise ValueError('not a pinned OnePlusOSS source')
    directory.mkdir(parents=True,exist_ok=False)
    archive=directory/'source.tar.gz'
    url='https://codeload.github.com/'+repo.removeprefix('https://github.com/')+'/tar.gz/'+commit
    with urllib.request.urlopen(urllib.request.Request(url,headers={'User-Agent':'ctnp-source-build'}),timeout=120) as stream,archive.open('xb') as out:
        size=0
        while data:=stream.read(1024*1024):
            size+=len(data)
            if size>4*1024**3:raise ValueError('source archive exceeds limits')
            out.write(data)
    if sha256(archive)!=digest:raise ValueError('source archive checksum mismatch')
    print('Verified official archive:',url,flush=True)
    result=subprocess.check_output([sys.executable,'-I',str(ROOT/'tools/unpack_source.py'),
        '--archive',str(archive),'--out',str(directory/'extracted'),
        '--repo',repo,'--commit',commit],text=True)
    print(result,end='')
    source=Path(result.splitlines()[0])
    return source


def main():
    p=argparse.ArgumentParser(description=__doc__)
    p.add_argument('--profile',required=True,choices=('6.1.141','6.1.157'))
    p.add_argument('--out',required=True,type=Path)
    a=p.parse_args()
    if a.out.exists():raise ValueError('download output must be a new directory')
    spec=json.loads((ROOT/'profiles'/a.profile/'profile.json').read_text())['source']
    source=download(spec['repo'],spec['commit'],spec['archive_sha256'],a.out/'kernel')
    modules=download(spec['modules_repo'],spec['modules_commit'],spec['modules_archive_sha256'],a.out/'modules')
    subprocess.run([sys.executable,'-I',str(ROOT/'tools/prepare_source.py'),
        '--source',str(source),'--modules',str(modules)],check=True)
    (a.out/'paths.json').write_text(json.dumps({'source':str(source),'modules':str(modules)},indent=2)+'\n')

if __name__=='__main__':
    try:main()
    except (ValueError,OSError,subprocess.SubprocessError) as exc:
        print('SOURCE FETCH FAILED:',exc,file=sys.stderr);sys.exit(1)
