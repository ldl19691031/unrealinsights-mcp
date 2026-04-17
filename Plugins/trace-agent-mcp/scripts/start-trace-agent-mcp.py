#!/usr/bin/env python
from __future__ import annotations

import os
import runpy
from pathlib import Path


SCRIPT_DIR = Path(__file__).resolve().parent
PLUGIN_ROOT = SCRIPT_DIR.parent
REPO_ROOT = PLUGIN_ROOT.parent.parent
SERVER_SCRIPT = REPO_ROOT / "Programs" / "TraceAgentMcp" / "trace_agent_mcp.py"
CLI_PATH = REPO_ROOT / "Binaries" / "Win64" / "TraceAgentCli.exe"
DEFAULT_TRACE = REPO_ROOT / "Trace" / "20260416_135353.utrace"


def main() -> int:
    if not SERVER_SCRIPT.exists():
        raise FileNotFoundError(f"TraceAgentMcp server script not found: {SERVER_SCRIPT}")
    if not CLI_PATH.exists():
        raise FileNotFoundError(f"TraceAgentCli executable not found: {CLI_PATH}")

    os.environ.setdefault("TRACE_AGENT_CLI", str(CLI_PATH))
    if DEFAULT_TRACE.exists():
        os.environ.setdefault("TRACE_AGENT_DEFAULT_TRACE", str(DEFAULT_TRACE))

    runpy.run_path(str(SERVER_SCRIPT), run_name="__main__")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
