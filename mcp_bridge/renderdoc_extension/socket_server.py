"""
File-based IPC Server for RenderDoc MCP Bridge
Uses file polling since RenderDoc's Python doesn't have socket/QtNetwork modules.

This version uses a plain threading-based polling loop instead of Qt
(QObject/QTimer), so it also works in builds where PySide2 is not available.
"""

import json
import os
import tempfile
import threading
import time
import traceback


# IPC directory
IPC_DIR = os.path.join(tempfile.gettempdir(), "renderdoc_mcp")
REQUEST_FILE = os.path.join(IPC_DIR, "request.json")
RESPONSE_FILE = os.path.join(IPC_DIR, "response.json")
LOCK_FILE = os.path.join(IPC_DIR, "lock")


class MCPBridgeServer(object):
    """File-based IPC server for MCP bridge communication"""

    def __init__(self, host, port, handler, parent=None):
        # host/port/parent are kept for API compatibility but not used
        self.handler = handler
        self._thread = None
        self._running = False
        self._poll_interval = 0.1  # seconds

        # Create IPC directory
        if not os.path.exists(IPC_DIR):
            os.makedirs(IPC_DIR)

    def start(self):
        """Start the server with polling thread"""
        self._running = True

        # Clean up old files
        self._cleanup_files()

        # Start polling thread (check every 100ms)
        self._thread = threading.Thread(target=self._poll_loop, name="MCPBridgeServer")
        self._thread.daemon = True
        self._thread.start()

        print("[MCP Bridge] File-based IPC server started (threading mode)")
        print("[MCP Bridge] IPC directory: %s" % IPC_DIR)
        return True

    def stop(self):
        """Stop the server"""
        self._running = False
        if self._thread:
            self._thread.join(timeout=2.0)
            self._thread = None
        self._cleanup_files()
        print("[MCP Bridge] Server stopped")

    def is_running(self):
        """Check if server is running"""
        return self._running

    def _cleanup_files(self):
        """Remove IPC files"""
        for f in [REQUEST_FILE, RESPONSE_FILE, LOCK_FILE]:
            try:
                if os.path.exists(f):
                    os.remove(f)
            except Exception:
                pass

    def _poll_loop(self):
        """Polling thread main loop"""
        while self._running:
            try:
                self._poll_request()
            except Exception:
                traceback.print_exc()
            time.sleep(self._poll_interval)

    def _poll_request(self):
        """Check for incoming request"""
        if not self._running:
            return

        # Check if request file exists
        if not os.path.exists(REQUEST_FILE):
            return

        # Check if lock file exists (client is still writing)
        if os.path.exists(LOCK_FILE):
            return

        try:
            # Read request
            with open(REQUEST_FILE, "r", encoding="utf-8") as f:
                request = json.load(f)

            # Remove request file
            os.remove(REQUEST_FILE)

            # Process request
            try:
                response = self.handler.handle(request)
            except Exception as e:
                traceback.print_exc()
                response = {
                    "id": request.get("id"),
                    "error": {"code": -32603, "message": str(e)}
                }

            # Write response
            with open(RESPONSE_FILE, "w", encoding="utf-8") as f:
                json.dump(response, f)

        except Exception as e:
            print("[MCP Bridge] Error processing request: %s" % str(e))
            traceback.print_exc()
