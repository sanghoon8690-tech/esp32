<#
  Grabs JPEG snapshot(s) from the XIAO-CAM ESP32 (AP mode, http://192.168.4.1/capture)
  and saves them to a local folder, auto-connecting this PC to the XIAO-CAM Wi-Fi
  network first if it isn't already on it.

  Examples:
    .\capture-to-pc.ps1                              # single shot
    .\capture-to-pc.ps1 -IntervalSeconds 2 -Count 50 # dataset burst: 50 shots, 2s apart
    .\capture-to-pc.ps1 -IntervalSeconds 3           # continuous, until Ctrl+C
#>
param(
  [string]$OutDir = (Join-Path $PSScriptRoot "captures"),
  [int]$IntervalSeconds = 0,   # 0 = single capture only
  [int]$Count = 0              # 0 = unlimited (only meaningful with IntervalSeconds > 0)
)

$CamUrl = "http://192.168.4.1/capture"
$Ssid = "XIAO-CAM"

if (-not (Test-Path $OutDir)) {
  New-Item -ItemType Directory -Path $OutDir | Out-Null
}

function Get-CurrentSsid {
  $line = netsh wlan show interfaces | Select-String "^\s*SSID\s+:\s+(.+)$"
  if ($line) { $line.Matches[0].Groups[1].Value.Trim() } else { "" }
}

# Windows roams back to any known network that has internet, since the
# camera AP has none. Suppressing auto-connect on other saved profiles
# while we're capturing is the only reliable way to keep the link up.
function Suspend-OtherProfiles {
  $names = (netsh wlan show profiles) |
    Select-String "^\s*All User Profile\s+:\s+(.+)$" |
    ForEach-Object { $_.Matches[0].Groups[1].Value.Trim() } |
    Where-Object { $_ -ne $Ssid }
  foreach ($n in $names) {
    netsh wlan set profileparameter name="$n" connectionmode=manual | Out-Null
  }
  return $names
}

function Resume-Profiles([string[]]$names) {
  foreach ($n in $names) {
    netsh wlan set profileparameter name="$n" connectionmode=auto | Out-Null
  }
}

function Wait-ForCamIp([int]$TimeoutSec = 10) {
  $sw = [System.Diagnostics.Stopwatch]::StartNew()
  while ($sw.Elapsed.TotalSeconds -lt $TimeoutSec) {
    $ip = (Get-NetIPAddress -AddressFamily IPv4 -InterfaceAlias "Wi-Fi" -ErrorAction SilentlyContinue |
      Where-Object { $_.IPAddress -like "192.168.4.*" })
    if ($ip) { return $true }
    Start-Sleep -Milliseconds 500
  }
  return $false
}

function Ensure-CamWifi {
  if ((Get-CurrentSsid) -ne $Ssid) {
    Write-Host "Connecting to $Ssid..."
    netsh wlan connect name="$Ssid" | Out-Null
  }
  if (-not (Wait-ForCamIp)) {
    throw "Could not get an IP on $Ssid within the timeout - still on another network?"
  }
}

function Take-Capture {
  Ensure-CamWifi
  $ts = Get-Date -Format "yyyyMMdd_HHmmss_fff"
  $path = Join-Path $OutDir "capture_$ts.jpg"
  try {
    Invoke-WebRequest -Uri $CamUrl -OutFile $path -TimeoutSec 8 -UseBasicParsing
    Write-Host "Saved: $path"
  } catch {
    Write-Warning "Capture failed: $_"
  }
}

$suspended = Suspend-OtherProfiles
try {
  if ($IntervalSeconds -le 0) {
    Take-Capture
  } else {
    $n = 0
    while ($Count -le 0 -or $n -lt $Count) {
      Take-Capture
      $n++
      if ($Count -le 0 -or $n -lt $Count) {
        Start-Sleep -Seconds $IntervalSeconds
      }
    }
  }
} finally {
  Resume-Profiles $suspended
  Write-Host "Restored normal Wi-Fi auto-connect."
}
