"""Frame export service: mesh -> glb, material -> json+png, scene -> .blend.

RenderDoc 1.46+ Python API, runs inside qrenderdoc.
Fine-grained tools share state via {out_dir}/manifest.json.
"""
import json
import os
import struct
import subprocess
import traceback

import renderdoc as rd

from .export_glb import (find_action, iter_actions, action_name, decode_attrib,
                         read_cb_bytes, find_matrix, read_var_floats, write_glb,
                         load_manifest, save_manifest,
                         mat_mul, mat_inv, mat_inv_rigid, zflip_matrix)

DEFAULT_OUT = os.path.join(os.path.expanduser('~'), 'Documents', 'rd_frame_export')


def _dbg_cb(pipe, stage, index):
    """Temporary diagnostic for GetConstantBlock 1.46 behaviour."""
    try:
        ud = pipe.GetConstantBlock(stage, index, 0)
        d = ud.descriptor
        acc = getattr(ud, 'access', None)
        accs = 'None' if acc is None else 'sz=%s off=%s' % (
            getattr(acc, 'byteSize', '?'), getattr(acc, 'byteOffset', '?'))
        return 'rid=%s off=%s acc(%s) udflds=%s' % (
            d.resource, d.byteOffset, accs,
            ','.join(a for a in dir(ud) if not a.startswith('_'))[:200])
    except Exception as ex:
        return 'EXC %s' % ex


def _res_is_texture(res):
    """1.46-compatible texture check on a ShaderResource (field names vary).
    1.46 exposes isTexture bool; descriptorType is an int enum (str() useless)."""
    if hasattr(res, 'isTexture'):
        try:
            return bool(res.isTexture)
        except Exception:
            pass
    for attr in ('resType', 'descriptorType', 'type'):
        t = getattr(res, attr, None)
        if t is not None and 'texture' in str(t).lower():
            return True
    return False


