# Copyright (c) Meta Platforms, Inc. and affiliates.
#
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
# You may obtain a copy of the License at
#
#     http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS,
# WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
# See the License for the specific language governing permissions and
# limitations under the License.

<#
.SYNOPSIS
  Build, flash and monitor Muse on a Waveshare ESP32-S3-Touch-AMOLED-1.8 from
  Windows PowerShell (the counterpart of `tools/muse/board.sh ... s318`).

.EXAMPLE
  # From an ESP-IDF v6.0.1 PowerShell prompt, in the repo's esp32 folder:
  .\tools\muse\board-s318.ps1 ports            # list serial ports (no reset)
  .\tools\muse\board-s318.ps1 backup           # save the factory firmware first
  .\tools\muse\board-s318.ps1 token            # set your SDK token (hidden input)
  .\tools\muse\board-s318.ps1 build
  .\tools\muse\board-s318.ps1 flash -Port COM5
  .\tools\muse\board-s318.ps1 monitor -Port COM5

.NOTES
  The SDK token is written only to build-muse-waveshare-s3-18\sdkconfig, which
  git ignores. It is never printed. Set $env:MUSE_SDK_TOKEN to skip the prompt.
#>
[CmdletBinding()]
param(
    [Parameter(Position = 0, Mandatory = $true)]
    [ValidateSet('ports', 'backup', 'token', 'build', 'flash', 'monitor', 'flash-monitor', 'menuconfig', 'erase', 'clean')]
    [string]$Command,
    [string]$Port,
    [int]$Baud = 460800,
    # Re-download managed components and rebuild everything (needed only
    # after building another board in this checkout).
    [switch]$Fresh
)

$ErrorActionPreference = 'Stop'
$Root = Resolve-Path (Join-Path $PSScriptRoot '..\..')
Set-Location $Root

$BoardProfile = 'waveshare-s3-18'
$Target = 'esp32s3'
$B = "build-muse-$BoardProfile"
$Sdkconfig = Join-Path $B 'sdkconfig'
$IdfArgs = @(
    '-B', $B,
    "-DIDF_TARGET=$Target",
    "-DSDKCONFIG=$B/sdkconfig",
    "-DSDKCONFIG_DEFAULTS=sdkconfig.defaults;devices/sdkconfig.muse;devices/sdkconfig.muse-$BoardProfile"
)

function Need-Idf {
    if (-not (Get-Command idf.py -ErrorAction SilentlyContinue)) {
        throw "idf.py not found. Open the 'ESP-IDF 6.0 PowerShell' shortcut (or run your ESP-IDF export.ps1) first."
    }
    $v = (& idf.py --version 2>$null) -join ' '
    if ($v -notmatch 'v6\.0\.1') {
        Write-Warning "This SDK supports ESP-IDF v6.0.1 only; this shell has: $v"
        Write-Warning "Errors about missing headers such as gcm.h or gpio_ll.h usually mean the wrong version."
    }
}

function Find-Port {
    if ($Port) { return $Port }
    # The S3's own USB Serial/JTAG: VID 303A, PID 1001.
    $hits = Get-CimInstance Win32_PnPEntity |
        Where-Object { $_.PNPDeviceID -match 'VID_303A&PID_1001' -and $_.Name -match '\((COM\d+)\)' } |
        ForEach-Object { if ($_.Name -match '\((COM\d+)\)') { $Matches[1] } }
    $hits = @($hits)
    if ($hits.Count -eq 1) { Write-Host "Using $($hits[0]) (Espressif USB Serial/JTAG)"; return $hits[0] }
    if ($hits.Count -eq 0) {
        throw "No Espressif USB Serial/JTAG port found. Use a data cable; if it still doesn't show, hold BOOT, tap RESET, release BOOT."
    }
    throw "Several Espressif ports ($($hits -join ', ')); pass -Port COMx."
}

