"""Helpers for frame export: vertex decode, glb writer, manifest IO.

RenderDoc 1.46+ Python API, runs inside qrenderdoc.
"""
import json
import os
import struct

import renderdoc as rd


def find_action(controller, eid):
    stack = list(controller.GetRootActions())
    while stack:
        act = stack.pop()
        if act.eventId == eid:
            return act
        stack.extend(act.children)
    return None


def action_name(act, sd):
    nm = getattr(act, 'customName', '') or ''
    if not nm:
        try:
            nm = act.GetName(sd) or ''
        except Exception:
            nm = ''
    return nm or ('eid%d' % act.eventId)


def iter_actions(controller):
    """Yield (action, marker_path) for every action in the frame."""
    sd = controller.GetStructuredFile()

    def rec(acts, path):
        for act in acts:
            p = path + '/' + action_name(act, sd)
            yield act, p
            for x in rec(act.children, p):
                yield x
    return rec(list(controller.GetRootActions()), '')


def decode_attrib(buf, off, stride, count, fmt):
    """Decode one vertex attribute stream -> flat float list, or None."""
    cc, bw = fmt.compCount, fmt.compByteWidth
    ct = str(fmt.compType).split('.')[-1]
    if bw == 0 or cc == 0:
        return None
    if ct == 'Float':
        code, scale = 'f', None
    elif ct == 'Double':
        code, scale = 'd', None
    elif ct in ('UNorm', 'UInt', 'UScaled'):
        code = {1: 'B', 2: 'H', 4: 'I'}.get(bw)
        scale = float((1 << (8 * bw)) - 1) if ct == 'UNorm' else None
    elif ct in ('SNorm', 'SInt', 'SScaled'):
        code = {1: 'b', 2: 'h', 4: 'i'}.get(bw)
        scale = float((1 << (8 * bw - 1)) - 1) if ct == 'SNorm' else None
    else:
        return None
    if code is None:
        return None
    sz = struct.calcsize('<' + code)
    out = []
    for i in range(count):
        base = off + i * stride
        for cp in range(cc):
            try:
                v = struct.unpack_from('<' + code, buf, base + cp * sz)[0]
            except Exception:
                v = 0
            if ct == 'Double':
                v = float(v)
            if scale:
                v = v / scale
            out.append(v)
    return out


def read_cb_bytes(controller, pipe, stage, index):
    """Read constant block raw bytes (RenderDoc 1.46 GetConstantBlock API).

    NOTE: UsedDescriptor.access is NOT a byte range for cbuffers in 1.46
    (observed byteSize=1); use the reflection block byteSize instead.
    """
    try:
        ud = pipe.GetConstantBlock(stage, index, 0)
        rid = ud.descriptor.resource
        off = ud.descriptor.byteOffset
        size = 0
        try:
            refl = pipe.GetShaderReflection(stage)
            size = refl.constantBlocks[index].byteSize
        except Exception:
            pass
        if not size:
            size = 65536
        data = controller.GetBufferData(rid, off, size)
        if data is None or len(data) == 0:
            return None
        return bytes(data)
    except Exception:
        return None


def _norm_var(n):
    """Strip HLSLcc decoration prefixes for loose-uniform name matching."""
    n = n.lower()
    for p in ('hlslcc_mtx4x4', 'hlslcc_'):
        if n.startswith(p):
            n = n[len(p):]
    return n


LOOSE_CB_LAST_ERR = []


def read_loose_cb_var(controller, pipe, stage, cb_index, var_name):
    """Read a variable from a non-UBO (loose uniform / $Globals) block via
    GetCBufferVariableContents. -> list of floats or None."""
    try:
        shader = pipe.GetShader(stage)
        pro = pipe.GetGraphicsPipelineObject()
        entry = 'main'
        try:
            entry = pipe.GetShaderReflection(stage).entryPoint
        except Exception:
            pass
        vs = controller.GetCBufferVariableContents(
            pro, shader, stage, entry, cb_index, rd.ResourceId.Null(), 0, 0)
        target = _norm_var(var_name)
        names = []
        for v in vs:
            names.append(v.name)
            if _norm_var(v.name) != target:
                continue
            # matrix vars are exposed as 4 row members (f32v itself is zero)
            mems = []
            try:
                mems = list(v.members)
            except Exception:
                pass
            if len(mems) >= 4:
                out = []
                for m in mems[:4]:
                    fv = m.value.f32v
                    out.extend(float(fv[i]) for i in range(4))
                return out
            fv = getattr(v.value, 'f32v', None)
            if fv is None:
                LOOSE_CB_LAST_ERR.append('no f32v for %s' % v.name)
                return None
            out = []
            for i in range(16):
                try:
                    out.append(float(fv[i]))
                except Exception:
                    break
            return out
        LOOSE_CB_LAST_ERR.append('no match %s in %s' % (target, names))
    except Exception as ex:
        LOOSE_CB_LAST_ERR.append('EXC %s' % ex)
        return None
    return None


