#!/usr/bin/env python3
"""Actual-body tests; a nonzero exit only counts with the exact expected diagnosis."""
from pathlib import Path
import argparse, hashlib, json, re, shlex, shutil, subprocess

p = argparse.ArgumentParser()
p.add_argument('--build', type=Path, required=True)
p.add_argument('--output', type=Path, required=True)
p.add_argument('--baseline', action='store_true')
p.add_argument('--mutation', choices=['ignore-list','always-initial','buffer-global','write-global',
    'initial-image','skip-companion','keep-range','keep-raw-va','drop-copy-mask'])
a = p.parse_args()
root = Path(__file__).resolve().parents[2]
build, out = a.build.resolve(), a.output.resolve()
assert not out.exists()
out.mkdir(parents=True)
sha = lambda p: hashlib.sha256(p.read_bytes()).hexdigest()

def function(text, name):
    m = re.search(r'(?m)^[a-zA-Z_][^\n;{}]*\b' + re.escape(name) + r'\s*\(', text)
    if not m:
        raise ValueError('missing function ' + name)
    # Strip comments and quoted strings preserving offsets, so braces in them do not count.
    syntax = re.sub(r'/\*.*?\*/|//[^\n]*|"(?:\\.|[^"\\])*"|\'(?:\\.|[^\'\\])*\'',
                    lambda x: ' ' * len(x.group()), text, flags=re.S)
    start = syntax.index('{', m.start())
    level = 1
    for end in range(start + 1, len(text)):
        level += (syntax[end] == '{') - (syntax[end] == '}')
        if not level:
            return text[m.start():end+1] + '\n'
    raise ValueError('unterminated function ' + name)

groups = {
    'libs/vkd3d/state.c': ['vkd3d_bindless_build_mutable_type_list',
        'vkd3d_bindless_state_init_null_descriptor_payloads'],
    'libs/vkd3d/resource.c': ['d3d12_descriptor_heap_add_null_descriptor_template_buffers',
        'd3d12_descriptor_heap_add_null_descriptor_template_descriptors',
        'd3d12_descriptor_heap_write_null_descriptor_template',
        'd3d12_descriptor_heap_write_null_descriptor_template_embedded',
        'd3d12_descriptor_heap_create_descriptor_buffer'],
}
sources = {n: (root/n).read_text() for n in groups}
if re.search(r'\nVkDescriptorType vkd3d_bindless_resolve_null_descriptor_type\(', sources['libs/vkd3d/state.c']):
    groups['libs/vkd3d/state.c'].insert(1, 'vkd3d_bindless_resolve_null_descriptor_type')
generated = ''.join(function(sources[n], f) for n, functions in groups.items() for f in functions)
mutations = {
    'ignore-list': ('if (types[i] == requested_type)', 'if (true)', 'write-image', 'raw write selected illegal/wrong active type'),
    'always-initial': ('return requested_type;', 'return set_info->vk_init_null_descriptor_type;', 'write-uav', 'typed write selected illegal/wrong active type'),
    'buffer-global': ('vkd3d_bindless_resolve_null_descriptor_type(bindless_state, set_info, vk_mutable_descriptor_type)',
        'vk_mutable_descriptor_type', 'buffer-image', 'raw payload mismatch'),
    'write-global': ('vkd3d_bindless_resolve_null_descriptor_type(\n                        bindless_state, set_info, vk_mutable_descriptor_type)',
        'vk_mutable_descriptor_type', 'write-image', 'raw write selected illegal/wrong active type'),
    'initial-image': ('&device->bindless_state, set_info->vk_init_null_descriptor_type',
        '&device->bindless_state, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE', 'buffer-initial', 'initial raw payload mismatch'),
    'skip-companion': ('num_writes = null_descriptor_template->num_writes;',
        'num_writes = null_descriptor_template->num_writes - 1;', 'write-image', 'raw write selected illegal/wrong active type'),
    'keep-range': ('memset(&buffer_ranges[desc.offset], 0, sizeof(*buffer_ranges));',
        '(void)buffer_ranges;', 'write-image', 'null did not clear buffer range'),
    'keep-raw-va': ('va[offset] = 0;', '(void)va;', 'write-image', 'null did not clear raw VA'),
    'drop-copy-mask': ('desc.types->set_info_mask = null_descriptor_template->set_info_mask;',
        'desc.types->set_info_mask = 2;', 'write-image', 'null lost companion set copy mask'),
}
if a.mutation:
    assert not a.baseline
    old, new, failing_case, diagnosis = mutations[a.mutation]
    assert generated.count(old) == 1
    generated = generated.replace(old,new)
