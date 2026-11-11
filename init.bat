@echo off
setlocal EnableExtensions

REM init.bat
REM Synchronizes and verifies the remote runtime libraries and models.
REM adb connect ip:port

set "ADB=adb"
if defined ANDROID_SERIAL set "ADB=adb -s %ANDROID_SERIAL%"

set "LOCAL_LIB_DIR=%~dp0lib\arm64-v8a"
set "REMOTE_ROOT=/data/local/tmp/com.enenh.ai"
set "REMOTE_LIB_DIR=%REMOTE_ROOT%/lib"
set "REMOTE_QNN_CACHE=%REMOTE_ROOT%/qnn_cache"
set "DEVICE_LIBRARY_DIRS=/vendor/lib64 /system_ext/lib64 /product/lib64 /system/lib64 /apex/com.android.runtime/lib64 /apex/com.android.runtime/lib64/bionic"
set "SYSTEM_LINKER_DIRS=/system/lib64 /apex/com.android.runtime/lib64 /apex/com.android.runtime/lib64/bionic"
set "REQUIRED_LOCAL_LIBS=libQnnHtp.so libQnnHtpPrepare.so libQnnHtpV68Skel.so libQnnHtpV68Stub.so libQnnHtpV69Skel.so libQnnHtpV69Stub.so libQnnHtpV73Skel.so libQnnHtpV73Stub.so libQnnHtpV75Skel.so libQnnHtpV75Stub.so libQnnHtpV79Skel.so libQnnHtpV79Stub.so libQnnHtpV81Skel.so libQnnHtpV81Stub.so libQnnSystem.so libQnnTFLiteDelegate.so libqnn_delegate_jni.so libtensorflowlite_jni.so"

if not exist "%LOCAL_LIB_DIR%" (
    echo Missing local runtime directory: %LOCAL_LIB_DIR%
    echo See README.md for locally supplied runtime libraries and models.
    exit /b 1
)

for %%L in (%REQUIRED_LOCAL_LIBS%) do (
    if not exist "%LOCAL_LIB_DIR%\%%L" (
        echo Missing local runtime library: %LOCAL_LIB_DIR%\%%L
        echo Runtime libraries are not distributed with this source repository. See README.md.
        exit /b 1
    )
)

for %%M in (model.tflite model_int8.tflite model_fast256_int8.tflite) do (
    if not exist "%LOCAL_LIB_DIR%\%%M" (
        echo Missing local model: %LOCAL_LIB_DIR%\%%M
        echo Model weights are supplied locally. See README.md.
        exit /b 1
    )
)

echo Preparing remote runtime directory with %ADB%
%ADB% shell "sh -c 'mkdir -p %REMOTE_ROOT% %REMOTE_LIB_DIR% %REMOTE_QNN_CACHE%'"
if errorlevel 1 exit /b 1

echo Synchronizing packaged runtime libraries...
for /f "delims=" %%L in ('dir /b /a-d "%LOCAL_LIB_DIR%\*.so"') do (
    call :pushIfSizeChanged "%LOCAL_LIB_DIR%\%%L" "%REMOTE_LIB_DIR%/%%L" "%%L"
    if errorlevel 1 exit /b 1
)

echo Synchronizing models...
for %%M in (model.tflite model_int8.tflite model_fast256_int8.tflite) do (
    call :pushIfSizeChanged "%LOCAL_LIB_DIR%\%%M" "%REMOTE_LIB_DIR%/%%M" "%%M"
    if errorlevel 1 exit /b 1
)

echo Mirroring device runtime dependencies...
call :mirrorRequired "ld-android.so" "%SYSTEM_LINKER_DIRS%"
if errorlevel 1 exit /b 1
call :mirrorRequired "libdl_android.so" "%SYSTEM_LINKER_DIRS%"
if errorlevel 1 exit /b 1
call :mirrorRequired "libc++.so" "%DEVICE_LIBRARY_DIRS%"
if errorlevel 1 exit /b 1
call :mirrorRequired "libbase.so" "%DEVICE_LIBRARY_DIRS%"
if errorlevel 1 exit /b 1
call :mirrorRequired "libcutils.so" "%DEVICE_LIBRARY_DIRS%"
if errorlevel 1 exit /b 1
call :mirrorRequired "libvndksupport.so" "%SYSTEM_LINKER_DIRS%"
if errorlevel 1 exit /b 1
call :mirrorRequired "libutils.so" "%DEVICE_LIBRARY_DIRS%"
if errorlevel 1 exit /b 1
call :mirrorRequired "libdmabufheap.so" "%DEVICE_LIBRARY_DIRS%"
if errorlevel 1 exit /b 1
call :mirrorRequired "libhardware.so" "%DEVICE_LIBRARY_DIRS%"
if errorlevel 1 exit /b 1
call :mirrorRequired "libhidlbase.so" "%DEVICE_LIBRARY_DIRS%"
if errorlevel 1 exit /b 1
call :mirrorOptional "libhwbinder.so" "%DEVICE_LIBRARY_DIRS%"
if errorlevel 1 exit /b 1
call :mirrorOptional "libhidltransport.so" "%DEVICE_LIBRARY_DIRS%"
if errorlevel 1 exit /b 1
call :mirrorOptional "android.hidl.memory@1.0.so" "%DEVICE_LIBRARY_DIRS%"
if errorlevel 1 exit /b 1
call :mirrorOptional "android.hidl.memory.token@1.0.so" "%DEVICE_LIBRARY_DIRS%"
if errorlevel 1 exit /b 1
call :mirrorOptional "libhidlmemory.so" "%DEVICE_LIBRARY_DIRS%"
if errorlevel 1 exit /b 1
call :mirrorRequired "android.hardware.common-V2-ndk.so" "%DEVICE_LIBRARY_DIRS%"
if errorlevel 1 exit /b 1
call :mirrorRequired "vendor.qti.hardware.dsp-V1-ndk.so" "%DEVICE_LIBRARY_DIRS%"
if errorlevel 1 exit /b 1
call :mirrorOptional "vendor.qti.hardware.dsp@1.0.so" "%DEVICE_LIBRARY_DIRS%"
if errorlevel 1 exit /b 1
call :mirrorRequired "libvmmem.so" "%DEVICE_LIBRARY_DIRS%"
if errorlevel 1 exit /b 1
call :mirrorRequired "libcdsprpc.so" "%DEVICE_LIBRARY_DIRS%"
if errorlevel 1 exit /b 1

