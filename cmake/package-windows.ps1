param([switch]$RequireVulkan)

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
& $verifier --engine $engine --probe
if ($LASTEXITCODE) { exit $LASTEXITCODE }
& $verifier --engine $engine --catalog (Join-Path $plugin 'catalog.json') --models $models --fetch --verify-files --wav (Join-Path $plugin 'test-en.wav')
if ($LASTEXITCODE) { exit $LASTEXITCODE }
& $verifier --engine $engine --catalog (Join-Path $plugin 'catalog.json') --models $models --wav (Join-Path $plugin 'test-en.wav') --profile moonshine-streaming-tiny/Q8_0 --compute vulkan/ci-unavailable
if ($LASTEXITCODE) { exit $LASTEXITCODE }
foreach ($locale in 'fr','es') {
    & $verifier --engine $engine --catalog (Join-Path $plugin 'catalog.json') --models $models --wav (Join-Path $plugin "test-$locale.wav") --profile parakeet-tdt-0.6b-v3/Q8_0
    if ($LASTEXITCODE) { exit $LASTEXITCODE }
    $recognitionLocale = @{ fr = 'fr-FR'; es = 'es-ES' }[$locale]
    & $verifier --engine $engine --catalog (Join-Path $plugin 'catalog.json') --models $models --wav (Join-Path $plugin "test-$locale.wav") --profile nemotron-3.5-asr-streaming-0.6b/Q8_0 --language $recognitionLocale
    if ($LASTEXITCODE) { exit $LASTEXITCODE }
}

$hash = (Get-FileHash -LiteralPath $zip -Algorithm SHA256).Hash.ToLowerInvariant()
Set-Content -LiteralPath "$zip.sha256" -Value "$hash  $([IO.Path]::GetFileName($zip))" -Encoding ascii
