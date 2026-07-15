[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)]
    [ValidateNotNullOrEmpty()]
    [string]$ProjectRoot,

    [Parameter(Mandatory = $true)]
    [ValidateNotNullOrEmpty()]
    [string]$ExecutablePath,

    [Parameter(Mandatory = $true)]
    [ValidateNotNullOrEmpty()]
    [string]$DistDir,

    [Parameter(Mandatory = $true)]
    [ValidateNotNullOrEmpty()]
    [string]$OutputExecutableName
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

function Resolve-FullPath {
    <#
    .SYNOPSIS
        将输入路径规范化为绝对路径。
    .PARAMETER Path
        待规范化的路径，可以是相对路径或绝对路径。
    .OUTPUTS
        返回规范化后的绝对路径字符串。
    #>
    param(
        [Parameter(Mandatory = $true)]
        [ValidateNotNullOrEmpty()]
        [string]$Path
    )

    return [System.IO.Path]::GetFullPath($Path)
}

function Copy-RequiredDirectory {
    <#
    .SYNOPSIS
        复制发布包必需的完整目录树。
    .PARAMETER SourceDir
        必需资源目录的源路径；目录不存在时抛出异常并终止打包。
    .PARAMETER DestinationDir
        资源目录在临时发布目录中的目标路径。
    .NOTES
        本函数会复制所有子目录和文件，不执行隐式过滤。
    #>
    param(
        [Parameter(Mandatory = $true)]
        [ValidateNotNullOrEmpty()]
        [string]$SourceDir,

        [Parameter(Mandatory = $true)]
        [ValidateNotNullOrEmpty()]
        [string]$DestinationDir
    )

    if (-not (Test-Path -LiteralPath $SourceDir -PathType Container)) {
        throw "发布所需目录不存在: $SourceDir"
    }

    Copy-Item -LiteralPath $SourceDir -Destination $DestinationDir -Recurse -Force
}

$projectRootPath = Resolve-FullPath -Path $ProjectRoot
$executableFullPath = Resolve-FullPath -Path $ExecutablePath
$distFullPath = Resolve-FullPath -Path $DistDir
$distParentPath = Split-Path -Parent $distFullPath
$distLeafName = Split-Path -Leaf $distFullPath
$stagingPath = Join-Path $distParentPath ".$distLeafName.staging"
$backupPath = Join-Path $distParentPath ".$distLeafName.previous"

if (-not (Test-Path -LiteralPath $projectRootPath -PathType Container)) {
    Write-Error "项目根目录不存在: $projectRootPath"
    exit 1
}

if (-not (Test-Path -LiteralPath $executableFullPath -PathType Leaf)) {
    Write-Error "编译产物不存在: $executableFullPath"
    exit 1
}

try {
    if (-not (Test-Path -LiteralPath $distParentPath -PathType Container)) {
        New-Item -ItemType Directory -Path $distParentPath -Force | Out-Null
    }

    # 使用临时目录完成全部归集，避免复制失败时污染现有可用发布目录。
    foreach ($obsoletePath in @($stagingPath, $backupPath)) {
        if (Test-Path -LiteralPath $obsoletePath) {
            Remove-Item -LiteralPath $obsoletePath -Recurse -Force
        }
    }
    New-Item -ItemType Directory -Path $stagingPath -Force | Out-Null

    $targetExecutablePath = Join-Path $stagingPath $OutputExecutableName
    Copy-Item -LiteralPath $executableFullPath -Destination $targetExecutablePath -Force

    Copy-RequiredDirectory `
        -SourceDir (Join-Path $projectRootPath 'www') `
        -DestinationDir (Join-Path $stagingPath 'www')
    Copy-RequiredDirectory `
        -SourceDir (Join-Path $projectRootPath 'content') `
        -DestinationDir (Join-Path $stagingPath 'content')

    $databaseCandidates = @(
        (Join-Path $projectRootPath 'yuanbook/ledger.db'),
        (Join-Path $projectRootPath 'ledger.db')
    )
    $databaseTemplate = $databaseCandidates | Where-Object {
        Test-Path -LiteralPath $_ -PathType Leaf
    } | Select-Object -First 1

    if ($databaseTemplate) {
        # 只复制 SQLite 主数据库模板；WAL/SHM 是运行时伴生文件，禁止进入发布包。
        Copy-Item -LiteralPath $databaseTemplate -Destination (Join-Path $stagingPath 'ledger.db') -Force
        Write-Host "[YuanBook] Database template: $databaseTemplate"
    } else {
        Write-Warning '[YuanBook] 未找到 ledger.db 模板；程序首次运行时将按现有逻辑创建并初始化数据库。'
    }

    # 在同一父目录内切换发布目录；失败时尝试恢复旧发布版本。
    if (Test-Path -LiteralPath $distFullPath) {
        Move-Item -LiteralPath $distFullPath -Destination $backupPath
    }

    try {
        Move-Item -LiteralPath $stagingPath -Destination $distFullPath
    } catch {
        if (Test-Path -LiteralPath $backupPath) {
            Move-Item -LiteralPath $backupPath -Destination $distFullPath
        }
        throw
    }

    if (Test-Path -LiteralPath $backupPath) {
        Remove-Item -LiteralPath $backupPath -Recurse -Force
    }

    Write-Host "[YuanBook] Release package completed: $distFullPath"
    Write-Host "[YuanBook] Included: $OutputExecutableName, www, content, ledger.db (if available)."
    Write-Host '[YuanBook] Excluded: ledger.db-wal, ledger.db-shm, logs, backups and test databases.'
    exit 0
} catch {
    if (Test-Path -LiteralPath $stagingPath) {
        Remove-Item -LiteralPath $stagingPath -Recurse -Force -ErrorAction SilentlyContinue
    }

    Write-Error "发布文件归集失败: $($_.Exception.Message)"
    exit 1
}
