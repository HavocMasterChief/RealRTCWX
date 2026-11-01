<#
  Uploads RealRTCW game data to the Xbox through Device Portal, in one go.

  Usage (PowerShell on your PC):
    powershell -ExecutionPolicy Bypass -File upload-gamedata.ps1 `
      -XboxIp 192.168.0.50 `
      -RtcwMain   "C:\Program Files (x86)\Steam\steamapps\common\Return to Castle Wolfenstein\Main" `
      -RealMain   "C:\Program Files (x86)\Steam\steamapps\common\RealRTCW\Main"

  It asks for your Device Portal username/password, finds the installed
  RealRTCW app, and uploads into its LocalState\main folder:
    - from the original game: pak0.pk3 and sp_pak*.pk3 (no multiplayer files)
    - from RealRTCW: every .pk3 and .cfg (no .dll files)
  Files already on the Xbox with the same size are skipped, so it is safe to
  re-run after a failure.
#>
param(
  [Parameter(Mandatory)] [string] $XboxIp,
  [Parameter(Mandatory)] [string] $RtcwMain,
  [Parameter(Mandatory)] [string] $RealMain,
  [string] $PackagePrefix = "RealRTCWX_"
)

$ErrorActionPreference = "Stop"
Add-Type -AssemblyName System.Net.Http

# Device Portal uses a self-signed certificate. A compiled callback is used
# because PowerShell script blocks can't run on HttpClient's I/O threads.
if (-not ("TrustAllCerts" -as [type])) {
  Add-Type -ReferencedAssemblies System.Net.Http -TypeDefinition @"
using System.Net.Http;
using System.Security.Cryptography.X509Certificates;
using System.Net.Security;
public static class TrustAllCerts {
  public static bool Accept(HttpRequestMessage m, X509Certificate2 c, X509Chain ch, SslPolicyErrors e) { return true; }
  public static void Apply(HttpClientHandler h) {
    h.ServerCertificateCustomValidationCallback = Accept;
  }
}
"@
}
$handler = New-Object System.Net.Http.HttpClientHandler
[TrustAllCerts]::Apply($handler)
$handler.CookieContainer = New-Object System.Net.CookieContainer
$cred = Get-Credential -Message "Xbox Device Portal username and password"
$handler.Credentials = $cred.GetNetworkCredential()
$client = New-Object System.Net.Http.HttpClient($handler)
$client.Timeout = [TimeSpan]::FromHours(2)
$base = "https://${XboxIp}:11443"

function Get-Json($path) {
  $r = $client.GetAsync("$base$path").Result
  if (-not $r.IsSuccessStatusCode) { throw "GET $path failed: $([int]$r.StatusCode) $($r.ReasonPhrase)" }
  return $r.Content.ReadAsStringAsync().Result | ConvertFrom-Json
}

# Any GET sets the CSRF-Token cookie that POSTs must echo back.
Write-Host "Connecting to $base ..."
$pkgs = Get-Json "/api/app/packagemanager/packages"
$pkg = $pkgs.InstalledPackages | Where-Object { $_.PackageFullName -like "$PackagePrefix*" } | Select-Object -First 1
if (-not $pkg) { throw "RealRTCW isn't installed on the Xbox (no package starting with $PackagePrefix)." }
$pfn = $pkg.PackageFullName
Write-Host "Found app: $pfn"
$csrf = ($handler.CookieContainer.GetCookies([Uri]$base) | Where-Object Name -eq "CSRF-Token").Value

$enc = [Uri]::EscapeDataString
$q = "knownfolderid=LocalAppData&packagefullname=$($enc.Invoke($pfn))"

# Make sure LocalState\main exists.
$mk = New-Object System.Net.Http.HttpRequestMessage([System.Net.Http.HttpMethod]::Post,
  "$base/api/filesystem/apps/folder?$q&path=$($enc.Invoke('\LocalState'))&newfoldername=main")
if ($csrf) { $mk.Headers.Add("X-CSRF-Token", $csrf) }
$null = $client.SendAsync($mk).Result   # fails harmlessly if it already exists

# What's already there?
$existing = @{}
try {
  $list = Get-Json "/api/filesystem/apps/files?$q&path=$($enc.Invoke('\LocalState\main'))"
  foreach ($f in $list.Items) { $existing[$f.Name.ToLower()] = [int64]$f.FileSize }
} catch { }

# Pick the files.
$files = @()
$files += Get-ChildItem $RtcwMain -File | Where-Object { $_.Name -match '^(pak0|sp_pak\d+)\.pk3$' }
$files += Get-ChildItem $RealMain -File | Where-Object { $_.Extension -in '.pk3', '.cfg' }
if (-not ($files | Where-Object Name -eq 'pak0.pk3')) { throw "pak0.pk3 not found in $RtcwMain" }

$total = ($files | Measure-Object Length -Sum).Sum
Write-Host ("{0} files, {1:N1} GB total" -f $files.Count, ($total / 1GB))

$i = 0
foreach ($f in $files) {
  $i++
  if ($existing.ContainsKey($f.Name.ToLower()) -and $existing[$f.Name.ToLower()] -eq $f.Length) {
    Write-Host "[$i/$($files.Count)] $($f.Name) already there, skipping"
    continue
  }
  Write-Host ("[{0}/{1}] {2} ({3:N0} MB) ..." -f $i, $files.Count, $f.Name, ($f.Length / 1MB)) -NoNewline
  $sw = [Diagnostics.Stopwatch]::StartNew()

  $stream = [IO.File]::OpenRead($f.FullName)
  try {
    $content = New-Object System.Net.Http.MultipartFormDataContent
    $fileContent = New-Object System.Net.Http.StreamContent($stream)
    $fileContent.Headers.ContentType = [System.Net.Http.Headers.MediaTypeHeaderValue]::Parse("application/octet-stream")
    $content.Add($fileContent, "file", $f.Name)

    $req = New-Object System.Net.Http.HttpRequestMessage([System.Net.Http.HttpMethod]::Post,
      "$base/api/filesystem/apps/file?$q&path=$($enc.Invoke('\LocalState\main'))")
    if ($csrf) { $req.Headers.Add("X-CSRF-Token", $csrf) }
    $req.Content = $content
    $resp = $client.SendAsync($req).Result
  } finally { $stream.Dispose() }

  if (-not $resp.IsSuccessStatusCode) {
    Write-Host " FAILED ($([int]$resp.StatusCode) $($resp.ReasonPhrase))" -ForegroundColor Red
    Write-Host $resp.Content.ReadAsStringAsync().Result
    throw "Upload of $($f.Name) failed. Re-run the script to resume."
  }
  Write-Host (" done in {0:N0}s" -f $sw.Elapsed.TotalSeconds) -ForegroundColor Green
}

Write-Host "`nAll files are on the Xbox. Launch RealRTCW." -ForegroundColor Green
