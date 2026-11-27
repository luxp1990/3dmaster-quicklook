$ErrorActionPreference = "Stop"

$scriptDir = Split-Path -Parent $MyInvocation.MyCommand.Definition
$projectRoot = (Resolve-Path (Join-Path $scriptDir "..")).Path
$csharpReleaseDir = Join-Path $scriptDir "bin\Release"
$previewExe = Join-Path $projectRoot "3dmaster-preview\build\Release\3dmaster-preview.exe"

$qtBin = Join-Path $projectRoot "qt6\6.8.2\msvc2022_64\bin"
if (-not (Test-Path $qtBin) -and $env:QT_ROOT_DIR) { $qtBin = Join-Path $env:QT_ROOT_DIR "bin" }
if (-not (Test-Path $qtBin)) { $qtBin = "D:\seer\3dmaster\qt6\6.8.2\msvc2022_64\bin" }

$qtPlugins = Join-Path $projectRoot "qt6\6.8.2\msvc2022_64\plugins"
if (-not (Test-Path $qtPlugins) -and $env:QT_ROOT_DIR) { $qtPlugins = Join-Path $env:QT_ROOT_DIR "plugins" }
if (-not (Test-Path $qtPlugins)) { $qtPlugins = "D:\seer\3dmaster\qt6\6.8.2\msvc2022_64\plugins" }

$occtBin = Join-Path $projectRoot "occt\win64\vc14\bin"
if (-not (Test-Path $occtBin)) { $occtBin = "D:\seer\3dmaster\occt\win64\vc14\bin" }

$distDir = Join-Path $projectRoot "dist\QuickLook.Plugin.ThreeDMaster"
$outputQlPlugin = Join-Path $projectRoot "QuickLook.Plugin.ThreeDMaster.qlplugin"
$outputZip = Join-Path $projectRoot "QuickLook.Plugin.ThreeDMaster_Portable.zip"

Write-Host "=========================================================="
Write-Host "  Building 3dmaster QuickLook 100% Self-Contained Package"
Write-Host "=========================================================="

if (-not (Test-Path $previewExe)) {
    throw "Missing 3dmaster-preview.exe at $previewExe"
}

Write-Host "[1/6] Compiling QuickLook C# plugin project..."
Push-Location $scriptDir
dotnet build QuickLook.Plugin.3DMaster.csproj -c Release
Pop-Location

if (Test-Path $distDir) {
    Remove-Item $distDir -Recurse -Force
}
New-Item -ItemType Directory -Path "$distDir\platforms" -Force | Out-Null
New-Item -ItemType Directory -Path "$distDir\styles" -Force | Out-Null
New-Item -ItemType Directory -Path "$distDir\imageformats" -Force | Out-Null

Write-Host "[2/6] Copying QuickLook C# plugin files & 3dmaster-preview.exe..."
Get-ChildItem -Path $csharpReleaseDir -Exclude *.pdb,*.xml,QuickLook.Common.dll | ForEach-Object {
    Copy-Item $_.FullName "$distDir\" -Force
}
Copy-Item $previewExe "$distDir\" -Force

Write-Host "[3/6] Locating Microsoft Visual C++ 2015-2022 (v143) Runtime directory..."
$vswhere = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
$vcRedistDir = "C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Redist\MSVC\14.44.35112\x64\Microsoft.VC143.CRT"
if (-not (Test-Path $vcRedistDir)) {
    $vcRedistRoots = @("C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Redist\MSVC")
    if (Test-Path $vswhere) {
        $vsInstall = & $vswhere -latest -products * -property installationPath 2>$null
        if ($vsInstall) {
            $vcRedistRoots = @((Join-Path $vsInstall "VC\Redist\MSVC")) + $vcRedistRoots
        }
    }
    foreach ($root in $vcRedistRoots) {
        if (Test-Path $root) {
            $found = Get-ChildItem -Path $root -Recurse -Filter "msvcp140.dll" -ErrorAction SilentlyContinue |
                Where-Object { $_.FullName -like "*x64\Microsoft.VC14*.CRT*" } |
                Select-Object -First 1
            if ($found) {
                $vcRedistDir = $found.DirectoryName
                break
            }
        }
    }
}
if (Test-Path $vcRedistDir) {
    Write-Host "  -> Found MSVC CRT redist directory: $vcRedistDir (closure members will be copied in step [5/6])"
} else {
    $vcRedistDir = $null
    Write-Warning "Could not find MSVC CRT redist directory! Target machine may require VC++ runtime."
}

