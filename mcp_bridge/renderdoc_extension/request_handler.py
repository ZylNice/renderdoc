"""
Request Handler for RenderDoc MCP Bridge
Routes incoming requests to appropriate facade methods.
"""

import traceback


class RequestHandler:
    """Handles incoming MCP bridge requests"""

    def __init__(self, facade):
        self.facade = facade
        self._methods = {
            "ping": self._handle_ping,
            "get_capture_status": self._handle_get_capture_status,
            "get_draw_calls": self._handle_get_draw_calls,
            "get_frame_summary": self._handle_get_frame_summary,
            "find_draws_by_shader": self._handle_find_draws_by_shader,
            "find_draws_by_texture": self._handle_find_draws_by_texture,
            "find_draws_by_resource": self._handle_find_draws_by_resource,
            "get_draw_call_details": self._handle_get_draw_call_details,
            "get_action_timings": self._handle_get_action_timings,
            "get_shader_info": self._handle_get_shader_info,
            "get_shader_source": self._handle_get_shader_source,
            "apply_shader_replacement": self._handle_apply_shader_replacement,
            "remove_shader_replacement": self._handle_remove_shader_replacement,
            "get_buffer_contents": self._handle_get_buffer_contents,
            "get_texture_info": self._handle_get_texture_info,
            "get_texture_data": self._handle_get_texture_data,
            "get_pipeline_state": self._handle_get_pipeline_state,
            "list_captures": self._handle_list_captures,
            "open_capture": self._handle_open_capture,
            "list_meshes": self._handle_list_meshes,
            "export_mesh": self._handle_export_mesh,
            "export_material": self._handle_export_material,
            "export_frame": self._handle_export_frame,
            "build_scene": self._handle_build_scene,
            "preview_scene": self._handle_preview_scene,
            "save_frame_image": self._handle_save_frame_image,
            "verify_transforms": self._handle_verify_transforms,
        }

    def handle(self, request):
        """Handle a request and return response"""
        request_id = request.get("id")
        method = request.get("method")
        params = request.get("params", {})

        try:
            if method not in self._methods:
                return self._error_response(
                    request_id, -32601, "Method not found: %s" % method
                )

            result = self._methods[method](params)
            return {"id": request_id, "result": result}

        except ValueError as e:
            return self._error_response(request_id, -32602, str(e))
        except Exception as e:
            traceback.print_exc()
            return self._error_response(request_id, -32000, str(e))

    def _error_response(self, request_id, code, message):
        """Create an error response"""
        return {"id": request_id, "error": {"code": code, "message": message}}

    def _handle_ping(self, params):
        """Handle ping request"""
        return {"status": "ok", "message": "pong"}

    def _handle_get_capture_status(self, params):
        """Handle get_capture_status request"""
        return self.facade.get_capture_status()

    def _handle_get_draw_calls(self, params):
        """Handle get_draw_calls request"""
        include_children = params.get("include_children", True)
        marker_filter = params.get("marker_filter")
        exclude_markers = params.get("exclude_markers")
        event_id_min = params.get("event_id_min")
        event_id_max = params.get("event_id_max")
        only_actions = params.get("only_actions", False)
        flags_filter = params.get("flags_filter")
        return self.facade.get_draw_calls(
            include_children=include_children,
            marker_filter=marker_filter,
            exclude_markers=exclude_markers,
            event_id_min=event_id_min,
            event_id_max=event_id_max,
            only_actions=only_actions,
            flags_filter=flags_filter,
        )

    def _handle_get_frame_summary(self, params):
        """Handle get_frame_summary request"""
        return self.facade.get_frame_summary()

    def _handle_find_draws_by_shader(self, params):
        """Handle find_draws_by_shader request"""
        shader_name = params.get("shader_name")
        if shader_name is None:
            raise ValueError("shader_name is required")
        stage = params.get("stage")
        return self.facade.find_draws_by_shader(shader_name, stage)

    def _handle_find_draws_by_texture(self, params):
        """Handle find_draws_by_texture request"""
        texture_name = params.get("texture_name")
        if texture_name is None:
            raise ValueError("texture_name is required")
        return self.facade.find_draws_by_texture(texture_name)

    def _handle_find_draws_by_resource(self, params):
        """Handle find_draws_by_resource request"""
        resource_id = params.get("resource_id")
        if resource_id is None:
            raise ValueError("resource_id is required")
        return self.facade.find_draws_by_resource(resource_id)

    def _handle_get_draw_call_details(self, params):
        """Handle get_draw_call_details request"""
        event_id = params.get("event_id")
        if event_id is None:
            raise ValueError("event_id is required")
        return self.facade.get_draw_call_details(int(event_id))

    def _handle_get_action_timings(self, params):
        """Handle get_action_timings request"""
        event_ids = params.get("event_ids")
        marker_filter = params.get("marker_filter")
        exclude_markers = params.get("exclude_markers")
        return self.facade.get_action_timings(
            event_ids=event_ids,
            marker_filter=marker_filter,
            exclude_markers=exclude_markers,
        )

    def _handle_get_shader_info(self, params):
        """Handle get_shader_info request"""
        event_id = params.get("event_id")
        stage = params.get("stage")
        if event_id is None:
            raise ValueError("event_id is required")
        if stage is None:
            raise ValueError("stage is required")
        return self.facade.get_shader_info(int(event_id), stage)

    def _handle_get_shader_source(self, params):
        """Handle get_shader_source request"""
        event_id = params.get("event_id")
        stage = params.get("stage")
        if event_id is None:
            raise ValueError("event_id is required")
        if stage is None:
            raise ValueError("stage is required")
        return self.facade.get_shader_source(int(event_id), stage)

    def _handle_apply_shader_replacement(self, params):
        """Handle apply_shader_replacement request"""
        event_id = params.get("event_id")
        stage = params.get("stage")
        source = params.get("source")
        if event_id is None:
            raise ValueError("event_id is required")
        if stage is None:
            raise ValueError("stage is required")
        if source is None:
            raise ValueError("source is required")
        return self.facade.apply_shader_replacement(int(event_id), stage, source)

    def _handle_remove_shader_replacement(self, params):
        """Handle remove_shader_replacement request"""
        event_id = params.get("event_id")
        stage = params.get("stage")
        if event_id is None:
            raise ValueError("event_id is required")
        if stage is None:
            raise ValueError("stage is required")
        return self.facade.remove_shader_replacement(int(event_id), stage)

    def _handle_get_buffer_contents(self, params):
        """Handle get_buffer_contents request"""
        resource_id = params.get("resource_id")
        if resource_id is None:
            raise ValueError("resource_id is required")
        offset = params.get("offset", 0)
        length = params.get("length", 0)
        return self.facade.get_buffer_contents(resource_id, offset, length)

    def _handle_get_texture_info(self, params):
        """Handle get_texture_info request"""
        resource_id = params.get("resource_id")
        if resource_id is None:
            raise ValueError("resource_id is required")
        return self.facade.get_texture_info(resource_id)

    def _handle_get_texture_data(self, params):
        """Handle get_texture_data request"""
        resource_id = params.get("resource_id")
        if resource_id is None:
            raise ValueError("resource_id is required")
        mip = params.get("mip", 0)
        slice_idx = params.get("slice", 0)
        sample = params.get("sample", 0)
        depth_slice = params.get("depth_slice")  # None = full volume
        return self.facade.get_texture_data(resource_id, mip, slice_idx, sample, depth_slice)

    def _handle_get_pipeline_state(self, params):
        """Handle get_pipeline_state request"""
        event_id = params.get("event_id")
        if event_id is None:
            raise ValueError("event_id is required")
        return self.facade.get_pipeline_state(int(event_id))

    def _handle_list_captures(self, params):
        """Handle list_captures request"""
        directory = params.get("directory")
        if directory is None:
            raise ValueError("directory is required")
        return self.facade.list_captures(directory)

    def _handle_open_capture(self, params):
        """Handle open_capture request"""
        capture_path = params.get("capture_path")
        if capture_path is None:
            raise ValueError("capture_path is required")
        return self.facade.open_capture(capture_path)

    def _handle_list_meshes(self, params):
        """Handle list_meshes request"""
        return self.facade.list_meshes(
            params.get("marker_filter", ""),
            int(params.get("eid_min", 0)),
            int(params.get("eid_max", 1 << 30)),
            int(params.get("min_indices", 0)),
            int(params.get("offset", 0)),
            int(params.get("limit", 50)),
        )

    def _handle_export_mesh(self, params):
        """Handle export_mesh request"""
        event_id = params.get("event_id")
        if event_id is None:
            raise ValueError("event_id is required")
        return self.facade.export_mesh(
            int(event_id), params.get("out_dir"), params.get("stage", "auto")
        )

    def _handle_export_material(self, params):
        """Handle export_material request"""
        event_id = params.get("event_id")
        if event_id is None:
            raise ValueError("event_id is required")
        return self.facade.export_material(int(event_id), params.get("out_dir"))

    def _handle_export_frame(self, params):
        """Handle export_frame request"""
        return self.facade.export_frame(
            params.get("out_dir"),
            params.get("marker_filter", ""),
            int(params.get("eid_min", 0)),
            int(params.get("eid_max", 1 << 30)),
            int(params.get("min_indices", 3)),
            params.get("stage", "auto"),
            bool(params.get("materials", True)),
            int(params.get("limit", 10000)),
            bool(params.get("fresh", False)),
            params.get("exclude_filter", ""),
        )

    def _handle_build_scene(self, params):
        """Handle build_scene request"""
        return self.facade.build_scene(
            params.get("out_dir"),
            params.get("blender_path"),
            bool(params.get("make_blend", True)),
        )

    def _handle_preview_scene(self, params):
        """Handle preview_scene request"""
        return self.facade.preview_scene(
            params.get("blend"),
            params.get("png"),
            params.get("hide", ""),
            params.get("camera"),
            params.get("blender_path"),
        )

    def _handle_save_frame_image(self, params):
        """Handle save_frame_image request"""
        event_id = params.get("event_id")
        if event_id is None:
            raise ValueError("event_id is required")
        return self.facade.save_frame_image(int(event_id),
                                            params.get("out_path"))

    def _handle_verify_transforms(self, params):
        """Handle verify_transforms request"""
        return self.facade.verify_transforms(
            params.get("marker_filter", ""),
            int(params.get("eid_min", 0)),
            int(params.get("eid_max", 1 << 30)),
            int(params.get("min_indices", 3)),
            int(params.get("limit", 1000)),
            params.get("exclude_filter", ""),
        )
