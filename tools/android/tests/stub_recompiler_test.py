#!/usr/bin/env python3
"""Checks that tools/android/stub-recompiler.py still writes what the build expects.

The stub puts the context header together from the XboxRecomp2 sources in
tools/XboxRecomp2, so a new revision of that submodule can break it. This runs it the
way ShadowRecompLib/cmake/recompile.cmake runs the recompiler.
"""
import re
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path

REPOSITORY = Path(__file__).resolve().parents[3]
SCRIPT = REPOSITORY / 'tools' / 'android' / 'stub-recompiler.py'

failures = 0
checks = 0


def check(condition, what):
    global failures, checks
    checks += 1
    if not condition:
        failures += 1
        print(f'FAIL {what}')


work = Path(tempfile.mkdtemp(prefix='stub-recompiler-test-'))
try:
    output = work / 'recompile' / 'x86'
    config = (REPOSITORY / 'ShadowRecompLib' / 'config' / 'recompConfig.toml').read_text(encoding='utf-8')
    config = re.sub(r'file_path = [^\n]*', "file_path = '../private/default.xbe'", config)
    config = re.sub(r'out_directory_path = [^\n]*', f"out_directory_path = '{output}'", config)
    (work / 'config.toml').write_text(config, encoding='utf-8')

    result = subprocess.run([sys.executable, str(SCRIPT), 'recompile', str(work / 'config.toml')],
                            stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
    check(result.returncode == 0, f'the stub runs: {result.stdout.strip()}')

    names = sorted(path.name for path in output.iterdir()) if output.is_dir() else []
    check(names == ['x86_config.h', 'x86_context.h', 'x86_func_mapping.cpp', 'x86_recomp.0.cpp', 'x86_recomp_shared.h'],
          f'it writes the files the build collects: {names}')

    if failures == 0:
        config_header = (output / 'x86_config.h').read_text()
        context = (output / 'x86_context.h').read_text()
        shared = (output / 'x86_recomp_shared.h').read_text()
        unit = (output / 'x86_recomp.0.cpp').read_text()
        mapping = (output / 'x86_func_mapping.cpp').read_text()

        check('#define X86_RECOMP_STUB 1' in config_header, 'the build is marked as a stub')
        check('#define X86_CODE_D3D8V' in config_header, 'the constants the game reads are defined')
        check('//@context:' not in context, 'every placeholder of the context header is filled in')
        check('X86_SINGLE_THREAD_LOCKS_FROM 0x16A1000u' in context, 'the options of the recompiler config are applied')
        check('struct X86Context' in context or 'X86Context' in context, 'the context header is the recompiler\'s')

        # The functions the game overrides or calls by name.
        game = (REPOSITORY / 'ShadowRecomp' / 'app.cpp').read_text(encoding='utf-8', errors='replace')
        used = set(re.findall(r'\b(sub_[0-9A-Fa-f]+)\b', game))
        check(len(used) > 0 and all(f'X86_EXTERN_FUNC({name});' in shared for name in used),
              'functions named by the game are declared')
        check(all(f'X86_WEAK_FUNC({name}) {{' in unit for name in used), 'and have a body')
        check(mapping.count('\t{ 0x') == shared.count('X86_EXTERN_FUNC('), 'every declared function is in the mapping table')
        check('X86_EXTERN_FUNC(__imp__DbgPrint);' in shared and 'X86_WEAK_FUNC(__imp__DbgPrint)' not in unit,
              'kernel imports are declared and left to the game to define')

    bad = subprocess.run([sys.executable, str(SCRIPT), 'recompile', str(work / 'missing.toml')],
                         stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
    check(bad.returncode != 0 and 'does not exist' in bad.stdout, 'a missing config is an error')
finally:
    shutil.rmtree(work, ignore_errors=True)

print(f'{checks} checks, {failures} failures')
sys.exit(1 if failures else 0)
