param(
    [Parameter(Mandatory = $true)]
    [string]$EngineRoot
)

$ErrorActionPreference = 'Stop'

$RepoRoot = [System.IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..'))
$EngineRoot = [System.IO.Path]::GetFullPath($EngineRoot)

if (-not (Test-Path -LiteralPath $EngineRoot)) {
    throw "EngineRoot does not exist: $EngineRoot"
}

$Mappings = @(
    @{ Source = (Join-Path $RepoRoot 'Source\\Programs\\TraceAgentCli'); Target = (Join-Path $EngineRoot 'Source\\Programs\\TraceAgentCli') },
    @{ Source = (Join-Path $RepoRoot 'Programs\\TraceAgentMcp'); Target = (Join-Path $EngineRoot 'Programs\\TraceAgentMcp') },
    @{ Source = (Join-Path $RepoRoot 'Plugins\\trace-agent-mcp'); Target = (Join-Path $EngineRoot 'Plugins\\trace-agent-mcp') },
    @{ Source = (Join-Path $RepoRoot '.agents\\plugins\\marketplace.json'); Target = (Join-Path $EngineRoot '.agents\\plugins\\marketplace.json') }
)

foreach ($Mapping in $Mappings) {
    if (-not (Test-Path -LiteralPath $Mapping.Source)) {
        throw "Missing source path: $($Mapping.Source)"
    }

    $Parent = Split-Path -Parent $Mapping.Target
    New-Item -ItemType Directory -Force $Parent | Out-Null

    if ((Get-Item -LiteralPath $Mapping.Source) -is [System.IO.DirectoryInfo]) {
        New-Item -ItemType Directory -Force $Mapping.Target | Out-Null
        Get-ChildItem -Force -LiteralPath $Mapping.Source | ForEach-Object {
            Copy-Item -Recurse -Force -LiteralPath $_.FullName -Destination $Mapping.Target
        }
    }
    else {
        Copy-Item -Force $Mapping.Source $Mapping.Target
    }

    Write-Host "Installed: $($Mapping.Target)"
}

Write-Host 'Install complete.'
