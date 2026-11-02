"""Assemble exported frame (glb meshes + material defs) into a .blend.

Run: blender -b --python assemble_blender.py -- <out_dir> <blend_out>
Reads {out_dir}/manifest.json.
"""
import json
import math
import os
import sys
import traceback

import bpy
from mathutils import Matrix

# glTF (+Y up) -> Blender (+Z up). The importer already applies this rotation
# to vertex data, so a glTF-space node transform must be CONJUGATED, not
# left-multiplied (left-multiplying double-rotates every mesh).
GLTF_TO_BLENDER = Matrix.Rotation(math.radians(90.0), 4, 'X')
GLTF_TO_BLENDER_INV = GLTF_TO_BLENDER.inverted()

LOG = None


def log(m):
    print('[assemble] %s' % m)
    if LOG:
        LOG.write(str(m) + '\n')
        LOG.flush()


def rows_to_matrix(rows):
    m = Matrix((tuple(rows[0:4]), tuple(rows[4:8]),
                tuple(rows[8:12]), tuple(rows[12:16])))
    return m


def load_image(out_dir, rel, non_color=False):
    p = os.path.normpath(os.path.join(out_dir, rel))
    if not os.path.exists(p):
        return None
    img = bpy.data.images.get(os.path.basename(p))
    if img is None:
        try:
            img = bpy.data.images.load(p)
        except Exception as ex:
            log('image load failed %s: %s' % (p, ex))
            return None
    if non_color:
        img.colorspace_settings.name = 'Non-Color'
    return img


def build_material(out_dir, key, mat):
    m = bpy.data.materials.get(key)
    if m is not None:
        return m
    m = bpy.data.materials.new(key)
    m.use_nodes = True
    nt = m.node_tree
    nt.nodes.clear()
    out = nt.nodes.new('ShaderNodeOutputMaterial')
    bsdf = nt.nodes.new('ShaderNodeBsdfPrincipled')
    out.location = (400, 0)
    bsdf.location = (100, 0)
    nt.links.new(bsdf.outputs['BSDF'], out.inputs['Surface'])
    bsdf.inputs['Metallic'].default_value = float(mat.get('metallic', 0.0))
    bsdf.inputs['Roughness'].default_value = float(mat.get('roughness', 0.8))

    if mat.get('type') == 'terrain_splat' and mat.get('terrain_layers'):
        build_terrain(nt, bsdf, out_dir, mat)
    else:
        bc = mat.get('baseColorTexture')
        img = load_image(out_dir, bc) if bc else None
        if img is not None:
            tex = nt.nodes.new('ShaderNodeTexImage')
            tex.image = img
            tex.location = (-400, 100)
            if 'lightmap' in bc.lower():
                # lightmap UVs are packed in .zw of the texcoord varying and
                # exported as TEXCOORD_1 (importer names it 'UVMap.001')
                uv = nt.nodes.new('ShaderNodeUVMap')
                uv.uv_map = 'UVMap.001'
                uv.location = (-650, 100)
                nt.links.new(uv.outputs['UV'], tex.inputs['Vector'])
            nt.links.new(tex.outputs['Color'], bsdf.inputs['Base Color'])
            if mat.get('alphaTest'):
                nt.links.new(tex.outputs['Alpha'], bsdf.inputs['Alpha'])
        elif mat.get('baseColorFactor'):
            bsdf.inputs['Base Color'].default_value = mat['baseColorFactor'][:4]
        else:
            # vertex-color-only shaders (FS: out = COLOR0)
            vc = nt.nodes.new('ShaderNodeVertexColor')
            vc.location = (-400, 100)
            nt.links.new(vc.outputs['Color'], bsdf.inputs['Base Color'])
        nm = mat.get('normalTexture')
        nimg = load_image(out_dir, nm, non_color=True) if nm else None
        if nimg is not None:
            tex = nt.nodes.new('ShaderNodeTexImage')
            tex.image = nimg
            tex.location = (-400, -300)
            nmap = nt.nodes.new('ShaderNodeNormalMap')
            nmap.location = (-100, -300)
            nt.links.new(tex.outputs['Color'], nmap.inputs['Color'])
            nt.links.new(nmap.outputs['Normal'], bsdf.inputs['Normal'])

    if mat.get('alphaTest'):
        m.blend_method = 'CLIP'
        try:
            m.alpha_threshold = float(mat['alphaTest'])
        except Exception:
            pass
    return m


