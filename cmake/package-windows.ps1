param([switch]$RequireVulkan, [switch]$FullCatalog)

$ErrorActionPreference = 'Stop'
$stage = Resolve-Path dist/stage
$vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
$vs = & $vswhere -latest -products '*' -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
$dumpbin = Get-ChildItem "$vs/VC/Tools/MSVC/*/bin/Hostx64/x64/dumpbin.exe" |
    Sort-Object FullName -Descending | Select-Object -First 1
if (!$dumpbin) { throw 'Visual Studio dumpbin is unavailable' }

$engineFolder = Join-Path $stage 'X-Speech-Rekon/bin'
if ($RequireVulkan -and !(Get-ChildItem $engineFolder -Filter '*ggml-vulkan.dll')) {
    throw 'Expected the Vulkan backend module'
}
$cpuBackends = Get-ChildItem $engineFolder -Filter '*ggml-cpu-*.dll'
if (-not ($cpuBackends.Name -match 'x64\.dll$') -or -not ($cpuBackends.Name -match 'haswell\.dll$')) {
    throw 'Expected both baseline and AVX2 CPU backends'
}
$files = @(
    'X-Speech-Rekon/64/win.xpl',
    'X-Speech-Rekon-Diagnostic/64/win.xpl',
    'X-Speech-Rekon/bin/xsr-engine.exe',
    'tools/xsr-check.exe'
) + @(Get-ChildItem $engineFolder -Filter '*.dll' | ForEach-Object { "X-Speech-Rekon/bin/$($_.Name)" })
foreach ($file in $files) {
    $binary = Join-Path $stage $file
    $info = & $dumpbin.FullName /headers /dependents $binary
    if ($LASTEXITCODE -or -not ($info -match '8664 machine')) { throw "Expected x86-64 PE at $file" }
    if ($info -match '(?i)\b(?:vcruntime|msvcp|libgcc|libstdc\+\+|libcurl|libcrypto)[\w+.-]*\.dll\b') {
        throw "Unexpected redistributable DLL dependency at $file"
    }
    if ($file -notmatch 'ggml-vulkan\.dll$' -and $info -match '(?i)\bvulkan-1\.dll\b') {
        throw "Vulkan must remain an optional dependency at $file"
    }
    if ($file.EndsWith('.xpl')) {
        $exports = & $dumpbin.FullName /exports $binary
        if ($LASTEXITCODE) { throw "Cannot read exports for $file" }
        foreach ($name in 'XPluginStart','XPluginStop','XPluginEnable','XPluginDisable','XPluginReceiveMessage') {
            if (-not ($exports -match "\b${name}\b")) { throw "Missing $name in $file" }
        }
    }
}

$zip = Join-Path (Resolve-Path dist) 'X-Speech-Rekon-Windows-x86-64.zip'
Compress-Archive -Path "$stage/*" -DestinationPath $zip -Force
$check = Join-Path $env:RUNNER_TEMP ('Essai vocal été ' + $env:GITHUB_RUN_ID)
Expand-Archive -LiteralPath $zip -DestinationPath $check -Force
$plugin = Join-Path $check 'X-Speech-Rekon'
$models = Join-Path $env:RUNNER_TEMP 'xsr-models'
$engine = Join-Path $plugin 'bin/xsr-engine.exe'
$verifier = Join-Path $check 'tools/xsr-check.exe'
$catalog = Join-Path $plugin 'catalog.json'
$profiles = @('moonshine-streaming-tiny/Q8_0', 'parakeet-tdt-0.6b-v3/Q8_0', 'nemotron-3.5-asr-streaming-0.6b/Q8_0')
if ($FullCatalog) {
    $profiles = @(Get-Content -LiteralPath $catalog -Raw | ConvertFrom-Json | ForEach-Object { "$($_.id)/$($_.quant)" })
}

Write-Host 'Checking the packaged engine protocol'
& $verifier --engine $engine --probe
if ($LASTEXITCODE) { exit $LASTEXITCODE }
foreach ($profile in $profiles) {
    for ($attempt = 1; $attempt -le 3; $attempt++) {
        Write-Host "Fetching $profile (attempt $attempt of 3)"
        & $verifier --engine $engine --catalog $catalog --models $models --profile $profile --fetch
        if ($LASTEXITCODE -eq 0) { break }
        if ($attempt -eq 3) { throw "Could not fetch and verify $profile after $attempt attempts" }
        Start-Sleep -Seconds (5 * $attempt)
    }
}

$englishWav = Join-Path $plugin 'test-en.wav'
Write-Host 'Checking model discovery and CPU fallback'
& $verifier --engine $engine --catalog $catalog --models $models --wav $englishWav --verify-files --profile moonshine-streaming-tiny/Q8_0 --compute vulkan/ci-unavailable
if ($LASTEXITCODE) { exit $LASTEXITCODE }
if ($FullCatalog) {
    Write-Host 'Testing transcription for all catalog profiles'
    & $verifier --engine $engine --catalog $catalog --models $models --wav $englishWav
    if ($LASTEXITCODE) { exit $LASTEXITCODE }
}
foreach ($locale in 'en','fr','es') {
    if ($FullCatalog -and $locale -eq 'en') { continue }
    Write-Host "Testing Parakeet and Nemotron with the $locale recording"
    $wav = Join-Path $plugin "test-$locale.wav"
    & $verifier --engine $engine --catalog $catalog --models $models --wav $wav --profile parakeet-tdt-0.6b-v3/Q8_0
    if ($LASTEXITCODE) { exit $LASTEXITCODE }
    $recognitionLocale = @{ en = 'en-US'; fr = 'fr-FR'; es = 'es-ES' }[$locale]
    & $verifier --engine $engine --catalog $catalog --models $models --wav $wav --profile nemotron-3.5-asr-streaming-0.6b/Q8_0 --language $recognitionLocale
    if ($LASTEXITCODE) { exit $LASTEXITCODE }
}

$hash = (Get-FileHash -LiteralPath $zip -Algorithm SHA256).Hash.ToLowerInvariant()
Set-Content -LiteralPath "$zip.sha256" -Value "$hash  $([IO.Path]::GetFileName($zip))" -Encoding ascii
