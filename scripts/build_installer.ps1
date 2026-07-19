[CmdletBinding()]
param(
    [ValidateSet('Debug', 'Release')]
    [string]$Configuration = 'Release',
    [switch]$SkipBuild,
    # Retains already-cached, verified prerequisite binaries. Useful for a
    # repeatable/offline packaging pass after a prior connected download.
    [switch]$SkipPrerequisiteDownload
)

$ErrorActionPreference = 'Stop'
$ProgressPreference = 'SilentlyContinue'
$root = Split-Path -Parent $PSScriptRoot
$stage = Join-Path $root 'build\installer_stage'
$prerequisiteCache = Join-Path $root 'build\installer_prerequisites'
$metadataPath = Join-Path $root 'installer\generated\prerequisite_sizes.iss'
$isccCandidates = @(@(
    (Get-Command ISCC.exe -ErrorAction SilentlyContinue | Select-Object -First 1 -ExpandProperty Source),
    "$env:ProgramFiles(x86)\Inno Setup 6\ISCC.exe",
    "$env:ProgramFiles\Inno Setup 6\ISCC.exe",
    "$env:LOCALAPPDATA\Programs\Inno Setup 6\ISCC.exe"
) | Where-Object { $_ -and (Test-Path -LiteralPath $_) })

function Copy-Required([string]$Source, [string]$Destination) {
    if (-not (Test-Path -LiteralPath $Source -PathType Leaf)) {
        throw "Required installer payload is missing: $Source"
    }
    New-Item -ItemType Directory -Path (Split-Path -Parent $Destination) -Force | Out-Null
    Copy-Item -LiteralPath $Source -Destination $Destination -Force
}

function Copy-Tree([string]$Source, [string]$Destination) {
    if (-not (Test-Path -LiteralPath $Source -PathType Container)) {
        throw "Required installer payload is missing: $Source"
    }
    New-Item -ItemType Directory -Path $Destination -Force | Out-Null
    Copy-Item -Path (Join-Path $Source '*') -Destination $Destination -Recurse -Force
}

function Assert-PortableExecutable([string]$Path, [string]$Label) {
    if (-not (Test-Path -LiteralPath $Path -PathType Leaf)) {
        throw "Required $Label was not staged: $Path"
    }
    $stream = [System.IO.File]::OpenRead($Path)
    try {
        $first = $stream.ReadByte()
        $second = $stream.ReadByte()
    } finally {
        $stream.Dispose()
    }
    if ($first -ne 0x4D -or $second -ne 0x5A) {
        throw "$Label is not a valid Windows executable (missing MZ signature): $Path"
    }
}

function Assert-IdenticalFile([string]$Source, [string]$Destination, [string]$Label) {
    $sourceHash = (Get-FileHash -LiteralPath $Source -Algorithm SHA256).Hash
    $destinationHash = (Get-FileHash -LiteralPath $Destination -Algorithm SHA256).Hash
    if ($sourceHash -ne $destinationHash) {
        throw "Staged $Label does not match the Release artifact."
    }
}

