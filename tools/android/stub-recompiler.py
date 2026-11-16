#!/usr/bin/env python3
"""Stands in for XboxRecomp2 so that the build can be tested without the game.

    usage: stub-recompiler.py recompile <config.toml>

XboxRecomp2 turns default.xbe into C++ at configure time, which makes the game the one
thing a build cannot do without. This script takes the place of the recompiler and writes
the same set of files, with nothing in them: every guest function the game's sources
refer to gets an empty body. The result compiles, links and packages exactly like a real
build, so the toolchain, the Gradle project and the CI workflow can be tested on their
own. It cannot run the game: the app says so and exits (see X86_RECOMP_STUB).

    cmake --preset android-release \\
        -DSHADOW_RECOMP_XBOXRECOMP2_EXECUTABLE=$PWD/tools/android/stub-recompiler.py

ShadowRecompLib/private/default.xbe still has to exist, but any file will do.

The context header is assembled from the XboxRecomp2 sources in tools/XboxRecomp2 the
way the recompiler does it, so it is the header a real build of the pinned version gets.
"""
import re
import sys
from pathlib import Path

REPOSITORY = Path(__file__).resolve().parents[2]
GAME_SOURCES = REPOSITORY / 'ShadowRecomp'
RECOMPILER_SOURCES = REPOSITORY / 'tools' / 'XboxRecomp2' / 'src'

# What XboxRecomp2 reports for the retail executable. Only X86_CODE_D3D8V is read by the
# game's sources (the Direct3D 8 build the game links against).
CONFIG_HEADER = '''#pragma once
#ifndef X86_CONFIG_H_INCLUDED
#define X86_CONFIG_H_INCLUDED

// Written by tools/android/stub-recompiler.py: this build has no game code in it.
#define X86_RECOMP_STUB 1

#define X86_IMAGE_BASE 0x10000ull
#define X86_IMAGE_SIZE 0x8D3000ull
#define X86_CODE_D3D8V 5849ull
#define X86_CODE_BASE 0x11000ull
#define X86_CODE_SIZE 0x400000ull

#ifdef X86_INCLUDE_DETAIL
#include "x86_detail.h"
#endif

#endif
'''


def fail(message):
    sys.exit(f'stub-recompiler: {message}')


def read_source(relative):
    path = RECOMPILER_SOURCES / relative
    if not path.is_file():
        fail(f'{path} is missing. Run: git submodule update --init tools/XboxRecomp2')
    return path.read_text(encoding='utf-8')


def config_value(config, name, default=None):
    """A scalar from the recompiler's TOML config; enough for the keys used here."""
    match = re.search(
        rf'^[ \t]*{re.escape(name)}[ \t]*=[ \t]*(?:"([^"\n]*)"|\'([^\'\n]*)\'|([^#\n]*?))[ \t]*(?:#.*)?$',
        config, re.M)
    if not match:
        return default
    return next(group for group in match.groups() if group is not None)


def context_header(config):
    """x86_context.h, put together like src/xbox-recomp2/src/context_header/mod.rs does."""
    core = read_source('xbox-recomp2/src/context_header/x86_context_core.h')
    simd = read_source('xbox-recomp2/src/context_header/x86_portable_simd.h')
    overlay = read_source('xbox-recomp2-xbox/src/x86_context_xbox.h')

    try:
        _, after_state = overlay.split('\n//@state\n', 1)
        state, declarations = after_state.split('\n//@declarations\n', 1)
    except ValueError:
        fail('x86_context_xbox.h has no //@state and //@declarations markers: '
             'tools/XboxRecomp2 changed and this script has to follow it.')

    snippets = {
        '//@context:declarations': declarations.removesuffix('\n'),
        '//@context:extra-state': state,
        '//@context:portable-simd': simd.removesuffix('\n'),
    }

    body = []
    used = set()
    for line in core.splitlines(keepends=True):
        key = line.removesuffix('\n')
        if key in snippets:
            used.add(key)
            if snippets[key]:
                body.append(snippets[key] + '\n')
        else:
            body.append(line)

    if used != set(snippets):
        fail('x86_context_core.h lacks ' + ', '.join(sorted(set(snippets) - used)) +
             ': tools/XboxRecomp2 changed and this script has to follow it.')

    def enabled(name):
        return 1 if config_value(config, name, 'false') == 'true' else 0

    locks_from = int(config_value(config, 'single_thread_locks_from', '0'), 0)
    prefix = (
        f"#define X86_RECOMP_DEBUG {enabled('debug')}\n"
        f"#define X86_NONVOLATILE_MEMORY {enabled('nonvolatile_memory')}\n"
        f"#define X86_SINGLE_THREAD_LOCKS_FROM 0x{locks_from:X}u\n"
        f"#define X86_CHECK_SINGLE_THREAD_LOCKS {enabled('check_single_thread_locks')}\n"
    )
    return '#pragma once\n#include "x86_config.h"\n\n' + prefix + ''.join(body)


