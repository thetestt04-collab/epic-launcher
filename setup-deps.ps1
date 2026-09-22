$ErrorActionPreference = "Stop"

$externalDirectory = Join-Path $PSScriptRoot "external"
$temporaryDirectory = Join-Path ([IO.Path]::GetTempPath()) "dependencies"

$dependencies = @(
    @{
        Name = "IUP 3.30 for MinGW-w64"
        Url = "https://downloads.sourceforge.net/project/iup/3.30/Windows%20Libraries/Dynamic/iup-3.30_Win64_dllw6_lib.zip"
        Sha256 = "AEA2AD93351233EEAC8F921CF4CC1EEF36AB33A88266489ACCB0ADE28F8C3A0D"
        Archive = Join-Path $temporaryDirectory "iup-3.30.zip"
        Destination = Join-Path $externalDirectory "iup-3.30_Win64_dllw6_lib"
    },
    @{
        Name = "WinDivert 2.2.2"
        Url = "https://github.com/basil00/WinDivert/releases/download/v2.2.2/WinDivert-2.2.2-A.zip"
        Sha256 = "63CB41763BB4B20F600B6DE04E991A9C2BE73279E317D4D82F237B150C5F3F15"
        Archive = Join-Path $temporaryDirectory "WinDivert-2.2.2-A.zip"
        Destination = $externalDirectory
    }
)

New-Item -ItemType Directory -Force -Path $externalDirectory, $temporaryDirectory | Out-Null

$curlCommand = Get-Command "curl.exe" -ErrorAction SilentlyContinue
if (-not $curlCommand) {
    throw "curl.exe is required to download redirected dependency archives."
}

foreach ($dependency in $dependencies) {
    Write-Host "Downloading $($dependency.Name)..."
    & $curlCommand.Source --location --fail --silent --show-error `
        --output $dependency.Archive $dependency.Url
    if ($LASTEXITCODE -ne 0) {
        throw "Download failed for $($dependency.Name)."
    }

    $actualHash = (Get-FileHash -LiteralPath $dependency.Archive -Algorithm SHA256).Hash
    if ($actualHash -ne $dependency.Sha256) {
        Remove-Item -LiteralPath $dependency.Archive -Force
        throw "Checksum verification failed for $($dependency.Name)."
    }

    New-Item -ItemType Directory -Force -Path $dependency.Destination | Out-Null
    Expand-Archive -LiteralPath $dependency.Archive -DestinationPath $dependency.Destination -Force
}

$windivertRoot = Join-Path $externalDirectory "WinDivert-2.2.2-A"
if (Test-Path -LiteralPath $windivertRoot) {
    Remove-Item -LiteralPath (Join-Path $windivertRoot "x86") -Recurse -Force -ErrorAction SilentlyContinue
    Remove-Item -LiteralPath (Join-Path $windivertRoot "doc") -Recurse -Force -ErrorAction SilentlyContinue
    foreach ($name in @("CHANGELOG", "README", "VERSION")) {
        Remove-Item -LiteralPath (Join-Path $windivertRoot $name) -Force -ErrorAction SilentlyContinue
    }
    Get-ChildItem -LiteralPath (Join-Path $windivertRoot "x64") -File -Filter "*.exe" -ErrorAction SilentlyContinue |
        Remove-Item -Force -ErrorAction SilentlyContinue
}

Write-Host "Dependencies are installed under $externalDirectory"

