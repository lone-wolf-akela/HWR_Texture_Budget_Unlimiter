# 构建 se9ccn_texture_budget.dll（32 位 Release）
#
# 用法：
#   pwsh -File build.ps1              # 构建
#   pwsh -File build.ps1 -Clean       # 清理后重建
#   pwsh -File build.ps1 -VSInstall 'C:\Program Files\Microsoft Visual Studio\18\Community'
#
# 依赖：Visual Studio（含"使用 C++ 的桌面开发"）+ CMake（VS 自带或单独安装）。
param(
    [switch]$Clean,
    [string]$VSInstall = ''
)
Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'
$root = $PSScriptRoot

function Find-VS {
    if ($VSInstall -ne '') {
        return (Resolve-Path -LiteralPath $VSInstall).Path
    }
    $programFilesX86 = ${env:ProgramFiles(x86)}
    if ($programFilesX86) {
        $vswhere = Join-Path $programFilesX86 'Microsoft Visual Studio\Installer\vswhere.exe'
        if (Test-Path -LiteralPath $vswhere) {
            $found = & $vswhere -latest -products * `
                -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 `
                -property installationPath
            if ($found) { return $found.Trim() }
        }
    }
    $guesses = @(
        'C:\Program Files\Microsoft Visual Studio\18\Community',
        'C:\Program Files\Microsoft Visual Studio\2022\Community',
        'C:\Program Files\Microsoft Visual Studio\2022\Professional',
        'C:\Program Files\Microsoft Visual Studio\2022\Enterprise',
        'C:\Program Files (x86)\Microsoft Visual Studio\2019\Community'
    )
    foreach ($g in $guesses) {
        if (Test-Path -LiteralPath (Join-Path $g 'VC\Auxiliary\Build\vcvarsall.bat')) {
            return $g
        }
    }
    throw '未找到 Visual Studio（需要 C++ 桌面开发组件）。可用 -VSInstall 指定安装目录。'
}

$vs = Find-VS
$vcvars = Join-Path $vs 'VC\Auxiliary\Build\vcvarsall.bat'
if (-not (Test-Path -LiteralPath $vcvars)) {
    throw "找不到 $vcvars"
}

Write-Host "==> 导入 32 位 MSVC 环境: $vcvars"
$cmdLine = '"' + $vcvars + '" x86 && set'
$envLines = cmd /c $cmdLine
foreach ($line in $envLines) {
    if ($line -match '^([^=]+)=(.*)$') {
        Set-Item -Path ('env:' + $matches[1]) -Value $matches[2] -ErrorAction SilentlyContinue
    }
}

$buildDir = Join-Path $root 'build'
if ($Clean -and (Test-Path -LiteralPath $buildDir)) {
    $resolved = (Resolve-Path -LiteralPath $buildDir).Path
    if (-not $resolved.StartsWith($root, [StringComparison]::OrdinalIgnoreCase)) {
        throw "拒绝删除不在本仓库下的目录: $resolved"
    }
    Write-Host "==> 清理 $resolved"
    Remove-Item -LiteralPath $resolved -Recurse -Force
}

$cmake = Get-Command cmake -ErrorAction SilentlyContinue
if (-not $cmake) {
    throw '找不到 cmake：请安装 CMake 并加入 PATH（Visual Studio 安装器里也可勾选）。'
}

Write-Host '==> CMake 配置（NMake / Release / x86）'
& $cmake.Source -S $root -B $buildDir -G 'NMake Makefiles' -DCMAKE_BUILD_TYPE=Release
if ($LASTEXITCODE -ne 0) { throw "cmake 配置失败（exit $LASTEXITCODE）" }

Write-Host '==> 编译'
& $cmake.Source --build $buildDir
if ($LASTEXITCODE -ne 0) { throw "构建失败（exit $LASTEXITCODE）" }

$dll = Join-Path $buildDir 'bin\se9ccn_texture_budget.dll'
if (-not (Test-Path -LiteralPath $dll)) { throw "未生成 $dll" }
$out = Join-Path $root 'se9ccn_texture_budget.dll'
Copy-Item -LiteralPath $dll -Destination $out -Force
Write-Host ''
Write-Host "==> 构建成功: $out"
Write-Host '    把该 DLL 复制到 HomeworldRM\bin\Release\ 即可生效（见 README.md）。'