function Set-Token {
    if (-not (Test-Path $Sdkconfig)) {
        Write-Host "Generating $Sdkconfig ..."
        & idf.py @IdfArgs reconfigure | Out-Null
        if ($LASTEXITCODE) { throw "reconfigure failed" }
    }
    $tok = $env:MUSE_SDK_TOKEN
    if (-not $tok) {
        $sec = Read-Host -AsSecureString 'Muse SDK token (mgst_..., hidden)'
        $tok = [Runtime.InteropServices.Marshal]::PtrToStringBSTR(
            [Runtime.InteropServices.Marshal]::SecureStringToBSTR($sec))
    }
    $tok = $tok.Trim()
    if ($tok -notmatch '^mgst_\S+$') { throw "That doesn't look like an SDK token (it should start with mgst_)." }
    $lines = Get-Content $Sdkconfig
    $line = "CONFIG_GADGET_SDK_TOKEN=`"$tok`""
    if ($lines -match '^CONFIG_GADGET_SDK_TOKEN=') {
        $lines = $lines -replace '^CONFIG_GADGET_SDK_TOKEN=.*$', $line
    } else {
        $lines += $line
    }
    # UTF-8 without BOM and LF endings, as Kconfig writes it.
    [IO.File]::WriteAllText((Resolve-Path $Sdkconfig), (($lines -join "`n") + "`n"), (New-Object Text.UTF8Encoding $false))
    Write-Host ("Token set ({0}...{1}) in {2}" -f $tok.Substring(0, 5), $tok.Substring($tok.Length - 3), $Sdkconfig)
}

function Has-Token {
    (Test-Path $Sdkconfig) -and ((Get-Content $Sdkconfig) -match '^CONFIG_GADGET_SDK_TOKEN="mgst_')
}

function Clean-Managed {
    # Boards resolve different component sets into one managed_components/,
    # so start each board build clean (as board.sh does).
    foreach ($p in 'managed_components', 'dependencies.lock') {
        if (Test-Path $p) { Remove-Item -Recurse -Force $p }
    }
}

switch ($Command) {
    'ports' {
        Get-CimInstance Win32_PnPEntity | Where-Object { $_.Name -match '\(COM\d+\)' } |
            Select-Object Name, @{ n = 'USB'; e = { if ($_.PNPDeviceID -match 'VID_([0-9A-F]{4})&PID_([0-9A-F]{4})') { "$($Matches[1]):$($Matches[2])" } } } |
            Format-Table -AutoSize
    }
    'backup' {
        Need-Idf
        $p = Find-Port
        $out = "s3-amoled-1.8-factory-$(Get-Date -Format yyyyMMdd-HHmmss).bin"
        Write-Host "Reading all 16 MB to $out (a few minutes)..."
        & python -m esptool --chip $Target -p $p -b $Baud read-flash 0 0x1000000 $out
        if ($LASTEXITCODE) { throw "backup failed" }
        Write-Host "Saved $out. To restore Waveshare's firmware: python -m esptool --chip $Target -p $p write-flash 0 $out"
    }
    'token' { Need-Idf; Set-Token }
    'menuconfig' { Need-Idf; & idf.py @IdfArgs menuconfig }
    'build' {
        Need-Idf
        if (-not (Has-Token)) {
            Write-Warning "No SDK token in $Sdkconfig yet."
            Set-Token
        }
        # managed_components/ is shared by every board. Wiping it forces a
        # re-download and a full rebuild, so only do it when it was resolved
        # for another board (or -Fresh).
        $lock = 'dependencies.lock'
        if ($Fresh -or ((Test-Path $lock) -and -not (Select-String -Quiet 'esp32_s3_touch_amoled_1_8' $lock))) {
            Write-Host 'Components were resolved for another board: starting clean.'
            Clean-Managed
        }
        # ccache (installed with ESP-IDF's tools) makes rebuilds of unchanged files nearly free.
        $ccache = @()
        if (Get-Command ccache -ErrorAction SilentlyContinue) { $ccache = @('--ccache') }
        & idf.py @ccache @IdfArgs build
        if ($LASTEXITCODE) { throw "build failed" }
        Write-Host "Built $B\muse-gadget.bin"
    }
    'flash' {
        Need-Idf
        $p = Find-Port
        & idf.py @IdfArgs -p $p -b $Baud flash
        if ($LASTEXITCODE) { throw "flash failed (hold BOOT, tap RESET, release BOOT, and retry)" }
    }
    'monitor' { Need-Idf; $p = Find-Port; & idf.py @IdfArgs -p $p monitor }
    'flash-monitor' {
        Need-Idf
        $p = Find-Port
        & idf.py @IdfArgs -p $p -b $Baud flash monitor
    }
    'erase' {
        Need-Idf
        $p = Find-Port
        $ok = Read-Host "Erase ALL flash on $p, including pairing and Wi-Fi? Type yes"
        if ($ok -eq 'yes') { & idf.py @IdfArgs -p $p erase-flash }
    }
    'clean' { Clean-Managed; if (Test-Path $B) { Remove-Item -Recurse -Force $B }; Write-Host "Removed $B (and your token with it)." }
}