Write-Host "[4/6] Copying Qt 6 runtime, plugins, and OpenGL/Direct3D fallback..."
$qtDlls = @(
    "Qt6Core.dll", "Qt6Gui.dll", "Qt6Widgets.dll", "Qt6OpenGL.dll", "Qt6OpenGLWidgets.dll", "Qt6Network.dll",
    "d3dcompiler_47.dll", "opengl32sw.dll"
)
foreach ($q in $qtDlls) {
    $src = Join-Path $qtBin $q
    if (Test-Path $src) {
        Copy-Item $src "$distDir\" -Force
    }
}
# Qt 平台插件
Copy-Item (Join-Path $qtPlugins "platforms\qwindows.dll") "$distDir\platforms\" -Force
# Qt 视觉样式与图片插件（防换机无桌面主题与图标加载失败）
if (Test-Path (Join-Path $qtPlugins "styles\qmodernwindowsstyle.dll")) {
    Copy-Item (Join-Path $qtPlugins "styles\qmodernwindowsstyle.dll") "$distDir\styles\" -Force
}
@("qjpeg.dll", "qpng.dll", "qsvg.dll") | ForEach-Object {
    $imgDll = Join-Path $qtPlugins "imageformats\$_"
    if (Test-Path $imgDll) { Copy-Item $imgDll "$distDir\imageformats\" -Force }
}

# 写入 qt.conf 确保完全绿色独立
Set-Content -Path "$distDir\qt.conf" -Value "[Paths]`nPrefix=.`nPlugins=.`n" -Encoding ASCII

Write-Host "[5/6] Computing transitive PE import closure via dumpbin..."
if (-not (Test-Path $vswhere)) {
    throw "vswhere.exe not found at $vswhere. Cannot locate dumpbin.exe for dependency-closure copy."
}
$vsPath = & $vswhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
if (-not $vsPath) {
    $vsPath = & $vswhere -latest -products * -property installationPath
}
if (-not $vsPath) {
    throw "Visual Studio installation not found via vswhere. Cannot locate dumpbin.exe."
}
$dumpbin = Get-ChildItem -Path (Join-Path $vsPath "VC\Tools\MSVC") -Recurse -Filter "dumpbin.exe" -ErrorAction SilentlyContinue |
    Where-Object { $_.FullName -match '\\Hostx64\\x64\\dumpbin\.exe$' } |
    Select-Object -First 1 -ExpandProperty FullName
if (-not $dumpbin) {
    $dumpbin = Get-ChildItem -Path (Join-Path $vsPath "VC\Tools\MSVC") -Recurse -Filter "dumpbin.exe" -ErrorAction SilentlyContinue |
        Select-Object -First 1 -ExpandProperty FullName
}
if (-not $dumpbin -or -not (Test-Path $dumpbin)) {
    throw "dumpbin.exe not found under $vsPath. Refusing to fall back to copy-all."
}
Write-Host "  -> Using dumpbin: $dumpbin"

$searchDirs = @()
if (Test-Path $occtBin) { $searchDirs += $occtBin }
if (Test-Path $qtBin) { $searchDirs += $qtBin }
if ($vcRedistDir -and (Test-Path $vcRedistDir)) { $searchDirs += $vcRedistDir }

$queue = New-Object System.Collections.Generic.Queue[string]
$visited = New-Object 'System.Collections.Generic.HashSet[string]' ([System.StringComparer]::OrdinalIgnoreCase)

# Seed closure with 3dmaster-preview.exe, staged C# plugin assemblies, and staged Qt binaries/plugins
Get-ChildItem -Path $distDir -Recurse -File -Include *.exe,*.dll | ForEach-Object {
    if ($visited.Add($_.Name)) {
        $queue.Enqueue($_.FullName)
    }
}

$copiedClosureCount = 0
while ($queue.Count -gt 0) {
    $currentPe = $queue.Dequeue()
    $dumpOutput = & $dumpbin /nologo /dependents $currentPe
    if ($LASTEXITCODE -ne 0) {
        throw "dumpbin /dependents failed for $currentPe (exit code $LASTEXITCODE)"
    }
    foreach ($line in $dumpOutput) {
        if ($line -match '^\s+([A-Za-z0-9_.\-+]+\.(?:dll|exe))\s*$') {
            $depName = $Matches[1]
            if (-not $visited.Add($depName)) { continue }

            $stagedTarget = Join-Path $distDir $depName
            if (Test-Path $stagedTarget) {
                $queue.Enqueue($stagedTarget)
                continue
            }

            foreach ($dir in $searchDirs) {
                $candidate = Join-Path $dir $depName
                if (Test-Path $candidate) {
                    Copy-Item $candidate "$distDir\" -Force
                    $copiedClosureCount++
                    $queue.Enqueue((Join-Path $distDir $depName))
                    break
                }
            }
        }
    }
}
Write-Host "  -> Copied $copiedClosureCount closure member(s) from OCCT / Qt / VC CRT directories."

Write-Host "[Gate] Verifying PE import tables in dist for debug CRTs..."
$forbiddenDebugCrts = @('vcruntime140d.dll', 'msvcp140d.dll', 'ucrtbased.dll')
$offenders = @()
Get-ChildItem -Path $distDir -Recurse -File -Include *.dll,*.exe | Sort-Object FullName | ForEach-Object {
    $peFile = $_
    $importsOutput = & $dumpbin /nologo /imports $peFile.FullName
    if ($LASTEXITCODE -ne 0) {
        throw "dumpbin /imports failed for $($peFile.FullName) (exit code $LASTEXITCODE)"
    }
    $matchedCrts = New-Object System.Collections.Generic.HashSet[string] ([System.StringComparer]::OrdinalIgnoreCase)
    foreach ($line in $importsOutput) {
        if ($line -match '^\s{4}([A-Za-z0-9_.\-+]+\.dll)\s*$') {
            $importedDll = $Matches[1]
            if ($forbiddenDebugCrts -contains $importedDll.ToLowerInvariant()) {
                [void]$matchedCrts.Add($importedDll)
            }
        }
    }
    if ($matchedCrts.Count -gt 0) {
        $rel = $peFile.FullName.Substring($distDir.Length).TrimStart('\', '/')
        $offenders += "  $rel -> $(($matchedCrts | Sort-Object) -join ', ')"
    }
}
if ($offenders.Count -gt 0) {
    Write-Host "ERROR: Debug CRT imports detected in staged dist binaries:" -ForegroundColor Red
    $offenders | ForEach-Object { Write-Host $_ -ForegroundColor Red }
    throw "Debug CRT import gate failed! Found $($offenders.Count) offender(s) in $distDir."
}
Write-Host "  -> Gate passed: 0 debug CRT imports across all .dll/.exe in dist."

Write-Host "[6/6] Generating archives with .NET ZipFile API..."
Add-Type -AssemblyName System.IO.Compression.FileSystem

if (Test-Path $outputQlPlugin) { Remove-Item $outputQlPlugin -Force }
[System.IO.Compression.ZipFile]::CreateFromDirectory($distDir, $outputQlPlugin, [System.IO.Compression.CompressionLevel]::Optimal, $false)

if (Test-Path $outputZip) { Remove-Item $outputZip -Force }
[System.IO.Compression.ZipFile]::CreateFromDirectory($distDir, $outputZip, [System.IO.Compression.CompressionLevel]::Optimal, $true)

$qlSizeMB = [math]::Round((Get-Item $outputQlPlugin).Length / 1MB, 2)
$zipSizeMB = [math]::Round((Get-Item $outputZip).Length / 1MB, 2)

Write-Host "=========================================================="
Write-Host "Packaging Complete! (100% Self-Contained)"
Write-Host "1. QuickLook One-Click Plugin: $outputQlPlugin ($qlSizeMB MB)"
Write-Host "2. Portable Dist Directory:    $distDir"
Write-Host "3. Portable Zip Archive:       $outputZip ($zipSizeMB MB)"
Write-Host "=========================================================="
