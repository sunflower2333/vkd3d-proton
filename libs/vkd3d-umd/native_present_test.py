#!/usr/bin/env python3
"""Run the production native Present recording/retention functions locally."""
import argparse
from pathlib import Path
import subprocess
import tempfile

def function(source, signature):
    start = source.index(signature)
    opening = source.index('{', start)
    depth = 1
    end = opening + 1
    while depth:
        depth += (source[end] == '{') - (source[end] == '}')
        end += 1
    return source[start:end]

def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--sanitize', action='store_true')
    parser.add_argument('--negative-control', choices=['drop-owner', 'drop-barrier'])
    args = parser.parse_args()
    here = Path(__file__).resolve().parent
    source = (here.parent / 'vkd3d/command.c').read_text()
    source = function(source, 'static HRESULT d3d12_command_list_wddm_retain(') + '\n' + \
        (here.parent / 'vkd3d/wddm_native_present.inc').read_text() + '\n' + \
        function(source, 'HRESULT vkd3d_wddm_command_prepare_present(')
    if args.negative_control == 'drop-owner':
        source = source.replace('IUnknown_AddRef(pending[i]);', '')
    elif args.negative_control == 'drop-barrier':
        source = source.replace('record(command, &dependency);', '(void)dependency;')
    fixture = (here / 'native_present_test.cpp').read_text().replace('// PRODUCTION_FUNCTIONS', source)
    with tempfile.TemporaryDirectory(prefix='vkd3d-native-present-') as directory:
        output = Path(directory)
        unit = output / 'fixture.cpp'; unit.write_text(fixture)
        command = ['c++', '-std=c++17', '-Wall', '-Wextra', '-Werror', '-Wno-missing-field-initializers',
            '-I', str(here.parents[1] / 'khronos/Vulkan-Headers/include'), str(unit), '-o', str(output / 'fixture')]
        if args.sanitize: command += ['-fsanitize=address,undefined', '-fno-omit-frame-pointer']
        subprocess.run(command, check=True)
        result = subprocess.run([str(output / 'fixture')], capture_output=True, text=True, timeout=10)
        print(result.stdout, end=''); print(result.stderr, end='')
        if args.negative_control:
            expected = 'retained image before tracking' if args.negative_control == 'drop-owner' else 'actual barrier dispatch'
            if result.returncode != 1 or expected not in result.stderr:
                raise SystemExit('negative control did not fail semantically')
            print('PASS rejected ' + args.negative_control)
            return 0
        return result.returncode

if __name__ == '__main__':
    raise SystemExit(main())
