param(
    [ValidateSet('Debug', 'Release', 'RelWithDebInfo', 'MinSizeRel')]
    [string]$Configuration = 'Release'
)

$cmake = Get-Command cmake -ErrorAction SilentlyContinue
if (-not $cmake) {
    Write-Error '[YuanBook] CMake was not found. Install CMake and a C++17 compiler, then run ./build.ps1 again.'
    exit 1
}

cmake -S . -B build -DCMAKE_BUILD_TYPE=$Configuration
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }

cmake --build build --config $Configuration
exit $LASTEXITCODE
