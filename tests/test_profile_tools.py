#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""Host regressions for the actual profile generator and kernel input parser."""
import importlib.util
import json
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
SPEC = importlib.util.spec_from_file_location('ctn_profile_header', ROOT / 'tools/profile_header.py')
HEADER = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(HEADER)


class ProfileTools(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(prefix='ctnp-profile-test-')
        self.addCleanup(self.temp.cleanup)
        self.directory = Path(self.temp.name)
        self.module = self.directory / 'module'
        (self.module / 'kernel').mkdir(parents=True)
        for name in ('Kbuild', 'ctn_patch.c', 'kernel/ctn_patch_internal.h'):
            shutil.copyfile(ROOT / name, self.module / name)
        self.profile = ROOT / 'profiles/6.1.157/profile.json'
        self.spec = json.loads(self.profile.read_text())

    def generate(self):
        return subprocess.check_output([
            sys.executable, '-I', str(ROOT / 'tools/profile_header.py'),
            '--profile', str(self.profile), '--module', str(self.module),
            '--compiler', 'clang 17.0.2 test fixture'], text=True).strip()

    def test_probe_labels_do_not_collide_with_size_member(self):
        source, labels = HEADER.probe_source(self.spec)
        self.assertEqual(len(labels), len(set(labels)))
        self.assertIn('module_layout.sizeof', labels)
        self.assertIn('module_layout.offsetof.size', labels)
        self.assertIn('sizeof(struct module_layout)', source)
        self.assertIn('offsetof(struct module_layout, size)', source)
        self.assertEqual(HEADER.gates(self.spec)['CTN_EXPECT_MODULE_LAYOUT_SIZE'], 80)
        self.assertEqual(HEADER.gates(self.spec)['CTN_EXPECT_MODULE_LAYOUT_SIZE_OFFSET'], 8)

    def test_generation_stable_and_ignores_generated_sources(self):
        first = self.generate()
        self.assertEqual(first, self.generate())
        (self.module / 'ctn_abi_probe.c').write_text('unrelated generated contents')
        (self.module / 'kernel/ctn_profile.h').write_text('old build identity')
        self.assertEqual(first, self.generate())

    def test_kbuild_and_real_header_change_build_identity(self):
        first = self.generate()
        p = self.module / 'Kbuild'
        p.write_bytes(p.read_bytes() + b'\nccflags-y += -DCTN_TEST_FLAG\n')
        second = self.generate()
        self.assertNotEqual(first, second)
        p = self.module / 'kernel/ctn_patch_internal.h'
        p.write_bytes(p.read_bytes() + b'\n/* source identity regression */\n')
        self.assertNotEqual(second, self.generate())

    def test_unsafe_input_symlink_rejected(self):
        p = self.module / 'kernel/ctn_patch_internal.h'
        data = p.read_bytes()
        p.unlink()
        target = self.directory / 'elsewhere.h'
        target.write_bytes(data)
        try:
            p.symlink_to(target)
        except OSError:
            self.skipTest('host symlink privilege unavailable')
        with self.assertRaises(subprocess.CalledProcessError):
            self.generate()

    def test_actual_kernel_parser_boundaries(self):
        cc = shutil.which('gcc')
        if not cc:
            self.skipTest('run kernel parser regression in Debian with gcc')
        source = self.directory / 'parser.c'
        source.write_text(r'''
#include "ctn_patch_internal.h"
#include <stdio.h>
#include <stdlib.h>
static void expect(int yes) { if (!yes) abort(); }
int main(void) {
    char names[CT_NUM][CT_NAME_LEN], input[256];
    expect(ctn_parse_names("A",1,names)==0);
    expect(!strcmp(names[0],"A") && !strcmp(names[1],"A"));
    expect(ctn_parse_names(" A\tB\n",5,names)==0);
    expect(!strcmp(names[0],"A") && !strcmp(names[1],"B"));
    expect(ctn_parse_names("A B C",5,names)==-EINVAL);
    expect(ctn_parse_names(" \n\t",3,names)==-EINVAL);
    expect(ctn_parse_names("A\0B",3,names)==-EINVAL);
    expect(ctn_parse_names("",0,names)==-E2BIG);
    memset(input,'a',sizeof(input));
    expect(ctn_parse_names(input,99,names)==0);
    expect(ctn_parse_names(input,100,names)==-ENAMETOOLONG);
    expect(ctn_parse_names(input,256,names)==-E2BIG);
    input[99]=' '; input[199]='\n';
    expect(ctn_parse_names(input,200,names)==0);
    expect(strlen(names[0])==99 && strlen(names[1])==99);
    puts("PASS: actual kernel parser boundary tests");
    return 0;
}
'''.replace(r'"', '"'))
        executable = self.directory / 'parser'
        subprocess.run([cc, '-std=c11', '-Wall', '-Wextra', '-Werror',
                        '-I', str(ROOT / 'kernel'), str(source), '-o', str(executable)], check=True)
        subprocess.run([str(executable)], check=True)


if __name__ == '__main__':
    unittest.main(verbosity=2)
