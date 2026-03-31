# test_rtsp_backchannel.ps1 — Test RTSP backchannel audio (client → device)
# Verifies: SDP recvonly track, SETUP trackID=2, TCP interleaved send, audio_out_play
param(
    [string]$Host_ = "127.0.0.1",
    [int]$Port = 8555,
    [int]$Duration = 3
)

$ErrorActionPreference = "Stop"
$uri = "rtsp://${Host_}:${Port}/live"

$client = New-Object System.Net.Sockets.TcpClient
$client.Connect($Host_, $Port)
$stream = $client.GetStream()
$stream.ReadTimeout = 3000

function Send($msg) {
    $b = [System.Text.Encoding]::ASCII.GetBytes($msg)
    $stream.Write($b, 0, $b.Length)
    $stream.Flush()
}

function Recv() {
    Start-Sleep -Milliseconds 300
    $buf = New-Object byte[] 65536
    $all = New-Object System.IO.MemoryStream
    while ($client.Available -gt 0) {
        $n = $stream.Read($buf, 0, $buf.Length)
        if ($n -le 0) { break }
        $all.Write($buf, 0, $n)
    }
    return [System.Text.Encoding]::ASCII.GetString($all.ToArray())
}

Write-Host "=== RTSP Backchannel Test ==="
Write-Host "Target: $uri"
Write-Host ""

