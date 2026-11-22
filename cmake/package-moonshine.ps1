param([Parameter(Mandatory)][string]$Stage)

$ErrorActionPreference = 'Stop'
$stage = (Resolve-Path -LiteralPath $Stage).Path
$moonshineSource = 'https://raw.githubusercontent.com/moonshine-ai/moonshine/d8d8fa29d05ca67441a7719e70099f9f48917a93'
$licenses = Join-Path $stage 'licenses/Moonshine'
New-Item -ItemType Directory -Path $licenses -Force | Out-Null
$notices = @{
    'moonshine.txt' = 'LICENSE'
    'cpp-annote.txt' = 'core/cpp-annote/LICENSE'
    'eigen-mpl2.txt' = 'core/third-party/Eigen/COPYING.MPL2'
    'kaldi-native-fbank.txt' = 'core/third-party/kaldi-native-fbank/LICENSE'
    'kissfft.txt' = 'core/third-party/kissfft/COPYING'
    'nlohmann-json.txt' = 'core/third-party/nlohmann/LICENSE.MIT'
    'utf-8.txt' = 'core/third-party/utf-8/LICENSE.txt'
    'utf8proc.md' = 'core/third-party/utf8proc/LICENSE.md'
}
foreach ($notice in $notices.GetEnumerator()) {
    curl.exe --fail --location --silent --show-error --retry 3 --output (Join-Path $licenses $notice.Key) "$moonshineSource/$($notice.Value)"
    if ($LASTEXITCODE) { throw "Could not fetch Moonshine notice $($notice.Value)" }
}
"Moonshine Voice v0.1.6 source: https://github.com/moonshine-ai/moonshine/tree/d8d8fa29d05ca67441a7719e70099f9f48917a93" |
    Set-Content -Encoding ascii (Join-Path $licenses 'SOURCE.txt')

$ort = Join-Path $env:RUNNER_TEMP 'onnxruntime-1.23.2.zip'
$ortUrl = 'https://api.nuget.org/v3-flatcontainer/microsoft.ml.onnxruntime/1.23.2/microsoft.ml.onnxruntime.1.23.2.nupkg'
curl.exe --fail --location --silent --show-error --retry 3 --output $ort $ortUrl
if ($LASTEXITCODE) { throw 'Could not fetch ONNX Runtime notices' }
$package = [System.IO.Compression.ZipFile]::OpenRead($ort)
try {
    $dll = $package.GetEntry('runtimes/win-x64/native/onnxruntime.dll').Open()
    try {
        $expected = (Get-FileHash -InputStream $dll -Algorithm SHA256).Hash
        $actual = (Get-FileHash -LiteralPath (Join-Path $stage 'X-Speech-Rekon/bin/onnxruntime.dll') -Algorithm SHA256).Hash
        if ($expected -ne $actual) { throw 'ONNX Runtime notices do not match the packaged DLL' }
    } finally {
        $dll.Dispose()
    }
    foreach ($name in 'LICENSE', 'ThirdPartyNotices.txt') {
        [System.IO.Compression.ZipFileExtensions]::ExtractToFile($package.GetEntry($name), (Join-Path $licenses "onnxruntime-$name"), $true)
    }
} finally {
    $package.Dispose()
}

$filename = 'X-Speech-Rekon-Moonshine-Windows-x86-64.zip'
$out = (New-Item -ItemType Directory -Path dist -Force).FullName
$zip = Join-Path $out $filename
Compress-Archive -Path (Join-Path $stage '*') -DestinationPath $zip -Force
$sha = (Get-FileHash -LiteralPath $zip -Algorithm SHA256).Hash.ToLowerInvariant()
"$sha  $filename" | Set-Content -Encoding ascii -LiteralPath "$zip.sha256"