echo Verifying remote runtime...
for /f "delims=" %%L in ('dir /b /a-d "%LOCAL_LIB_DIR%\*.so"') do (
    call :requireRemoteFile "%%L"
    if errorlevel 1 exit /b 1
)
for %%M in (model.tflite model_int8.tflite model_fast256_int8.tflite) do (
    call :requireRemoteFile "%%M"
    if errorlevel 1 exit /b 1
)
for %%D in (
    ld-android.so
    libdl_android.so
    libc++.so
    libbase.so
    libcutils.so
    libvndksupport.so
    libutils.so
    libdmabufheap.so
    libhardware.so
    libhidlbase.so
    android.hardware.common-V2-ndk.so
    vendor.qti.hardware.dsp-V1-ndk.so
    libvmmem.so
    libcdsprpc.so
) do (
    call :requireRemoteFile "%%D"
    if errorlevel 1 exit /b 1
)

%ADB% shell "sh -c 'chmod 755 %REMOTE_LIB_DIR%/*.so'"
if errorlevel 1 exit /b 1

echo Runtime libraries and models are ready.
exit /b 0

:pushIfSizeChanged
set "LOCAL_PATH=%~1"
set "REMOTE_PATH=%~2"
set "LABEL=%~3"
for %%F in ("%LOCAL_PATH%") do set "LOCAL_SIZE=%%~zF"
call :remoteFileSize "%REMOTE_PATH%" REMOTE_SIZE
if "%REMOTE_SIZE%"=="%LOCAL_SIZE%" (
    echo %LABEL% is current.
    exit /b 0
)
echo Pushing %LABEL% ...
%ADB% push "%LOCAL_PATH%" "%REMOTE_PATH%"
if errorlevel 1 exit /b 1
exit /b 0

:remoteFileSize
set "REMOTE_SIZE_RESULT=-1"
for /f "delims=" %%S in ('%ADB% shell "if [ -f %~1 ]; then wc -c < %~1; else echo -1; fi"') do (
    set "REMOTE_SIZE_RESULT=%%S"
    goto :remoteFileSizeDone
)
:remoteFileSizeDone
set "%~2=%REMOTE_SIZE_RESULT%"
exit /b 0

:mirrorRequired
call :mirrorDeviceLibrary "%~1" "%~2" "1"
exit /b %errorlevel%

:mirrorOptional
call :mirrorDeviceLibrary "%~1" "%~2" "0"
exit /b %errorlevel%

:mirrorDeviceLibrary
set "DEP_NAME=%~1"
set "DEP_DIRS=%~2"
set "DEP_REQUIRED=%~3"
echo Mirroring %DEP_NAME% ...
%ADB% shell "sh -c 'found=0; copied=0; for dir in %DEP_DIRS%; do src=$dir/%DEP_NAME%; if [ -f $src ]; then found=1; cp -f $src %REMOTE_LIB_DIR%/%DEP_NAME% && chmod 755 %REMOTE_LIB_DIR%/%DEP_NAME% && copied=1 && break || exit 3; fi; done; if [ $copied -eq 1 ]; then exit 0; fi; if [ %DEP_REQUIRED% -eq 1 ]; then echo missing required device library %DEP_NAME% >&2; exit 2; fi; exit 0'"
if errorlevel 1 exit /b 1
exit /b 0

:requireRemoteFile
%ADB% shell "sh -c 'test -f %REMOTE_LIB_DIR%/%~1'"
if errorlevel 1 (
    echo Missing remote runtime file: %~1
    exit /b 1
)
exit /b 0
