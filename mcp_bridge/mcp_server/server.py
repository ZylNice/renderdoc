"""
RenderDoc MCP Server
FastMCP 2.0 server providing access to RenderDoc capture data.
"""

from typing import Literal

from fastmcp import FastMCP

from .bridge.client import RenderDocBridge, RenderDocBridgeError
from .config import settings

# Initialize FastMCP server
mcp = FastMCP(
    name="Rendertest MCP Server",
)

# RenderDoc bridge client
bridge = RenderDocBridge(host=settings.renderdoc_host, port=settings.renderdoc_port)


@mcp.tool
def get_capture_status() -> dict:
    """
    Check if a capture is currently loaded in RenderDoc.
    Returns the capture status and API type if loaded.
    """
    return bridge.call("get_capture_status")


@mcp.tool
def get_draw_calls(
    include_children: bool = True,
    marker_filter: str | None = None,
    exclude_markers: list[str] | None = None,
    event_id_min: int | None = None,
    event_id_max: int | None = None,
    only_actions: bool = False,
    flags_filter: list[str] | None = None,
) -> dict:
    """
    Get the list of all draw calls and actions in the current capture.

    Args:
        include_children: Include child actions in the hierarchy (default: True)
        marker_filter: Only include actions under markers containing this string (partial match)
        exclude_markers: Exclude actions under markers containing these strings (list of partial matches)
        event_id_min: Only include actions with event_id >= this value
        event_id_max: Only include actions with event_id <= this value
        only_actions: If True, exclude marker actions (PushMarker/PopMarker/SetMarker)
        flags_filter: Only include actions with these flags (list of flag names, e.g. ["Drawcall", "Dispatch"])

    Returns a hierarchical tree of actions including markers, draw calls,
    dispatches, and other GPU events.
    """
    params: dict[str, object] = {"include_children": include_children}
    if marker_filter is not None:
        params["marker_filter"] = marker_filter
    if exclude_markers is not None:
        params["exclude_markers"] = exclude_markers
    if event_id_min is not None:
        params["event_id_min"] = event_id_min
    if event_id_max is not None:
        params["event_id_max"] = event_id_max
    if only_actions:
        params["only_actions"] = only_actions
    if flags_filter is not None:
        params["flags_filter"] = flags_filter
    return bridge.call("get_draw_calls", params)


@mcp.tool
def get_frame_summary() -> dict:
    """
    Get a summary of the current capture frame.

    Returns statistics about the frame including:
    - API type (D3D11, D3D12, Vulkan, etc.)
    - Total action count
    - Statistics: draw calls, dispatches, clears, copies, presents, markers
    - Top-level markers with event IDs and child counts
    - Resource counts: textures, buffers
    """
    return bridge.call("get_frame_summary")


@mcp.tool
def find_draws_by_shader(
    shader_name: str,
    stage: Literal["vertex", "hull", "domain", "geometry", "pixel", "compute"] | None = None,
) -> dict:
    """
    Find all draw calls using a shader with the given name (partial match).

    Args:
        shader_name: Partial name to search for in shader names or entry points
        stage: Optional shader stage to search (if not specified, searches all stages)

    Returns a list of matching draw calls with event IDs and match reasons.
    """
    params: dict[str, object] = {"shader_name": shader_name}
    if stage is not None:
        params["stage"] = stage
    return bridge.call("find_draws_by_shader", params)


@mcp.tool
def find_draws_by_texture(texture_name: str) -> dict:
    """
    Find all draw calls using a texture with the given name (partial match).

    Args:
        texture_name: Partial name to search for in texture resource names

    Returns a list of matching draw calls with event IDs and match reasons.
    Searches SRVs, UAVs, and render targets.
    """
    return bridge.call("find_draws_by_texture", {"texture_name": texture_name})