def referenced_symbols():
    """Guest functions (sub_<address>) and kernel imports the game's sources name."""
    functions, imports = set(), set()
    for path in sorted(GAME_SOURCES.rglob('*')):
        if path.suffix not in ('.cpp', '.h', '.inl') or not path.is_file():
            continue
        text = path.read_text(encoding='utf-8', errors='replace')
        functions.update(re.findall(r'\b(?:__imp__)?(sub_[0-9A-Fa-f]+)\b', text))
        imports.update(name for name in re.findall(r'\b(__imp__[A-Za-z_]\w*)\b', text)
                       if not name.startswith('__imp__sub_'))
    return sorted(functions, key=lambda name: int(name[4:], 16)), sorted(imports)


def main():
    if len(sys.argv) != 3 or sys.argv[1] != 'recompile':
        fail('usage: stub-recompiler.py recompile <config.toml>')

    config_path = Path(sys.argv[2])
    if not config_path.is_file():
        fail(f'{config_path} does not exist')
    config = config_path.read_text(encoding='utf-8')

    output = config_value(config, 'out_directory_path')
    if not output:
        fail(f'{config_path} has no out_directory_path')
    output = Path(output)
    if not output.is_absolute():
        output = config_path.parent / output
    output.mkdir(parents=True, exist_ok=True)

    functions, imports = referenced_symbols()
    if not functions:
        fail(f'no guest functions found in {GAME_SOURCES}')

    shared = ['#pragma once\n\n#include "x86_config.h"\n#include "x86_context.h"\n\n']
    mapping = ['#include "x86_recomp_shared.h"\n\nX86FuncMapping X86FuncMappings[] = {\n']
    unit = ['#include "x86_recomp_shared.h"\n\n']

    def declare(name, address):
        shared.append(f'#define X86_OVERRIDE_ALLOWED_{name} 1\n'
                      f'#define X86_OVERRIDE_ADDRESS_{name} "0x{address:X}"\n'
                      f'X86_EXTERN_FUNC({name});\n')
        mapping.append(f'\t{{ 0x{address:X}, {name} }},\n')

    for name in functions:
        declare(name, int(name[4:], 16))
        unit.append(f'X86_WEAK_FUNC({name}) {{\n}}\n\n')

    # Kernel imports are defined by the game itself (kernel/imports.cpp); they only need
    # an address, which the real recompiler takes from the executable's import table.
    for index, name in enumerate(imports):
        declare(name, 0x800000 + index * 16)

    mapping.append('\t{ 0, nullptr }\n};\n')

    (output / 'x86_config.h').write_text(CONFIG_HEADER, encoding='utf-8')
    (output / 'x86_context.h').write_text(context_header(config), encoding='utf-8')
    (output / 'x86_recomp_shared.h').write_text(''.join(shared), encoding='utf-8')
    (output / 'x86_func_mapping.cpp').write_text(''.join(mapping), encoding='utf-8')
    (output / 'x86_recomp.0.cpp').write_text(''.join(unit), encoding='utf-8')

    print(f'stub-recompiler: wrote {len(functions)} empty functions and {len(imports)} imports '
          f'to {output}. This build cannot run the game.')


if __name__ == '__main__':
    main()
