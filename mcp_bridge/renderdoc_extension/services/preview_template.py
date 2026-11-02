"""Render a preview PNG of an assembled scene.blend (headless Blender).

Run: blender -b --python preview_blender.py -- <blend> <png> [hide_csv] [cam_json]

  blend     : input .blend file
  png       : output PNG path
  hide_csv  : optional comma-separated object names (or eids; "m_" prefix is
              added automatically for pure numbers) to hide from the render
  cam_json  : optional JSON camera spec in Blender space:
              {"location": [x,y,z], "forward": [x,y,z], "vfov_deg": 52.3,
               "width": 610, "height": 1356}
              Omit (or pass "") for an automatic overview camera that frames
              the whole scene (oversized outliers are hidden).
  Trailing args are order-tolerant: any arg starting with "{" is treated as
  cam_json, anything else as hide_csv (some shells drop empty-string args).

Coordinate note: meshes exported by this extension land Unity->glTF->Blender,
so Unity (x,y,z) maps to Blender (x,z,y) for both positions and directions.
"""
import json
import math
import statistics
import sys

import bpy
from mathutils import Vector

argv = sys.argv[sys.argv.index('--') + 1:]
blend_in, png_out = argv[0], argv[1]
hide_csv, cam = '', None
for a in argv[2:]:
    a = a.strip().strip('"')
    if not a:
        continue
    if a.startswith('{'):
        cam = json.loads(a)
    elif not hide_csv:
        hide_csv = a

bpy.ops.wm.open_mainfile(filepath=blend_in)

for nm in (s for s in hide_csv.split(',') if s):
    ob = bpy.data.objects.get(nm) or bpy.data.objects.get('m_' + nm)
    if ob is not None:
        ob.hide_render = True

cam_data = bpy.data.cameras.new('PreviewCam')
cam_ob = bpy.data.objects.new('PreviewCam', cam_data)
bpy.context.scene.collection.objects.link(cam_ob)

if cam and cam.get('location') and cam.get('forward'):
    cam_ob.location = Vector(cam['location'])
    cam_ob.rotation_euler = Vector(cam['forward']).to_track_quat('-Z', 'Y').to_euler()
    cam_data.sensor_fit = 'VERTICAL'
    cam_data.angle = math.radians(float(cam.get('vfov_deg', 60.0)))
    cam_data.clip_start = float(cam.get('clip_start', 0.1))
    cam_data.clip_end = float(cam.get('clip_end', 2000.0))
    res_x, res_y = int(cam.get('width', 1280)), int(cam.get('height', 720))
else:
    objs = []
    for ob in bpy.data.objects:
        if ob.type != 'MESH' or ob.hide_render:
            continue
        pts = [ob.matrix_world @ Vector(c) for c in ob.bound_box]
        mn = Vector((min(p[i] for p in pts) for i in range(3)))
        mx = Vector((max(p[i] for p in pts) for i in range(3)))
        objs.append((ob, mn, mx))
    diags = sorted((mx - mn).length for _, mn, mx in objs)
    med = statistics.median(diags) if diags else 1.0
    limit = max(med * 20, 50.0)
    hidden = 0
    for ob, mn, mx in objs:
        if (mx - mn).length > limit:
            ob.hide_render = True
            hidden += 1
    objs = [(ob, mn, mx) for ob, mn, mx in objs if not ob.hide_render]
    if not objs:
        print('[preview] no visible mesh objects')
        sys.exit(1)
    mins = Vector((min(mn[i] for _, mn, _ in objs) for i in range(3)))
    maxs = Vector((max(mx[i] for _, _, mx in objs) for i in range(3)))
    print('[preview] mesh objects: %d (hidden oversize: %d)' % (len(objs), hidden))
    print('[preview] bounds min %s max %s' % (tuple(round(v, 2) for v in mins),
                                              tuple(round(v, 2) for v in maxs)))
    center = (mins + maxs) / 2
    size = max((maxs - mins).length, 1.0)
    cam_data.clip_start = size / 1000
    cam_data.clip_end = size * 10
    offset = Vector((0.35, -0.55, 0.75))
    offset.normalize()
    cam_ob.location = center + offset * size * 0.9
    direction = center - cam_ob.location
    cam_ob.rotation_euler = direction.to_track_quat('-Z', 'Y').to_euler()
    res_x, res_y = 1280, 720

bpy.context.scene.camera = cam_ob

sun_data = bpy.data.lights.new('Sun', type='SUN')
sun_data.energy = 4.0
sun = bpy.data.objects.new('Sun', sun_data)
bpy.context.scene.collection.objects.link(sun)
sun.rotation_euler = (math.radians(50), 0, math.radians(30))

sc = bpy.context.scene
for eng in ('BLENDER_EEVEE_NEXT', 'BLENDER_EEVEE'):
    try:
        sc.render.engine = eng
        break
    except TypeError:
        continue
w = sc.world or bpy.data.worlds.new('W')
sc.world = w
w.use_nodes = True
bg = w.node_tree.nodes.get('Background')
if bg:
    bg.inputs[0].default_value = (0.35, 0.45, 0.6, 1.0)
    bg.inputs[1].default_value = 1.0

sc.render.resolution_x = res_x
sc.render.resolution_y = res_y
sc.render.resolution_percentage = 100
sc.render.filepath = png_out

bpy.ops.render.render(write_still=True)
print('[preview] wrote %s' % png_out)
