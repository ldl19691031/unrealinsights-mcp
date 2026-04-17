# TraceAgentMcp

`TraceAgentMcp` is a thin local MCP server that wraps `TraceAgentCli.exe`.

It exposes Unreal trace queries over MCP stdio without launching Unreal Editor or Unreal Insights UI.

Current tools:

- `summary`
- `list_tracks`
- `list_timers`
- `query_timing_events`
- `list_counters`
- `query_counter_values`
- `query_bookmarks`

The server script is:

- [trace_agent_mcp.py](/F:/Source/UE/uesrc_main/Engine/Programs/TraceAgentMcp/trace_agent_mcp.py)

It expects `TraceAgentCli.exe` at the default build location:

- `F:\Source\UE\uesrc_main\Engine\Binaries\Win64\TraceAgentCli.exe`

You can override that path with the `TRACE_AGENT_CLI` environment variable.
