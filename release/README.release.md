# unrealinsights-mcp release bundle

This release bundle is intended for machines that only need to run the MCP server.

It includes:

- `Binaries\Win64\TraceAgentCli.exe`
- `Programs\TraceAgentMcp\trace_agent_mcp.py`
- `Plugins\trace-agent-mcp\...`
- `.mcp.json`

## Requirements

- Python on `PATH`
- Claude Code

## Using With Claude Code

### User-scoped server

Example:

```powershell
claude mcp add unrealinsights-mcp --scope user -- `
  python D:\Tools\unrealinsights-mcp-release\Plugins\trace-agent-mcp\scripts\start-trace-agent-mcp.py
```

With a default trace:

```powershell
claude mcp add unrealinsights-mcp --scope user `
  --env TRACE_AGENT_DEFAULT_TRACE=D:\Traces\sample.utrace -- `
  python D:\Tools\unrealinsights-mcp-release\Plugins\trace-agent-mcp\scripts\start-trace-agent-mcp.py
```

### Project-scoped server

If you open this release folder itself as a Claude Code project, the bundled `.mcp.json` can be used directly.

## Updating

Download the latest release zip from GitHub Releases and overwrite the previous install directory, or unpack into a new versioned folder and re-register the MCP path in Claude Code.
