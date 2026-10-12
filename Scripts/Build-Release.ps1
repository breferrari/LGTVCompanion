<#
.SYNOPSIS
    Builds the fork's installer (Release, x64) and copies it to dist\ with the version in its name.

.DESCRIPTION
    Builds the five applications and the WiX installer project, which references them. The version is
    ForkVersion from Directory.Build.props. See Docs/Fork.md for the prerequisites and the release steps.
#>
$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot

$version = ([xml](Get-Content (Join-Path $root 'Directory.Build.props'))).Project.PropertyGroup.ForkVersion
if (-not $version) { throw 'ForkVersion not found in Directory.Build.props' }

$vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
$msbuild = & $vswhere -latest -requires Microsoft.Component.MSBuild -find 'MSBuild\**\Bin\amd64\MSBuild.exe' | Select-Object -First 1
if (-not $msbuild) { throw 'MSBuild not found. Install Visual Studio with the C++ desktop workload' }
if (-not (Get-Command dotnet -ErrorAction SilentlyContinue)) { throw '.NET SDK not found. It is required to build the WiX installer' }

Write-Host "Building LGTV Companion $version"
& $msbuild (Join-Path $root 'LGTV Companion Setup\LGTV Companion Setup.wixproj') /restore /p:Configuration=Release /p:Platform=x64 /m /nologo /v:minimal
if ($LASTEXITCODE -ne 0) { throw "Build failed ($LASTEXITCODE)" }

$msi = Get-ChildItem (Join-Path $root 'LGTV Companion Setup\bin\x64\Release') -Filter '*.msi' -Recurse |
    Sort-Object LastWriteTime -Descending | Select-Object -First 1
if (-not $msi) { throw 'The installer was not produced' }

$dist = New-Item -ItemType Directory -Force (Join-Path $root 'dist')
$target = Join-Path $dist "LGTV.Companion.Setup.v$version.x64.msi"
Copy-Item $msi.FullName $target -Force
Write-Host "Installer: $target"