function Assert-ReleaseStage([string]$Stage) {
    # The installed engine needs source/header files for native project
    # scripting, but it must never inherit a developer checkout's caches,
    # old templates, crash reports, or test trees.
    foreach ($required in @(
        'hub.exe', 'editor.exe', 'SDL2.dll', 'editor_symbols.map',
        'templates\abyss-of-hollows\project.json',
        'build\scripts_module_fast\abyss_of_hollows\Release',
        'engine_cpp\engine\core.cpp', 'editor\src\editor_state.hpp', 'editor\third_party\imgui\imgui.h', 'editor\scripts_module\CMakeLists.txt',
        'cmake\msbuild_host_x64.props',
        'nlohmann\json.hpp', 'third_party\sdl2\include\SDL2\SDL.h',
        'third_party\sdl2\lib\x64\SDL2.lib')) {
        if (-not (Test-Path -LiteralPath (Join-Path $Stage $required))) {
            throw "Required final-release payload is missing after staging: $required"
        }
    }
    foreach ($forbidden in @(
        'templates\novaSlash', 'templates\game5', '.git', '.vs',
        '__pycache__', 'crash_reports', 'installer_smoke', 'engine_cpp\tests',
        'templates\abyss-of-hollows\tools')) {
        if (Test-Path -LiteralPath (Join-Path $Stage $forbidden)) {
            throw "Release staging contains a development-only path: $forbidden"
        }
    }
    $residue = Get-ChildItem -LiteralPath $Stage -Recurse -File | Where-Object {
        $_.Extension -in @('.pdb', '.obj', '.ilk', '.tmp') -or $_.Name -like '*_hot_*.dll'
    }
    if ($residue) {
        $names = ($residue | Select-Object -First 5 -ExpandProperty FullName) -join '; '
        throw "Release staging contains generated build residue: $names"
    }
    $retiredScripts = Get-ChildItem -LiteralPath (Join-Path $Stage 'templates\abyss-of-hollows\scripts') -Filter 'NewScript*.cpp' -File -ErrorAction SilentlyContinue
    if ($retiredScripts) {
        throw 'Release staging contains retired numbered showcase scripts.'
    }
    $retiredModules = Get-ChildItem -LiteralPath (Join-Path $Stage 'build\scripts_module_fast\abyss_of_hollows\Release') -Filter '*NewScript*.dll' -File -ErrorAction SilentlyContinue
    if ($retiredModules) {
        throw 'Release staging contains retired numbered showcase script modules.'
    }
}

function Format-ByteSize([Int64]$Bytes) {
    if ($Bytes -ge 1GB) { return ('{0:N2} GB' -f ($Bytes / 1GB)) }
    if ($Bytes -ge 1MB) { return ('{0:N1} MB' -f ($Bytes / 1MB)) }
    if ($Bytes -ge 1KB) { return ('{0:N1} KB' -f ($Bytes / 1KB)) }
    return "$Bytes bytes"
}

function Assert-MicrosoftOrLunarGSignature([string]$Path, [string]$ExpectedPublisher) {
    $signature = Get-AuthenticodeSignature -LiteralPath $Path
    if ($signature.Status -ne 'Valid') {
        throw "Downloaded prerequisite does not have a valid Authenticode signature: $Path ($($signature.Status))"
    }
    $subject = [string]$signature.SignerCertificate.Subject
    if ($subject -notmatch [regex]::Escape($ExpectedPublisher)) {
        throw "Downloaded prerequisite signer was '$subject', expected '$ExpectedPublisher': $Path"
    }
}

function Get-OfficialFile([string]$Uri, [string]$Destination, [string]$Publisher) {
    if (-not (Test-Path -LiteralPath $Destination -PathType Leaf)) {
        if ($SkipPrerequisiteDownload) {
            throw "Missing cached prerequisite while -SkipPrerequisiteDownload was selected: $Destination"
        }
        New-Item -ItemType Directory -Path (Split-Path -Parent $Destination) -Force | Out-Null
        $temporary = "$Destination.download"
        Remove-Item -LiteralPath $temporary -Force -ErrorAction SilentlyContinue
        Write-Host "Downloading official prerequisite: $Uri"
        # curl follows the publisher's CDN redirects and, unlike the older
        # PowerShell web cmdlet, has a bounded connection timeout. A failed
        # or blocked mirror must fail the release visibly rather than leave a
        # zero-byte .download file and an apparently frozen package build.
        $curl = Get-Command curl.exe -ErrorAction SilentlyContinue | Select-Object -First 1 -ExpandProperty Source
        if ($curl) {
            & $curl --location --fail --retry 3 --retry-delay 3 --connect-timeout 30 --max-time 1800 --output $temporary $Uri
            if ($LASTEXITCODE -ne 0) { throw "Official prerequisite download failed (curl exit $LASTEXITCODE): $Uri" }
        } else {
            Invoke-WebRequest -Uri $Uri -OutFile $temporary -UseBasicParsing -TimeoutSec 1800
        }
        Assert-PortableExecutable $temporary ([System.IO.Path]::GetFileName($Destination))
        Assert-MicrosoftOrLunarGSignature $temporary $Publisher
        Move-Item -LiteralPath $temporary -Destination $Destination -Force
    }
    Assert-PortableExecutable $Destination ([System.IO.Path]::GetFileName($Destination))
    Assert-MicrosoftOrLunarGSignature $Destination $Publisher
    return Get-Item -LiteralPath $Destination
}

