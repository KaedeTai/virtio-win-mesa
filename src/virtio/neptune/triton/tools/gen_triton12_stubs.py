#!/usr/bin/env python3
# Copyright 2026 Turing Software LLC
# SPDX-License-Identifier: MIT
"""Emit triton12_stubs.h from the WDK's d3d12umddi.h and dxgiddi.h.

Every DDI table slot the driver does not implement gets a stub with the
slot's own prototype, so the stub returns the way the caller expects on
every ABI (x86 __stdcall pops the real argument bytes; a struct returned
by value keeps its hidden return pointer) and answers a zeroed value of
the real return type.

Slots of the D3D12 tables that an installer in the given sources assigns
(`->pfnX =`) get no stub; nothing installs into the DXGI table.  A slot
whose installer is later removed is left NULL rather than silently
answered, until this is re-run.

usage: gen_triton12_stubs.py <d3d12umddi.h> <dxgiddi.h> <out.h> <src.c>...
"""

import re
import sys

TABLES = [
    ('D3D12DDI_DEVICE_FUNCS_CORE_0022', 'd3d12'),
    ('D3D12DDI_DEVICE_FUNCS_CORE_0080', 'd3d12'),
    ('D3D12DDI_COMMAND_LIST_FUNCS_3D_0022', 'd3d12'),
    ('D3D12DDI_COMMAND_LIST_FUNCS_3D_0074', 'd3d12'),
    ('D3D12DDI_COMMAND_QUEUE_FUNCS_CORE_0001', 'd3d12'),
    ('D3D12DDI_EXTENDED_FEATURES_FUNCS_0021', 'd3d12'),
    ('DXGI1_6_1_DDI_BASE_FUNCTIONS', 'dxgi'),
]

SAL = re.compile(r'\b_[A-Z][A-Za-z0-9_]*_(\s*\([^()]*(\([^()]*\)[^()]*)*\))?')
NON_NAMES = {'CONST', 'const', 'struct', 'enum', 'unsigned', 'signed',
             'volatile'}


def strip_comments(s):
    s = re.sub(r'/\*.*?\*/', ' ', s, flags=re.S)
    return re.sub(r'//[^\n]*', '', s)


def split_params(s):
    out, depth, cur = [], 0, ''
    for ch in s:
        if ch == '(':
            depth += 1
        elif ch == ')':
            depth -= 1
        if ch == ',' and depth == 0:
            out.append(cur.strip())
            cur = ''
        else:
            cur += ch
    if cur.strip():
        out.append(cur.strip())
    return out


def matching_paren(s, i):
    depth = 0
    for j in range(i, len(s)):
        if s[j] == '(':
            depth += 1
        elif s[j] == ')':
            depth -= 1
            if depth == 0:
                return j
    raise ValueError('unbalanced')


def parse_pfn_typedefs(text):
    pfns = {}
    for m in re.finditer(r'typedef\s+([^;{}()]*?)\(\s*([A-Za-z_ ]*?)\*\s*(PFN\w+)\s*\)\s*\(', text):
        start = m.end() - 1
        end = matching_paren(text, start)
        pfns[m.group(3)] = (m.group(1).strip(), m.group(2).strip(),
                            text[start + 1:end])
    return pfns


def struct_body(text, name):
    m = re.search(r'typedef struct %s\s*\{(.*?)\}\s*%s\s*;' % (name, name),
                  text, re.S)
    if not m:
        raise SystemExit('struct %s not found' % name)
    return m.group(1)


def name_params(params):
    params = [p for p in split_params(params)]
    if len(params) == 1 and params[0] in ('VOID', 'void', ''):
        return []
    named = []
    for i, p in enumerate(params):
        bare = SAL.sub(' ', p)
        idents = [t for t in re.findall(r'[A-Za-z_]\w*', bare)
                  if t not in NON_NAMES]
        if '[' in bare:
            k = bare.index('[')
            named.append('%s p%d%s' % (bare[:k].strip(), i, bare[k:].strip()))
        elif len(idents) >= 2 and not bare.rstrip().endswith('*'):
            named.append(bare.strip())
        else:
            named.append('%s p%d' % (bare.strip(), i))
    return named


