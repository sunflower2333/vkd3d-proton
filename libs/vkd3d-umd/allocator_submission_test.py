#!/usr/bin/env python3
"""Exercise production Execute/submit/allocator retirement with controlled Vulkan peers."""
import argparse
from pathlib import Path
import re
import subprocess
import tempfile


def definition(source, name):
    match = re.search(r'^(?:static )?(?:ULONG(?: STDMETHODCALLTYPE)? |void \*|void |VkResult |HRESULT )' + name +
                      r'\([^;]*?\)\s*\{', source, re.M)
    if not match:
        raise ValueError(name)
    depth = 0
    for i in range(source.index('{', match.start()), len(source)):
        depth += (source[i] == '{') - (source[i] == '}')
        if not depth:
            return source[match.start():i + 1]
    raise ValueError(name)


parser = argparse.ArgumentParser()
parser.add_argument('--negative-control', choices=['skip-cleanup', 'forget-attempt', 'drop-retirement'])
args = parser.parse_args()
here = Path(__file__).resolve().parent
root = here.parents[1]
source = (root / 'libs/vkd3d/command.c').read_text()
names = ['d3d12_command_allocator_free_command_pool', 'd3d12_command_allocator_dec_ref',
         'd3d12_command_allocator_Release', 'vkd3d_waiting_fence_release',
         'vkd3d_waiting_fence_set_callback', 'vkd3d_waiting_fence_release_submission',
         'vkd3d_enqueue_timeline_semaphore', 'vkd3d_waiting_fence_complete_submissions',
         'vkd3d_wait_for_gpu_timeline_semaphore', 'd3d12_command_queue_submit_wddm_tracked',
         'd3d12_command_queue_submit_split_locked_tracked', 'd3d12_command_queue_execute']
functions = '\n\n'.join(definition(source, name) for name in names)
if args.negative_control == 'skip-cleanup':
    old, new = 'if (vr != VK_SUCCESS && !submission_attempted)', 'if (0 && vr != VK_SUCCESS && !submission_attempted)'
elif args.negative_control == 'forget-attempt':
    old, new = '*attempted = true;', '*attempted = false;'
elif args.negative_control == 'drop-retirement':
    old, new = 'i < info->num_command_allocators', 'i < 0 * info->num_command_allocators'
else:
    old = new = None
if old:
    expected = 2 if args.negative_control == 'forget-attempt' else 1
    if functions.count(old) != expected:
        raise ValueError('production negative-control anchor changed: ' + old)
    functions = functions.replace(old, new)
fixture = (here / 'allocator_submission_test.c').read_text().replace('// PRODUCTION_FUNCTIONS', functions)
with tempfile.TemporaryDirectory(prefix='vkd3d-allocator-submission-') as output:
    output = Path(output)
    unit, exe = output / 'fixture.c', output / 'fixture'
    unit.write_text(fixture)
    subprocess.run(['cc', '-std=c11', '-Wall', '-Wextra', '-Werror', '-Wno-unused-parameter', '-pthread',
                    '-fsanitize=address,undefined', '-fno-omit-frame-pointer',
                    '-I', str(root / 'include/private'),
                    '-I', str(root / 'khronos/Vulkan-Headers/include'),
                    str(unit), '-o', str(exe)], check=True)
    result = subprocess.run([str(exe)], text=True, capture_output=True)
    print(result.stdout, end='')
    print(result.stderr, end='')
    if args.negative_control:
        if not result.returncode or 'FAIL ' not in result.stderr:
            raise SystemExit('negative control did not fail a behavioral assertion')
        print('PASS rejected ' + args.negative_control)
    else:
        raise SystemExit(result.returncode)