function Get-LunarGWindowsVersion {
    if ($SkipPrerequisiteDownload) {
        $cached = Get-ChildItem -LiteralPath $prerequisiteCache -Filter 'vulkansdk-windows-X64-*.exe' -File -ErrorAction SilentlyContinue |
            Sort-Object LastWriteTime -Descending | Select-Object -First 1
        if ($cached -and $cached.Name -match '([0-9]+\.[0-9]+\.[0-9]+\.[0-9]+)') { return $Matches[1] }
        throw 'No cached Vulkan SDK version was found. Run once without -SkipPrerequisiteDownload.'
    }
    $version = (Invoke-WebRequest -Uri 'https://vulkan.lunarg.com/sdk/latest/windows.txt' -UseBasicParsing).Content.Trim()
    if ($version -notmatch '^\d+\.\d+\.\d+\.\d+$') {
        throw "LunarG returned an unexpected Windows Vulkan SDK version: '$version'"
    }
    return $version
}

function Get-Sdl2Root {
    # Packaged native-script builds use an engine-local third_party/sdl2 tree.
    # For packaging a developer checkout, honor user/toolchain configuration
    # and the existing CMake cache rather than assuming one machine path.
    $candidates = New-Object System.Collections.Generic.List[string]
    foreach ($value in @($env:SDL2_ROOT, $env:SDL2_DIR, (Join-Path $root 'third_party\sdl2'))) {
        if ($value) { $candidates.Add($value) }
    }
    $cache = Join-Path $root 'build\CMakeCache.txt'
    if (Test-Path -LiteralPath $cache) {
        foreach ($line in Get-Content -LiteralPath $cache) {
            if ($line -match '^SDL2_(?:ROOT_DIR|DIR):(?:PATH|STRING)=(.+)$') {
                $candidates.Add($Matches[1])
            }
        }
    }
    foreach ($candidate in $candidates) {
        $path = [System.IO.Path]::GetFullPath($candidate)
        for ($levels = 0; $levels -lt 4; ++$levels) {
            $header = Join-Path $path 'include\SDL2\SDL.h'
            $library = Join-Path $path 'lib\x64\SDL2.lib'
            if ((Test-Path -LiteralPath $header -PathType Leaf) -and (Test-Path -LiteralPath $library -PathType Leaf)) {
                return $path
            }
            $parent = Split-Path -Parent $path
            if (-not $parent -or $parent -eq $path) { break }
            $path = $parent
        }
    }
    throw 'SDL2 development headers and x64 import library were not found. Configure this checkout once with SDL2_ROOT pointing to a valid SDK, then package again.'
}

function Get-FileVersionOrFallback([string]$Path, [string]$Fallback) {
    $item = Get-Item -LiteralPath $Path
    # LunarG's Windows SDK bootstrap has no useful PE product version, but
    # its immutable official filename contains the release version.
    if ($item.Name -match '(\d+\.\d+\.\d+\.\d+)') { return $Matches[1] }
    foreach ($candidate in @($item.VersionInfo.ProductVersion, $item.VersionInfo.FileVersion)) {
        if (-not [string]::IsNullOrWhiteSpace($candidate) -and $candidate -match '\d+\.\d+') {
            return ($candidate -replace '[\r\n]+', ' ').Trim()
        }
    }
    return $Fallback
}

