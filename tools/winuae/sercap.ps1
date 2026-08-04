# Collect the Amiga's serial output from WinUAE and write it to a file.
#
# WinUAE cannot write the emulated serial port to a file: the only backends it
# has are a real COM port, an inter-process pipe, a loopback, and TCP.  So the
# config points the serial port at TCP://0.0.0.0:<port>/wait, WinUAE listens,
# and blocks the emulation until something connects, and this script is the
# something.  /wait is what makes the capture race-free: no character can be
# transmitted before we are attached, so the first line of the guest's log is
# never lost.
#
# The socket closes when WinUAE exits, which ends the read loop by itself.
#
# SPDX-License-Identifier: MIT

param(
    [int]$Port = 11111,
    [string]$Out = "C:\aminetxduo\run\serial.log",
    [int]$Wait = 60
)

# Truncate first: a stale log from the previous run reads exactly like output
# from this one.
[System.IO.File]::WriteAllBytes($Out, @())

$sw = [System.Diagnostics.Stopwatch]::StartNew()
$client = $null
while ($sw.Elapsed.TotalSeconds -lt $Wait -and -not $client) {
    try {
        $client = New-Object System.Net.Sockets.TcpClient("127.0.0.1", $Port)
    } catch {
        Start-Sleep -Milliseconds 250
    }
}
if (-not $client) {
    # Say so in the log itself.  An empty serial log is ambiguous, it could
    # mean the guest printed nothing, and this failure is not that.
    [System.IO.File]::WriteAllText($Out, "(serial capture never connected to port $Port)`n")
    exit 1
}

$stream = $client.GetStream()
# FileShare.Read so the runner can read the log while it is still being written.
$fs = [System.IO.File]::Open($Out, 'Create', 'Write', 'Read')
$buf = New-Object byte[] 4096
try {
    while ($true) {
        $n = $stream.Read($buf, 0, $buf.Length)
        if ($n -le 0) { break }
        $fs.Write($buf, 0, $n)
        $fs.Flush()
    }
} catch {
    # WinUAE being killed shows up as a reset connection.  That is the normal
    # end of a timed-out run, not an error worth failing on.
}
$fs.Close()
$client.Close()
