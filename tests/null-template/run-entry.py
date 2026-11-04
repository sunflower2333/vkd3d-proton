#!/usr/bin/env python3
"""CPU Vulkan entry/readback checks with strict, counted validation diagnostics."""
from pathlib import Path
import argparse, collections, hashlib, json, os, re, shlex, shutil, subprocess

p = argparse.ArgumentParser()
p.add_argument('--build', type=Path, required=True)
p.add_argument('--output', type=Path, required=True)
p.add_argument('--baseline-resource', type=Path)
p.add_argument('--mode', choices=['split','merged','nonmutable'], default='split')
p.add_argument('--scope', choices=['entries','readbacks'], default='entries')
a = p.parse_args()
root = Path(__file__).resolve().parents[2]
build, out = a.build.resolve(), a.output.resolve()
assert not out.exists(); out.mkdir(parents=True)
sha = lambda p: hashlib.sha256(p.read_bytes()).hexdigest()
stages = []
def run(label, cmd, env=None):
    q = subprocess.run(cmd, cwd=build, env=env, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, timeout=90)
    (out/(label+'.log')).write_bytes(q.stdout)
    stages.append(dict(label=label, command=cmd, exit=q.returncode, log_sha256=sha(out/(label+'.log'))))
    return q

commands = json.loads((build/'compile_commands.json').read_text())
row = next(c for c in commands if c['file'].endswith('/resource.c'))
original = shlex.split(row['command']); compile_flags = []; i = 0
while i < len(original):
    if original[i] in ['-MQ', '-MF', '-o', '-c']: i += 2
    elif original[i] == '-MD': i += 1
    else: compile_flags.append(original[i]); i += 1
fixture = Path(__file__).with_name('null_entry_test.c')
for name in ['tests/null-template/null_entry_test.c','tests/null-template/run-entry.py',
             'libs/vkd3d-umd/backend_test.c','libs/vkd3d/vkd3d_private.h']:
    dest = out/'source'/name; dest.parent.mkdir(parents=True,exist_ok=True); shutil.copy2(root/name,dest)
q = run('compile', compile_flags + ['-DVKDU_ENABLE_TEST_DEVICE', '-o',str(out/'test.o'),'-c',str(fixture)])
assert q.returncode == 0, q.stdout.decode()[-3000:]
objects = [str(out/'test.o'), 'libs/vkd3d-umd/vkd3d-umd-backend-test.p/backend.c.o',
           'libs/vkd3d-umd/vkd3d-umd-backend-test.p/debug_policy.c.o']
if a.baseline_resource:
    q = run('compile-baseline-resource', compile_flags + ['-o',str(out/'baseline-resource.o'),'-c',str(a.baseline_resource.resolve())])
    assert q.returncode == 0, q.stdout.decode()[-3000:]
    objects.append(str(out/'baseline-resource.o'))
link = shlex.split(subprocess.check_output(['ninja','-t','commands','libs/vkd3d-umd/vkd3d-umd-backend-test'],cwd=build).decode().splitlines()[-1])
q = run('link',[link[0],'-o',str(out/'test'),*objects,*link[link.index('-Wl,--as-needed'):]])
assert q.returncode == 0, q.stdout.decode()[-3000:]

workspace = root.parents[2]
layer = workspace/'artifacts/vulkan-submit-bench-20261013/validation/manifests/VkLayer_khronos_validation.json'
baseline = workspace/'artifacts/vkd3d-umd-null-template-20261019/baseline-null-vvl01.json'
settings = json.loads(baseline.read_text())['environment'].copy()
settings['LP_NUM_THREADS'] = '2'
settings['VKD3D_SHADER_DUMP_PATH'] = str(out/'spirv'); (out/'spirv').mkdir()
if a.mode == 'merged': settings['VKD3D_CONFIG'] += ',mutable_single_set'
if a.mode == 'nonmutable': settings['VKD3D_DISABLE_EXTENSIONS'] += ',VK_EXT_mutable_descriptor_type,VK_VALVE_mutable_descriptor_type'
env = {k:v for k,v in os.environ.items() if not k.startswith(('VK','MESA_','GALLIUM_'))}; env.update(settings)
q = run('run',[str(out/'test'),a.scope],env)
text = q.stdout.decode(errors='replace')
errors = [line for line in text.splitlines() if 'Validation Error:' in line]
warnings = [line for line in text.splitlines() if 'Validation Warning:' in line]
vuids = dict(collections.Counter(m.group(1) for line in errors if (m:=re.search(r'\[ (VUID-[^ ]+) \]',line))))
active = text.count("Attempting to send invalid call to VVL to verify that it's active")
runtime = re.findall(r'RUNTIME mutable=(\d) split=(\d) descriptor_buffer=(\d) embedded=(\d)',text)
expected_mode = {'split':('1','1','0','0'),'merged':('1','0','0','0'),'nonmutable':('0','0','0','0')}[a.mode]
expected_errors = {'VUID-VkWriteDescriptorSet-dstSet-04611':7 if a.scope=='entries' else 3} if a.baseline_resource and a.mode=='split' else {}
data = dict(cpu_vulkan_only=True, baseline_resource_sha256=sha(a.baseline_resource) if a.baseline_resource else None,
    mode=a.mode, scope=a.scope, environment=settings, stages=stages, exit=q.returncode, runtime=runtime,
    error_count=len(errors), error_vuids=vuids, warning_count=len(warnings), warnings=warnings, active_probe_count=active,
    expected_errors=expected_errors, executable_sha256=sha(out/'test'), fixture_sha256=sha(fixture),
    runner_sha256=sha(Path(__file__)), validation_error_free=not errors,
    input_sha256={str(build/q):sha(build/q) for q in objects if not Path(q).is_absolute()},
    linked_archives={q:sha(build/q) for q in link if q.endswith('.a')},
    passed=q.returncode==0 and len(errors)==sum(expected_errors.values()) and
        vuids==expected_errors and active==2 and runtime==[expected_mode] and
        ('Shader cache is explicitly disabled' in text) and 'PASS ' in text)
(out/'receipt.json').write_text(json.dumps(data,indent=2)+'\n')
print(json.dumps({k:v for k,v in data.items() if k not in ['stages','environment','warnings']},indent=2))
print('\n'.join(line for line in text.splitlines() if line.startswith(('PASS','FAIL','RUNTIME'))))
raise SystemExit(0 if data['passed'] else 1)
