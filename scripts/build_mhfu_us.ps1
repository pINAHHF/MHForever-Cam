[CmdletBinding()]
param(
    [string]$Iso,
    [string]$Exe,
    [int]$Jobs = 0,
    [switch]$Launch
)

$ErrorActionPreference = "Stop"
Set-StrictMode -Version Latest

$RepoRoot = (Resolve-Path (Join-Path $PSScriptRoot "..")).Path
$Region = "ULUS10391"
$BuildDir = Join-Path $RepoRoot "build\windows-msvc-game"
$GeneratedDir = Join-Path $RepoRoot "user_data\generated\$Region"

function Stop-WithMessage([string]$Message) {
    Write-Host ""
    Write-Host $Message -ForegroundColor Red
    exit 1
}

function Invoke-Checked([string]$File, [string[]]$Arguments, [string]$Step) {
    Write-Host ""
    Write-Host "== $Step ==" -ForegroundColor Cyan
    & $File @Arguments
    if ($LASTEXITCODE -ne 0) {
        Stop-WithMessage "$Step falhou (código $LASTEXITCODE). O log acima mostra o primeiro erro."
    }
}

$CMakeCommand = Get-Command "cmake.exe" -ErrorAction SilentlyContinue
if (-not $CMakeCommand) { $CMakeCommand = Get-Command "cmake" -ErrorAction SilentlyContinue }
$NinjaCommand = Get-Command "ninja.exe" -ErrorAction SilentlyContinue
if (-not $NinjaCommand) { $NinjaCommand = Get-Command "ninja" -ErrorAction SilentlyContinue }
$CompilerCommand = Get-Command "cl.exe" -ErrorAction SilentlyContinue
if (-not $CompilerCommand) { $CompilerCommand = Get-Command "cl" -ErrorAction SilentlyContinue }

if (-not $CMakeCommand -or -not $NinjaCommand -or -not $CompilerCommand) {
    Stop-WithMessage @"
Abra o 'Developer PowerShell for VS 2022' e execute o script novamente.
Ele precisa encontrar CMake, Ninja e o compilador MSVC (cl.exe).
No Visual Studio Installer, instale a carga 'Desenvolvimento para desktop com C++'
e os componentes CMake/Ninja.
"@
}

$PythonLauncher = Get-Command "py.exe" -ErrorAction SilentlyContinue
if (-not $PythonLauncher) { $PythonLauncher = Get-Command "py" -ErrorAction SilentlyContinue }
$PythonCommand = Get-Command "python.exe" -ErrorAction SilentlyContinue
if (-not $PythonCommand) { $PythonCommand = Get-Command "python" -ErrorAction SilentlyContinue }

if ($PythonLauncher) {
    $PythonFile = $PythonLauncher.Source
    $PythonPrefix = @("-3")
} elseif ($PythonCommand) {
    $PythonFile = $PythonCommand.Source
    $PythonPrefix = @()
} else {
    Stop-WithMessage "Python 3 não foi encontrado. Instale Python 3 e execute novamente."
}

$dataRoot = if ($env:MHF_DATA_DIR) {
    $env:MHF_DATA_DIR
} elseif ($env:APPDATA) {
    Join-Path $env:APPDATA "MHForever"
} else {
    ""
}

$gameDataDir = if ($dataRoot) { Join-Path $dataRoot "games\$Region" } else { "" }

if (-not $Iso) {
    if ($gameDataDir -and (Test-Path -LiteralPath (Join-Path $gameDataDir "image.iso"))) {
        $Iso = Join-Path $gameDataDir "image.iso"
    } else {
        Stop-WithMessage @"
Não encontrei a ISO em <dados do MHForever>\games\$Region\image.iso.
Passe o caminho da sua ISO original assim:
  .\scripts\build_mhfu_us.ps1 -Iso "D:\Jogos\MHFU.iso"
"@
    }
}

if (-not (Test-Path -LiteralPath $Iso -PathType Leaf)) {
    Stop-WithMessage "ISO não encontrada: $Iso"
}
$Iso = (Resolve-Path -LiteralPath $Iso).Path

