$ErrorActionPreference = 'Stop'
$PSNativeCommandUseErrorActionPreference = $true

$version = if ($env:GITHUB_REF_TYPE -eq 'tag') {
    $env:GITHUB_REF_NAME.Replace('/', '-')
} else {
    'git-' + $env:GITHUB_SHA.Substring(0, 12)
}
$name = "lincupl-$version-windows-x86_64"
$package = Join-Path $PWD "package/$name"
Move-Item package/stage $package

$tools = 'cupl', 'csim', 'cbld', 'backpin', 'cfitr', 'jedinspect',
         'jedcompare', 'fit1502', 'fit1504', 'fit1508', 'lincupl', 'linsim'
foreach ($tool in $tools) {
    if (!(Test-Path "$package/bin/$tool.exe")) {
        throw "Missing executable: $tool.exe"
    }
}
foreach ($gui in 'lincupl', 'linsim') {
    & "$env:QT_ROOT/bin/windeployqt.exe" --release --no-translations `
        --no-compiler-runtime --include-plugins qsvgicon,qsvg "$package/bin/$gui.exe"
}
foreach ($dll in 'msvcp140.dll', 'vcruntime140.dll', 'vcruntime140_1.dll') {
    if (!(Test-Path "$package/bin/$dll")) {
        throw "Missing Visual C++ runtime: $dll"
    }
}
foreach ($plugin in 'platforms/qwindows.dll', 'iconengines/qsvgicon.dll', 'imageformats/qsvg.dll') {
    if (!(Test-Path "$package/bin/$plugin")) {
        throw "Missing Qt plugin: $plugin"
    }
}
Copy-Item README.md, LICENSE, wincupl-extract-data $package
Copy-Item doc/windows.md "$package/WINDOWS.md"
New-Item -ItemType Directory -Force "$package/licenses", dist | Out-Null
Copy-Item third_party/espresso/COPYRIGHT "$package/licenses/espresso-COPYRIGHT"
python cmake/qt-notices.py "$env:RUNNER_TEMP/QtSources" "$package/licenses/Qt"

$archive = Join-Path $PWD "dist/$name.zip"
Compress-Archive -Path $package -DestinationPath $archive
"WINDOWS_ARCHIVE=$archive" >> $env:GITHUB_ENV