@mcp.tool
def find_draws_by_resource(resource_id: str) -> dict:
    """
    Find all draw calls using a specific resource ID (exact match).

    Args:
        resource_id: Resource ID to search for (e.g. "ResourceId::12345" or "12345")

    Returns a list of matching draw calls with event IDs and match reasons.
    Searches shaders, SRVs, UAVs, render targets, and depth targets.
    """
    return bridge.call("find_draws_by_resource", {"resource_id": resource_id})


@mcp.tool
def get_draw_call_details(event_id: int) -> dict:
    """
    Get detailed information about a specific draw call.

    Args:
        event_id: The event ID of the draw call to inspect

    Includes vertex/index counts, resource outputs, and other metadata.
    """
    return bridge.call("get_draw_call_details", {"event_id": event_id})


@mcp.tool
def get_action_timings(
    event_ids: list[int] | None = None,
    marker_filter: str | None = None,
    exclude_markers: list[str] | None = None,
) -> dict:
    """
    Get GPU timing information for actions (draw calls, dispatches, etc.).

    Args:
        event_ids: Optional list of specific event IDs to get timings for.
                   If not specified, returns timings for all actions.
        marker_filter: Only include actions under markers containing this string (partial match).
        exclude_markers: Exclude actions under markers containing these strings.

    Returns timing data including:
    - available: Whether GPU timing counters are supported
    - unit: Time unit (typically "seconds")
    - timings: List of {event_id, name, duration_seconds, duration_ms}
    - total_duration_ms: Sum of all durations
    - count: Number of timing entries

    Note: GPU timing counters may not be available on all hardware/drivers.
    """
    params: dict[str, object] = {}
    if event_ids is not None:
        params["event_ids"] = event_ids
    if marker_filter is not None:
        params["marker_filter"] = marker_filter
    if exclude_markers is not None:
        params["exclude_markers"] = exclude_markers
    return bridge.call("get_action_timings", params)


@mcp.tool
def get_shader_info(
    event_id: int,
    stage: Literal["vertex", "hull", "domain", "geometry", "pixel", "compute"],
) -> dict:
    """
    Get shader information for a specific stage at a given event.

    Args:
        event_id: The event ID to inspect the shader at
        stage: The shader stage (vertex, hull, domain, geometry, pixel, compute)

    Returns shader disassembly, constant buffer values, and resource bindings.
    """
    return bridge.call("get_shader_info", {"event_id": event_id, "stage": stage})


@mcp.tool
def get_shader_source(
    event_id: int,
    stage: Literal["vertex", "hull", "domain", "geometry", "pixel", "compute"],
) -> dict:
    """
    Get the editable source (decompiled GLSL) of the shader bound at an event.

    Args:
        event_id: The event ID to inspect
        stage: The shader stage (vertex, hull, domain, geometry, pixel, compute)

    Returns the shader resource ID, entry point, editable source text,
    and the source encodings accepted by BuildTargetShader.
    Use this before apply_shader_replacement to obtain the source to edit.
    """
    return bridge.call("get_shader_source", {"event_id": event_id, "stage": stage})


@mcp.tool
def apply_shader_replacement(
    event_id: int,
    stage: Literal["vertex", "hull", "domain", "geometry", "pixel", "compute"],
    source: str,
) -> dict:
    """
    Compile shader source and replace the shader bound at an event, then re-replay.

    Args:
        event_id: The event ID whose bound shader should be replaced
        stage: The shader stage (vertex, hull, domain, geometry, pixel, compute)
        source: The new shader source code (e.g. edited GLSL from get_shader_source)

    Compiles the source with BuildTargetShader, applies it via ReplaceResource,
    and forces a re-replay so the change is visible immediately in the Texture Viewer.
    The replacement affects ALL draws using this shader, and does not modify the .rdc file.
    Returns original/replacement shader IDs, or compile_errors on failure.
    """
    return bridge.call(
        "apply_shader_replacement",
        {"event_id": event_id, "stage": stage, "source": source},
    )


