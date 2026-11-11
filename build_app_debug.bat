@echo off
setlocal EnableExtensions

for %%I in ("%~dp0.") do set "ROOT_DIR=%%~fI"
set "APP_DIR=%ROOT_DIR%\app"
if not defined KAIAGENTD_CMAKE set "KAIAGENTD_CMAKE=cmake"
if not defined KAIAGENTD_ABI set "KAIAGENTD_ABI=arm64-v8a"
if not defined ANDROID_NDK_HOME if defined ANDROID_NDK_ROOT set "ANDROID_NDK_HOME=%ANDROID_NDK_ROOT%"

if not defined ANDROID_NDK_HOME (
    echo Set ANDROID_NDK_HOME to your Android NDK installation directory.
    exit /b 1
)
if not exist "%ANDROID_NDK_HOME%\build\cmake\android.toolchain.cmake" (
    echo ANDROID_NDK_HOME does not contain an Android NDK toolchain.
    exit /b 1
)
if not exist "%APP_DIR%\gradlew.bat" (
    echo Missing Gradle wrapper: %APP_DIR%\gradlew.bat
    exit /b 1
)
"%KAIAGENTD_CMAKE%" --version >nul 2>&1
if errorlevel 1 (
    echo Install CMake on PATH or set KAIAGENTD_CMAKE to cmake.exe.
    exit /b 1
)
where ninja >nul 2>&1
if errorlevel 1 (
    echo Install Ninja on PATH before building.
    exit /b 1
)

pushd "%APP_DIR%" >nul
call gradlew.bat assembleDebug ^
    "-Pkaiagentd.cmake=%KAIAGENTD_CMAKE%" ^
    "-Pkaiagentd.ndk=%ANDROID_NDK_HOME%" ^
    "-Pkaiagentd.buildType=Debug" ^
    "-Pkaiagentd.buildDir=cmake-build-debug" ^
    "-Pkaiagentd.abi=%KAIAGENTD_ABI%" ^
    %*
set "BUILD_RESULT=%ERRORLEVEL%"
popd >nul
if not "%BUILD_RESULT%"=="0" exit /b %BUILD_RESULT%

echo.
echo Debug APK outputs:
for %%F in ("%APP_DIR%\installer\build\outputs\apk\debug\*.apk") do if exist "%%~fF" echo %%~fF
for %%F in ("%APP_DIR%\runner\build\outputs\apk\debug\*.apk") do if exist "%%~fF" echo %%~fF

exit /b 0