class ExportService:
    def __init__(self, ctx, invoke_fn):
        self.ctx = ctx
        self._invoke = invoke_fn

    def _guard(self):
        if not self.ctx.IsCaptureLoaded():
            raise ValueError("No capture loaded")

    # ---------------------------------------------------------- list_meshes
    def list_meshes(self, marker_filter='', eid_min=0, eid_max=1 << 30,
                    min_indices=0, offset=0, limit=50, exclude_filter=''):
        self._guard()
        box = {}

        def cb(controller):
            try:
                box['r'] = self._list_meshes_impl(controller, marker_filter,
                                                  eid_min, eid_max, min_indices,
                                                  offset, limit, exclude_filter)
            except Exception:
                box['e'] = traceback.format_exc()
        self._invoke(cb)
        if 'e' in box:
            raise ValueError(box['e'])
        return box['r']

    def export_mesh(self, eid, out_dir=None, stage='auto'):
        self._guard()
        box = {}

        def cb(controller):
            try:
                box['r'] = self._export_mesh_impl(controller, eid, out_dir, stage)
            except Exception:
                box['e'] = traceback.format_exc()
        self._invoke(cb)
        if 'e' in box:
            raise ValueError(box['e'])
        return box['r']

    def export_material(self, eid, out_dir=None):
        self._guard()
        box = {}

        def cb(controller):
            try:
                box['r'] = self._export_material_impl(controller, eid, out_dir)
            except Exception:
                box['e'] = traceback.format_exc()
        self._invoke(cb)
        if 'e' in box:
            raise ValueError(box['e'])
        return box['r']

    def export_frame(self, out_dir=None, marker_filter='', eid_min=0,
                     eid_max=1 << 30, min_indices=3, stage='auto',
                     materials=True, limit=10000, fresh=False,
                     exclude_filter=''):
        self._guard()
        box = {}

        def cb(controller):
            try:
                box['r'] = self._export_frame_impl(
                    controller, out_dir, marker_filter, eid_min, eid_max,
                    min_indices, stage, materials, limit, fresh, exclude_filter)
            except Exception:
                box['e'] = traceback.format_exc()
        self._invoke(cb)
        if 'e' in box:
            raise ValueError(box['e'])
        return box['r']

    # ---------------------------------------------------------- impls
    def _list_meshes_impl(self, c, marker_filter, eid_min, eid_max,
                          min_indices, offset, limit, exclude_filter=''):
        items = []
        excl = [s for s in exclude_filter.lower().split(',') if s]
        for act, path in iter_actions(c):
            if not (act.flags & rd.ActionFlags.Drawcall):
                continue
            if not (eid_min <= act.eventId <= eid_max):
                continue
            if act.numIndices < min_indices:
                continue
            lp = path.lower()
            if marker_filter and marker_filter.lower() not in lp:
                continue
            if excl and any(s in lp for s in excl):
                continue
            items.append({'e': act.eventId, 'n': path.rsplit('/', 1)[-1][:40],
                          'i': act.numIndices, 'p': path[-60:]})
        return {'total': len(items), 'offset': offset, 'limit': limit,
                'draws': items[offset:offset + limit]}

    # ---------------------------------------------------------- export_frame
    def _export_frame_impl(self, c, out_dir, marker_filter, eid_min, eid_max,
                           min_indices, stage, materials, limit, fresh,
                           exclude_filter=''):
        out_dir = out_dir or DEFAULT_OUT
        if fresh:
            mp = os.path.join(out_dir, 'manifest.json')
            if os.path.exists(mp):
                os.remove(mp)
        draws = self._list_meshes_impl(c, marker_filter, eid_min, eid_max,
                                       min_indices, 0, limit,
                                       exclude_filter)['draws']
        ok, failed = [], []
        mat_eids = {}
        for it in draws:
            eid = it['e']
            r = self._export_mesh_impl(c, eid, out_dir, stage)
            if 'error' in r:
                failed.append({'e': eid, 'error': r['error'],
                               'warnings': r.get('warnings', [])[:3]})
                continue
            entry = {'e': eid, 'v': r['vertices'], 't': r['triangles'],
                     'skinned': r['skinned']}
            if r.get('warnings'):
                entry['warnings'] = r['warnings'][:3]
            ok.append(entry)
            mk = r.get('material_key')
            if mk and mk not in mat_eids:
                mat_eids[mk] = eid
        mats_ok, mats_failed = 0, []
        if materials:
            for mk, eid in mat_eids.items():
                r = self._export_material_impl(c, eid, out_dir)
                if 'error' in r:
                    mats_failed.append({'key': mk, 'e': eid,
                                        'error': r['error'][:200]})
                else:
                    mats_ok += 1
        man = load_manifest(out_dir)
        return {'out_dir': out_dir, 'draws_listed': len(draws),
                'meshes_ok': len(ok), 'meshes_failed': len(failed),
                'materials_ok': mats_ok, 'materials_failed': len(mats_failed),
                'failed': failed[:20], 'mats_failed': mats_failed[:20],
                'meshes': ok,
                'manifest_meshes': len(man['meshes']),
                'manifest_materials': len(man['materials'])}

    # ---------------------------------------------------------- export_mesh
    def _export_mesh_impl(self, c, eid, out_dir, stage):
        out_dir = out_dir or DEFAULT_OUT
        self._last_vp = None
        self._last_dbg = None
        self._last_vlayout = None
        c.SetFrameEvent(eid, False)
        act = find_action(c, eid)
        if act is None:
            return {'error': 'no action at eid %d' % eid}
        if not (act.flags & rd.ActionFlags.Drawcall):
            return {'error': 'eid %d is not a drawcall' % eid}
        pipe = c.GetPipelineState()
        vs_refl = pipe.GetShaderReflection(rd.ShaderStage.Vertex)

        vs_tex = any(_res_is_texture(r) for r in vs_refl.readOnlyResources)
        skinned = (stage == 'vsout') or (stage == 'auto' and vs_tex)

        mesh_dir = os.path.join(out_dir, 'meshes')
        os.makedirs(mesh_dir, exist_ok=True)
        name = 'm_%d' % eid
        path = os.path.join(mesh_dir, name + '.glb')

        warn = []
        if skinned:
            attrs, indices, nv = self._extract_vsout(c, pipe, vs_refl, act, warn)
        else:
            attrs, indices, nv = self._extract_vsin(c, pipe, act, warn)
            if self._positions_bad(attrs):
                # glDrawElementsBaseVertex on GLES 1.46: VSIn PostVS is empty
                # and raw VB readback decodes to garbage -> retry via VSOut
                # (world space recovered by unprojecting with VP).
                warn.append('vsin positions invalid; fell back to vsout')
                a2, i2, n2 = self._extract_vsout(c, pipe, vs_refl, act, warn)
                if a2 and 'POSITION' in a2:
                    attrs, indices, nv = a2, i2, n2
                    skinned = True
            else:
                # SRP Batcher on GLES 1.46 misreports the per-draw UBO offset
                # for some draws (e.g. projector/blob-shadow draws): the M we
                # read is then NOT the matrix the GPU used. Cross-check M*obj
                # against the actual GPU vertex output; fall back on mismatch.
                m_chk, _ = find_matrix(c, pipe, rd.ShaderStage.Vertex,
                                       ['objecttoworld', 'object2world'])
                if m_chk is not None:
                    a2, i2, n2 = self._extract_vsout(c, pipe, vs_refl, act, warn)
                    if (a2 and 'POSITION' in a2
                            and not self._positions_bad(a2, 1e4)
                            and self._worlds_disagree(attrs, m_chk,
                                                      a2['POSITION'],
                                                      indices)):
                        warn.append('M*obj disagrees with GPU vsout; '
                                    'fell back to vsout')
                        attrs, indices, nv = a2, i2, n2
                        skinned = True
        if skinned:
            matrix = [1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1]
        else:
            matrix, mname = find_matrix(c, pipe, rd.ShaderStage.Vertex,
                                        ['objecttoworld', 'object2world'])
            if matrix is None:
                matrix = [1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1]
                cbvars = []
                try:
                    for cb in vs_refl.constantBlocks:
                        cbvars.append('%s:[%s]' % (
                            cb.name, ','.join(v.name for v in cb.variables[:6])))
                except Exception:
                    pass
                warn.append('world matrix not found; identity used; vs cbs: %s'
                            % (' | '.join(cbvars)[:300]))
                warn.append('dbg cb0: %s' % _dbg_cb(pipe, rd.ShaderStage.Vertex, 0))
        if attrs is None or 'POSITION' not in attrs:
            return {'error': 'position decode failed', 'warnings': warn}
        if self._positions_bad(attrs, 1e4):
            return {'error': 'positions out of sane range', 'warnings': warn}

        self._unity_to_gltf(attrs, indices)
        matrix = zflip_matrix(matrix)

        try:
            write_glb(path, attrs, indices, name)
        except Exception as ex:
            return {'error': 'glb write failed: %s' % ex}

        man = load_manifest(out_dir)
        man['meshes'][str(eid)] = {
            'file': os.path.relpath(path, out_dir), 'name': name,
            'matrix': matrix, 'skinned': skinned, 'vertices': nv,
            'triangles': (len(indices) // 3) if indices else nv // 3,
            'attributes': sorted(attrs.keys()),
            'material_key': self._material_key(pipe),
            'draw_name': action_name(act, c.GetStructuredFile()),
        }
        save_manifest(out_dir, man)
        pos = attrs.get('POSITION') or []
        bounds = None
        if pos:
            bounds = {'min': [round(min(pos[i::3]), 3) for i in range(3)],
                      'max': [round(max(pos[i::3]), 3) for i in range(3)]}
        return {'event_id': eid, 'file': path, 'vertices': nv,
                'triangles': man['meshes'][str(eid)]['triangles'],
                'skinned': skinned, 'attributes': sorted(attrs.keys()),
                'vp': getattr(self, '_last_vp', None), 'pos_bounds': bounds,
                'dbg': getattr(self, '_last_dbg', None),
                'vlayout': getattr(self, '_last_vlayout', None),
                'material_key': man['meshes'][str(eid)]['material_key'],
                'warnings': warn}

    # ------------------------------------------------ VSIn extraction
    _SEM_MAP = {'POSITION': 'POSITION', 'NORMAL': 'NORMAL', 'TANGENT': 'TANGENT',
                'COLOR': 'COLOR_0', 'TEXCOORD': 'TEXCOORD'}

    @staticmethod
    def _positions_bad(attrs, thresh=1e5):
        """Sample POSITION stream; absurd magnitudes mean the raw-VB decode
        was misaligned (observed on glDrawElementsBaseVertex, GLES 1.46)."""
        p = (attrs or {}).get('POSITION')
        if not p:
            return True
        step = max(1, len(p) // 1000)
        return max(abs(v) for v in p[::step]) > thresh

    @staticmethod
    def _worlds_disagree(vattrs, m, wp, indices=None, tol=0.05):
        """Compare M*obj (vsin path) against vsout-recovered world positions
        via order-free stats (sampled centroid + bbox size). True when the
        read matrix does not reproduce the GPU output (wrong UBO slot).
        Only index-referenced vertices are compared: shared VBs contain
        unrelated verts that would skew the bbox."""
        pa = (vattrs or {}).get('POSITION')
        if not pa or not wp:
            return False
        na, nb = len(pa) // 3, len(wp) // 3
        if na == 0 or nb == 0:
            return False
        if indices:
            sel = sorted(set(i2 for i2 in indices if 0 <= i2 < na))
        else:
            sel = list(range(na))
        if not sel:
            return False

        def stats(vals):
            cnt = 0
            cen = [0.0, 0.0, 0.0]
            mn = [1e30] * 3
            mx = [-1e30] * 3
            for w in vals:
                if not all(abs(v) < 1e30 for v in w):
                    continue  # skip NaN/Inf
                for j in range(3):
                    cen[j] += w[j]
                    mn[j] = min(mn[j], w[j])
                    mx[j] = max(mx[j], w[j])
                cnt += 1
            if cnt == 0:
                return None
            return [c / cnt for c in cen], [mx[j] - mn[j] for j in range(3)]

        step = max(1, len(sel) // 64)
        sa = stats((m[0] * pa[v * 3] + m[1] * pa[v * 3 + 1] + m[2] * pa[v * 3 + 2] + m[3],
                    m[4] * pa[v * 3] + m[5] * pa[v * 3 + 1] + m[6] * pa[v * 3 + 2] + m[7],
                    m[8] * pa[v * 3] + m[9] * pa[v * 3 + 1] + m[10] * pa[v * 3 + 2] + m[11])
                   for v in sel[::step])
        sb = stats(((wp[v * 3], wp[v * 3 + 1], wp[v * 3 + 2]))
                   for v in range(0, nb, max(1, nb // 64)))
        if sa is None or sb is None:
            return False
        dcen = max(abs(sa[0][i] - sb[0][i]) for i in range(3))
        dsz = max(abs(sa[1][i] - sb[1][i]) for i in range(3))
        scale = max(max(sa[1]), 1e-3)
        return max(dcen, dsz) / scale > tol

    @staticmethod
    def _vertex_count(fmt):
        if fmt.vertexByteStride > 0 and fmt.vertexByteSize > 0:
            return fmt.vertexByteSize // fmt.vertexByteStride
        return 0

    def _extract_vsin(self, c, pipe, act, warn):
        eid = act.eventId
        fmt = c.GetPostVSData(eid, 0, rd.MeshDataStage.VSIn)
        nv = self._vertex_count(fmt)
        vbs = pipe.GetVBuffers()
        vinputs = pipe.GetVertexInputs()
        if not vinputs or len(vbs) == 0:
            warn.append('no vertex inputs')
            return None, None, 0

        self._last_vlayout = [
            {'name': a.name, 'slot': a.vertexBuffer, 'off': a.byteOffset,
             'cc': a.format.compCount, 'bw': a.format.compByteWidth,
             'ct': str(a.format.compType).split('.')[-1],
             'vb_stride': (vbs[a.vertexBuffer].byteStride
                           if a.vertexBuffer < len(vbs) else -1),
             'vb_off': (vbs[a.vertexBuffer].byteOffset
                        if a.vertexBuffer < len(vbs) else -1)}
            for a in vinputs]
        self._last_vlayout.append(
            {'postvs_stride': fmt.vertexByteStride,
             'postvs_size': fmt.vertexByteSize, 'nv': nv,
             'numIndices': act.numIndices})

        # fetch used vb bytes
        vb_data = {}
        for a in vinputs:
            slot = a.vertexBuffer
            if slot in vb_data or slot >= len(vbs):
                continue
            b = vbs[slot]
            size = b.byteStride * nv if nv else 0
            if size <= 0:
                size = 1 << 20
            d = c.GetBufferData(b.resourceId, b.byteOffset, size)
            if d is not None:
                vb_data[slot] = bytes(d)

        if nv == 0:
            # fall back: infer vertex count from position stream length
            for a in vinputs:
                if a.name.upper().startswith('IN_POSITION') and a.vertexBuffer in vb_data:
                    b = vbs[a.vertexBuffer]
                    nv = len(vb_data[a.vertexBuffer]) // max(1, b.byteStride)
                    break

        attrs = {}
        for a in vinputs:
            if not a.name or a.vertexBuffer not in vb_data:
                continue
            up = a.name.upper().lstrip('IN_')
            sem = None
            for k, v in self._SEM_MAP.items():
                if up.startswith(k):
                    sem = v
                    break
            if sem is None:
                continue
            if sem == 'TEXCOORD':
                idx = ''.join(ch for ch in up if ch.isdigit())
                sem = 'TEXCOORD_%d' % (int(idx) if idx else 0)
            b = vbs[a.vertexBuffer]
            flat = decode_attrib(vb_data[a.vertexBuffer], a.byteOffset,
                                 b.byteStride, nv, a.format)
            if flat is None:
                warn.append('skip attr %s (format)' % a.name)
                continue
            # normalize component counts
            if sem in ('POSITION', 'NORMAL'):
                cc = a.format.compCount
                if cc != 3:
                    flat = self._repack(flat, cc, 3, 1.0 if cc < 3 else 0.0)
            if sem == 'COLOR_0':
                cc = a.format.compCount
                if cc != 4:
                    flat = self._repack(flat, cc, 4, 1.0)
            if sem.startswith('TEXCOORD'):
                cc = a.format.compCount
                if cc != 2:
                    flat = self._repack(flat, cc, 2, 0.0)
            attrs[sem] = flat

        indices = None
        # VSIn MeshFormat has no index binding on GLES 1.46; borrow the
        # VSOut stage's index buffer info (same draw, same IB).
        ib_fmt = fmt
        if ib_fmt.indexResourceId == rd.ResourceId.Null():
            try:
                ib_fmt = c.GetPostVSData(eid, 0, rd.MeshDataStage.VSOut)
            except Exception:
                pass
        if ib_fmt.indexResourceId != rd.ResourceId.Null() and act.numIndices:
            istride = ib_fmt.indexByteStride or 2
            idata = c.GetBufferData(ib_fmt.indexResourceId,
                                    ib_fmt.indexByteOffset,
                                    act.numIndices * istride)
            if idata:
                code = 'H' if istride == 2 else 'I'
                n = len(idata) // istride
                indices = list(struct.unpack_from('<%d%s' % (n, code), bytes(idata), 0))
                if ib_fmt.baseVertex:
                    indices = [i + ib_fmt.baseVertex for i in indices]
        else:
            warn.append('no index buffer; triangle soup')
        return attrs, indices, nv

    @staticmethod
    def _repack(flat, src_cc, dst_cc, pad):
        n = len(flat) // src_cc
        out = []
        for i in range(n):
            row = flat[i * src_cc:(i + 1) * src_cc]
            row = row[:dst_cc] + [pad] * max(0, dst_cc - len(row))
            out.extend(row)
        return out

    # ------------------------------------------------ VSOut extraction (skinned)
    def _extract_vsout(self, c, pipe, vs_refl, act, warn):
        eid = act.eventId
        fmt = c.GetPostVSData(eid, 0, rd.MeshDataStage.VSOut)
        data = c.GetBufferData(fmt.vertexResourceId, fmt.vertexByteOffset,
                               fmt.vertexByteSize)
        if not data:
            warn.append('vsout buffer read failed')
            return None, None, 0
        data = bytes(data)
        stride = fmt.vertexByteStride
        if stride <= 0:
            warn.append('vsout stride 0')
            return None, None, 0
        nv = len(data) // stride

        sig = list(vs_refl.outputSignature)
        ncol = stride // 16
        cols = []
        for i, p in enumerate(sig):
            if i >= ncol:
                break
            cols.append((p.varName.upper(), p.semanticName.upper(), i))

        def read_col(idx, ncomp=4):
            out = []
            for v in range(nv):
                base = v * stride + idx * 16
                out.extend(struct.unpack_from('<4f', data, base)[:ncomp])
            return out

        pos_col = None
        uv_col = None
        nrm_col = None
        col_col = None
        for vn, sn, i in cols:
            if 'GL_POSITION' in vn or 'SV_POSITION' in sn or sn == 'POSITION0':
                pos_col = i
            elif uv_col is None and ('TEXCOORD0' in vn or 'TEXCOORD0' in sn):
                uv_col = i
            elif nrm_col is None and ('NORMAL0' in vn or 'NORMAL0' in sn):
                nrm_col = i
            elif col_col is None and ('COLOR0' in vn or 'COLOR0' in sn):
                col_col = i
        if pos_col is None:
            pos_col = 0
            warn.append('vsout position column guessed')

        clip = read_col(pos_col, 4)

        vp, vpname = find_matrix(c, pipe, rd.ShaderStage.Vertex,
                                 ['matrixvp', 'matrix_viewproj', 'viewproj'])
        self._last_vp = vpname
        if vp is None:
            from . import export_glb as _eg
            err = _eg.LOOSE_CB_LAST_ERR[-1] if _eg.LOOSE_CB_LAST_ERR else '-'
            warn.append('VP matrix not found; clip-space dump (%s)' % err)
            pos = []
            for v in range(nv):
                x, y, z, w = clip[v * 4:v * 4 + 4]
                w = w if abs(w) > 1e-9 else 1.0
                pos += [x / w, y / w, z / w]
        else:
            ivp = mat_inv(vp)
            if ivp is None:
                warn.append('VP inverse failed')
                return None, None, 0
            pos = []
            for v in range(nv):
                x, y, z, w = clip[v * 4:v * 4 + 4]
                wx = ivp[0] * x + ivp[1] * y + ivp[2] * z + ivp[3] * w
                wy = ivp[4] * x + ivp[5] * y + ivp[6] * z + ivp[7] * w
                wz = ivp[8] * x + ivp[9] * y + ivp[10] * z + ivp[11] * w
                ww = ivp[12] * x + ivp[13] * y + ivp[14] * z + ivp[15] * w
                if abs(ww) > 1e-9:
                    wx, wy, wz = wx / ww, wy / ww, wz / ww
                pos += [wx, wy, wz]
            # debug: VP roundtrip sanity on vertex 0
            try:
                x, y, z, w = clip[0:4]
                rt = [vp[0] * pos[0] + vp[1] * pos[1] + vp[2] * pos[2] + vp[3],
                      vp[4] * pos[0] + vp[5] * pos[1] + vp[6] * pos[2] + vp[7],
                      vp[8] * pos[0] + vp[9] * pos[1] + vp[10] * pos[2] + vp[11],
                      vp[12] * pos[0] + vp[13] * pos[1] + vp[14] * pos[2] + vp[15]]
                self._last_dbg = {
                    'vp': [round(v2, 4) for v2 in vp],
                    'clip0': [round(v2, 4) for v2 in (x, y, z, w)],
                    'clip1': [round(v2, 4) for v2 in clip[4:8]],
                    'clip2': [round(v2, 4) for v2 in clip[8:12]],
                    'clip_min': [round(min(clip[i::4]), 3) for i in range(4)],
                    'clip_max': [round(max(clip[i::4]), 3) for i in range(4)],
                    'world0': [round(v2, 4) for v2 in pos[0:3]],
                    'reclip0': [round(v2, 4) for v2 in rt],
                    'stride': stride, 'nv': nv, 'pos_col': pos_col,
                }
                # ground truth cross-check via vsin M*obj (debug only;
                # doubles per-draw cost, skipped in batch runs)
                if getattr(self, '_dbg_xcheck', False):
                    try:
                        vattrs, _, vnv = self._extract_vsin(c, pipe, act, [])
                        m, _ = find_matrix(c, pipe, rd.ShaderStage.Vertex,
                                           ['objecttoworld'])
                        vp2 = vattrs.get('POSITION')
                        if vp2 and m:
                            wp = []
                            for i in range(0, len(vp2), 3):
                                ox, oy, oz = vp2[i], vp2[i + 1], vp2[i + 2]
                                wp += [m[0] * ox + m[1] * oy + m[2] * oz + m[3],
                                       m[4] * ox + m[5] * oy + m[6] * oz + m[7],
                                       m[8] * ox + m[9] * oy + m[10] * oz + m[11]]
                            self._last_dbg['vsin_world_min'] = [
                                round(min(wp[i::3]), 3) for i in range(3)]
                            self._last_dbg['vsin_world_max'] = [
                                round(max(wp[i::3]), 3) for i in range(3)]
                    except Exception as ex:
                        self._last_dbg['vsin_ref_exc'] = str(ex)
            except Exception:
                pass

        attrs = {'POSITION': pos}
        if uv_col is not None:
            uv4 = read_col(uv_col, 4)
            attrs['TEXCOORD_0'] = [c for v in range(nv)
                                   for c in uv4[v * 4:v * 4 + 2]]
            # lightmap-style shaders pack the 2nd UV set in .zw of the same
            # varying; keep it as TEXCOORD_1 when it actually carries data
            zw = [c for v in range(nv) for c in uv4[v * 4 + 2:v * 4 + 4]]
            if any(abs(x) > 1e-6 for x in zw[::max(1, len(zw) // 200)]):
                attrs['TEXCOORD_1'] = zw
        if nrm_col is not None:
            attrs['NORMAL'] = read_col(nrm_col, 3)
        if col_col is not None:
            attrs['COLOR_0'] = read_col(col_col, 4)

        indices = None
        if fmt.indexResourceId != rd.ResourceId.Null() and act.numIndices:
            istride = fmt.indexByteStride or 2
            idata = c.GetBufferData(fmt.indexResourceId, fmt.indexByteOffset,
                                    act.numIndices * istride)
            if idata:
                code = 'H' if istride == 2 else 'I'
                n = len(idata) // istride
                indices = list(struct.unpack_from('<%d%s' % (n, code), bytes(idata), 0))
        return attrs, indices, nv

    @staticmethod
    def _unity_to_gltf(attrs, indices):
        pos = attrs.get('POSITION')
        if pos:
            for i in range(2, len(pos), 3):
                pos[i] = -pos[i]
        nrm = attrs.get('NORMAL')
        if nrm:
            for i in range(2, len(nrm), 3):
                nrm[i] = -nrm[i]
        if indices:
            for i in range(0, len(indices) - 2, 3):
                indices[i + 1], indices[i + 2] = indices[i + 2], indices[i + 1]
        for sem in list(attrs):
            if sem.startswith('TEXCOORD'):
                uv = attrs[sem]
                for i in range(1, len(uv), 2):
                    uv[i] = 1.0 - uv[i]

    @staticmethod
    def _material_key(pipe):
        try:
            return 'sh_%s' % str(pipe.GetShader(rd.ShaderStage.Pixel))
        except Exception:
            return 'sh_unknown'

    # ---------------------------------------------------------- export_material
    _BASE_COLOR = ('albedo0', 'albedo', 'maintex', 'basemap', 'basecolor', 'diffuse')
    _NORMAL = ('normalmap', 'bumpmap', 'normal')
    _SPLAT_LAYER = ('albedo0', 'albedo1', 'albedo2', 'albedo3')
    _SPLAT_WEIGHT = ('packed0', 'packed1', 'packed2', 'packed3', 'splat', 'control')

    def _export_material_impl(self, c, eid, out_dir):
        out_dir = out_dir or DEFAULT_OUT
        c.SetFrameEvent(eid, False)
        act = find_action(c, eid)
        if act is None:
            return {'error': 'no action at eid %d' % eid}
        pipe = c.GetPipelineState()
        key = self._material_key(pipe)
        tex_dir = os.path.join(out_dir, 'textures')
        os.makedirs(tex_dir, exist_ok=True)

        try:
            refl = pipe.GetShaderReflection(rd.ShaderStage.Pixel)
            ro = pipe.GetReadOnlyResources(rd.ShaderStage.Pixel)
        except Exception as ex:
            return {'error': 'reflection failed: %s' % ex}

        names = [r.name for r in refl.readOnlyResources]
        textures = {}
        saved = 0
        for i, ud in enumerate(ro):
            nm = names[i] if i < len(names) else ('res%d' % i)
            rid = ud.descriptor.resource
            if rid == rd.ResourceId.Null():
                continue
            if i < len(refl.readOnlyResources) and not _res_is_texture(
                    refl.readOnlyResources[i]):
                continue  # skip buffers
            lnm = nm.lower()
            if lnm.startswith('unity_') or lnm.startswith('hlslcc_'):
                continue  # skip global/system bindings
            fname = '%s_%s.png' % (nm.replace('/', '_'), str(rid).replace('ResourceId::', ''))
            fpath = os.path.join(tex_dir, fname)
            if not os.path.exists(fpath):
                ts = rd.TextureSave()
                ts.resourceId = rid
                ts.destType = rd.FileType.PNG
                try:
                    c.SaveTexture(ts, fpath)
                except Exception as ex:
                    return {'error': 'SaveTexture %s failed: %s' % (nm, ex)}
                saved += 1
            textures[nm] = os.path.relpath(fpath, out_dir)

        # material params from all PS constant blocks
        params = {}
        try:
            for bi, cb in enumerate(refl.constantBlocks):
                data = read_cb_bytes(c, pipe, rd.ShaderStage.Pixel, bi)
                if not data:
                    continue
                for v in cb.variables:
                    vals = read_var_floats(data, cb, v, 16)
                    if vals:
                        params[v.name] = [round(x, 6) for x in vals]
        except Exception:
            pass

        mat = self._heuristic_pbr(textures, params)
        mat['shader'] = key
        mat['textures'] = textures
        mat['params'] = params

        man = load_manifest(out_dir)
        man['materials'][key] = mat
        save_manifest(out_dir, man)
        dbg = {'ro': len(ro), 'names': len(names)}
        try:
            if len(ro):
                dbg['ro0_flds'] = ','.join(a for a in dir(ro[0])
                                           if not a.startswith('_'))[:200]
                dbg['ro0_rid'] = str(ro[0].descriptor.resource)
            if names:
                r0 = refl.readOnlyResources[0]
                dbg['refl0_flds'] = ','.join(a for a in dir(r0)
                                             if not a.startswith('_'))[:200]
                dbg['refl0_istex'] = _res_is_texture(r0)
        except Exception as ex:
            dbg['exc'] = str(ex)
        return {'material_key': key, 'textures': len(textures),
                'saved_now': saved, 'params': len(params), 'debug': dbg,
                'pbr': {k: v for k, v in mat.items()
                        if k in ('baseColorTexture', 'normalTexture',
                                 'metallic', 'roughness', 'alphaTest',
                                 'terrain_layers')}}

    def _heuristic_pbr(self, textures, params):
        mat = {}
        # names differ across engines in leading _, u_, _u_ prefixes and in
        # suffixes (Map/Tex/Texture): normalise then substring-match
        def norm(n):
            s = n.lower().lstrip('_')
            if s.startswith('u_'):
                s = s[2:]
            return s
        lname = {}
        for n in textures:
            lname.setdefault(norm(n), n)

        def find_tex(*frags, exclude=()):
            for frag in frags:
                for k, orig in lname.items():
                    if frag in k and not any(x in k for x in exclude):
                        return textures[orig]
            return None

        # terrain splat detection
        layers = [lname.get(s) for s in self._SPLAT_LAYER if lname.get(s)]
        terrain = len(layers) >= 2
        if terrain:
            mat['type'] = 'terrain_splat'
            mat['terrain_layers'] = [textures[n] for n in layers]
            weights = [lname.get(s) for s in self._SPLAT_WEIGHT if lname.get(s)]
            if weights:
                mat['terrain_weights'] = [textures[n] for n in weights]
        if not terrain:
            bc = find_tex(*self._BASE_COLOR,
                          exclude=('normal', 'params', 'lut', 'dfg'))
            if bc is None:
                # single-texture shaders (e.g. lightmap-only scenery): use the
                # only plausible colour-ish texture as base color
                bc = find_tex('', exclude=('normal', 'params', 'lut', 'dfg',
                                           'env', 'mask', 'occlusion',
                                           'emissive', 'shadow'))
            if bc is not None:
                mat['baseColorTexture'] = bc
            nm = find_tex(*self._NORMAL)
            if nm is not None:
                mat['normalTexture'] = nm
        def pnum(*frags):
            for nm, vals in params.items():
                ln = nm.lower()
                if ln.startswith('xhlslcc_unusedx'):
                    continue  # HLSLcc dead uniform
                if any(f in ln for f in frags) and vals:
                    return vals
            return None
        m = pnum('_metallic')
        mat['metallic'] = m[0] if m else 0.0
        g = pnum('_smoothness', '_glossiness')
        r = pnum('_roughness')
        if r:
            mat['roughness'] = max(0.0, min(1.0, r[0]))
        elif g:
            mat['roughness'] = max(0.0, min(1.0, 1.0 - g[0]))
        else:
            mat['roughness'] = 0.8
        col = pnum('_basecolor', '_color')
        if col and len(col) >= 3:
            mat['baseColorFactor'] = col[:4] if len(col) >= 4 else col[:3] + [1.0]
        cut = pnum('_cutoff')
        if cut and cut[0] > 0.001:
            mat['alphaTest'] = cut[0]
        return mat

    @staticmethod
    def _vsout_col_dump(c, vs_refl, eid):
        """Per-column stats of the VSOut PostVS stream (column forensics)."""
        try:
            fmt = c.GetPostVSData(eid, 0, rd.MeshDataStage.VSOut)
            data = c.GetBufferData(fmt.vertexResourceId, fmt.vertexByteOffset,
                                   fmt.vertexByteSize)
            if not data or fmt.vertexByteStride <= 0:
                return None
            data = bytes(data)
            stride = fmt.vertexByteStride
            nv = len(data) // stride
            sig = list(vs_refl.outputSignature)
            ncol = stride // 16
            cols = []
            for i in range(min(ncol, 6)):
                nm = sig[i].varName if i < len(sig) else '?'
                v0 = struct.unpack_from('<4f', data, i * 16)
                mn = [1e30] * 4
                mx = [-1e30] * 4
                for v in range(0, nv, max(1, nv // 200)):
                    f = struct.unpack_from('<4f', data, v * stride + i * 16)
                    for j in range(4):
                        mn[j] = min(mn[j], f[j])
                        mx[j] = max(mx[j], f[j])
                cols.append({'i': i, 'name': nm,
                             'v0': [round(x, 3) for x in v0],
                             'min': [round(x, 2) for x in mn],
                             'max': [round(x, 2) for x in mx]})
            return {'stride': stride, 'nv': nv, 'ncol': ncol,
                    'sig': [p.varName for p in sig], 'cols': cols}
        except Exception as ex:
            return {'exc': str(ex)}

    # ---------------------------------------------------------- verify
    def verify_transforms(self, marker_filter='', eid_min=0, eid_max=1 << 30,
                          min_indices=3, limit=1000, exclude_filter=''):
        self._guard()
        box = {}

        def cb(controller):
            try:
                box['r'] = self._verify_transforms_impl(
                    controller, marker_filter, eid_min, eid_max, min_indices,
                    limit, exclude_filter)
            except Exception:
                box['e'] = traceback.format_exc()
        self._invoke(cb)
        if 'e' in box:
            raise ValueError(box['e'])
        return box['r']

    @staticmethod
    def _bbox_stats(p):
        n = len(p) // 3
        mn = [min(p[i::3]) for i in range(3)]
        mx = [max(p[i::3]) for i in range(3)]
        cen = [sum(p[i::3]) / n for i in range(3)]
        return mn, mx, cen

    def _verify_transforms_impl(self, c, marker_filter, eid_min, eid_max,
                                min_indices, limit, exclude_filter):
        """Cross-check per draw: M*obj (vsin path, UBO matrix) against
        inv(VP)*clip (vsout path, actual GPU output). They only agree when
        BOTH matrices are correct."""
        draws = self._list_meshes_impl(c, marker_filter, eid_min, eid_max,
                                       min_indices, 0, limit,
                                       exclude_filter)['draws']
        rows = []
        for it in draws:
            eid = it['e']
            c.SetFrameEvent(eid, False)
            act = find_action(c, eid)
            if act is None or not (act.flags & rd.ActionFlags.Drawcall):
                continue
            pipe = c.GetPipelineState()
            vs_refl = pipe.GetShaderReflection(rd.ShaderStage.Vertex)
            # VS reads textures -> VAT/procedural vertices; M*obj is
            # meaningless by construction (export uses vsout for these)
            vs_tex = any(_res_is_texture(r) for r in vs_refl.readOnlyResources)
            warn = []
            try:
                vattrs, vind, vnv = self._extract_vsin(c, pipe, act, warn)
            except Exception:
                vattrs, vind, vnv = None, None, 0
            m, _ = find_matrix(c, pipe, rd.ShaderStage.Vertex,
                               ['objecttoworld', 'object2world'])
            try:
                wattrs, _, wnv = self._extract_vsout(c, pipe, vs_refl, act, warn)
            except Exception:
                wattrs, wnv = None, 0
            pa = (vattrs or {}).get('POSITION')
            pb = (wattrs or {}).get('POSITION')
            if not pa or not pb or m is None or self._positions_bad(vattrs):
                rows.append({'e': eid, 'verdict': 'skip',
                             'warn': [w[:80] for w in warn[:2]]})
                continue
            if vs_tex:
                rows.append({'e': eid, 'verdict': 'vat'})
                continue
            # compare only vertices actually referenced by the index buffer
            # (shared VBs contain unrelated verts that skew the bbox)
            nvv = len(pa) // 3
            if vind:
                sel = sorted(set(i2 for i2 in vind if 0 <= i2 < nvv))
            else:
                sel = list(range(nvv))
            wa = []
            for vi in sel:
                ox, oy, oz = pa[vi * 3], pa[vi * 3 + 1], pa[vi * 3 + 2]
                wa += [m[0] * ox + m[1] * oy + m[2] * oz + m[3],
                       m[4] * ox + m[5] * oy + m[6] * oz + m[7],
                       m[8] * ox + m[9] * oy + m[10] * oz + m[11]]
            amin, amax, acen = self._bbox_stats(wa)
            bmin, bmax, bcen = self._bbox_stats(pb)
            asz = [amax[i] - amin[i] for i in range(3)]
            bsz = [bmax[i] - bmin[i] for i in range(3)]
            dcen = max(abs(acen[i] - bcen[i]) for i in range(3))
            dsz = max(abs(asz[i] - bsz[i]) for i in range(3))
            scale = max(max(asz), 1e-3)
            rel = max(dcen, dsz) / scale
            row = {'e': eid,
                   'dcen': round(dcen, 4), 'dsz': round(dsz, 4),
                   'rel': round(rel, 5), 'size': round(scale, 2),
                   'nv_a': len(sel), 'nv_b': len(pb) // 3,
                   'abox': [round(v, 2) for v in amin + amax],
                   'bbox': [round(v, 2) for v in bmin + bmax],
                   'verdict': 'ok' if rel < 0.02 else 'MISMATCH'}
            if row['verdict'] == 'MISMATCH':
                row['cols'] = self._vsout_col_dump(c, vs_refl, eid)
            rows.append(row)
        ok = [r for r in rows if r.get('verdict') == 'ok']
        bad = [r for r in rows if r.get('verdict') == 'MISMATCH']
        skip = [r for r in rows if r.get('verdict') == 'skip']
        vat = [r for r in rows if r.get('verdict') == 'vat']
        rels = sorted(r['rel'] for r in ok)
        return {'total': len(rows), 'ok': len(ok), 'mismatch': len(bad),
                'skipped': len(skip), 'vat': len(vat),
                'rel_p50': rels[len(rels) // 2] if rels else None,
                'rel_max': rels[-1] if rels else None,
                'mismatch_rows': bad,
                'skipped_eids': [r['e'] for r in skip]}

    # ---------------------------------------------------------- frame image
    def save_frame_image(self, eid, out_path=None):
        self._guard()
        box = {}

        def cb(controller):
            try:
                box['r'] = self._save_frame_image_impl(controller, eid, out_path)
            except Exception:
                box['e'] = traceback.format_exc()
        self._invoke(cb)
        if 'e' in box:
            raise ValueError(box['e'])
        return box['r']

    def _save_frame_image_impl(self, c, eid, out_path):
        c.SetFrameEvent(eid, False)
        pipe = c.GetPipelineState()
        outs = pipe.GetOutputTargets()
        rid = outs[0].resource if outs else rd.ResourceId.Null()
        if rid == rd.ResourceId.Null():
            return {'error': 'no output target at eid %d' % eid}
        out_path = out_path or os.path.join(DEFAULT_OUT, 'frame_%d.png' % eid)
        os.makedirs(os.path.dirname(out_path), exist_ok=True)
        ts = rd.TextureSave()
        ts.resourceId = rid
        ts.destType = rd.FileType.PNG
        c.SaveTexture(ts, out_path)
        return {'eid': eid, 'rt': str(rid), 'file': out_path,
                'exists': os.path.exists(out_path)}

    # ---------------------------------------------------------- build_scene
    _BLENDER_CANDIDATES = [
        r'C:\Program Files\Blender Foundation\Blender 4.5\blender.exe',
        r'C:\Program Files\Blender Foundation\Blender 4.4\blender.exe',
        r'C:\Program Files\Blender Foundation\Blender 4.3\blender.exe',
        r'C:\Program Files\Blender Foundation\Blender 4.2\blender.exe',
        r'C:\Program Files\Blender Foundation\Blender 4.1\blender.exe',
        r'C:\Program Files\Blender Foundation\Blender 4.0\blender.exe',
        r'C:\Program Files\Blender Foundation\Blender 3.6\blender.exe',
    ]

    def _find_blender(self, override=None):
        if override and os.path.exists(override):
            return override
        env = os.environ.get('RENDERDOC_MCP_BLENDER')
        if env and os.path.exists(env):
            return env
        for p in self._BLENDER_CANDIDATES:
            if os.path.exists(p):
                return p
        try:
            base = r'C:\Program Files\Blender Foundation'
            for d in sorted(os.listdir(base), reverse=True):
                p = os.path.join(base, d, 'blender.exe')
                if os.path.exists(p):
                    return p
        except Exception:
            pass
        return None

    def build_scene(self, out_dir=None, blender_path=None, make_blend=True):
        out_dir = out_dir or DEFAULT_OUT
        man = load_manifest(out_dir)
        if not man['meshes']:
            return {'error': 'manifest empty; export meshes first'}
        script = os.path.join(out_dir, 'assemble_blender.py')
        with open(os.path.join(os.path.dirname(__file__), 'assemble_template.py'),
                  'r', encoding='utf-8') as f:
            tpl = f.read()
        with open(script, 'w', encoding='utf-8') as f:
            f.write(tpl)
        result = {'meshes': len(man['meshes']), 'materials': len(man['materials']),
                  'assemble_script': script}
        if not make_blend:
            return result
        bp = self._find_blender(blender_path)
        if bp is None:
            result['error'] = ('blender.exe not found; set blender_path or '
                               'RENDERDOC_MCP_BLENDER env')
            return result
        blend_out = os.path.join(out_dir, 'scene.blend')
        try:
            proc = subprocess.run(
                [bp, '-b', '--python', script, '--', out_dir, blend_out],
                stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                universal_newlines=True, timeout=1800)
            result['blender_rc'] = proc.returncode
            result['blend'] = blend_out if os.path.exists(blend_out) else None
            if result['blend'] is None:
                result['blender_tail'] = (proc.stdout or '')[-1500:] + (proc.stderr or '')[-500:]
        except Exception as ex:
            result['error'] = 'blender launch failed: %s' % ex
        return result

    # ---------------------------------------------------------- preview_scene
    def preview_scene(self, blend=None, png=None, hide='', camera=None,
                      blender_path=None):
        """Render a preview PNG of an assembled .blend via headless Blender.

        camera: optional dict {location:[x,y,z], forward:[x,y,z], vfov_deg,
        width, height} in Blender space; None = auto overview camera.
        Unity (x,y,z) -> Blender (x,z,y) for both positions and directions.
        """
        blend = blend or os.path.join(DEFAULT_OUT, 'scene.blend')
        if not os.path.exists(blend):
            return {'error': 'blend not found: %s' % blend}
        png = png or os.path.join(os.path.dirname(blend), 'preview.png')
        bp = self._find_blender(blender_path)
        if bp is None:
            return {'error': ('blender.exe not found; set blender_path or '
                              'RENDERDOC_MCP_BLENDER env')}
        script = os.path.join(os.path.dirname(__file__), 'preview_template.py')
        cam_json = json.dumps(camera) if camera else ''
        try:
            proc = subprocess.run(
                [bp, '-b', '--python', script, '--', blend, png, hide, cam_json],
                stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                universal_newlines=True, timeout=600)
            tail = ((proc.stdout or '') + (proc.stderr or ''))[-1200:]
            ok = proc.returncode == 0 and os.path.exists(png)
            result = {'blend': blend, 'png': png if ok else None,
                      'blender_rc': proc.returncode}
            if not ok:
                result['error'] = 'render failed'
                result['blender_tail'] = tail
            return result
        except Exception as ex:
            return {'error': 'blender launch failed: %s' % ex}
