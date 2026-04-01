$tcp = New-Object System.Net.Sockets.TcpClient("127.0.0.1", 8555)
$stream = $tcp.GetStream()

function Send-Rtsp($msg) {
    $b = [System.Text.Encoding]::ASCII.GetBytes($msg)
    $stream.Write($b, 0, $b.Length)
    $stream.Flush()
}

function Read-Rtsp() {
    Start-Sleep -Milliseconds 500
    $out = ""
    while ($stream.DataAvailable) {
        $buf = New-Object byte[] 4096
        $n = $stream.Read($buf, 0, 4096)
        $out += [System.Text.Encoding]::ASCII.GetString($buf, 0, $n)
    }
    return $out
}

function md5h($s) {
    $md5 = [System.Security.Cryptography.MD5]::Create()
    $b = [System.Text.Encoding]::ASCII.GetBytes($s)
    $h = $md5.ComputeHash($b)
    return (-join ($h | ForEach-Object { '{0:x2}' -f $_ }))
}

# Step 1: DESCRIBE without auth -> expect 401
Send-Rtsp "DESCRIBE rtsp://127.0.0.1:8555/live RTSP/1.0`r`nCSeq: 1`r`nAccept: application/sdp`r`n`r`n"
$r1 = Read-Rtsp
Write-Host "=== Step 1: DESCRIBE without auth ==="
Write-Host $r1

# Extract nonce
if ($r1 -match 'nonce="([^"]+)"') {
    $nonce = $Matches[1]
    Write-Host "Extracted nonce: $nonce"
} else {
    Write-Host "ERROR: No nonce found in 401 response!"
    $tcp.Close()
    exit 1
}

# Step 2: Compute Digest and retry DESCRIBE
$ha1 = md5h "admin:SmartLockIntercom:test123"
$ha2 = md5h "DESCRIBE:rtsp://127.0.0.1:8555/live"
$resp = md5h "${ha1}:${nonce}:${ha2}"

$authLine = "Authorization: Digest username=`"admin`", realm=`"SmartLockIntercom`", nonce=`"$nonce`", uri=`"rtsp://127.0.0.1:8555/live`", response=`"$resp`""

Send-Rtsp "DESCRIBE rtsp://127.0.0.1:8555/live RTSP/1.0`r`nCSeq: 2`r`nAccept: application/sdp`r`n${authLine}`r`n`r`n"
$r2 = Read-Rtsp
Write-Host "=== Step 2: DESCRIBE with valid auth ==="
Write-Host $r2

# Step 3: Test wrong password
$ha1bad = md5h "admin:SmartLockIntercom:wrongpass"
$respbad = md5h "${ha1bad}:${nonce}:${ha2}"
$authBad = "Authorization: Digest username=`"admin`", realm=`"SmartLockIntercom`", nonce=`"$nonce`", uri=`"rtsp://127.0.0.1:8555/live`", response=`"$respbad`""

Send-Rtsp "DESCRIBE rtsp://127.0.0.1:8555/live RTSP/1.0`r`nCSeq: 3`r`nAccept: application/sdp`r`n${authBad}`r`n`r`n"
$r3 = Read-Rtsp
Write-Host "=== Step 3: DESCRIBE with wrong password ==="
Write-Host $r3

$tcp.Close()
Write-Host "`n=== Auth test complete ==="
