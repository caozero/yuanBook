param(
    [ValidateSet('aarch64', 'armhf')]
    [string]$Arch = 'aarch64',

    [ValidateSet('Debug', 'Release', 'RelWithDebInfo', 'MinSizeRel')]
    [string]$Configuration = 'Release',

    [string]$BuildDir = '',
    [string]$ToolchainRoot = $env:RPI_TOOLCHAIN_ROOT,
    [string]$Sysroot = $env:RPI_SYSROOT,
    [string]$ToolchainPrefix = $env:RPI_TOOLCHAIN_PREFIX,
    [string]$Generator = '',
    [switch]$Clean,
    [switch]$ConfigureOnly
)

$ErrorActionPreference = 'Stop'

function Convert-ToCMakeBuildPath {
    param(
        [Parameter(Mandatory = $true)]
        [string]$Path
    )

    if (-not [System.IO.Path]::IsPathRooted($Path)) {
        return $Path
    }

    $rootFullPath = [System.IO.Path]::GetFullPath($PSScriptRoot).TrimEnd([System.IO.Path]::DirectorySeparatorChar, [System.IO.Path]::AltDirectorySeparatorChar)
    $buildFullPath = [System.IO.Path]::GetFullPath($Path).TrimEnd([System.IO.Path]::DirectorySeparatorChar, [System.IO.Path]::AltDirectorySeparatorChar)

    if ($buildFullPath.StartsWith($rootFullPath, [System.StringComparison]::OrdinalIgnoreCase)) {
        $relativePath = $buildFullPath.Substring($rootFullPath.Length).TrimStart([System.IO.Path]::DirectorySeparatorChar, [System.IO.Path]::AltDirectorySeparatorChar)
        if (-not [string]::IsNullOrWhiteSpace($relativePath)) {
            return $relativePath
        }
    }

    return $Path
}

$cmake = Get-Command cmake -ErrorAction SilentlyContinue
if (-not $cmake) {
    Write-Error '[YuanBook] CMake was not found. Install CMake, then run ./build-rpi.ps1 again.'
    exit 1
}

if ([string]::IsNullOrWhiteSpace($ToolchainPrefix)) {
    if ($Arch -eq 'aarch64') {
        $ToolchainPrefix = 'aarch64-none-linux-gnu'
    } else {
        $ToolchainPrefix = 'arm-linux-gnueabihf'
    }
}

if ([string]::IsNullOrWhiteSpace($BuildDir)) {
    $BuildDir = "build-rpi-$Arch"
}

$toolchainFile = Join-Path $PSScriptRoot "cmake/toolchain-rpi-$Arch.cmake"
if (-not (Test-Path -LiteralPath $toolchainFile)) {
    Write-Error "[YuanBook] Toolchain file was not found: $toolchainFile"
    exit 1
}

if (-not [string]::IsNullOrWhiteSpace($ToolchainRoot) -and -not (Test-Path -LiteralPath $ToolchainRoot)) {
    Write-Error "[YuanBook] RPI_TOOLCHAIN_ROOT does not exist: $ToolchainRoot"
    exit 1
}

if (-not [string]::IsNullOrWhiteSpace($Sysroot) -and -not (Test-Path -LiteralPath $Sysroot)) {
    Write-Error "[YuanBook] RPI_SYSROOT does not exist: $Sysroot"
    exit 1
}

$compilerNames = @("$ToolchainPrefix-gcc", "$ToolchainPrefix-g++")
$missingCompilers = @()
foreach ($compilerName in $compilerNames) {
    $compiler = $null
    if (-not [string]::IsNullOrWhiteSpace($ToolchainRoot)) {
        $candidate = Join-Path $ToolchainRoot $compilerName
        $candidateExe = "$candidate.exe"
        if (Test-Path -LiteralPath $candidate) {
            $compiler = $candidate
        } elseif (Test-Path -LiteralPath $candidateExe) {
            $compiler = $candidateExe
        } else {
            $rootBinCandidate = Join-Path (Join-Path $ToolchainRoot 'bin') $compilerName
            $rootBinCandidateExe = "$rootBinCandidate.exe"
            if (Test-Path -LiteralPath $rootBinCandidate) {
                $compiler = $rootBinCandidate
            } elseif (Test-Path -LiteralPath $rootBinCandidateExe) {
                $compiler = $rootBinCandidateExe
            }
        }
    }

    if (-not $compiler) {
        $compiler = Get-Command $compilerName -ErrorAction SilentlyContinue
    }

    if (-not $compiler) {
        $missingCompilers += $compilerName
    }
}

if ($missingCompilers.Count -gt 0) {
    Write-Error ("[YuanBook] Raspberry Pi $Arch cross compiler was not found: {0}. Install/provide the toolchain, add it to PATH, or pass -ToolchainRoot / set RPI_TOOLCHAIN_ROOT." -f ($missingCompilers -join ', '))
    exit 1
}

if ($Clean -and (Test-Path -LiteralPath $BuildDir)) {
    Remove-Item -LiteralPath $BuildDir -Recurse -Force
}

$cmakeBuildDir = Convert-ToCMakeBuildPath -Path $BuildDir

$configureArgs = @(
    '-S', '.',
    '-B', $cmakeBuildDir,
    "-DCMAKE_BUILD_TYPE=$Configuration",
    "-DCMAKE_TOOLCHAIN_FILE=$toolchainFile",
    "-DRPI_TOOLCHAIN_PREFIX=$ToolchainPrefix",
    '-DYUANBOOK_USE_BUNDLED_SQLITE=ON'
)

if (-not [string]::IsNullOrWhiteSpace($ToolchainRoot)) {
    $configureArgs += "-DRPI_TOOLCHAIN_ROOT=$ToolchainRoot"
}

if (-not [string]::IsNullOrWhiteSpace($Sysroot)) {
    $configureArgs += "-DRPI_SYSROOT=$Sysroot"
}

if (-not [string]::IsNullOrWhiteSpace($Generator)) {
    $configureArgs += @('-G', $Generator)
}

Write-Host "[YuanBook] Configuring Raspberry Pi $Arch build in '$cmakeBuildDir'..."
& cmake @configureArgs
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }

if ($ConfigureOnly) {
    Write-Host "[YuanBook] Configure completed. Skipping build because -ConfigureOnly was specified."
    exit 0
}

Write-Host "[YuanBook] Building Raspberry Pi $Arch main target only..."
& cmake --build $cmakeBuildDir --config $Configuration --target YuanBook
exit $LASTEXITCODE