@mcp.tool
def remove_shader_replacement(
    event_id: int,
    stage: Literal["vertex", "hull", "domain", "geometry", "pixel", "compute"],
) -> dict:
    """
    Remove the shader replacement for the shader bound at an event, then re-replay.

    Args:
        event_id: The event ID whose bound shader replacement should be removed
        stage: The shader stage (vertex, hull, domain, geometry, pixel, compute)

    Restores the original shader and forces a re-replay.
    """
    return bridge.call(
        "remove_shader_replacement", {"event_id": event_id, "stage": stage}
    )


@mcp.tool
def get_buffer_contents(
    resource_id: str,
    offset: int = 0,
    length: int = 0,
) -> dict:
    """
    Read the contents of a buffer resource.

    Args:
        resource_id: The resource ID of the buffer to read
        offset: Byte offset to start reading from (default: 0)
        length: Number of bytes to read, 0 for entire buffer (default: 0)

    Returns buffer data as base64-encoded bytes along with metadata.
    """
    return bridge.call(
        "get_buffer_contents",
        {"resource_id": resource_id, "offset": offset, "length": length},
    )


@mcp.tool
def get_texture_info(resource_id: str) -> dict:
    """
    Get metadata about a texture resource.

    Args:
        resource_id: The resource ID of the texture

    Includes dimensions, format, mip levels, and other properties.
    """
    return bridge.call("get_texture_info", {"resource_id": resource_id})


@mcp.tool
def get_texture_data(
    resource_id: str,
    mip: int = 0,
    slice: int = 0,
    sample: int = 0,
    depth_slice: int | None = None,
) -> dict:
    """
    Read the pixel data of a texture resource.

    Args:
        resource_id: The resource ID of the texture to read
        mip: Mip level to retrieve (default: 0)
        slice: Array slice or cube face index (default: 0)
               For cube maps: 0=X+, 1=X-, 2=Y+, 3=Y-, 4=Z+, 5=Z-
        sample: MSAA sample index (default: 0)
        depth_slice: For 3D textures only, extract a specific depth slice (default: None = full volume)
                     When specified, returns only the 2D slice at that depth index

    Returns texture pixel data as base64-encoded bytes along with metadata
    including dimensions at the requested mip level and format information.
    """
    params = {"resource_id": resource_id, "mip": mip, "slice": slice, "sample": sample}
    if depth_slice is not None:
        params["depth_slice"] = depth_slice
    return bridge.call("get_texture_data", params)


@mcp.tool
def get_pipeline_state(event_id: int) -> dict:
    """
    Get the full graphics pipeline state at a specific event.

    Args:
        event_id: The event ID to get pipeline state at

    Returns detailed pipeline state including:
    - Bound shaders with entry points for each stage
    - Shader resources (SRVs): textures and buffers with dimensions, format, slot, name
    - UAVs (RWTextures/RWBuffers): resource details with dimensions and format
    - Samplers: addressing modes, filter settings, LOD parameters
    - Constant buffers: slot, size, variable count
    - Render targets and depth target
    - Viewports and input assembly state
    """
    return bridge.call("get_pipeline_state", {"event_id": event_id})


@mcp.tool
def list_captures(directory: str) -> dict:
    """
    List all RenderDoc capture files (.rdc) in the specified directory.

    Args:
        directory: The directory path to search for capture files

    Returns a list of capture files with their metadata including:
    - filename: The capture file name
    - path: Full path to the file
    - size_bytes: File size in bytes
    - modified_time: Last modified timestamp (ISO format)
    """
    return bridge.call("list_captures", {"directory": directory})


@mcp.tool
def open_capture(capture_path: str) -> dict:
    """
    Open a RenderDoc capture file (.rdc).

    Args:
        capture_path: Full path to the capture file to open

    Returns success status and information about the opened capture.
    Note: This will close any currently open capture.
    """
    return bridge.call("open_capture", {"capture_path": capture_path})


