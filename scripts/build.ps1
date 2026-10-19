param([switch]$clean)

$ndk = $env:ANDROID_NDK_HOME
if (-not $ndk) { $ndk = $env:ANDROID_NDK_ROOT }
if (-not $ndk -and (Test-Path ndk-path.txt)) { $ndk = (Get-Content ndk-path.txt -Raw).Trim() }
if (-not $ndk) { Write-Error "Set ANDROID_NDK_HOME to your NDK folder (r27)"; exit 1 }

if ($clean -and (Test-Path build)) { Remove-Item build -Recurse -Force }
if (-not (Test-Path build)) { New-Item -ItemType Directory build | Out-Null }

& cmake -G "Ninja" `
  -DCMAKE_BUILD_TYPE="RelWithDebInfo" `
  -DCMAKE_TOOLCHAIN_FILE="$ndk/build/cmake/android.toolchain.cmake" `
  -DANDROID_ABI=arm64-v8a -DANDROID_PLATFORM=android-24 -DANDROID_STL=c++_static `
  -B build
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
& cmake --build ./build
exit $LASTEXITCODE