(out/'actual-functions.inc').write_text(generated)
for name in [*groups, 'libs/vkd3d/vkd3d_private.h', 'tests/null-template/null_template_test.c', 'tests/null-template/run.py']:
    dest = out/'source'/name; dest.parent.mkdir(parents=True,exist_ok=True); shutil.copy2(root/name,dest)

commands = json.loads((build/'compile_commands.json').read_text())
row = next(c for c in commands if c['file'].endswith('/resource.c'))
original = shlex.split(row['command'])
compile_cmd = []
i = 0
while i < len(original):
    if original[i] in ['-MQ', '-MF', '-o', '-c']:
        i += 2
    elif original[i] == '-MD':
        i += 1
    else:
        compile_cmd.append(original[i]); i += 1
compile_cmd += ['-I'+str(out), '-o', str(out/'test.o'), '-c', str(Path(__file__).with_name('null_template_test.c'))]
link_line = subprocess.check_output(['ninja','-t','commands','libs/vkd3d-umd/vkd3d-umd-backend-test'],cwd=build).decode().splitlines()[-1]
link = shlex.split(link_line)
policy = 'libs/vkd3d-umd/vkd3d-umd-backend-test.p/debug_policy.c.o'
link_cmd = [link[0], '-o', str(out/'test'), str(out/'test.o'), policy] + link[link.index('-Wl,--as-needed'):]
stages = []
def run(label, command):
    q = subprocess.run(command, cwd=build, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, timeout=60)
    (out/(label+'.log')).write_bytes(q.stdout)
    stages.append(dict(label=label, command=command, exit=q.returncode, log_sha256=sha(out/(label+'.log'))))
    return q
for label, command in [('compile',compile_cmd), ('link',link_cmd)]:
    q = run(label, command)
    if q.returncode:
        print(q.stdout.decode()[-3000:]); raise SystemExit(q.returncode)

cases = {
    'write-image': 'raw write selected illegal/wrong active type',
    'write-cbv': 'typed write selected illegal/wrong active type',
    'write-uav': 'raw write selected illegal/wrong active type',
    'write-texel': 'raw write selected illegal/wrong active type',
    'buffer-image': 'raw payload mismatch',
    'buffer-cbv': 'typed payload mismatch',
    'buffer-uav': 'raw payload mismatch',
    'buffer-initial': 'initial raw payload mismatch',
    'merged-image': None, 'merged-cbv': None, 'fixed-raw-image': None,
    'fixed-raw-cbv': 'typed write selected illegal/wrong active type', 'embedded': None,
    'nonmutable': None, 'nonmutable-buffer': None,
}
if a.mutation:
    cases = {failing_case:diagnosis}
for case, baseline_error in cases.items():
    q = run(case, [str(out/'test'),case])
    text = q.stdout.decode()
    expected = baseline_error if a.baseline or a.mutation else None
    good = q.returncode == (1 if expected else 0) and (
        text == 'FAIL '+case+': '+expected+'\n' if expected else
        re.fullmatch(r'PASS '+re.escape(case)+r': \d+ checks, actual production functions and private layouts\n',text) is not None)
    stages[-1].update(expected_error=expected, accepted=good)
    print(text.strip())
    if not good:
        raise AssertionError((case,q.returncode,text,expected))
receipt = dict(baseline=a.baseline, mutation=a.mutation, cpu_contract_only=True, device_actions=False,
    source={n:sha(root/n) for n in [*groups,'libs/vkd3d/vkd3d_private.h']},
    fixture_sha256=sha(Path(__file__).with_name('null_template_test.c')), runner_sha256=sha(Path(__file__)),
    generated_sha256=sha(out/'actual-functions.inc'), executable_sha256=sha(out/'test'), stages=stages)
(out/'receipt.json').write_text(json.dumps(receipt,indent=2)+'\n')
