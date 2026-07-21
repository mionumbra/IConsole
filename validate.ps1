param(
    [switch]$SkipGenerate
)

$ErrorActionPreference = "Stop"
$root = $PSScriptRoot
$project = Join-Path $root "project\IConsole.yyp"
$buildDir = Join-Path $root "out\validate\win-x64-release"
$extensionDir = Join-Path $root "project\extensions\IConsole"

function Invoke-Checked {
    param(
        [string]$Command,
        [string[]]$Arguments
    )

    & $Command @Arguments
    if ($LASTEXITCODE -ne 0) {
        throw "$Command failed with exit code $LASTEXITCODE"
    }
}

$extgenOutput = @(& extgen --version 2>&1)
$extgenVersion = $extgenOutput[0].ToString().Trim()
if ($extgenVersion -ne "extgen v1.225bddc") {
    throw "Expected extgen v1.225bddc, found '$extgenVersion'"
}

$gmCliOutput = @(& gm-cli --version 2>&1)
$gmCliVersion = $gmCliOutput[0].ToString().Trim()
if ($gmCliVersion -ne "2.2.0") {
    throw "Expected gm-cli 2.2.0, found '$gmCliVersion'"
}

if (!$SkipGenerate) {
    Invoke-Checked "extgen" @("--config", (Join-Path $root "config.json"))
}

Invoke-Checked "cmake" @(
    "--fresh",
    "-S", $root,
    "-B", $buildDir,
    "-A", "x64",
    "-DEXT_OUTPUT_DIR=$extensionDir"
)
Invoke-Checked "cmake" @("--build", $buildDir, "--config", "Release")
Invoke-Checked "gm-cli" @("compile", $project, "--target=windows", "--runtime=vm", "--config=Default")

$previousSelfTest = $env:ICONSOLE_SELF_TEST
$stdoutPath = Join-Path $buildDir "runner.stdout.log"
$stderrPath = Join-Path $buildDir "runner.stderr.log"
try {
    $env:ICONSOLE_SELF_TEST = "1"
    $gmCliCommand = (Get-Command gm-cli.cmd).Source
    $process = Start-Process $gmCliCommand `
        -ArgumentList @("run", $project, "--target=windows", "--runtime=vm", "--config=Default") `
        -RedirectStandardOutput $stdoutPath `
        -RedirectStandardError $stderrPath `
        -NoNewWindow `
        -PassThru
    if (!$process.WaitForExit(120000)) {
        & taskkill /PID $process.Id /T /F | Out-Null
        throw "gm-cli run exceeded the 120 second validation timeout"
    }
    $runExitCode = $process.ExitCode
    $runOutput = @(
        [System.IO.File]::ReadAllText($stdoutPath)
        [System.IO.File]::ReadAllText($stderrPath)
    )
    $runOutput | ForEach-Object { $_ }
} finally {
    if ($null -eq $previousSelfTest) {
        Remove-Item Env:ICONSOLE_SELF_TEST -ErrorAction SilentlyContinue
    } else {
        $env:ICONSOLE_SELF_TEST = $previousSelfTest
    }
}

if ($runExitCode -ne 0) {
    throw "gm-cli run failed with exit code $runExitCode"
}
if (!($runOutput -match "ICONSOLE_SELF_TEST_PASS")) {
    throw "Runner exited without ICONSOLE_SELF_TEST_PASS"
}
if (!($runOutput -match "ICONSOLE_REDIRECTED_OUTPUT_PASS")) {
    throw "iconsole_open did not preserve Runner's redirected output channel"
}
if ($runOutput -match "ICONSOLE_SELF_TEST_FAIL") {
    throw "Runner reported an IConsole self-test failure"
}

Write-Host "IConsole validation passed."
