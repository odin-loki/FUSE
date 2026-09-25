#!/usr/bin/env python3
"""FUSE Relight RL-6.2: regenerates ../remixapi_layout_ref.inc, the reference layouts of the Remix API ABI gate.

  gen_remixapi_layout.py <remix_c.h> <work dir>

<remix_c.h>: dxvk-remix public/include/remix/remix_c.h (MIT; not vendored: fetch it at the pinned commit). Every
`typedef struct remixapi_*` is compiled with x86_64-w64-mingw32-gcc and i686-w64-mingw32-gcc (REMIX_ALLOW_X86) into a
table of sizeof / _Alignof / offsetof values that is read back from the object's .rdata (nothing is executed); the
struct-type / error-code / category / camera / output enum values are read the same way. Writes
<work dir>/remixapi_layout_ref.inc; copy it over ../remixapi_layout_ref.inc.
"""
import re, subprocess, struct, sys, os
S = sys.argv[2]; os.makedirs(S, exist_ok=True)
hdr = open(sys.argv[1]).read()
structs = []
for m in re.finditer(r'typedef struct (remixapi_\w+) \{(.*?)\}\s*(remixapi_\w+);', hdr, re.S):
    name, body = m.group(1), m.group(2)
    fields = []
    for line in body.split('\n'):
        line = line.split('//')[0].strip()
        if not line or not line.endswith(';'):
            continue
        decl = line[:-1].strip()
        fm = re.match(r'.*?[\s\*]+(\w+)\s*((\[\d+\])*)$', decl)
        fields.append(fm.group(1))
    structs.append((name, fields))
enums = re.findall(r'\b(REMIXAPI_(?:STRUCT_TYPE|ERROR_CODE|INSTANCE_CATEGORY_BIT|CAMERA_TYPE|DXVK_COPY_RENDERING_OUTPUT_TYPE)_\w+)\s*[=,\n]', hdr)
enums = list(dict.fromkeys(enums))
c = ['#define REMIX_WINAPI_NO_INCLUDE', '#define REMIX_ALLOW_X86', '#define REMIX_WINAPI_NO_LIBRARY_LOADER',
     '#include "%s"' % os.path.abspath(sys.argv[1]), '#include <stddef.h>', 'const unsigned long long kLayout[] = {']
for name, fields in structs:
    c.append('  sizeof(%s), _Alignof(%s),' % (name, name))
    for f in fields:
        c.append('  offsetof(%s, %s),' % (name, f))
for e in enums:
    c.append('  (unsigned long long)(unsigned int)%s,' % e)
c.append('};')
open(os.path.join(S, 'layout.c'), 'w').write('\n'.join(c) + '\n')
vals = {}
for arch, cc in (('64', 'x86_64-w64-mingw32-gcc'), ('32', 'i686-w64-mingw32-gcc')):
    obj = os.path.join(S, 'layout%s.o' % arch)
    subprocess.check_call([cc, '-std=c11', '-c', os.path.join(S, 'layout.c'), '-o', obj])
    binf = obj + '.bin'
    subprocess.check_call([cc.replace('gcc', 'objcopy'), '-O', 'binary', '-j', '.rdata', obj, binf])
    data = open(binf, 'rb').read()
    n = len(data) // 8
    vals[arch] = list(struct.unpack('<%dQ' % n, data[:n * 8]))
out = ['// Generated from dxvk-remix public/include/remix/remix_c.h@0867d3c (MIT, Remix API 0.6.5) by compiling it with',
       '// x86_64-w64-mingw32-gcc and i686-w64-mingw32-gcc (REMIX_ALLOW_X86) and reading sizeof / _Alignof / offsetof back:',
       '// the reference layouts of the ABI gate (test_api_layout.cpp). RL_STRUCT(type, size64, align64, size32, align32),',
       '// RL_FIELD(type, field, offset64, offset32), RL_ENUM(name, value). Do not edit by hand.']
i = 0
a, b = vals['64'], vals['32']
for name, fields in structs:
    out.append('RL_STRUCT(%s, %d, %d, %d, %d)' % (name, a[i], a[i+1], b[i], b[i+1])); i += 2
    for f in fields:
        out.append('RL_FIELD(%s, %s, %d, %d)' % (name, f, a[i], b[i])); i += 1
for e in enums:
    assert a[i] == b[i]
    out.append('RL_ENUM(%s, 0x%X)' % (e, a[i])); i += 1
open(os.path.join(S, 'remixapi_layout_ref.inc'), 'w').write('\n'.join(out) + '\n')
print(len(structs), 'structs', sum(len(f) for _, f in structs), 'fields', len(enums), 'enums')