@mcp.tool
def list_meshes(marker_filter: str = "", eid_min: int = 0, eid_max: int = 0,
                min_indices: int = 0, offset: int = 0, limit: int = 50) -> dict:
    """
    List exportable draw calls in the capture (paginated), for frame export.

    Args:
        marker_filter: Only draws under a marker whose path contains this
            substring (e.g. "DrawOpaque"). Empty = whole frame.
        eid_min: Minimum event id (inclusive)
        eid_max: Maximum event id (inclusive). 0 = no limit
        min_indices: Skip draws with fewer indices (filters tiny UI draws)
        offset: Pagination offset
        limit: Page size (max 200)

    Returns total count and a page of draws: event id, name, index count,
    marker path.
    """
    return bridge.call("list_meshes", {
        "marker_filter": marker_filter,
        "eid_min": eid_min,
        "eid_max": eid_max if eid_max > 0 else (1 << 30),
        "min_indices": min_indices,
        "offset": offset,
        "limit": min(limit, 200),
    })


@mcp.tool
def export_mesh(event_id: int, out_dir: str = "", stage: str = "auto") -> dict:
    """
    Export the mesh of one draw call to a .glb file and register it in the
    frame export manifest.

    Args:
        event_id: The event ID of the draw call
        out_dir: Export root directory (empty = default
            ~/Documents/rd_frame_export). Shared manifest.json lives here.
        stage: "auto" (detect skinning), "vsin" (model space, static meshes)
            or "vsout" (post-vertex-shader world space, for GPU-skinned draws)

    Returns glb path, vertex/triangle counts, detected attributes, material
    key, skinned flag and warnings.
    """
    return bridge.call("export_mesh", {
        "event_id": event_id, "out_dir": out_dir or None, "stage": stage,
    })


@mcp.tool
def export_material(event_id: int, out_dir: str = "") -> dict:
    """
    Export the material bound at a draw call: all pixel-shader textures are
    saved as PNG and constant-buffer parameters are dumped, then heuristically
    mapped to PBR (base color / normal / metallic / roughness / alpha test).
    Multi-layer terrain splat materials (albedo0-3 + weight maps) are
    detected and exported with all layers.

    Args:
        event_id: The event ID of the draw call
        out_dir: Export root directory (empty = default)

    Returns material key, texture count and the PBR mapping summary.
    """
    return bridge.call("export_material", {
        "event_id": event_id, "out_dir": out_dir or None,
    })


@mcp.tool
def export_frame(out_dir: str = "", marker_filter: str = "",
                 eid_min: int = 0, eid_max: int = 0,
                 min_indices: int = 3, stage: str = "auto",
                 materials: bool = True, limit: int = 10000,
                 fresh: bool = False, exclude_filter: str = "") -> dict:
    """
    Batch export meshes (+ unique materials) for a set of draw calls into
    out_dir, updating the shared manifest.json. Incremental by default: pass
    fresh=True to wipe the manifest and re-export from scratch.

    Args:
        out_dir: Export root directory (empty = default)
        marker_filter: Only draws under a marker whose path contains this
            substring (e.g. "DrawOpaque"). Empty = whole frame.
        eid_min: Minimum event id (inclusive)
        eid_max: Maximum event id (inclusive). 0 = no limit
        min_indices: Skip draws with fewer indices (filters tiny UI draws)
        stage: "auto" | "vsin" | "vsout" (see export_mesh)
        materials: Also export the material of each unique pixel shader
        limit: Max number of draws to process
        fresh: Delete manifest.json first (clean re-export)
        exclude_filter: Comma-separated substrings; draws whose marker path
            contains any of them are skipped (e.g. "Shadow,Depth")

    Returns per-run counts, failure list, and total manifest size.
    """
    return bridge.call("export_frame", {
        "out_dir": out_dir or None,
        "marker_filter": marker_filter,
        "eid_min": eid_min,
        "eid_max": eid_max if eid_max > 0 else (1 << 30),
        "min_indices": min_indices,
        "stage": stage,
        "materials": materials,
        "limit": limit,
        "fresh": fresh,
        "exclude_filter": exclude_filter,
    })


