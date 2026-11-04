#!/usr/bin/env python3
"""Execute the production native-image Vulkan calls with controlled callbacks."""
import argparse
from pathlib import Path
import subprocess
import tempfile

def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--sanitize', action='store_true')
    parser.add_argument('--negative-control', choices=['skip-row-pitch', 'drop-memory-release'])
    args = parser.parse_args()
    here = Path(__file__).resolve().parent
    root = here.parents[1]
    source = (here.parent / 'vkd3d/wddm_native_image.inc').read_text()
    if args.negative_control == 'skip-row-pitch':
        source = source.replace('layout.rowPitch != desc->stride', 'false')
    elif args.negative_control == 'drop-memory-release':
        source = source.replace('if (image->memory) device->free_memory(device->device, image->memory, NULL);', '')
    fixture = (here / 'native_image_test.cpp').read_text().replace('// PRODUCTION_FUNCTIONS', source)
    with tempfile.TemporaryDirectory(prefix='vkd3d-native-image-') as directory:
        output = Path(directory)
        unit = output / 'fixture.cpp'; unit.write_text(fixture)
        command = ['c++', '-std=c++17', '-Wall', '-Wextra', '-Werror', '-Wno-missing-field-initializers',
            '-include', 'initializer_list', '-I', str(root / 'include/private'),
            '-I', str(root / 'khronos/Vulkan-Headers/include'), str(unit), '-o', str(output / 'fixture')]
        if args.sanitize: command += ['-fsanitize=address,undefined', '-fno-omit-frame-pointer']
        subprocess.run(command, check=True)
        result = subprocess.run([str(output / 'fixture')], capture_output=True, text=True, timeout=10)
        print(result.stdout, end=''); print(result.stderr, end='')
        if args.negative_control:
            expected = 'rejected layout mismatch' if args.negative_control == 'skip-row-pitch' else 'successful import releases exactly once'
            if result.returncode != 1 or expected not in result.stderr:
                raise SystemExit('negative control did not fail semantically')
            print('PASS rejected ' + args.negative_control)
            return 0
        return result.returncode

if __name__ == '__main__':
    raise SystemExit(main())