def main():
    d3d12 = strip_comments(open(sys.argv[1], encoding='latin-1').read())
    dxgi = strip_comments(open(sys.argv[2], encoding='latin-1').read())
    pfns = parse_pfn_typedefs(d3d12)
    installed = set()
    for src in sys.argv[4:]:
        installed |= set(re.findall(r'->(pfn\w+)\s*=', open(src).read()))

    stubs = {}      # (slot, signature) -> stub name
    order = []
    fills = []
    for table, src in TABLES:
        body = struct_body(d3d12 if src == 'd3d12' else dxgi, table)
        slots = []
        if src == 'd3d12':
            for pfn, slot in re.findall(r'(PFN\w+)\s+(pfn\w+)\s*;', body):
                ret, cc, params = pfns[pfn]
                slots.append((slot, ret, cc or 'APIENTRY', params))
        else:
            for ret, slot, params in re.findall(
                    r'(\w+)\s*\(\s*__stdcall\s*\*\s*(pfn\w+)\s*\)\s*\(([^)]*)\)\s*;',
                    body):
                slots.append((slot, ret, 'APIENTRY', params))
        entries = []
        for slot, ret, cc, params in slots:
            if src == 'd3d12' and slot in installed:
                continue
            proto = name_params(params)
            key = (slot if src == 'd3d12' else 'dxgi.' + slot, ret, cc,
                   tuple(proto))
            if key not in stubs:
                n = sum(1 for k in stubs if k[0] == key[0])
                prefix = 'Dxgi' if src == 'dxgi' else ''
                stubs[key] = 't12stub_%s%s%s' % (prefix, slot[3:],
                                                 '_%d' % n if n else '')
                order.append(key)
            entries.append((slot, stubs[key]))
        fills.append((table, entries))

    o = []
    o.append('/*\n * Copyright 2026 Turing Software LLC\n'
             ' * SPDX-License-Identifier: MIT\n *\n'
             ' * Generated by tools/gen_triton12_stubs.py from d3d12umddi.h and\n'
             ' * dxgiddi.h and the triton*12.c installers; do not edit.  Typed\n'
             ' * stubs for the DDI table slots no installer fills, and per-table\n'
             ' * fills that hand each such slot its stub.\n */\n')
    o.append('#ifndef TRITON12_STUBS_H\n#define TRITON12_STUBS_H\n')
    o.append('#define T12_STUB_HIT(slot) do { static LONG once_; \\\n'
             '        if (!InterlockedExchange(&once_, 1)) \\\n'
             '            TR_LOG("12.STUB[%s] hit", slot); } while (0)\n')
    for key in order:
        slot, ret, cc, proto = key
        name = stubs[key]
        args = ', '.join(proto) if proto else 'void'
        if ret in ('VOID', 'void'):
            o.append('static VOID %s\n%s(%s)\n{ T12_STUB_HIT("%s"); }\n'
                     % (cc, name, args, slot))
        else:
            o.append('static %s %s\n%s(%s)\n{ %s r; T12_STUB_HIT("%s"); '
                     'memset(&r, 0, sizeof(r)); return r; }\n'
                     % (ret, cc, name, args, ret, slot))
    for table, entries in fills:
        o.append('static inline void\nt12StubFill_%s(%s *t, SIZE_T size)\n{'
                 % (table, table))
        for slot, stub in entries:
            o.append('    if (offsetof(%s, %s) < size && !t->%s) t->%s = %s;'
                     % (table, slot, slot, slot, stub))
        o.append('}\n')
    o.append('#endif /* TRITON12_STUBS_H */\n')
    open(sys.argv[3], 'w').write('\n'.join(o))


if __name__ == '__main__':
    main()
