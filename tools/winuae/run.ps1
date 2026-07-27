# Windows side of tools/winuae-run.sh: start WinUAE, wait for the guest, reap.
#
# WHY PSEXEC.  WinUAE is a GUI application.  It creates a window and a D3D
# device even with headless=true (that option only hides the window -- it does
# not remove it), and from an SSH session, which lands in session 0, that init
# never completes: the process sits there after "Windowsmouse initialization"
# and has to be killed.  PsExec -i <session> launches it into the interactive
# console session instead, where a real display adapter exists, and there it
# runs.  Measured on winbuilder: session 0 hangs every time, session 1 boots
# Kickstart and runs the guest in about seven seconds.
#
# HOW A RUN ENDS.  Three ways, in order of preference:
#   * the guest's Startup-Sequence runs UAEquit, WinUAE exits by itself, and
#     its log is flushed properly -- this is the normal path;
#   * DH0:.done appears and we kill the emulator (UAEquit missing or refused);
#   * the timeout expires and we kill it.
# The exit status always comes from DH0:.done, never from the emulator, for the
# same reason FS-UAE's harness does it that way: WinUAE's own exit code says
# nothing about the program under test.
#
# SPDX-License-Identifier: MIT

param(
    [Parameter(Mandatory = $true)][string]$Config,
    [Parameter(Mandatory = $true)][string]$Hd,
    [int]$Timeout = 180,
    [int]$SerialPort = 0,
    [string]$Serial = "",
    [string]$WinUAE = "C:\Program Files\WinUAE\winuae64.exe",
    [string]$PsExec = "C:\aminetxduo\pstools\PsExec64.exe",
    [string]$Tools = "C:\aminetxduo\tools",
    [int]$Session = 1,
    [int]$LockWait = 1200
)

$ErrorActionPreference = "Continue"

# ONE EMULATOR AT A TIME.  There is a single interactive console session on
# this box and a single serial TCP port, so two concurrent runs fight over both
# and one of them dies in a way that looks exactly like a crash in the code
# under test -- the failure mode that already cost this project time under
# FS-UAE.  A named mutex is the right primitive on Windows because the kernel
# releases it when the holder dies, so a killed run cannot wedge the queue.
$mutex = New-Object System.Threading.Mutex($false, "Global\AmiNetXDuoWinUAE")
$held = $false
try { $held = $mutex.WaitOne($LockWait * 1000) }
catch [System.Threading.AbandonedMutexException] { $held = $true }
if (-not $held) { Write-Output "WINUAE-RESULT reason=busy rc="; exit 2 }

$done = Join-Path $Hd ".done"
Remove-Item -Force -ErrorAction SilentlyContinue $done

if (-not (Test-Path $WinUAE)) { Write-Output "WINUAE-RESULT reason=nowinuae rc="; exit 2 }
if (-not (Test-Path $PsExec)) { Write-Output "WINUAE-RESULT reason=nopsexec rc="; exit 2 }

if ($SerialPort -gt 0 -and $Serial -ne "") {
    Start-Process -FilePath "powershell" -WindowStyle Hidden -ArgumentList @(
        "-NoProfile", "-ExecutionPolicy", "Bypass",
        "-File", (Join-Path $Tools "sercap.ps1"),
        "-Port", $SerialPort, "-Out", $Serial, "-Wait", ($Timeout + 10)) | Out-Null
}

# Identify our own emulator by elimination rather than by parsing PsExec's
# chatter: PsExec prints the pid to stderr mixed with its progress messages,
# and stderr from a native command is awkward to capture reliably in
# PowerShell.  Anything that was not running before the launch is ours.
$before = @(Get-Process winuae64 -ErrorAction SilentlyContinue | ForEach-Object { $_.Id })

# -ArgumentList joins with spaces and does NOT quote, so the one path with a
# space in it gets its quotes here.
Start-Process -FilePath $PsExec -NoNewWindow -Wait -ArgumentList @(
    "-accepteula", "-nobanner", "-i", $Session, "-d", ('"' + $WinUAE + '"'),
    "-f", $Config) 2>$null | Out-Null

$emu = $null
$spin = [System.Diagnostics.Stopwatch]::StartNew()
while ($spin.Elapsed.TotalSeconds -lt 15 -and -not $emu) {
    $emu = Get-Process winuae64 -ErrorAction SilentlyContinue |
           Where-Object { $before -notcontains $_.Id } | Select-Object -First 1
    if (-not $emu) { Start-Sleep -Milliseconds 200 }
}
if (-not $emu) { Write-Output "WINUAE-RESULT reason=nostart rc="; exit 2 }

$reason = "timeout"
$sw = [System.Diagnostics.Stopwatch]::StartNew()
while ($sw.Elapsed.TotalSeconds -lt $Timeout) {
    if (Test-Path $done) { $reason = "done"; break }
    if ($emu.HasExited) { $reason = "quit"; break }
    Start-Sleep -Milliseconds 500
}

# UAEquit runs immediately after the guest writes DH0:.done, so on the "quit"
# path the file may still be settling when the process disappears.  Give the
# directory filesystem a moment before deciding it was never written.
if ($reason -eq "quit" -and -not (Test-Path $done)) { Start-Sleep -Milliseconds 1500 }

if (-not $emu.HasExited) {
    Stop-Process -Id $emu.Id -Force -ErrorAction SilentlyContinue
    # A killed emulator leaves the serial collector blocked on a socket that
    # will never close cleanly; it times out on its own, but not before the
    # caller has copied the log back. Give the reset a moment to land.
    Start-Sleep -Milliseconds 750
}

$rc = ""
if (Test-Path $done) { $rc = (Get-Content -Raw $done).Trim() }
Write-Output ("WINUAE-RESULT reason={0} rc={1} seconds={2:N1}" -f $reason, $rc, $sw.Elapsed.TotalSeconds)

$mutex.ReleaseMutex()
