#!/usr/bin/env python3
"""Run the production embedded pageable object enumeration and token collector."""
import argparse
import os
from pathlib import Path
import shutil
import subprocess
import tempfile


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--sanitize', action='store_true')
    parser.add_argument('--negative-control', choices=['drop-descriptor-pool', 'drop-query-pool', 'keep-duplicate'])
    args = parser.parse_args()
    here = Path(__file__).resolve().parent
    source = (here.parent / 'vkd3d/wddm_pageable.inc').read_text()
    if args.negative_control == 'drop-descriptor-pool':
        source = source.replace('(uint64_t)heap->vk_descriptor_pool', '0')
    elif args.negative_control == 'drop-query-pool':
        source = source.replace('(uint64_t)heap->vk_query_pool', '0')
    elif args.negative_control == 'keep-duplicate':
        source = source.replace('if (j < length)', 'if (false && j < length)')
    fixture = (here / 'pageable_backing_test.cpp').read_text().replace('// PRODUCTION_FUNCTIONS', source)
    with tempfile.TemporaryDirectory(prefix='vkd3d-pageable-') as output:
        directory = Path(output)
        unit = directory / 'fixture.cpp'; unit.write_text(fixture)
        compiler = shutil.which('clang-cl') if os.name == 'nt' else None
        executable = directory / ('fixture.exe' if compiler else 'fixture')
        includes = str(here.parents[1] / 'include/private')
        if compiler:
            command = [compiler, '/nologo', '/EHsc', '/W4', '/WX', '/std:c++20',
                       '/I' + includes, str(unit), '/Fe' + str(executable)]
        else:
            command = ['c++', '-std=c++20', '-Wall', '-Wextra', '-Werror',
                       '-Wno-missing-field-initializers',
                       '-I', includes, str(unit), '-o', str(executable)]
            if args.sanitize: command += ['-fsanitize=address,undefined', '-fno-omit-frame-pointer']
        subprocess.run(command, cwd=directory, check=True)
        result = subprocess.run([str(executable)], cwd=directory, timeout=15, capture_output=True, text=True)
        print(result.stdout, end='')
        if args.negative_control:
            expected = {'drop-descriptor-pool': 'FAIL actual descriptor backing omitted pool or auxiliary memory',
                        'drop-query-pool': 'FAIL actual query backing omitted pool or result memory',
                        'keep-duplicate': 'FAIL pageable collector retained duplicate backing'}[args.negative_control]
            if result.returncode != 1 or expected not in result.stderr:
                raise SystemExit('negative control did not fail semantically: ' + result.stderr)
            print('PASS rejected ' + args.negative_control + ': ' + expected)
            return 0
        print(result.stderr, end=''); return result.returncode


if __name__ == '__main__':
    raise SystemExit(main())