function Escape-Inno([string]$Value) {
    return $Value.Replace('"', "''")
}

function Write-PrerequisiteMetadata([string]$EngineRoot, [hashtable]$Prerequisites) {
    $engineBytes = (Get-ChildItem -LiteralPath $EngineRoot -File -Recurse | Measure-Object -Property Length -Sum).Sum
    $lines = @(
        '; Generated by scripts\build_installer.ps1. Do not hand-edit.',
        "#define EnginePayloadSize `"$(Escape-Inno (Format-ByteSize $engineBytes))`""
    )
    foreach ($entry in @(
        @{ Key = 'VcRedist'; File = 'vc_redist.x64.exe'; Fallback = 'Microsoft VC++ x64' },
        @{ Key = 'VSCode'; File = 'VSCodeUserSetup-x64.exe'; Fallback = 'VS Code stable x64' },
        @{ Key = 'BuildTools'; File = 'vs_buildtools.exe'; Fallback = 'Visual Studio Build Tools' },
        @{ Key = 'VulkanRuntime'; File = 'VulkanRT-x64.exe'; Fallback = 'LunarG Vulkan Runtime' },
        @{ Key = 'VulkanSdk'; File = 'vulkan_sdk.exe'; Fallback = 'LunarG Vulkan SDK' }
    )) {
        $file = $Prerequisites[$entry.File]
        $version = Get-FileVersionOrFallback $file.FullName $entry.Fallback
        $hash = (Get-FileHash -LiteralPath $file.FullName -Algorithm SHA256).Hash
        $lines += "#define $($entry.Key)Version `"$(Escape-Inno $version)`""
        $lines += "#define $($entry.Key)Size `"$(Escape-Inno (Format-ByteSize $file.Length))`""
        $lines += "; $($entry.File) SHA-256: $hash"
    }
    New-Item -ItemType Directory -Path (Split-Path -Parent $metadataPath) -Force | Out-Null
    [System.IO.File]::WriteAllLines($metadataPath, $lines, [System.Text.UTF8Encoding]::new($false))
}

if (-not $SkipBuild) {
    # This is the only build action in the packaging pipeline. It occurs
    # before staging, after all source changes are complete.
    & cmake --build (Join-Path $root 'build') --config $Configuration --target editor hub --parallel 1
    if ($LASTEXITCODE -ne 0) { throw 'Release editor/hub build failed; installer was not created.' }
}

# Fetch official prerequisite installers first. Their filenames in the cache
# preserve upstream versions; the stage uses stable names consumed by Inno.
New-Item -ItemType Directory -Path $prerequisiteCache -Force | Out-Null
$vulkanVersion = Get-LunarGWindowsVersion
$vulkanSdkSource = Join-Path $prerequisiteCache "vulkansdk-windows-X64-$vulkanVersion.exe"
$vulkanRuntimeSource = Join-Path $prerequisiteCache "VulkanRT-X64-$vulkanVersion-Installer.exe"
$prerequisites = @{}
$prerequisites['vc_redist.x64.exe'] = Get-OfficialFile 'https://aka.ms/vc14/vc_redist.x64.exe' (Join-Path $prerequisiteCache 'vc_redist.x64.exe') 'Microsoft'
$prerequisites['vs_buildtools.exe'] = Get-OfficialFile 'https://aka.ms/vs/17/release/vs_buildtools.exe' (Join-Path $prerequisiteCache 'vs_buildtools.exe') 'Microsoft'
$prerequisites['VSCodeUserSetup-x64.exe'] = Get-OfficialFile 'https://update.code.visualstudio.com/latest/win32-x64-user/stable' (Join-Path $prerequisiteCache 'VSCodeUserSetup-x64.exe') 'Microsoft'
$prerequisites['VulkanRT-x64.exe'] = Get-OfficialFile "https://sdk.lunarg.com/sdk/download/$vulkanVersion/windows/VulkanRT-X64-$vulkanVersion-Installer.exe" $vulkanRuntimeSource 'LunarG'
$prerequisites['vulkan_sdk.exe'] = Get-OfficialFile "https://sdk.lunarg.com/sdk/download/$vulkanVersion/windows/vulkansdk-windows-X64-$vulkanVersion.exe" $vulkanSdkSource 'LunarG'

# Stage a deterministic, self-contained engine payload. Installed engines do
# not depend on this machine's SDK paths: SDL2 runtime and native-authoring
# headers/import library are copied into the engine itself.
Remove-Item -LiteralPath $stage -Recurse -Force -ErrorAction SilentlyContinue
New-Item -ItemType Directory -Path $stage -Force | Out-Null

$editorRelease = Join-Path $root "build\editor\$Configuration"
$hubRelease = Join-Path $root "build\hub\$Configuration"
Copy-Required (Join-Path $hubRelease 'hub.exe') (Join-Path $stage 'hub.exe')
Copy-Required (Join-Path $editorRelease 'editor.exe') (Join-Path $stage 'editor.exe')
Copy-Required (Join-Path $root 'SDL2.dll') (Join-Path $stage 'SDL2.dll')
Copy-Required (Join-Path $editorRelease 'editor_symbols.map') (Join-Path $stage 'editor_symbols.map')
Copy-Tree (Join-Path $editorRelease 'shaders') (Join-Path $stage 'shaders')
Copy-Tree (Join-Path $editorRelease 'assets') (Join-Path $stage 'assets')
Copy-Tree (Join-Path $root 'engine_cpp') (Join-Path $stage 'engine_cpp')
Remove-Item -LiteralPath (Join-Path $stage 'engine_cpp\tests') -Recurse -Force -ErrorAction SilentlyContinue
Copy-Tree (Join-Path $root 'cmake') (Join-Path $stage 'cmake')
Copy-Required (Join-Path $root 'editor\CMakeLists.txt') (Join-Path $stage 'editor\CMakeLists.txt')
# Native game-script modules include a small, shared subset of the editor
# model through engine_cpp/prefab_system.hpp.  Stage the source headers so a
# project created by Hub has everything it needs to configure and rebuild
# its own C++ scripts after installation.
Copy-Tree (Join-Path $root 'editor\src') (Join-Path $stage 'editor\src')
Copy-Tree (Join-Path $root 'editor\third_party\imgui') (Join-Path $stage 'editor\third_party\imgui')
Copy-Tree (Join-Path $root 'editor\scripts_module') (Join-Path $stage 'editor\scripts_module')
Copy-Tree (Join-Path $hubRelease 'assets') (Join-Path $stage 'assets')
Copy-Tree (Join-Path $hubRelease 'templates') (Join-Path $stage 'templates')
# The sample template must open with all of its native gameplay classes
# registered. Stage only the final DLLs, never MSVC/CMake intermediates.
$templateModules = Join-Path $root 'build\scripts_module_fast\Release'
$templateModuleStage = Join-Path $stage 'build\scripts_module_fast\abyss_of_hollows\Release'
New-Item -ItemType Directory -Path $templateModuleStage -Force | Out-Null
$templateSourceNames = Get-ChildItem -LiteralPath (Join-Path $root 'games\abyss-of-hollows\scripts') -Filter '*.cpp' -File |
    Select-Object -ExpandProperty BaseName
$templatePrefix = 'abyss_of_hollows_'
$templateDlls = Get-ChildItem -LiteralPath $templateModules -Filter "$templatePrefix*.dll" -File -ErrorAction SilentlyContinue |
    Where-Object {
        $_.Name -notlike '*_hot_*.dll' -and
        $templateSourceNames -contains $_.BaseName.Substring($templatePrefix.Length)
    }
if (-not $templateDlls) {
    throw 'Prebuilt Abyss of Hollows script modules are missing. Build the abyss-of-hollows script modules before packaging.'
}
foreach ($dll in $templateDlls) {
    Copy-Required $dll.FullName (Join-Path $templateModuleStage $dll.Name)
}
Copy-Tree (Join-Path $root 'nlohmann') (Join-Path $stage 'nlohmann')
Copy-Required (Join-Path $root 'CMakeLists.txt') (Join-Path $stage 'CMakeLists.txt')
Copy-Required (Join-Path $root 'installer\THIRD_PARTY_NOTICES.md') (Join-Path $stage 'THIRD_PARTY_NOTICES.md')
Get-ChildItem -LiteralPath $root -Filter 'LICENSE*' -File -ErrorAction SilentlyContinue | ForEach-Object {
    Copy-Required $_.FullName (Join-Path $stage $_.Name)
}
$sdl2Root = Get-Sdl2Root
Copy-Tree (Join-Path $sdl2Root 'include') (Join-Path $stage 'third_party\sdl2\include')
Copy-Tree (Join-Path $sdl2Root 'lib\x64') (Join-Path $stage 'third_party\sdl2\lib\x64')
# SDL's import library is required by packaged native-script builds, but its
# accompanying debug-symbol files are neither loaded nor useful to an end
# user. Keep the SDK link input and remove only symbol residue from staging.
Get-ChildItem -LiteralPath (Join-Path $stage 'third_party\sdl2\lib\x64') -Filter '*.pdb' -File -ErrorAction SilentlyContinue |
    Remove-Item -Force

$stagePrerequisites = Join-Path $stage 'prerequisites'
foreach ($name in $prerequisites.Keys) {
    Copy-Required $prerequisites[$name].FullName (Join-Path $stagePrerequisites $name)
}

Assert-PortableExecutable (Join-Path $stage 'hub.exe') 'Hub executable'
Assert-PortableExecutable (Join-Path $stage 'editor.exe') 'Editor executable'
Assert-IdenticalFile (Join-Path $hubRelease 'hub.exe') (Join-Path $stage 'hub.exe') 'Hub executable'
Assert-IdenticalFile (Join-Path $editorRelease 'editor.exe') (Join-Path $stage 'editor.exe') 'Editor executable'
foreach ($required in @('SDL2.dll', 'editor_symbols.map', 'shaders\sprite.vert.spv', 'shaders\sprite.frag.spv',
                        'assets\fonts\default.ttf', 'templates\abyss-of-hollows\project.json',
                        'build\scripts_module_fast\abyss_of_hollows\Release',
                        'engine_cpp\engine\core.cpp', 'editor\CMakeLists.txt', 'editor\src\editor_state.hpp', 'editor\third_party\imgui\imgui.h', 'editor\scripts_module\CMakeLists.txt',
                        'cmake\msbuild_host_x64.props',
                        'nlohmann\json.hpp', 'third_party\sdl2\include\SDL2\SDL.h',
                        'third_party\sdl2\lib\x64\SDL2.lib')) {
    if (-not (Test-Path -LiteralPath (Join-Path $stage $required))) {
        throw "Required installer payload is missing after staging: $required"
    }
}
foreach ($name in $prerequisites.Keys) {
    Assert-PortableExecutable (Join-Path $stagePrerequisites $name) "prerequisite $name"
}
Assert-ReleaseStage $stage

Write-PrerequisiteMetadata $stage $prerequisites
if (-not $isccCandidates) {
    throw 'Inno Setup 6 was not found. Install it, then rerun scripts\build_installer.ps1.'
}
& $isccCandidates[0] (Join-Path $root 'installer\GameEngine2DPro.iss')
if ($LASTEXITCODE -ne 0) { throw 'Inno Setup compilation failed.' }
Write-Host "Installer created under $(Join-Path $root 'dist')"
