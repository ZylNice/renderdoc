"""Run inside RenderDoc's Python Shell with a capture loaded. Does not change replacements."""
import json
import os
import traceback

import renderdoc as rd


def snapshot(value, depth=0):
    if value is None or isinstance(value, (bool, int, float, str)):
        return value
    if isinstance(value, bytes):
        return {"byte_length": len(value)}
    if isinstance(value, rd.ResourceId):
        return str(value)
    if depth >= 5:
        return str(value)
    if isinstance(value, (list, tuple)):
        return [snapshot(item, depth + 1) for item in value[:64]]
    fields = {}
    for name in dir(value):
        if name.startswith("_") or name in ("this", "thisown"):
            continue
        try:
            item = getattr(value, name)
            if not callable(item):
                fields[name] = snapshot(item, depth + 1)
        except Exception as exc:
            fields[name] = {"error": str(exc)}
    return fields if fields else str(value)


def diagnose(ctx):
    if not ctx.IsCaptureLoaded():
        raise RuntimeError("Open the capture and apply the shader edit first.")
    restore_event = ctx.CurEvent()
    resources = list(ctx.GetResources())
    replacements = {}
    for resource in resources:
        if ctx.IsResourceReplaced(resource.resourceId):
            replacements[str(resource.resourceId)] = ctx.GetResourceReplacement(resource.resourceId)
    report = {
        "capture": ctx.GetCaptureFilename(),
        "ui_event": restore_event,
        "ui_replacements": {key: str(value) for key, value in replacements.items()},
        "events": [],
        "errors": [],
    }

    def inspect(controller):
        try:
            for event in (1674, 2130):
                controller.SetFrameEvent(event, True)
                pipe = controller.GetPipelineState()
                vk = controller.GetVulkanPipelineState()
                shader_id = pipe.GetShader(rd.ShaderStage.Pixel)
                reflection = pipe.GetShaderReflection(rd.ShaderStage.Pixel)
                pipeline_id = pipe.GetGraphicsPipelineObject()
                data = {
                    "event": event,
                    "public_pipeline_id": str(pipeline_id),
                    "public_fragment_id": str(shader_id),
                    "reflection_fragment_id": str(reflection.resourceId) if reflection else None,
                    "fragment_entry": pipe.GetShaderEntryPoint(rd.ShaderStage.Pixel),
                    "output_targets": snapshot(pipe.GetOutputTargets()),
                }
                report["events"].append(data)
                if reflection:
                    data["state_reflection_disassembly"] = controller.DisassembleShader(
                        pipeline_id, reflection, ""
                    )
                replacement = replacements.get(str(shader_id))
                if replacement is not None and replacement != rd.ResourceId.Null():
                    entries = controller.GetShaderEntryPoints(replacement)
                    data["replacement_entries"] = snapshot(entries)
                    for entry in entries:
                        if entry.stage == rd.ShaderStage.Pixel:
                            replaced_reflection = controller.GetShader(
                                pipeline_id, replacement, entry
                            )
                            if replaced_reflection:
                                data["replacement_disassembly"] = controller.DisassembleShader(
                                    rd.ResourceId.Null(), replaced_reflection, ""
                                )
                            break
                if vk:
                    for field in ("colorBlend", "depthStencil", "rasterizer", "multisample",
                                  "viewState", "currentPass", "conditionalRendering"):
                        data[field] = snapshot(getattr(vk, field, None))
        except Exception:
            report["errors"].append(traceback.format_exc())
        finally:
            try:
                controller.SetFrameEvent(restore_event, True)
            except Exception:
                report["errors"].append(traceback.format_exc())

    ctx.Replay().BlockInvoke(inspect)
    path = os.path.join("C:/Project/Renderdoc/renderdoc", "shader_replacement_diagnostic.json")
    with open(path, "w", encoding="utf-8") as output:
        json.dump(report, output, ensure_ascii=False, indent=2)
    print("Diagnostic saved: " + path)
    print("UI replacements: " + str(report["ui_replacements"]))
    for event in report["events"]:
        print("Event %s: public FS=%s, reflection FS=%s, replacement disassembly=%s" % (
            event["event"], event["public_fragment_id"], event["reflection_fragment_id"],
            "replacement_disassembly" in event,
        ))
    for error in report["errors"]:
        print(error)


if "pyrenderdoc" not in globals():
    raise RuntimeError("Run this script inside the RenderDoc Python Shell, not system Python.")
diagnose(pyrenderdoc)
