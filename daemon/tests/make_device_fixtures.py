#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""Create only synthetic fixtures for the isolated Android daemon test."""
import argparse
import json
from pathlib import Path
import sqlite3


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--out', type=Path, required=True)
    args = parser.parse_args()
    args.out.mkdir(parents=True, exist_ok=False)
    configs = {
        'valid.db': {'ctn': 'CTNMain CTNRender', 'ctb': 1},
        'changed.db': {'ctn': 'CTNNew CTNNewGfx', 'ctb': 0},
        'empty.db': {'ctn': ''},
        'space.db': {'ctn': ' \t\r\n '},
        'unsupported.db': 'not-valid-json!',
        'missing.db': None,
    }
    for name, config in configs.items():
        path = args.out / name
        with sqlite3.connect(path) as db:
            db.execute('CREATE TABLE PackageConfigBean(package_name TEXT, game_config TEXT)')
            if config is not None:
                value = config if isinstance(config, str) else json.dumps(config, separators=(',', ':'))
                db.execute('INSERT INTO PackageConfigBean VALUES (?, ?)', ('com.ctnp.synthetic', value))
            db.commit()
    (args.out / 'broken.db').write_bytes(b'CTNP intentionally invalid synthetic SQLite database\n')
    print('Generated synthetic fixtures:', args.out)


if __name__ == '__main__':
    main()
