# Analyze whether a GLES capture's shaders carry RelaxedPrecision (mediump/lowp -> fp16 on Adreno)
# using DisassembleShader output, weighted by GPU time of heavy draws.
import sys

sys.path.insert(0, r"C:\Project\Renderdoc\renderdoc\x64\Release\pymodules")

import renderdoc as rd

rd.InitialiseReplay(rd.GlobalEnvironment(), [])

path = r"D:\Renderdoc 截帧\王者万象棋\OpengGL\局内-有泛光.rdc"

cap = rd.OpenCaptureFile()
try:
    status = cap.OpenFile(path, "", None)
    if status != rd.ResultCode.Succeeded:
        print("OpenFile failed: %s" % status)
        sys.exit(1)

    status, controller = cap.OpenCapture(rd.ReplayOptions(), None)
    if status != rd.ResultCode.Succeeded:
        print("OpenCapture failed: %s" % status)
        sys.exit(1)
    print("capture opened, api=%s" % cap.DriverName(), flush=True)

    targets = controller.GetDisassemblyTargets(True)
    print("DISASM TARGETS:", flush=True)
    for t in targets:
        print("  - %s" % t, flush=True)

    # positive control: compile a mediump ES shader through RenderDoc's own glslang chain
    try:
        ctrl_src = ("#version 310 es\nprecision mediump float;\n"
                    "uniform vec4 u_col;\nlayout(location=0) out vec4 o_col;\n"
                    "void main(){ mediump vec3 c = u_col.rgb * 0.5; o_col = vec4(c, 1.0); }\n")
        encs = controller.GetTargetShaderEncodings()
        print("TARGET ENCODINGS: %s" % [str(e) for e in encs], flush=True)
        pair = controller.BuildTargetShader("main", rd.ShaderEncoding.GLSL,
                                            ctrl_src.encode("utf-8"),
                                            rd.ShaderCompileFlags(), rd.ShaderStage.Pixel)
        print("BuildTargetShader -> %s , %s" % (pair[0], pair[1][:200] if pair[1] else ""), flush=True)
        if pair[0] != rd.ResourceId.Null():
            ctrl_refl = controller.GetShader(rd.ResourceId.Null(), pair[0],
                                             rd.ShaderEntryPoint("main", rd.ShaderStage.Pixel))
            for t in targets:
                try:
                    txt = controller.DisassembleShader(rd.ResourceId.Null(), ctrl_refl, t)
                except Exception as e:
                    print("  ctrl target %s failed: %s" % (t, e))
                    continue
                print("  CONTROL target %s: len=%d RelaxedPrecision_hits=%d" % (
                    t, len(txt), txt.count("RelaxedPrecision")), flush=True)
            controller.RemoveReplacement(pair[0])
    except Exception as e:
        print("positive control failed: %s" % e, flush=True)

    # probe one known shader with each target, find which keeps precision decorations
    controller.SetFrameEvent(1540, False)
    pipe = controller.GetPipelineState()
    refl = pipe.GetShaderReflection(rd.ShaderStage.Pixel)
    best_target = None
    for t in targets:
        try:
            txt = controller.DisassembleShader(pipe.GetGraphicsPipelineObject(), refl, t)
        except Exception as e:
            print("target %s failed: %s" % (t, e))
            continue
        n_relaxed = txt.count("RelaxedPrecision")
        print("TARGET %s: len=%d RelaxedPrecision_hits=%d" % (t, len(txt), n_relaxed), flush=True)
        if n_relaxed and best_target is None:
            best_target = t
    if best_target is None:
        # no decorations anywhere visible; fall back to first target and treat absence as fp32-intent
        best_target = targets[0]
        print("NOTE: no RelaxedPrecision visible in ANY target; using %s" % best_target, flush=True)

    def relaxed_count(pipe, stage):
        refl = pipe.GetShaderReflection(stage)
        if refl is None:
            return None
        txt = controller.DisassembleShader(pipe.GetGraphicsPipelineObject(), refl, best_target)
        return txt.count("RelaxedPrecision")

    # GPU-time-weighted sample: heaviest pixel draws (timings fetched via MCP earlier)
    heavy_ms = {1101: 5.545, 1540: 5.234, 2300: 4.311, 574: 4.092, 7518: 3.844,
                6244: 3.773, 4448: 3.596, 4301: 2.406, 986: 2.016, 1160: 2.039,
                5070: 1.932, 47: 1.798, 618: 1.740, 1716: 1.565, 7063: 1.516,
                458: 1.451, 7388: 1.436, 953: 1.444, 409: 1.423, 97: 1.363,
                318: 1.391, 708: 1.339, 664: 1.337, 3: 1.324, 754: 1.331,
                1010: 1.310, 1122: 1.276, 2689: 1.278, 845: 1.237, 7750: 1.263,
                1500: 1.042}

    shader_relaxed = {}
    relaxed_time = 0.0
    fp32_time = 0.0
    unknown_time = 0.0

    for eid in sorted(heavy_ms):
        controller.SetFrameEvent(eid, False)
        pipe = controller.GetPipelineState()
        t = heavy_ms[eid]
        rid = str(pipe.GetShader(rd.ShaderStage.Pixel))
        if rid not in shader_relaxed:
            n = relaxed_count(pipe, rd.ShaderStage.Pixel)
            shader_relaxed[rid] = n if n is not None else -1
        n = shader_relaxed[rid]
        if n > 0:
            relaxed_time += t
            tag = "RELAXED"
        elif n == 0:
            fp32_time += t
            tag = "FP32"
        else:
            unknown_time += t
            tag = "?"
        print("E%-5d %6.2fms  ps=%s  %s  relaxed_decs=%s" % (eid, t, rid, tag, n), flush=True)

    print("\nUNIQUE PIXEL SHADERS: %d" % len(shader_relaxed))
    for rid, n in sorted(shader_relaxed.items()):
        print("  %s  relaxed_decs=%s" % (rid, n))
    print("\nWEIGHTED PIXEL TIME: relaxed(mediump/lowp)=%.2fms  fp32=%.2fms  unknown=%.2fms" % (
        relaxed_time, fp32_time, unknown_time))

    controller.Shutdown()
finally:
    cap.Shutdown()
    rd.ShutdownReplay()
