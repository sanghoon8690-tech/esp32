<#
  Uploads esp32/dataset/<label>/*.jpg to Edge Impulse, one label at a time,
  using the API key from .env (EI_API_KEY=...) so the key never has to be
  typed into chat or a terminal command directly.

  Usage: .\upload-dataset.ps1
#>
$ErrorActionPreference = "Stop"
$root = $PSScriptRoot
$envFile = Join-Path $root ".env"
$datasetDir = Join-Path $root "dataset"

if (-not (Test-Path $envFile)) {
  throw ".env not found at $envFile - copy .env.example to .env and fill in EI_API_KEY first."
}

$apiKey = $null
foreach ($line in Get-Content $envFile) {
  if ($line -match '^\s*EI_API_KEY\s*=\s*(.+?)\s*$') {
    $apiKey = $matches[1]
  }
}
if ([string]::IsNullOrWhiteSpace($apiKey)) {
  throw "EI_API_KEY is empty in $envFile - open it and paste your Edge Impulse API key, then rerun this script."
}

if (-not (Test-Path $datasetDir)) {
  throw "No dataset folder at $datasetDir"
}

Get-ChildItem $datasetDir -Directory | ForEach-Object {
  $label = $_.Name
  $files = Get-ChildItem $_.FullName -Filter *.jpg | Select-Object -ExpandProperty FullName
  if ($files.Count -eq 0) {
    Write-Host "Skipping '$label' (no files)"
    return
  }
  Write-Host "Uploading $($files.Count) file(s) labeled '$label'..."
  edge-impulse-uploader --api-key $apiKey --category training --label $label @files
}
