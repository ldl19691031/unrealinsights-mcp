# unrealinsights-mcp

`unrealinsights-mcp` supports two workflows:

- Source-sync workflow
  You sync these files into a source-built Unreal Engine checkout and build `TraceAgentCli` there.
- Release workflow
  You build `TraceAgentCli` once on a build machine, package a release bundle, and let target machines only download the bundle and run the MCP.

If your target machine already has Python and you want "download and run" with Claude Code, the release workflow is the simpler option.

## Recommended For Claude Code Targets

For target machines that use Claude Code, the recommended flow is:

1. Build `TraceAgentCli.exe` on your build machine.
2. Run `scripts/make-release.ps1` in this repository.
3. Upload the generated zip to GitHub Releases.
4. On the target machine, download the release zip and unzip it anywhere.
5. Register the bundled MCP launcher in Claude Code.

This avoids syncing into a full Unreal Engine checkout on the target machine.

## Release Workflow

### Build Machine

You still need one machine that can build `TraceAgentCli` from Unreal source.

After building `TraceAgentCli`, run:

```powershell
pwsh -File .\scripts\make-release.ps1 `
  -EngineRoot 'F:\Source\UE\uesrc_main\Engine' `
  -Version 'v0.1.0'
```

If `pwsh` is not available:

```powershell
powershell -ExecutionPolicy Bypass -File .\scripts\make-release.ps1 `
  -EngineRoot 'F:\Source\UE\uesrc_main\Engine' `
  -Version 'v0.1.0'
```

This creates:

- `dist\unrealinsights-mcp-release-v0.1.0\`
- `dist\unrealinsights-mcp-release-v0.1.0.zip`

The release bundle contains:

- `Binaries\Win64\TraceAgentCli.exe`
- `Programs\TraceAgentMcp\trace_agent_mcp.py`
- `Plugins\trace-agent-mcp\...`
- `.mcp.json`
- `README.md`

### Target Machine

The target machine only needs:

- Python on `PATH`
- Claude Code

It does **not** need:

- Unreal source
- UBT
- Visual Studio Unreal build setup

#### Option A: Open the release folder as a Claude Code project

The generated release bundle includes a project-scoped `.mcp.json` in the root.

If you open the extracted release folder itself as the Claude Code project, Claude Code can use that `.mcp.json` directly.

#### Option B: Add it to Claude Code as a user-scoped MCP server

Anthropic's Claude Code MCP docs indicate Claude Code supports project/user-scoped MCP servers and environment-variable expansion in `.mcp.json`. It also supports local stdio servers launched by command and args. Source:

- [Claude Code MCP docs](https://docs.anthropic.com/en/docs/claude-code/mcp)

For a release extracted to:

- `D:\Tools\unrealinsights-mcp-release-v0.1.0`

you can add it as a user-scoped server like this:

```powershell
claude mcp add unrealinsights-mcp --scope user -- `
  python D:\Tools\unrealinsights-mcp-release-v0.1.0\Plugins\trace-agent-mcp\scripts\start-trace-agent-mcp.py
```

If you want a default trace file on that target machine, set it when adding the server:

```powershell
claude mcp add unrealinsights-mcp --scope user `
  --env TRACE_AGENT_DEFAULT_TRACE=D:\Traces\sample.utrace -- `
  python D:\Tools\unrealinsights-mcp-release-v0.1.0\Plugins\trace-agent-mcp\scripts\start-trace-agent-mcp.py
```

If you later want to inspect the registered server:

```powershell
claude mcp get unrealinsights-mcp
```

### Keeping Release Targets Updated

The simplest update flow for release consumers is:

1. Download the latest GitHub Release zip.
2. Extract it over the previous install directory, or extract to a versioned directory and switch your Claude Code MCP registration to the new path.

For example:

```powershell
claude mcp remove unrealinsights-mcp
claude mcp add unrealinsights-mcp --scope user -- `
  python D:\Tools\unrealinsights-mcp-release-v0.1.1\Plugins\trace-agent-mcp\scripts\start-trace-agent-mcp.py
