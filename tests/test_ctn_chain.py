#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""CTN 链路核心主机回归 runner：GCC/Clang × 无 sanitizer/ASan/UBSan。

sanitizer 不可用视为失败（CTN_CHAIN_SANITIZERS=0 才允许显式跳过）。
"""
import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
NDK = Path(os.environ.get('NDK', '/opt/ndkroot/android-ndk-r26b'))
SOURCE = ROOT / 'tests/test_ctn_chain.c'
FLAGS = ['-std=c11', '-O1', '-g', '-Wall', '-Wextra', '-Werror', '-Wpedantic',
         '-Wshadow', '-Wformat=2', '-Wstrict-prototypes', '-Wmissing-prototypes',
         '-Wcast-qual', '-Wwrite-strings', '-Wundef']


class ChainCoreTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(prefix='ctn-chain-test-')
        self.addCleanup(self.temp.cleanup)
        self.directory = Path(self.temp.name)

    def compilers(self):
        if os.environ.get('CC'):
            named = shutil.which(os.environ['CC'])
            if not named:
                self.fail('requested compiler unavailable: ' + os.environ['CC'])
            return [named]
        found = []
        for name in ('gcc', 'clang'):
            path = shutil.which(name)
            if path:
                found.append(path)
        if not found:
            self.fail('no native C compiler; run in Debian')
        return found

    def run_case(self, compiler, sanitizer):
        out = self.directory / (Path(compiler).name + '-' + sanitizer)
        out.mkdir()
        command = [compiler, *FLAGS]
        link = []
        if sanitizer != 'none':
            command += ['-fsanitize=' + sanitizer, '-fno-omit-frame-pointer']
            if compiler.startswith(str(NDK)):
                command += ['-fno-sanitize-link-runtime']
                link = ['-lasan'] if sanitizer == 'address' else ['-lubsan']
        command += ['-I', str(ROOT / 'kernel'), str(SOURCE), *link,
                    '-o', str(out / 'chain_test')]
        subprocess.run(command, check=True)
        environment = dict(os.environ)
        if sanitizer == 'address':
            environment['ASAN_OPTIONS'] = 'detect_leaks=1:abort_on_error=1'
        else:
            environment['UBSAN_OPTIONS'] = 'halt_on_error=1:print_stacktrace=1'
        subprocess.run([str(out / 'chain_test')], check=True, env=environment)

    def test_chain_core_all_compilers_and_sanitizers(self):
        sanitizers = ('none', 'address', 'undefined')
        if os.environ.get('CTN_CHAIN_SANITIZERS') == '0':
            sanitizers = ('none',)
        for compiler in self.compilers():
            for sanitizer in sanitizers:
                with self.subTest(compiler=Path(compiler).name, sanitizer=sanitizer):
                    self.run_case(compiler, sanitizer)


if __name__ == '__main__':
    unittest.main(verbosity=2)