if (-not $Exe) {
    $candidates = @()
    if ($gameDataDir) { $candidates += (Join-Path $gameDataDir "executable.elf") }
    $candidates += (Join-Path $RepoRoot "user_data\exe\ULUS10391_EBOOT.BIN")
    $candidates += (Join-Path (Split-Path -Parent $Iso) "ULUS10391_EBOOT.BIN")
    $candidates += (Join-Path (Split-Path -Parent $Iso) "EBOOT.BIN")
    $Exe = $candidates | Where-Object { Test-Path -LiteralPath $_ -PathType Leaf } | Select-Object -First 1
}

if (-not $Exe -or -not (Test-Path -LiteralPath $Exe -PathType Leaf)) {
    Stop-WithMessage @"
Não encontrei o executável já preparado pelo MHForever.
Adicione a sua cópia do jogo pelo launcher primeiro, ou informe o ELF preparado:
  .\scripts\build_mhfu_us.ps1 -Iso "D:\Jogos\MHFU.iso" -Exe "C:\caminho\executable.elf"
"@
}
$Exe = (Resolve-Path -LiteralPath $Exe).Path

if ($Jobs -eq 0) { $Jobs = [Math]::Min(8, [Environment]::ProcessorCount) }
if ($Jobs -lt 1 -or $Jobs -gt 64) { Stop-WithMessage "Jobs deve estar entre 1 e 64." }

Write-Host "MHFU camera build — North America ($Region v1.01)" -ForegroundColor Green
Write-Host "ISO: $Iso"
Write-Host "Executable: $Exe"
Write-Host "Parallel jobs: $Jobs"
Write-Host "Generated game code stays local in user_data/ and is not uploaded."

$configureArgs = @(
    "-S", $RepoRoot,
    "-B", $BuildDir,
    "-G", "Ninja",
    "-DCMAKE_C_COMPILER=cl",
    "-DCMAKE_CXX_COMPILER=cl",
    "-DCMAKE_BUILD_TYPE=Release",
    "-DMHF_BUILD_TESTS=OFF",
    "-DMHF_BUILD_LAUNCHER=OFF",
    "-DMHF_WITH_SDL=ON"
)

Invoke-Checked $CMakeCommand.Source $configureArgs "Configurar ferramentas"
Invoke-Checked $CMakeCommand.Source @("--build", $BuildDir, "--target", "mhf_recomp", "mhfu_extract_overlays", "--parallel", "$Jobs") "Compilar ferramentas"

$prepareScript = Join-Path $RepoRoot "games\mhfu\tools\prepare.py"
$prepareArgs = $PythonPrefix + @(
    $prepareScript,
    "--iso", $Iso,
    "--exe", $Exe,
    "--tools", $BuildDir,
    "--region", $Region
)
Invoke-Checked $PythonFile $prepareArgs "Gerar código local do MHFU"

$configureGameArgs = $configureArgs + @(
    "-DMHF_MHFU_GENERATED=$GeneratedDir",
    "-DMHF_MHFU_PROGRAM_NAME=mhfu-us"
)
Invoke-Checked $CMakeCommand.Source $configureGameArgs "Configurar executável do jogo"
Invoke-Checked $CMakeCommand.Source @("--build", $BuildDir, "--target", "mhfu", "--parallel", "$Jobs") "Compilar mhfu-us.exe"

$GameExe = Join-Path $BuildDir "games\mhfu\mhfu-us.exe"
if (-not (Test-Path -LiteralPath $GameExe -PathType Leaf)) {
    Stop-WithMessage "A compilação terminou sem criar o executável esperado: $GameExe"
}

Write-Host ""
Write-Host "SUCESSO: $GameExe" -ForegroundColor Green
Write-Host "Para testar, use:"
Write-Host ('  & "{0}" --iso "{1}" --exe "{2}" --window' -f $GameExe, $Iso, $Exe)

if ($Launch) {
    Write-Host ""
    Write-Host "Iniciando o MHFU modificado..." -ForegroundColor Cyan
    & $GameExe --iso $Iso --exe $Exe --window
    if ($LASTEXITCODE -ne 0) {
        Write-Host "O jogo fechou com código $LASTEXITCODE." -ForegroundColor Yellow
    }
}