def from_column_major(vals):
    """GL mat4 data (std140 UBO bytes / loose-uniform column members) is
    column-major; convert flat 16 floats to the row-major layout used by
    mat_inv / world-transform code below."""
    return [vals[c * 4 + r] for r in range(4) for c in range(4)]


def find_matrix(controller, pipe, stage, fragments):
    """Find a 4x4 float matrix uniform by name fragment.
    -> (16 floats row-major, name) or (None, None)."""
    try:
        refl = pipe.GetShaderReflection(stage)
    except Exception:
        return None, None
    for i, cb in enumerate(refl.constantBlocks):
        for v in cb.variables:
            ln = v.name.lower()
            if any(f.lower() in ln for f in fragments):
                data = read_cb_bytes(controller, pipe, stage, i)
                if data and len(data) >= v.byteOffset + 64:
                    raw = struct.unpack_from('<16f', data, v.byteOffset)
                    return from_column_major(list(raw)), v.name
                # loose uniforms (GLES $Globals) have no backing buffer;
                # matrix members are the 4 column vec4s -> same layout fix
                vals = read_loose_cb_var(controller, pipe, stage, i, v.name)
                if vals and len(vals) >= 16:
                    return from_column_major(vals[:16]), v.name
    return None, None


def read_var_floats(data, cb, var, max_floats=16):
    """Slice a cbuffer variable using next var offset; return floats."""
    offs = sorted(x.byteOffset for x in cb.variables if x.byteOffset > var.byteOffset)
    end = offs[0] if offs else min(cb.byteSize, len(data))
    end = min(end, len(data))
    n = min(max_floats, (end - var.byteOffset) // 4)
    if n <= 0:
        return []
    return list(struct.unpack_from('<%df' % n, data, var.byteOffset))


def write_glb(path, attributes, indices, name):
    """Write a minimal glTF 2.0 binary (.glb) with one mesh.
    attributes: dict SEMANTIC -> flat float list. indices: int list or None."""
    buffer_views, accessors, bin_parts = [], [], []
    offset = 0
    gltf_attrs = {}

    def add_view(raw, target):
        nonlocal offset
        buffer_views.append({'buffer': 0, 'byteOffset': offset,
                             'byteLength': len(raw), 'target': target})
        bin_parts.append(raw)
        pad = (4 - len(raw) % 4) % 4
        if pad:
            bin_parts.append(b'\x00' * pad)
        offset += len(raw) + pad
        return len(buffer_views) - 1

    for sem, flat in attributes.items():
        ncomp = 4 if sem in ('TANGENT', 'COLOR_0') else (2 if sem.startswith('TEXCOORD') else 3)
        count = len(flat) // ncomp
        if count == 0:
            continue
        raw = struct.pack('<%df' % len(flat), *flat)
        bv = add_view(raw, 34962)
        acc = {'bufferView': bv, 'componentType': 5126, 'count': count,
               'type': {2: 'VEC2', 3: 'VEC3', 4: 'VEC4'}[ncomp]}
        if sem == 'POSITION':
            acc['min'] = [min(flat[0::3]), min(flat[1::3]), min(flat[2::3])]
            acc['max'] = [max(flat[0::3]), max(flat[1::3]), max(flat[2::3])]
        accessors.append(acc)
        gltf_attrs[sem] = len(accessors) - 1

    idx_acc = None
    if indices:
        use32 = max(indices) > 65535
        raw = struct.pack(('<%dI' if use32 else '<%dH') % len(indices), *indices)
        bv = add_view(raw, 34963)
        accessors.append({'bufferView': bv, 'componentType': 5125 if use32 else 5123,
                          'count': len(indices), 'type': 'SCALAR'})
        idx_acc = len(accessors) - 1

    prim = {'attributes': gltf_attrs, 'mode': 4}
    if idx_acc is not None:
        prim['indices'] = idx_acc
    g = {'asset': {'version': '2.0', 'generator': 'renderdoc-mcp frame-export'},
         'scene': 0, 'scenes': [{'nodes': [0]}],
         'nodes': [{'mesh': 0, 'name': name}],
         'meshes': [{'name': name, 'primitives': [prim]}],
         'bufferViews': buffer_views, 'accessors': accessors,
         'buffers': [{'byteLength': offset}]}
    js = json.dumps(g, separators=(',', ':')).encode()
    js += b' ' * ((4 - len(js) % 4) % 4)
    bn = b''.join(bin_parts)
    with open(path, 'wb') as f:
        f.write(struct.pack('<III', 0x46546C67, 2, 12 + 8 + len(js) + 8 + len(bn)))
        f.write(struct.pack('<II', len(js), 0x4E4F534A))
        f.write(js)
        f.write(struct.pack('<II', len(bn), 0x004E4942))
        f.write(bn)


def load_manifest(out_dir):
    p = os.path.join(out_dir, 'manifest.json')
    if os.path.exists(p):
        try:
            with open(p, 'r', encoding='utf-8') as f:
                return json.load(f)
        except Exception:
            pass
    return {'meshes': {}, 'materials': {}, 'out_dir': out_dir}


def save_manifest(out_dir, m):
    os.makedirs(out_dir, exist_ok=True)
    with open(os.path.join(out_dir, 'manifest.json'), 'w', encoding='utf-8') as f:
        json.dump(m, f, ensure_ascii=False, indent=1)


def mat_mul(a, b):
    """Row-major 4x4 multiply a*b."""
    o = [0.0] * 16
    for r in range(4):
        for c in range(4):
            o[r * 4 + c] = sum(a[r * 4 + k] * b[k * 4 + c] for k in range(4))
    return o


def mat_inv(m):
    """General 4x4 inverse via Gauss-Jordan (row-major flat 16). None if singular."""
    n = 4
    a = [[m[r * 4 + ci] for ci in range(n)] + [1.0 if ci == r else 0.0
                                               for ci in range(n)]
         for r in range(n)]
    for col in range(n):
        piv = max(range(col, n), key=lambda r: abs(a[r][col]))
        if abs(a[piv][col]) < 1e-12:
            return None
        a[col], a[piv] = a[piv], a[col]
        d = a[col][col]
        a[col] = [x / d for x in a[col]]
        for r in range(n):
            if r != col and a[r][col] != 0.0:
                f = a[r][col]
                a[r] = [x - f * y for x, y in zip(a[r], a[col])]
    return [a[r][n + ci] for r in range(n) for ci in range(n)]


def mat_inv_rigid(m):
    """Inverse of affine 4x4 (general 3x3 inverse + translation)."""
    a = [m[0], m[1], m[2]], [m[4], m[5], m[6]], [m[8], m[9], m[10]]
    det = (a[0][0] * (a[1][1] * a[2][2] - a[1][2] * a[2][1])
           - a[0][1] * (a[1][0] * a[2][2] - a[1][2] * a[2][0])
           + a[0][2] * (a[1][0] * a[2][1] - a[1][1] * a[2][0]))
    if abs(det) < 1e-12:
        return None
    idet = 1.0 / det
    inv = [[(a[1][1] * a[2][2] - a[1][2] * a[2][1]) * idet,
            (a[0][2] * a[2][1] - a[0][1] * a[2][2]) * idet,
            (a[0][1] * a[1][2] - a[0][2] * a[1][1]) * idet],
           [(a[1][2] * a[2][0] - a[1][0] * a[2][2]) * idet,
            (a[0][0] * a[2][2] - a[0][2] * a[2][0]) * idet,
            (a[0][2] * a[1][0] - a[0][0] * a[1][2]) * idet],
           [(a[1][0] * a[2][1] - a[1][1] * a[2][0]) * idet,
            (a[0][1] * a[2][0] - a[0][0] * a[2][1]) * idet,
            (a[0][0] * a[1][1] - a[0][1] * a[1][0]) * idet]]
    t = [m[3], m[7], m[11]]
    it = [-(inv[r][0] * t[0] + inv[r][1] * t[1] + inv[r][2] * t[2]) for r in range(3)]
    return [inv[0][0], inv[0][1], inv[0][2], it[0],
            inv[1][0], inv[1][1], inv[1][2], it[1],
            inv[2][0], inv[2][1], inv[2][2], it[2],
            0.0, 0.0, 0.0, 1.0]


def zflip_matrix(m):
    """Conjugate row-major 4x4 by S=diag(1,1,-1,1): Unity(LH) -> glTF(RH).
    Sign-flip elements where exactly one of (row, col) == 2."""
    o = list(m)
    for r in range(4):
        for c in range(4):
            if (r == 2) != (c == 2):
                o[r * 4 + c] = -o[r * 4 + c]
    return o