@mcp.tool
def verify_transforms(marker_filter: str = "", eid_min: int = 0,
                      eid_max: int = 0, min_indices: int = 3,
                      limit: int = 1000, exclude_filter: str = "") -> dict:
    """
    Cross-check the exported transform path per draw call: M*obj (vsin path,
    UBO matrix) against inv(VP)*clip (vsout path, actual GPU output). They
    only agree when BOTH matrices are correct. Run this after export_frame
    to catch SRP-Batcher UBO misreads before assembling a scene.

    Args:
        marker_filter: Only draws under a marker containing this substring
        eid_min: Minimum event id (inclusive)
        eid_max: Maximum event id (inclusive). 0 = no limit
        min_indices: Skip draws with fewer indices
        limit: Max number of draws to check
        exclude_filter: Comma-separated substrings to skip

    Returns ok/mismatch/skipped counts, rel error p50/max, and mismatch rows
    (with per-column VSOut forensics).
    """
    return bridge.call("verify_transforms", {
        "marker_filter": marker_filter,
        "eid_min": eid_min,
        "eid_max": eid_max if eid_max > 0 else (1 << 30),
        "min_indices": min_indices,
        "limit": limit,
        "exclude_filter": exclude_filter,
    })


@mcp.tool
def build_scene(out_dir: str = "", blender_path: str = "",
                make_blend: bool = True) -> dict:
    """
    Assemble all exported meshes/materials (manifest in out_dir) into a
    Blender scene: imports every .glb, restores world transforms, builds
    Principled BSDF materials (incl. terrain splat node groups), then saves
    scene.blend. Requires Blender 3.6+ installed.

    Args:
        out_dir: Export root directory (empty = default)
        blender_path: Path to blender.exe (empty = auto-detect common install
            locations or RENDERDOC_MCP_BLENDER env var)
        make_blend: If false, only (re)generate the assemble script without
            launching Blender

    Returns mesh/material counts, blender exit code and .blend path.
    """
    return bridge.call("build_scene", {
        "out_dir": out_dir or None,
        "blender_path": blender_path or None,
        "make_blend": make_blend,
    })


@mcp.tool
def preview_scene(blend: str = "", png: str = "", hide: str = "",
                  camera: dict | None = None,
                  blender_path: str = "") -> dict:
    """
    Render a preview PNG of an assembled .blend via headless Blender
    (Eevee + sun light). Use this to visually validate a rebuilt scene
    against save_frame_image ground truth.

    Args:
        blend: .blend file (empty = default scene.blend in the export dir)
        png: Output PNG path (empty = preview.png next to the blend)
        hide: Comma-separated object names or event ids to hide from the
            render (e.g. "m_1227,m_415" or "1227,415")
        camera: Optional explicit camera in Blender space, e.g.
            {"location": [x,y,z], "forward": [x,y,z], "vfov_deg": 52.3,
             "width": 610, "height": 1356}
            Unity (x,y,z) maps to Blender (x,z,y) for positions/directions.
            Omit for an automatic overview camera framing the whole scene.
        blender_path: Path to blender.exe (empty = auto-detect)

    Returns the png path on success, or blender_tail output on failure.
    """
    return bridge.call("preview_scene", {
        "blend": blend or None,
        "png": png or None,
        "hide": hide,
        "camera": camera,
        "blender_path": blender_path or None,
    })


@mcp.tool
def save_frame_image(event_id: int, out_path: str = "") -> dict:
    """
    Save the current output target (RT0) at an event to a PNG. Use at the
    final present/composite event to capture ground truth for visual
    comparison against a rebuilt scene.

    Args:
        event_id: The event ID whose output target should be saved
        out_path: Destination PNG (empty = frame_<eid>.png in the export dir)

    Returns the file path and the RT resource id.
    """
    return bridge.call("save_frame_image", {
        "event_id": event_id, "out_path": out_path or None,
    })


def main():
    """Run the MCP server"""
    mcp.run()


if __name__ == "__main__":
    main()