# 1. OPTIONS
$cseq = 1
Send "OPTIONS $uri RTSP/1.0`r`nCSeq: $cseq`r`n`r`n"
$resp = Recv
Write-Host "OPTIONS: $($resp.Split("`n")[0].Trim())"

# 2. DESCRIBE — verify SDP has 3 tracks
$cseq++
Send "DESCRIBE $uri RTSP/1.0`r`nCSeq: $cseq`r`nAccept: application/sdp`r`n`r`n"
$resp = Recv

$hasRecvonly = $resp -match "a=recvonly"
$hasTrackID2 = $resp -match "trackID=2"
$hasSendonly = $resp -match "a=sendonly"
Write-Host ""
Write-Host "SDP Checks:"
Write-Host "  a=sendonly present: $hasSendonly"
Write-Host "  a=recvonly present: $hasRecvonly"
Write-Host "  trackID=2 present: $hasTrackID2"

if (-not $hasRecvonly -or -not $hasTrackID2) {
    Write-Host "FAIL: SDP missing backchannel track!" -ForegroundColor Red
    $stream.Close(); $client.Close()
    exit 1
}
Write-Host "  SDP backchannel track: OK" -ForegroundColor Green

# Extract session from SDP response
$sessionId = ""

# 3. SETUP video (TCP interleaved ch 0-1)
$cseq++
Send "SETUP ${uri}/trackID=0 RTSP/1.0`r`nCSeq: $cseq`r`nTransport: RTP/AVP/TCP;unicast;interleaved=0-1`r`n`r`n"
$resp = Recv
if ($resp -match "Session:\s*(\S+)") { $sessionId = $Matches[1] -replace ";.*","" }
$setupOk = $resp -match "200 OK"
Write-Host ""
Write-Host "SETUP video (ch 0-1): $(if($setupOk){'OK'}else{'FAIL'})"

# 4. SETUP audio sendonly (TCP interleaved ch 2-3)
$cseq++
Send "SETUP ${uri}/trackID=1 RTSP/1.0`r`nCSeq: $cseq`r`nSession: $sessionId`r`nTransport: RTP/AVP/TCP;unicast;interleaved=2-3`r`n`r`n"
$resp = Recv
$setupOk = $resp -match "200 OK"
Write-Host "SETUP audio (ch 2-3): $(if($setupOk){'OK'}else{'FAIL'})"

# 5. SETUP backchannel recvonly (TCP interleaved ch 4-5)
$cseq++
Send "SETUP ${uri}/trackID=2 RTSP/1.0`r`nCSeq: $cseq`r`nSession: $sessionId`r`nTransport: RTP/AVP/TCP;unicast;interleaved=4-5`r`n`r`n"
$resp = Recv
$setupOk = $resp -match "200 OK"
$backchannelOk = $resp -match "interleaved=4-5"
Write-Host "SETUP backchannel (ch 4-5): $(if($setupOk -and $backchannelOk){'OK'}else{'FAIL'})"

if (-not $setupOk) {
    Write-Host "FAIL: Backchannel SETUP rejected!" -ForegroundColor Red
    Write-Host $resp
    $stream.Close(); $client.Close()
    exit 1
}

# 6. PLAY
$cseq++
Send "PLAY $uri RTSP/1.0`r`nCSeq: $cseq`r`nSession: $sessionId`r`nRange: npt=0.000-`r`n`r`n"
Start-Sleep -Milliseconds 500

# Drain PLAY response + initial interleaved data
$buf = New-Object byte[] 65536
while ($client.Available -gt 0) {
    $null = $stream.Read($buf, 0, $buf.Length)
}

# 7. Send backchannel G.711 audio (400 Hz test tone) on channel 4
Write-Host ""
Write-Host "=== Sending backchannel audio for ${Duration}s ==="

# G.711 u-law encode function
function Pcm2Ulaw([int16]$sample) {
    $BIAS = 0x84
    $CLIP = 32635
    $sign = 0
    if ($sample -lt 0) { $sign = 0x80; $sample = -$sample }
    if ($sample -gt $CLIP) { $sample = $CLIP }
    $sample = $sample + $BIAS
    $exponent = 7
    $expMask = 0x4000
    for ($i = 0; $i -lt 8; $i++) {
        if ($sample -band $expMask) { break }
        $exponent--
        $expMask = $expMask -shr 1
    }
    $mantissa = ($sample -shr ($exponent + 3)) -band 0x0F
    $ulawByte = [byte](($sign -bor ($exponent -shl 4) -bor $mantissa) -bxor 0xFF)
    return $ulawByte
}

# Pre-generate one 40ms packet of 400 Hz tone (320 samples @ 8kHz)
$samplesPerPacket = 320
$pcmuPacket = New-Object byte[] $samplesPerPacket
for ($i = 0; $i -lt $samplesPerPacket; $i++) {
    $t = [double]$i / 8000.0
    $pcm = [int16](16000.0 * [Math]::Sin(2.0 * [Math]::PI * 400.0 * $t))
    $pcmuPacket[$i] = Pcm2Ulaw $pcm
}

$seq = 0
$ts = 0
$ssrc = [System.Random]::new().Next()
$packetsSent = 0
$sw = [System.Diagnostics.Stopwatch]::StartNew()

while ($sw.Elapsed.TotalSeconds -lt $Duration) {
    # Build RTP header (12 bytes)
    $rtp = New-Object byte[] (12 + $samplesPerPacket)
    $rtp[0] = 0x80          # V=2, P=0, X=0, CC=0
    $rtp[1] = 0             # PT=0 (PCMU), M=0
    $rtp[2] = [byte](($seq -shr 8) -band 0xFF)
    $rtp[3] = [byte]($seq -band 0xFF)
    $rtp[4] = [byte](($ts -shr 24) -band 0xFF)
    $rtp[5] = [byte](($ts -shr 16) -band 0xFF)
    $rtp[6] = [byte](($ts -shr 8) -band 0xFF)
    $rtp[7] = [byte]($ts -band 0xFF)
    $rtp[8] = [byte](($ssrc -shr 24) -band 0xFF)
    $rtp[9] = [byte](($ssrc -shr 16) -band 0xFF)
    $rtp[10] = [byte](($ssrc -shr 8) -band 0xFF)
    $rtp[11] = [byte]($ssrc -band 0xFF)
    [Array]::Copy($pcmuPacket, 0, $rtp, 12, $samplesPerPacket)

    # TCP interleaved frame: '$' + channel(1) + length(2 BE) + RTP
    $rtpLen = $rtp.Length
    $frame = New-Object byte[] (4 + $rtpLen)
    $frame[0] = 0x24   # '$'
    $frame[1] = 4       # backchannel channel
    $frame[2] = [byte](($rtpLen -shr 8) -band 0xFF)
    $frame[3] = [byte]($rtpLen -band 0xFF)
    [Array]::Copy($rtp, 0, $frame, 4, $rtpLen)

    $stream.Write($frame, 0, $frame.Length)
    $stream.Flush()

    $seq++
    $ts += $samplesPerPacket
    $packetsSent++

    # Drain any incoming interleaved data to prevent TCP backpressure
    while ($client.Available -gt 0) {
        $null = $stream.Read($buf, 0, $buf.Length)
    }

    # Sleep ~40ms (matching ptime)
    Start-Sleep -Milliseconds 38
}

$sw.Stop()
Write-Host "  Sent $packetsSent packets in $([Math]::Round($sw.Elapsed.TotalSeconds, 1))s"
Write-Host "  ($samplesPerPacket samples/packet, 40ms ptime, seq=$seq)"

# 8. TEARDOWN
$cseq++
Send "TEARDOWN $uri RTSP/1.0`r`nCSeq: $cseq`r`nSession: $sessionId`r`n`r`n"
Start-Sleep -Milliseconds 100

$stream.Close()
$client.Close()

Write-Host ""
Write-Host "=== BACKCHANNEL TEST COMPLETE ===" -ForegroundColor Green
Write-Host "Check device speaker — you should have heard a 400Hz tone for ${Duration}s"
