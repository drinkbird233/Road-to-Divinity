<#
.SYNOPSIS
    一键提交并推送到 GitHub（Road-to-Divinity / 校赛智能车）

.DESCRIPTION
    做四件事：
      1. 把本地工程（D:\car\smartcar\SmartCar_F407）的改动提交
      2. 把提交后的文件树导出到暂存仓库的 校赛智能车\ 子目录
      3. 在暂存仓库提交
      4. 推送到 GitHub

    为什么要有"暂存仓库"：本地工程是仓库根，而 GitHub 上要求"一个项目一个目录"，
    两者的目录结构不同，直接给本地工程配 remote 会 push 冲突。所以用一份
    clon 出来的仓库当跳板（见主档「七、Git 仓库与项目上传流程」）。

.PARAMETER Message
    提交信息。不填则用「更新：yyyy-MM-dd HH:mm」。

.EXAMPLE
    .\push.ps1
    .\push.ps1 "修好循迹 I2C 的 25 kHz 限制"
#>
param(
    [string]$Message = ""
)

# 注意：不能用 "Stop"。git 会把警告（比如 CRLF 提示）写到 stderr，
# 在 Stop 模式下 PowerShell 会把它当成终止性错误，导致脚本半路退出。
# 所以统一用 Continue，每个 git 调用后面自己检查 $LASTEXITCODE。
$ErrorActionPreference = "Continue"

# ---------------- 配置 ----------------
$local   = "D:\car\smartcar\SmartCar_F407"          # 本地工程（代码源）
$staging = "D:\car\smartcar\Road-to-Divinity"       # 远端仓库的本地副本（跳板）
$projDir = "校赛智能车"                              # GitHub 上的项目目录名
$branch  = "main"
$repoUrl = "https://github.com/drinkbird233/Road-to-Divinity.git"

function Info($t) { Write-Host $t -ForegroundColor Cyan }
function Ok($t)   { Write-Host $t -ForegroundColor Green }
function Warn($t) { Write-Host $t -ForegroundColor Yellow }

# ---------------- 0. 环境检查 ----------------
if (-not (Test-Path (Join-Path $local ".git"))) {
    throw "本地仓库不存在：$local"
}
if (-not (Test-Path (Join-Path $staging ".git"))) {
    throw "暂存仓库不存在：$staging`n请先执行：git clone --depth 1 $repoUrl `"$staging`""
}
if (-not $Message) {
    $Message = "更新：" + (Get-Date -Format "yyyy-MM-dd HH:mm")
}

# ---------------- 1. 本地提交 ----------------
Info "[1/4] 检查本地改动 ..."
git -C $local add -A 2>$null
$pending = git -C $local status --porcelain 2>$null
if ($pending) {
    $n = ($pending | Measure-Object).Count
    git -C $local -c i18n.commitEncoding=UTF-8 commit -m $Message | Out-Null
    if ($LASTEXITCODE -ne 0) { throw "本地提交失败" }
    Ok "      已提交 $n 处改动"
} else {
    Warn "      没有改动，跳过提交"
}

# ---------------- 2. 导出到暂存仓库 ----------------
Info "[2/4] 导出文件到暂存仓库 ..."
$dest = Join-Path $staging $projDir
if (Test-Path $dest) { Remove-Item $dest -Recurse -Force }
New-Item -ItemType Directory -Path $dest -Force | Out-Null

# 用 checkout-index 从 git 索引导出，只会导出被跟踪的文件，
# 不会把 Objects/Listings 这类构建产物带上去。
$prefix = ($dest -replace '\\', '/') + "/"
git -C $local checkout-index -a -f --prefix=$prefix
if ($LASTEXITCODE -ne 0) { throw "导出失败（checkout-index）" }

$files = (Get-ChildItem -LiteralPath $dest -Recurse -File -Force | Measure-Object).Count
Ok "      已导出 $files 个文件"

# ---------------- 3. 暂存仓库提交 ----------------
Info "[3/4] 暂存仓库提交 ..."
git -C $staging add -A 2>$null
$pending2 = git -C $staging status --porcelain 2>$null
if ($pending2) {
    git -C $staging -c i18n.commitEncoding=UTF-8 commit -m $Message | Out-Null
    if ($LASTEXITCODE -ne 0) { throw "暂存仓库提交失败" }
    Ok "      已提交"
} else {
    Warn "      内容与远端一致，跳过提交"
}

# ---------------- 4. 推送 ----------------
Info "[4/4] 推送到 GitHub ..."
$env:GIT_TERMINAL_PROMPT = "0"      # 凭据缺失时立刻报错，不挂起等输入
git -C $staging push origin $branch
if ($LASTEXITCODE -ne 0) {
    throw "推送失败：检查网络或 GitHub 凭据（Windows 凭据管理器）"
}

Ok ""
Ok "完成。项目地址："
Ok "  https://github.com/drinkbird233/Road-to-Divinity/tree/$branch/$projDir"
