$ErrorActionPreference = "Stop"

$scriptDir = Split-Path -Parent $MyInvocation.MyCommand.Definition
$projectRoot = (Resolve-Path (Join-Path $scriptDir "..")).Path
$csharpReleaseDir = Join-Path $scriptDir "bin\Release"
$previewExe = Join-Path $projectRoot "3dmaster-preview\build\Release\3dmaster-preview.exe"

$qtBin = Join-Path $projectRoot "qt6\6.8.2\msvc2022_64\bin"
if (-not (Test-Path $qtBin)) { $qtBin = "D:\seer\3dmaster\qt6\6.8.2\msvc2022_64\bin" }

$qtPlugins = Join-Path $projectRoot "qt6\6.8.2\msvc2022_64\plugins"
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

Write-Host "[3/6] Copying Microsoft Visual C++ 2015-2022 (v143) Runtime Closure..."
$vcRedistDir = "C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Redist\MSVC\14.44.35112\x64\Microsoft.VC143.CRT"
if (-not (Test-Path $vcRedistDir)) {
    $found = Get-ChildItem -Path "C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Redist\MSVC" -Recurse -Filter "msvcp140.dll" -ErrorAction SilentlyContinue | Where-Object { $_.FullName -like "*x64\Microsoft.VC14*.CRT*" } | Select-Object -First 1
    if ($found) { $vcRedistDir = $found.DirectoryName }
}
if (Test-Path $vcRedistDir) {
    Get-ChildItem -Path $vcRedistDir -Filter "*.dll" | ForEach-Object {
        Copy-Item $_.FullName "$distDir\" -Force
    }
    Write-Host "  -> Bundled official MSVC CRT runtime files from $vcRedistDir"
} else {
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

Write-Host "[5/6] Copying OpenCASCADE 8.0 & TBB multithreading runtime closure..."
$occtDlls = @(
    "avcodec-57.dll", "avformat-57.dll", "avutil-55.dll", "FreeImage.dll", "freetype.dll",
    "jemalloc.dll", "openvr_api.dll", "swscale-4.dll",
    "tbb12.dll", "tbbmalloc.dll", "tbbmalloc_proxy.dll",
    "zlib.dll", "zlib1.dll", "liblzma.dll",
    "TKBO.dll", "TKBool.dll", "TKBRep.dll", "TKCAF.dll", "TKCDF.dll", "TKDE.dll", "TKDEGLTF.dll",
    "TKDEIGES.dll", "TKDESTEP.dll", "TKernel.dll", "TKG2d.dll", "TKG3d.dll",
    "TKGeomAlgo.dll", "TKGeomBase.dll", "TKHLR.dll", "TKLCAF.dll", "TKMath.dll",
    "TKMesh.dll", "TKPrim.dll", "TKRWMesh.dll", "TKService.dll", "TKShHealing.dll",
    "TKTopAlgo.dll", "TKV3d.dll", "TKVCAF.dll", "TKXCAF.dll", "TKXSBase.dll"
)
foreach ($o in $occtDlls) {
    $src = Join-Path $occtBin $o
    if (-not (Test-Path $src)) { throw "Missing OCCT DLL: $src" }
    Copy-Item $src "$distDir\" -Force
}

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
