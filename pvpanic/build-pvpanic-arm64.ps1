# Builds pvpanic (ARM64, Win11 Release) in the Windows build VM and signs it with the
# fixed "viogpu pVM Test" certificate (same recipe as build-viogpu.ps1 in fold8-desktop).
# Usage: powershell -File C:\winbuild\build-pvpanic-arm64.ps1 [-Src C:\winbuild\src]
param([string]$Src = 'C:\winbuild\src')
$ErrorActionPreference = 'Stop'
$root = 'C:\winbuild'
$vswhere = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
$msbuild = & $vswhere -products * -latest -requires Microsoft.Component.MSBuild -find 'MSBuild\**\Bin\MSBuild.exe' | Select-Object -First 1
if (-not $msbuild) { throw 'MSBuild not found' }
Set-Location $Src
$common = @('/m','/p:ApiValidator_Enable=false','/p:SpectreMitigation=false','/p:SignMode=Off','/p:EnableInfVerif=false','/p:RunInfVerif=false','/p:InfVerif_Enable=false','/p:Feature_PackOne=false','/p:_BUILD_MINOR_VERSION_=58100')
& $msbuild pvpanic/pvpanic/pvpanic.vcxproj /p:Configuration="Win11 Release" /p:Platform=ARM64 @common
if ($LASTEXITCODE -ne 0) { throw "pvpanic build failed ($LASTEXITCODE)" }

$kitbin = (Get-ChildItem "C:\Program Files (x86)\Windows Kits\10\bin\10.*" -Directory | Where-Object { Test-Path (Join-Path $_.FullName 'x64\signtool.exe') } | Sort-Object Name -Descending | Select-Object -First 1).FullName
$signtool = Join-Path $kitbin 'x64\signtool.exe'
$inf2cat  = Join-Path $kitbin 'x86\inf2cat.exe'
$stampinf = Join-Path $kitbin 'x86\stampinf.exe'
$out = "$root\out"
if (Test-Path $out) { Remove-Item $out -Recurse -Force }
$dest = "$out\pvpanic"
New-Item -ItemType Directory -Force $dest | Out-Null
$sys = Get-ChildItem pvpanic -Recurse -Filter pvpanic.sys -File | Where-Object { $_.FullName -match 'ARM64' } | Sort-Object LastWriteTime -Descending | Select-Object -First 1
if (-not $sys) { throw 'pvpanic.sys not found' }
Copy-Item $sys.FullName $dest
$inf = Get-ChildItem pvpanic -Recurse -Filter pvpanic.inf -File | Where-Object { $_.FullName -match 'ARM64' } | Sort-Object LastWriteTime -Descending | Select-Object -First 1
if (-not $inf) { $inf = Get-Item pvpanic/pvpanic/pvpanic.inf }
Copy-Item $inf.FullName "$dest\pvpanic.inf"
$infPath = "$dest\pvpanic.inf"
$map = [ordered]@{
  'INX_PLATFORM_DRIVERS_DIR'='13'; 'INX_COMPANY'='Red Hat, Inc.'; 'INX_PREFIX_QEMU'='';
  'INX_COPYRIGHT_1'='Copyright (c) Red Hat Inc.'; 'INX_COPYRIGHT_2'='' }
$raw = Get-Content $infPath -Raw
foreach ($k in $map.Keys) { $raw = $raw.Replace($k, [string]$map[$k]) }
Set-Content -LiteralPath $infPath -Value $raw -NoNewline -Encoding utf8
if ((Get-Content $infPath -Raw) -match '\$ARCH\$|\$KMDFVERSION\$|01/01/2008') { & $stampinf -f $infPath -a ARM64 -k 1.15 -d "*" -v "*" }
if ((Get-Content $infPath -Raw) -match 'INX_[A-Z_]+|\$ARCH\$|\$KMDFVERSION\$') { Get-Content $infPath | Write-Host; throw 'token left in inf' }

$certDir = "$root\cert"; $pfx = "$certDir\viogpu.pfx"; $pwFile = "$certDir\pfx-password.txt"
if (-not (Test-Path $pfx)) { throw 'test certificate missing (run build-viogpu.ps1 once)' }
Copy-Item "$certDir\viogpu_Test.cer" "$out\viogpu_Test.cer"
$thumb = (New-Object System.Security.Cryptography.X509Certificates.X509Certificate2("$certDir\viogpu_Test.cer")).Thumbprint
if (-not (Test-Path "Cert:\LocalMachine\My\$thumb")) {
  Import-PfxCertificate -FilePath $pfx -CertStoreLocation Cert:\LocalMachine\My -Password (ConvertTo-SecureString (Get-Content $pwFile -Raw) -AsPlainText -Force) | Out-Null
}
$sign = @('sign','/fd','SHA256','/sm','/s','My','/sha1',$thumb)
& $signtool @sign "$dest\pvpanic.sys"
if ($LASTEXITCODE -ne 0) { throw 'signtool (sys) failed' }
& $inf2cat /driver:$dest /os:Server10_ARM64
if ($LASTEXITCODE -ne 0) { throw 'inf2cat failed' }
Get-ChildItem "$dest\*.cat" | ForEach-Object { & $signtool @sign $_.FullName; if ($LASTEXITCODE -ne 0) { throw 'signtool (cat) failed' } }
Get-ChildItem -Recurse $out -File | Select-Object FullName, Length | Out-String | Write-Host
Write-Host 'BUILD OK'
