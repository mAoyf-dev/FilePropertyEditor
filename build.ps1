param([ValidateSet('x86','x64','all')][string]$Architecture = 'all', [switch]$Test)
$ErrorActionPreference = 'Stop'
$vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
$install = & $vswhere -version '[17.0,18.0)' -latest -products '*' -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
if (-not $install) { throw '需要 Visual Studio 2022 C++ 桌面开发工具和 Windows SDK。' }
$vc = Get-ChildItem (Join-Path $install 'VC\Tools\MSVC') -Directory | Sort-Object Name -Descending | Select-Object -First 1 -ExpandProperty FullName
$sdk = Join-Path ${env:ProgramFiles(x86)} 'Windows Kits\10'
$sdkVersion = Get-ChildItem "$sdk/Include" -Directory | Sort-Object Name -Descending | Select-Object -First 1 -ExpandProperty Name
$oldPath = $env:PATH
Push-Location $PSScriptRoot
try {
  foreach ($arch in $(if ($Architecture -eq 'all') { @('x86','x64') } else { @($Architecture) })) {
    $bin = "$vc/bin/Hostx64/$arch"
    $env:PATH = "$bin;$oldPath"
    New-Item -ItemType Directory -Path "build/$arch/Release" -Force | Out-Null
    $common = @('/nologo','/std:c++17','/utf-8','/W4','/permissive-','/EHsc','/MT','/O2','/DUNICODE','/D_UNICODE','/DWIN32_LEAN_AND_MEAN','/DNOMINMAX','/D_WIN32_WINNT=0x0601','/DNTDDI_VERSION=0x06010000',"/I$vc/include", "/I$sdk/Include/$sdkVersion/ucrt", "/I$sdk/Include/$sdkVersion/shared", "/I$sdk/Include/$sdkVersion/um", "/I$sdk/Include/$sdkVersion/winrt",'/Isrc')
    $libs = @("/LIBPATH:$vc/lib/$arch", "/LIBPATH:$sdk/Lib/$sdkVersion/ucrt/$arch", "/LIBPATH:$sdk/Lib/$sdkVersion/um/$arch",'user32.lib','gdi32.lib','comctl32.lib','comdlg32.lib','shell32.lib','shlwapi.lib','propsys.lib','ole32.lib','oleaut32.lib','uuid.lib','version.lib','advapi32.lib','wintrust.lib','crypt32.lib','msxml6.lib')
    & "$sdk/bin/$sdkVersion/x64/rc.exe" /nologo "/I$sdk/Include/$sdkVersion/um" "/I$sdk/Include/$sdkVersion/shared" /fo "build/$arch/app.res" src/app.rc
    if ($LASTEXITCODE) { throw "资源编译失败: $arch" }
    $core = @('src/core.cpp','src/inspect.cpp','src/edit.cpp','src/version.cpp','src/export.cpp')
    & "$bin/cl.exe" @common @core src/app.cpp "build/$arch/app.res" "/Fobuild/$arch/" "/Febuild/$arch/Release/FilePropertyEditor.exe" /link @libs /MANIFEST:NO /SUBSYSTEM:WINDOWS,6.01
    if ($LASTEXITCODE) { throw "应用构建失败: $arch" }
    if ($Test) {
      & "$bin/cl.exe" @common @core tests/tests.cpp "/Fobuild/$arch/" "/Febuild/$arch/Release/property_tests.exe" /link @libs /SUBSYSTEM:CONSOLE,6.01
      if ($LASTEXITCODE) { throw "测试构建失败: $arch" }
      & "./build/$arch/Release/property_tests.exe"
      if ($LASTEXITCODE) { throw "测试失败: $arch" }
    }
    New-Item -ItemType Directory -Path "dist/$arch" -Force | Out-Null
    Copy-Item -LiteralPath "build/$arch/Release/FilePropertyEditor.exe" -Destination "dist/$arch/文件属性查改器.exe"
  }
} finally { $env:PATH = $oldPath; Pop-Location }
