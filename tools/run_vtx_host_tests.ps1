[CmdletBinding()]
param(
    [string]$EnvironmentName = 'm5stack-basic-v27',
    [string]$Compiler,
    [string]$GitExecutable,
    [switch]$VerifyDependencyOnly
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

$pinnedCommit = '54c21169305499667b9039022282288177ef1699'
$repositoryRoot = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..'))

function Resolve-Application {
    param([string[]]$Candidates, [string]$Description)
    foreach ($candidate in $Candidates) {
        if ([string]::IsNullOrWhiteSpace($candidate)) { continue }
        if (Test-Path -LiteralPath $candidate -PathType Leaf) {
            return [IO.Path]::GetFullPath($candidate)
        }
        $found = @(Get-Command -Name $candidate -CommandType Application -ErrorAction SilentlyContinue)
        if ($found.Count -gt 0) { return $found[0].Source }
    }
    throw "$Description was not found. Supply its executable path explicitly."
}

if ($EnvironmentName -notmatch '\A[A-Za-z0-9][A-Za-z0-9_.-]*\z') {
    throw 'EnvironmentName must be a single PlatformIO environment name, without path separators.'
}

$dependencyRoot = [IO.Path]::GetFullPath((Join-Path $repositoryRoot ".pio/libdeps/$EnvironmentName/VTXControl_ESP32"))
$dependencySource = Join-Path $dependencyRoot 'src'
if (-not (Test-Path -LiteralPath $dependencySource -PathType Container)) {
    throw "Installed dependency is missing: $dependencySource. Install the project's pinned PlatformIO dependencies first; this runner never installs them."
}
if (-not (Test-Path -LiteralPath (Join-Path $dependencyRoot '.git'))) {
    throw "Cannot verify the installed dependency commit: $dependencyRoot has no Git metadata. Use the pinned Git dependency from platformio.ini."
}

$gitCandidates = if ($GitExecutable) { @($GitExecutable) } else {
    @('git.exe', 'git', 'C:/Program Files/Git/cmd/git.exe', 'C:/msys64/usr/bin/git.exe')
}
$gitBinary = Resolve-Application -Candidates $gitCandidates -Description 'Git'

function Invoke-DependencyGit {
    param([string[]]$GitArguments)
    $messages = @(& $gitBinary -C $dependencyRoot @GitArguments 2>&1)
    if ($LASTEXITCODE -ne 0) {
        throw "Dependency Git verification failed: $($messages -join ' ')"
    }
    return ($messages -join [Environment]::NewLine).Trim()
}

$actualRoot = [IO.Path]::GetFullPath((Invoke-DependencyGit -GitArguments @('rev-parse', '--show-toplevel')))
if (-not $actualRoot.Equals($dependencyRoot, [StringComparison]::OrdinalIgnoreCase)) {
    throw "Dependency Git root is outside the selected installed package: $actualRoot"
}
$actualCommit = Invoke-DependencyGit -GitArguments @('rev-parse', '--verify', 'HEAD')
if (-not $actualCommit.Equals($pinnedCommit, [StringComparison]::OrdinalIgnoreCase)) {
    throw "Installed VTXControl_ESP32 commit is $actualCommit; expected $pinnedCommit."
}
$origin = Invoke-DependencyGit -GitArguments @('remote', 'get-url', 'origin')
$normalizedOrigin = $origin.TrimEnd('/').ToLowerInvariant() -replace '\.git$', ''
if ($normalizedOrigin -notin @('https://github.com/igorka48/vtxcontrol_esp32', 'git@github.com:igorka48/vtxcontrol_esp32')) {
    throw "Installed dependency origin is unexpected: $origin"
}

$headers = @('VTX_SmartAudio.h', 'VTX_Tramp.h')
$stubDirectory = Join-Path $repositoryRoot 'tests/vtx_host/stubs'
$includeDirectory = Join-Path $repositoryRoot 'include'
foreach ($header in $headers) {
    if (-not (Test-Path -LiteralPath (Join-Path $dependencySource $header) -PathType Leaf)) {
        throw "Pinned protocol header is missing: $header"
    }
    $null = Invoke-DependencyGit -GitArguments @('ls-files', '--error-unmatch', '--', "src/$header")
    foreach ($earlierInclude in @($stubDirectory, $includeDirectory)) {
        if (Test-Path -LiteralPath (Join-Path $earlierInclude $header)) {
            throw "A local header would shadow the installed dependency: $(Join-Path $earlierInclude $header)"
        }
    }
}
$sourceChanges = Invoke-DependencyGit -GitArguments @('status', '--porcelain', '--untracked-files=all', '--', 'src')
if ($sourceChanges) {
    throw "Installed dependency src differs from the pinned commit: $sourceChanges"
}

Write-Output "Verified VTXControl_ESP32 commit $actualCommit"
Write-Output "Protocol headers: $dependencySource"
if ($VerifyDependencyOnly) {
    Write-Output 'Dependency provenance checks passed; no compilation requested.'
    return
}

$compilerCandidates = if ($Compiler) { @($Compiler) } else {
    @('C:/msys64/mingw64/bin/g++.exe', 'C:/msys64/ucrt64/bin/g++.exe',
      'g++.exe', 'g++', 'clang++.exe', 'clang++')
}
$compilerBinary = Resolve-Application -Candidates $compilerCandidates -Description 'A C++11 compiler'
$testBinary = Join-Path $repositoryRoot 'tests/vtx_host/test_vtx.exe'
$compileArguments = @('-std=c++11', "-I$stubDirectory", "-I$includeDirectory", "-I$dependencySource",
                      (Join-Path $repositoryRoot 'tests/vtx_host/test_vtx.cpp'),
                      (Join-Path $repositoryRoot 'src/vtx_manager.cpp'), '-o', $testBinary)

$originalPath = $env:PATH
try {
    # Windows searches the test executable's directory for DLLs. Make the
    # compiler's companion runtime available without changing the user PATH.
    $env:PATH = "$(Split-Path -Parent $compilerBinary);$originalPath"
    Write-Output "Compiler: $compilerBinary"
    & $compilerBinary @compileArguments
    if ($LASTEXITCODE -ne 0) { throw "VTX host compilation failed with exit code $LASTEXITCODE." }
    & $testBinary
    if ($LASTEXITCODE -ne 0) { throw "VTX host tests failed with exit code $LASTEXITCODE." }
    Write-Output 'VTX host tests passed against the pinned installed dependency.'
} finally {
    $env:PATH = $originalPath
}
