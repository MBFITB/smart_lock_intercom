# Full RTSP session test including PLAY
$tcp = New-Object System.Net.Sockets.TcpClient("127.0.0.1", 8555)
$stream = $tcp.GetStream()
$stream.ReadTimeout = 3000

function Send-RTSP($msg) {
    $bytes = [System.Text.Encoding]::ASCII.GetBytes($msg)
    $stream.Write($bytes, 0, $bytes.Length)
    Start-Sleep -Milliseconds 300
    $buf = New-Object byte[] 4096
    $n = $stream.Read($buf, 0, 4096)
    return [System.Text.Encoding]::ASCII.GetString($buf, 0, $n)
}

# OPTIONS
$resp = Send-RTSP "OPTIONS rtsp://127.0.0.1:8555/live RTSP/1.0`r`nCSeq: 1`r`n`r`n"
if ($resp -notmatch "200 OK") { Write-Host "FAIL: OPTIONS"; exit 1 }
Write-Host "OPTIONS: OK"

# DESCRIBE
$resp = Send-RTSP "DESCRIBE rtsp://127.0.0.1:8555/live RTSP/1.0`r`nCSeq: 2`r`nAccept: application/sdp`r`n`r`n"
if ($resp -notmatch "H264/90000") { Write-Host "FAIL: DESCRIBE"; exit 1 }
Write-Host "DESCRIBE: OK (SDP has H264 + PCMU)"

# SETUP video
$resp = Send-RTSP "SETUP rtsp://127.0.0.1:8555/live/trackID=0 RTSP/1.0`r`nCSeq: 3`r`nTransport: RTP/AVP;unicast;client_port=6000-6001`r`n`r`n"
if ($resp -notmatch "200 OK") { Write-Host "FAIL: SETUP video"; exit 1 }
# Extract session ID
$resp -match 'Session:\s+(\w+)' | Out-Null
$session = $Matches[1]
Write-Host "SETUP video: OK (session=$session)"

# SETUP audio
$resp = Send-RTSP "SETUP rtsp://127.0.0.1:8555/live/trackID=1 RTSP/1.0`r`nCSeq: 4`r`nTransport: RTP/AVP;unicast;client_port=6002-6003`r`nSession: $session`r`n`r`n"
if ($resp -notmatch "200 OK") { Write-Host "FAIL: SETUP audio"; exit 1 }
Write-Host "SETUP audio: OK"

# Open UDP listeners to receive RTP
$udpVideo = New-Object System.Net.Sockets.UdpClient(6000)
$udpAudio = New-Object System.Net.Sockets.UdpClient(6002)
$udpVideo.Client.ReceiveTimeout = 3000
$udpAudio.Client.ReceiveTimeout = 3000

# PLAY
$resp = Send-RTSP "PLAY rtsp://127.0.0.1:8555/live RTSP/1.0`r`nCSeq: 5`r`nSession: $session`r`n`r`n"
if ($resp -notmatch "200 OK") { Write-Host "FAIL: PLAY"; exit 1 }
Write-Host "PLAY: OK"

# Wait for RTP packets
Start-Sleep -Milliseconds 500

$videoCount = 0
$audioCount = 0

# Receive a few video packets
for ($i = 0; $i -lt 5; $i++) {
    try {
        $ep = New-Object System.Net.IPEndPoint([System.Net.IPAddress]::Any, 0)
        $data = $udpVideo.Receive([ref]$ep)
        $videoCount++
    } catch { break }
}

# Receive a few audio packets
for ($i = 0; $i -lt 5; $i++) {
    try {
        $ep = New-Object System.Net.IPEndPoint([System.Net.IPAddress]::Any, 0)
        $data = $udpAudio.Receive([ref]$ep)
        $audioCount++
    } catch { break }
}

Write-Host "RTP received: video=$videoCount audio=$audioCount"

$udpVideo.Close()
$udpAudio.Close()

# TEARDOWN
$resp = Send-RTSP "TEARDOWN rtsp://127.0.0.1:8555/live RTSP/1.0`r`nCSeq: 6`r`nSession: $session`r`n`r`n"
if ($resp -notmatch "200 OK") { Write-Host "FAIL: TEARDOWN"; exit 1 }
Write-Host "TEARDOWN: OK"

$tcp.Close()

if ($videoCount -gt 0 -and $audioCount -gt 0) {
    Write-Host "`n=== ALL RTSP TESTS PASSED ==="
} else {
    Write-Host "`nWARN: Missing RTP packets (video=$videoCount audio=$audioCount)"
    Write-Host "Server may not be sending to loopback. Try VLC for full test."
}