```

If you prefer stable paths, always extract to the same directory:

- `D:\Tools\unrealinsights-mcp-current`

and overwrite that directory with the latest release contents.

Then your Claude Code registration does not need to change.

It contains four pieces:

- `Source/Programs/TraceAgentCli`
  A standalone Unreal `Program` target that reads `.utrace` files directly and emits JSON.
- `Programs/TraceAgentMcp`
  A thin stdio MCP server that wraps `TraceAgentCli`.
- `Plugins/trace-agent-mcp`
  A repo-local Codex plugin manifest and launcher.
- `.agents/plugins/marketplace.json`
  A local marketplace entry so the plugin can be discovered from the Unreal Engine workspace.

This repository is not a full Unreal Engine fork. The intended workflow is:

1. Clone this repository anywhere convenient.
2. Sync its contents into a source Unreal Engine `Engine` directory.
3. Build `TraceAgentCli`.
4. Open that Unreal Engine `Engine` directory as your Codex workspace.

## Prerequisites

- A source-built Unreal Engine checkout.
- Windows with the Unreal toolchain already working for that checkout.
- Python available on `PATH`.
- Git available on `PATH`.

## Repository Layout

The folder layout in this repository mirrors the target layout inside `Engine/`:

```text
Source/Programs/TraceAgentCli
Programs/TraceAgentMcp
Plugins/trace-agent-mcp
.agents/plugins/marketplace.json
```

The install script copies those paths into your Unreal Engine `Engine` folder.

## First-Time Setup On Another Computer

### 1. Clone This Repository

```powershell
git clone https://github.com/ldl19691031/unrealinsights-mcp.git D:\Dev\unrealinsights-mcp
cd D:\Dev\unrealinsights-mcp
```

### 2. Install Into Your Unreal Engine `Engine` Directory

Example target:

- `F:\Source\UE\uesrc_main\Engine`

Run:

```powershell
pwsh -File .\scripts\install-into-engine.ps1 -EngineRoot 'F:\Source\UE\uesrc_main\Engine'
```

If `pwsh` is not available on that machine, run it with Windows PowerShell:

```powershell
powershell -ExecutionPolicy Bypass -File .\scripts\install-into-engine.ps1 -EngineRoot 'F:\Source\UE\uesrc_main\Engine'
```

What this installs:

- `Engine\Source\Programs\TraceAgentCli`
- `Engine\Programs\TraceAgentMcp`
- `Engine\Plugins\trace-agent-mcp`
- `Engine\.agents\plugins\marketplace.json`

### 3. Build `TraceAgentCli`

From the Unreal Engine `Engine` directory:

```powershell
& 'F:\Source\UE\uesrc_main\Engine\Binaries\DotNET\UnrealBuildTool\UnrealBuildTool.exe' TraceAgentCli Win64 Development
```

After a successful build, the executable should exist at:

- `Engine\Binaries\Win64\TraceAgentCli.exe`

### 4. Verify the CLI

```powershell
& 'F:\Source\UE\uesrc_main\Engine\Binaries\Win64\TraceAgentCli.exe' summary --json-only '-i=F:\Source\UE\uesrc_main\Engine\Trace\20260416_135353.utrace'
```

If you already have another `.utrace`, use that instead.

### 5. Use It From Codex

Open the Unreal Engine `Engine` directory as the Codex workspace.

The repo-local plugin is installed under:

- `Engine\Plugins\trace-agent-mcp`

The local marketplace entry is:

- `Engine\.agents\plugins\marketplace.json`

The plugin launcher starts:

- `Engine\Programs\TraceAgentMcp\trace_agent_mcp.py`

which in turn uses:

- `Engine\Binaries\Win64\TraceAgentCli.exe`

By default, if this file exists:

- `Engine\Trace\20260416_135353.utrace`

the plugin launcher exports it as `TRACE_AGENT_DEFAULT_TRACE`, so tools can be called without explicitly passing `trace_file`.

## MCP Capabilities

Current MCP tools include:

- `summary`
- `list_tracks`
- `find_tracks`
- `list_timers`
- `query_timing_events`
- `query_frames`
- `list_counters`
- `query_counter_values`
- `query_bookmarks`
- `repo_info`

The query tools support:

- absolute time windows: `start_time`, `end_time`
- percentage windows: `head_percent`, `tail_percent`, `range_start_percent`, `range_end_percent`
- frame windows: `frame_number`, `end_frame_number`, `frame_count`, `head_frame_count`, `tail_frame_count`

## Keeping It Updated From GitHub

### Manual Update Flow

In the `unrealinsights-mcp` repository:

```powershell
git pull --ff-only
pwsh -File .\scripts\install-into-engine.ps1 -EngineRoot 'F:\Source\UE\uesrc_main\Engine'
```

Then rebuild the CLI:

```powershell
& 'F:\Source\UE\uesrc_main\Engine\Binaries\DotNET\UnrealBuildTool\UnrealBuildTool.exe' TraceAgentCli Win64 Development
```

If only Python/plugin files changed, the reinstall step is enough. If anything under `Source/Programs/TraceAgentCli` changed, rebuild.

### One-Step Update Script

You can also run:

```powershell
pwsh -File .\scripts\update-from-github.ps1 -EngineRoot 'F:\Source\UE\uesrc_main\Engine'
```

That script does:

1. `git pull --ff-only`
2. re-run the install sync into your Engine folder

You should still rebuild `TraceAgentCli` after pulling if the CLI sources changed.

## Suggested Periodic Update Routine

For a machine that uses this regularly, the practical routine is:

1. `git pull --ff-only` in `unrealinsights-mcp`
2. run `scripts/update-from-github.ps1`
3. rebuild `TraceAgentCli`
4. restart Codex or reload the workspace if needed

## Notes

- This repository intentionally does not include the built `TraceAgentCli.exe`.
- The CLI is built from Unreal source on the target machine.
- The MCP server is plain Python and does not require extra Python packages.
- The Codex plugin is repo-local relative to the Unreal Engine workspace after installation.
