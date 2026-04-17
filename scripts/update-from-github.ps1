param(
    [Parameter(Mandatory = $true)]
    [string]$EngineRoot
)

$ErrorActionPreference = 'Stop'

$RepoRoot = [System.IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..'))
Set-Location $RepoRoot

git pull --ff-only

& (Join-Path $PSScriptRoot 'install-into-engine.ps1') -EngineRoot $EngineRoot

Write-Host 'Update sync complete.'
