param(
    [Parameter(Mandatory = $true)]
    [string]$EngineRoot,

    [Parameter(Mandatory = $false)]
    [string]$Version = 'dev',

    [Parameter(Mandatory = $false)]
    [string]$OutputRoot = ''
)

$ErrorActionPreference = 'Stop'

$RepoRoot = [System.IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..'))
$EngineRoot = [System.IO.Path]::GetFullPath($EngineRoot)

if (-not $OutputRoot) {
    $OutputRoot = Join-Path $RepoRoot 'dist'
}
$OutputRoot = [System.IO.Path]::GetFullPath($OutputRoot)

$ReleaseName = "unrealinsights-mcp-release-$Version"
$StageRoot = Join-Path $OutputRoot $ReleaseName
$ZipPath = Join-Path $OutputRoot "$ReleaseName.zip"

$RequiredFiles = @(
    (Join-Path $EngineRoot 'Binaries\Win64\TraceAgentCli.exe'),
    (Join-Path $RepoRoot 'Programs\TraceAgentMcp\trace_agent_mcp.py'),
    (Join-Path $RepoRoot 'Plugins\trace-agent-mcp\scripts\start-trace-agent-mcp.py'),
    (Join-Path $RepoRoot 'release\README.release.md'),
    (Join-Path $RepoRoot 'release\.mcp.json')
)

foreach ($Path in $RequiredFiles) {
    if (-not (Test-Path -LiteralPath $Path)) {
        throw "Missing required input: $Path"
    }
}

if (Test-Path -LiteralPath $StageRoot) {
    Remove-Item -Recurse -Force $StageRoot
}
if (Test-Path -LiteralPath $ZipPath) {
    Remove-Item -Force $ZipPath
}

New-Item -ItemType Directory -Force $StageRoot | Out-Null
New-Item -ItemType Directory -Force (Join-Path $StageRoot 'Binaries\Win64') | Out-Null
New-Item -ItemType Directory -Force (Join-Path $StageRoot 'Programs\TraceAgentMcp') | Out-Null
New-Item -ItemType Directory -Force (Join-Path $StageRoot 'Plugins\trace-agent-mcp') | Out-Null

Copy-Item -Force (Join-Path $EngineRoot 'Binaries\Win64\TraceAgentCli.exe') (Join-Path $StageRoot 'Binaries\Win64\TraceAgentCli.exe')
Copy-Item -Force (Join-Path $RepoRoot 'Programs\TraceAgentMcp\trace_agent_mcp.py') (Join-Path $StageRoot 'Programs\TraceAgentMcp\trace_agent_mcp.py')
Copy-Item -Force (Join-Path $RepoRoot 'Programs\TraceAgentMcp\README.md') (Join-Path $StageRoot 'Programs\TraceAgentMcp\README.md')

New-Item -ItemType Directory -Force (Join-Path $StageRoot 'Plugins\trace-agent-mcp\.codex-plugin') | Out-Null
New-Item -ItemType Directory -Force (Join-Path $StageRoot 'Plugins\trace-agent-mcp\assets') | Out-Null
New-Item -ItemType Directory -Force (Join-Path $StageRoot 'Plugins\trace-agent-mcp\scripts') | Out-Null

Copy-Item -Force (Join-Path $RepoRoot 'Plugins\trace-agent-mcp\.mcp.json') (Join-Path $StageRoot 'Plugins\trace-agent-mcp\.mcp.json')
Copy-Item -Force (Join-Path $RepoRoot 'Plugins\trace-agent-mcp\.codex-plugin\plugin.json') (Join-Path $StageRoot 'Plugins\trace-agent-mcp\.codex-plugin\plugin.json')
Copy-Item -Force (Join-Path $RepoRoot 'Plugins\trace-agent-mcp\assets\trace-agent.svg') (Join-Path $StageRoot 'Plugins\trace-agent-mcp\assets\trace-agent.svg')
Copy-Item -Force (Join-Path $RepoRoot 'Plugins\trace-agent-mcp\scripts\start-trace-agent-mcp.py') (Join-Path $StageRoot 'Plugins\trace-agent-mcp\scripts\start-trace-agent-mcp.py')

Copy-Item -Force (Join-Path $RepoRoot 'release\.mcp.json') (Join-Path $StageRoot '.mcp.json')
Copy-Item -Force (Join-Path $RepoRoot 'release\README.release.md') (Join-Path $StageRoot 'README.md')

Compress-Archive -Path (Join-Path $StageRoot '*') -DestinationPath $ZipPath -Force

Write-Host "Release directory: $StageRoot"
Write-Host "Release zip: $ZipPath"
