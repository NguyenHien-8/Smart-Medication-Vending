$ErrorActionPreference = "Stop"

$repo = (Resolve-Path (Join-Path $PSScriptRoot "../..")).Path
$vsDevCmd = "C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\Common7\Tools\VsDevCmd.bat"
$envCommand = 'call "' + $vsDevCmd + '" -arch=x64 >nul && set'
$environment = & cmd.exe /d /s /c $envCommand
foreach ($line in $environment) {
    $separator = $line.IndexOf('=')
    if ($separator -gt 0) {
        [Environment]::SetEnvironmentVariable(
            $line.Substring(0, $separator),
            $line.Substring($separator + 1),
            'Process')
    }
}

$buildDir = Join-Path $repo ".superpowers/sdd/2026-09-22-smartmedivend-fail-closed-vending/host-tests"
New-Item -ItemType Directory -Force -Path $buildDir | Out-Null
$compiler = "C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Tools\MSVC\14.44.35207\bin\Hostx64\x64\cl.exe"
$cjsonDir = "C:\Espressif\frameworks\esp-idf-v5.5.5\components\json\cJSON"
$cjsonObject = Join-Path $buildDir "cjson.obj"
$medicalExe = Join-Path $buildDir "medical_advisor_test.exe"

Push-Location $repo
try {
    $cjsonArgs = @(
        "/nologo", "/c", "/TC", "/W3", "/D_CRT_SECURE_NO_WARNINGS",
        "/I$cjsonDir", (Join-Path $cjsonDir "cJSON.c"), "/Fo$cjsonObject"
    )
    & $compiler @cjsonArgs
    if ($LASTEXITCODE -ne 0) { throw "cJSON compilation failed: $LASTEXITCODE" }

    $medicalArgs = @(
        "/nologo", "/std:c++20", "/EHsc", "/W4", "/WX", "/D_CRT_SECURE_NO_WARNINGS",
        "/Imain", "/Imain/medical", "/I$cjsonDir",
        "main/medical/medical_advisor.cc", "main/medical/tests/test_medical_advisor.cc",
        $cjsonObject, "/Fe:$medicalExe"
    )
    & $compiler @medicalArgs
    if ($LASTEXITCODE -ne 0) { throw "medical test compilation failed: $LASTEXITCODE" }

    & $medicalExe data/medical_rules.json data/medicines.json data/pharmacist_review.json
    if ($LASTEXITCODE -ne 0) { throw "medical tests failed: $LASTEXITCODE" }
} finally {
    Pop-Location
}
