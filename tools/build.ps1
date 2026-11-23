# SPDX-License-Identifier: Apache-2.0
<#
.SYNOPSIS
  Build (and optionally flash) the firmware with regular status lines.

.DESCRIPTION
  Wraps `idf.py build` and prints a heartbeat line every -ReportSeconds with the ninja step counter, percent done, elapsed
  time and the file being compiled, so a long background build can be watched from its output. Errors are echoed the moment
  they appear. Ends with one DONE line (ok or FAILED, exit code, elapsed time, image size). Everything else idf.py prints
  goes to a log file next to the status file (-LogFile), not to the console.

  Run it from a shell where ESP-IDF is already exported (export.ps1), and set the build directory with -BuildDir or the
  KNOB_BUILD_DIR environment variable. Keep the build directory outside the source tree.

  Heartbeats are driven by build output, so a long silent step (the cmake configure, about 100 s on a cold tree) shows no
  heartbeat until it ends. A "[build] started" line is printed first so that is not mistaken for a hang.

.EXAMPLE
  .\tools\build.ps1 -BuildDir D:\build\knob
  .\tools\build.ps1 -BuildDir D:\build\knob -Flash -Port COM5 -ReportSeconds 10
#>
param(
    [string]$BuildDir = $env:KNOB_BUILD_DIR,
    [int]$ReportSeconds = 20,
    [switch]$Flash,
    [string]$Port,
    [string]$LogFile,
    [string]$StatusFile
)

$ErrorActionPreference = 'Stop'
if (-not $BuildDir) { throw 'Set -BuildDir or the KNOB_BUILD_DIR environment variable.' }
if (-not (Get-Command idf.py -ErrorAction SilentlyContinue)) { throw 'idf.py not found. Export ESP-IDF first (export.ps1).' }
if ($Flash -and -not $Port) { throw '-Flash needs -Port.' }
if (-not $LogFile) { $LogFile = Join-Path $BuildDir 'build-status.log' }
if (-not $StatusFile) { $StatusFile = Join-Path $BuildDir 'build-status.txt' }

$firmware = Join-Path (Split-Path -Parent $PSScriptRoot) 'firmware'
$clock = [System.Diagnostics.Stopwatch]::StartNew()

function Write-Status([string]$text) {
    $line = '[build] {0:mm\:ss} {1}' -f $clock.Elapsed, $text
    Write-Host $line
    try { Set-Content -Path $StatusFile -Value $line -Encoding ascii } catch { }
}

function Invoke-Idf([string[]]$IdfArgs, [string]$Label) {
    # Windows PowerShell 5.1 turns any stderr line of a native command into an error record when stderr is merged
    # (2>&1). idf.py prints harmless notes on stderr, so do not stop on them. The exit code decides.
    $ErrorActionPreference = 'Continue'
    $script:step = 0; $script:total = 0; $script:last = ''
    $lastReport = [System.Diagnostics.Stopwatch]::StartNew()
    Write-Status "$Label started"
    Push-Location $firmware
    try {
        & idf.py -B $BuildDir @IdfArgs 2>&1 | ForEach-Object {
            $l = "$_"
            Add-Content -Path $LogFile -Value $l
            if ($l -match '^\[(\d+)/(\d+)\]\s+(.*)$') {
                $script:step = [int]$Matches[1]; $script:total = [int]$Matches[2]
                $script:last = ($Matches[3] -replace '^(Building|Linking|Generating)\s+\w*\s*', '$1 ')
                if ($script:last.Length -gt 60) { $script:last = $script:last.Substring(0, 60) }
            }
            if ($l -match '(\berror\b:|\bFAILED:|undefined reference|CMake Error)') {
                Write-Status ("ERROR " + $(if ($l.Length -gt 160) { $l.Substring(0, 160) + '...' } else { $l }))
            }
            if ($lastReport.Elapsed.TotalSeconds -ge $ReportSeconds) {
                $lastReport.Restart()
                $pct = if ($script:total) { [int](100 * $script:step / $script:total) } else { 0 }
                Write-Status ("{0} {1}/{2} ({3}%) {4}" -f $Label, $script:step, $script:total, $pct, $script:last)
            }
        }
        return $LASTEXITCODE
    } finally { Pop-Location }
}

New-Item -ItemType Directory -Force -Path $BuildDir | Out-Null
Set-Content -Path $LogFile -Value ''
$rc = Invoke-Idf @('build') 'build'
if ($rc -eq 0) {
    $bin = Get-ChildItem -Path $BuildDir -Filter '*.bin' | Where-Object { $_.Name -notmatch 'ota_data|storage|bootloader|partition|flashed' } |
        Sort-Object Length -Descending | Select-Object -First 1
    $size = if ($bin) { '{0} {1:N0} bytes' -f $bin.Name, $bin.Length } else { '' }
    Write-Status "DONE build ok, $size"
    if ($Flash) {
        $rc = Invoke-Idf @('-p', $Port, 'flash') 'flash'
        if ($rc -eq 0) {
            # A 1200 baud reboot leaves the chip's force-download flag set, and the normal reset after flashing keeps it in
            # the bootloader. A watchdog reset clears the flag and starts the firmware.
            $ErrorActionPreference = 'Continue'
            & python -m esptool --chip esp32s3 -p $Port --after watchdog-reset read-mac 2>&1 | Add-Content -Path $LogFile
            $ErrorActionPreference = 'Stop'
            Write-Status "DONE flash ok on $Port, watchdog reset sent"
        } else { Write-Status "DONE flash FAILED exit $rc, see $LogFile" }
    }
} else {
    Write-Status "DONE build FAILED exit $rc, see $LogFile"
}
exit $rc
