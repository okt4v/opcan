# Downloads the pinned official Stockfish release for Windows, checks its sha256 and puts the binary at
# <out>\stockfish.exe (+ its GPL license). Use with: cmake -DOPCAN_STOCKFISH_PATH=<out>/stockfish.exe
# usage: packaging\fetch-stockfish.ps1 [-Out packaging\stockfish] [-Arch x86-64|arm64]
param([string]$Out = "packaging/stockfish", [string]$Arch = "x86-64")
$ErrorActionPreference = "Stop"
$tag = "sf_19"
$sums = @{
  "x86-64" = "3c8bf1f9ea66a09350a40df4f632288285ac206d99f33ab5842c408fc30b48a7"
  "arm64"  = "8372ad3f0d7276deb2c70f801f541ec7db463219fc6d9c7592864e542aa4f401"
}
if (-not $sums.ContainsKey($Arch)) { throw "unknown arch $Arch" }
$name = "stockfish-windows-$Arch-universal.zip"
$tmp = Join-Path ([IO.Path]::GetTempPath()) ("sf-" + [guid]::NewGuid())
New-Item -ItemType Directory -Force $tmp, $Out | Out-Null
try {
  $zip = Join-Path $tmp $name
  Invoke-WebRequest -UseBasicParsing -Uri "https://github.com/official-stockfish/Stockfish/releases/download/$tag/$name" -OutFile $zip
  $got = (Get-FileHash -Algorithm SHA256 $zip).Hash.ToLower()
  if ($got -ne $sums[$Arch]) { throw "sha256 mismatch for ${name}: $got" }
  Expand-Archive -Path $zip -DestinationPath (Join-Path $tmp "x")
  $bin = Get-ChildItem -Recurse -File (Join-Path $tmp "x") -Filter "stockfish*.exe" | Select-Object -First 1
  if (-not $bin) { throw "no stockfish exe in $name" }
  Copy-Item $bin.FullName (Join-Path $Out "stockfish.exe") -Force
  $lic = Get-ChildItem -Recurse -File (Join-Path $tmp "x") -Filter "Copying.txt" | Select-Object -First 1
  if ($lic) { Copy-Item $lic.FullName (Join-Path $Out "stockfish-COPYING.txt") -Force }
  Write-Host "stockfish $tag -> $Out\stockfish.exe"
} finally { Remove-Item -Recurse -Force $tmp }