def build_terrain(nt, bsdf, out_dir, mat):
    """4-layer splat: layer0..3 mixed by weight map RGBA (or evenly)."""
    layers = mat['terrain_layers'][:4]
    weights = mat.get('terrain_weights') or []
    wimg = load_image(out_dir, weights[0], non_color=True) if weights else None
    wtex = None
    if wimg is not None:
        wtex = nt.nodes.new('ShaderNodeTexImage')
        wtex.image = wimg
        wtex.location = (-900, -500)
    prev = None  # output socket chain
    # Blender 4.x renamed SeparateRGB -> SeparateColor (sockets renamed too)
    v4 = bpy.app.version >= (4, 0, 0)
    sep_type = 'ShaderNodeSeparateColor' if v4 else 'ShaderNodeSeparateRGB'
    sep_in = 'Color' if v4 else 'Image'
    ch_names = ['Red', 'Green', 'Blue'] if v4 else ['R', 'G', 'B']
    for i, rel in enumerate(layers):
        img = load_image(out_dir, rel)
        if img is None:
            continue
        tex = nt.nodes.new('ShaderNodeTexImage')
        tex.image = img
        tex.location = (-900, 300 - i * 250)
        if prev is None:
            prev = tex.outputs['Color']
            continue
        mix = nt.nodes.new('ShaderNodeMixRGB')
        mix.blend_type = 'MIX'
        mix.location = (-400, 300 - i * 250)
        if wtex is not None and i - 1 < len(ch_names):
            sep = nt.nodes.get('splat_sep')
            if sep is None:
                sep = nt.nodes.new(sep_type)
                sep.name = 'splat_sep'
                sep.location = (-650, -500)
                nt.links.new(wtex.outputs['Color'], sep.inputs[sep_in])
            ch = ch_names[i - 1]
            try:
                nt.links.new(sep.outputs[ch], mix.inputs['Fac'])
            except Exception:
                pass
        nt.links.new(prev, mix.inputs[1])
        nt.links.new(tex.outputs['Color'], mix.inputs[2])
        prev = mix.outputs['Color']
    if prev is not None:
        nt.links.new(prev, bsdf.inputs['Base Color'])


def main():
    global LOG
    argv = sys.argv[sys.argv.index('--') + 1:]
    out_dir, blend_out = argv[0], argv[1]
    LOG = open(os.path.join(out_dir, 'assemble.log'), 'w', encoding='utf-8')

    with open(os.path.join(out_dir, 'manifest.json'), 'r', encoding='utf-8') as f:
        man = json.load(f)

    bpy.ops.wm.read_factory_settings(use_empty=True)

    mats = {}
    for key, mat in (man.get('materials') or {}).items():
        try:
            mats[key] = build_material(out_dir, key, mat)
        except Exception:
            log('material %s failed:\n%s' % (key, traceback.format_exc()))

    ok, fail = 0, 0
    for eid, entry in man.get('meshes', {}).items():
        try:
            glb = os.path.join(out_dir, entry['file'])
            if not os.path.exists(glb):
                log('missing %s' % glb)
                fail += 1
                continue
            before = set(bpy.data.objects)
            bpy.ops.import_scene.gltf(filepath=glb)
            new_objs = [o for o in bpy.data.objects if o not in before]
            m = rows_to_matrix(entry.get('matrix') or
                               [1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1])
            world = GLTF_TO_BLENDER @ m @ GLTF_TO_BLENDER_INV
            mat = mats.get(entry.get('material_key'))
            for o in new_objs:
                if o.type == 'MESH':
                    o.matrix_world = world
                    o.name = entry.get('name') or o.name
                    if mat is not None:
                        if len(o.data.materials) == 0:
                            o.data.materials.append(mat)
                        else:
                            o.data.materials[0] = mat
            ok += 1
        except Exception:
            log('mesh %s failed:\n%s' % (eid, traceback.format_exc()))
            fail += 1

    bpy.ops.wm.save_as_mainfile(filepath=blend_out)
    log('DONE ok=%d fail=%d -> %s' % (ok, fail, blend_out))
    LOG.close()


main()
