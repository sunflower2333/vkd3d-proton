#!/usr/bin/env python3
"""Run the production logical queue submit/split/enqueue-drain functions."""
import argparse
from pathlib import Path
import re
import subprocess
import tempfile


def definition(source, name):
    match = re.search(r'^(?:static )?(?:VkResult|void|HRESULT|int32_t) ' + name + r'\([^;]*?\)\s*\{', source, re.M)
    if not match:
        raise ValueError(name)
    depth = 0
    for i in range(source.index('{', match.start()), len(source)):
        depth += (source[i] == '{') - (source[i] == '}')
        if not depth:
            return source[match.start():i + 1]
    raise ValueError(name)


parser = argparse.ArgumentParser()
parser.add_argument('--negative-control', choices=['drop-route', 'shared-token', 'skip-drain', 'skip-execute-drain'])
args = parser.parse_args()
here = Path(__file__).resolve().parent
root = here.parents[1]
source = (root / 'libs/vkd3d/command.c').read_text()
names = ['d3d12_command_queue_submit_wddm', 'd3d12_command_queue_submit_split_locked',
         'd3d12_command_queue_acquire_serialized', 'd3d12_command_queue_release_serialized',
         'vkd3d_wddm_queue_drain_enqueue']
functions = '\n'.join(definition(source, name) for name in names)
functions += '\n' + definition((here / 'backend.c').read_text(), 'vkdu_queue_execute')
if args.negative_control == 'drop-route':
    functions = functions.replace('copies[i].pNext = &routes[i];', '/* lost metadata */')
elif args.negative_control == 'shared-token':
    functions = functions.replace('routes[i].queue = queue->wddm_queue_token;',
                                  'routes[i].queue = queue->device->wddm_runtime_owner;')
elif args.negative_control == 'skip-drain':
    functions = functions.replace('d3d12_command_queue_acquire_serialized(queue);\n'
                                  '    d3d12_command_queue_release_serialized(queue);',
                                  'if (0) { d3d12_command_queue_acquire_serialized(queue);\n'
                                  '    d3d12_command_queue_release_serialized(queue); }')
elif args.negative_control == 'skip-execute-drain':
    original = 'if (FAILED(hr = vkdu_queue_drain_enqueue(queue))) return hr;'
    if functions.count(original) != 1:
        raise ValueError('production Execute drain anchor changed')
    functions = functions.replace(original, 'if (0 && FAILED(hr = vkdu_queue_drain_enqueue(queue))) return hr;')
fixture = (here / 'queue_worker_test.c').read_text().replace('// PRODUCTION_FUNCTIONS', functions)
with tempfile.TemporaryDirectory(prefix='vkd3d-queue-worker-') as output:
    output = Path(output)
    unit, exe = output / 'fixture.c', output / 'fixture'
    unit.write_text(fixture)
    subprocess.run(['cc', '-std=c11', '-Wall', '-Wextra', '-Werror', '-pthread',
                    '-fsanitize=address,undefined', '-fno-omit-frame-pointer',
                    '-I', str(root / 'include/private'), str(unit), '-o', str(exe)], check=True)
    result = subprocess.run([str(exe)]).returncode
    if args.negative_control:
        if not result:
            raise SystemExit('negative control escaped')
        print('PASS rejected ' + args.negative_control)
    else:
        raise SystemExit(result)
