# Regenerate the DXIL byte-array headers under video/filter/rife_shaders/.
# Run from a Windows dev box that has dxc.exe available (Windows SDK or Vulkan
# SDK). The committed *.dxil.h files are the source of truth at build time;
# meson does NOT invoke dxc, so CI does not need any HLSL toolchain.
#
# Usage:
#   pwsh ./video/filter/rife_shaders/regen.ps1
#   pwsh ./video/filter/rife_shaders/regen.ps1 -Dxc "C:\path\to\dxc.exe"

param(
    [string]$Dxc = ""
)

$ErrorActionPreference = "Stop"

if (-not $Dxc) {
    foreach ($cand in @(
        "C:\VulkanSDK\1.4.341.1\Bin\dxc.exe",
        "C:\Program Files (x86)\Windows Kits\10\bin\10.0.22621.0\x64\dxc.exe",
        "C:\Program Files (x86)\Windows Kits\10\bin\10.0.26100.0\x64\dxc.exe"
    )) {
        if (Test-Path $cand) { $Dxc = $cand; break }
    }
    if (-not $Dxc) {
        $cmd = Get-Command dxc.exe -ErrorAction SilentlyContinue
        if ($cmd) { $Dxc = $cmd.Source }
    }
}
if (-not (Test-Path $Dxc)) {
    throw "dxc.exe not found. Pass -Dxc <path>."
}
Write-Host "Using dxc: $Dxc"

$dir = $PSScriptRoot
foreach ($name in @("cs_pack_rgb0","cs_fill_meta","cs_unpack_bgra","cs_copy_rgba_to_bgra","cs_nv12_to_rgba","cs_frame_diff")) {
    $hlsl = Join-Path $dir "$name.hlsl"
    $dxil = Join-Path $dir "$name.dxil"
    & $Dxc -T cs_6_0 -E main -O3 -Fo $dxil $hlsl
    if ($LASTEXITCODE -ne 0) { throw "dxc failed for $name" }

    $bytes = [System.IO.File]::ReadAllBytes($dxil)
    $sb = New-Object System.Text.StringBuilder
    [void]$sb.AppendLine("// Auto-generated from $name.hlsl via dxc -T cs_6_0 -E main -O3")
    [void]$sb.AppendLine("// DO NOT EDIT BY HAND. Regenerate via: video/filter/rife_shaders/regen.ps1")
    [void]$sb.AppendLine("#pragma once")
    [void]$sb.AppendLine("#include <stddef.h>")
    [void]$sb.AppendLine("#include <stdint.h>")
    [void]$sb.AppendLine("")
    [void]$sb.AppendLine("static const uint8_t ${name}_dxil[] = {")
    for ($i = 0; $i -lt $bytes.Length; $i += 16) {
        $line = "    "
        for ($j = 0; $j -lt 16 -and ($i+$j) -lt $bytes.Length; $j++) {
            $line += "0x{0:x2}, " -f $bytes[$i+$j]
        }
        [void]$sb.AppendLine($line)
    }
    [void]$sb.AppendLine("};")
    [void]$sb.AppendLine("static const size_t ${name}_dxil_size = sizeof(${name}_dxil);")
    Set-Content -Path (Join-Path $dir "$name.dxil.h") -Value $sb.ToString() -NoNewline -Encoding ascii
    Remove-Item $dxil
    Write-Host "Wrote $name.dxil.h ($($bytes.Length) bytes of DXIL)"
}
