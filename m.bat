@echo off
setlocal EnableExtensions

REM m.bat | $CMakeCurrentProductFile$ | $ProjectFileDir$
REM Runs kaiagentd with the already-initialized runtime from init.bat.
REM kaiagentd starts with the built-in fast256 model by default.
REM adb connect ip:port

if "%~1"=="" (
    echo Usage: %~nx0 ^<local libkaiagentd.so^>
    echo Run init.bat first after changing runtime libraries or models.
    exit /b 1
)

set "ADB=adb"
if defined ANDROID_SERIAL set "ADB=adb -s %ANDROID_SERIAL%"

set "LOCAL_BINARY=%~1"
set "REMOTE_ROOT=/data/local/tmp/com.enenh.ai"
set "REMOTE_LIB_DIR=%REMOTE_ROOT%/lib"
set "REMOTE_PID=%REMOTE_ROOT%/kaiagentd.pid"
set "REMOTE_LOG=%REMOTE_ROOT%/kaiagentd.log"

for %%F in ("%LOCAL_BINARY%") do set "BINARY_NAME=%%~nxF"

if not exist "%LOCAL_BINARY%" (
    echo Missing binary: %LOCAL_BINARY%
    exit /b 1
)

%ADB% shell "sh -c 'test -d %REMOTE_LIB_DIR% && test -f %REMOTE_LIB_DIR%/libtensorflowlite_jni.so && test -f %REMOTE_LIB_DIR%/libQnnTFLiteDelegate.so'"
if errorlevel 1 (
    echo Remote runtime is incomplete. Run init.bat first.
    exit /b 1
)

%ADB% push "%LOCAL_BINARY%" "%REMOTE_LIB_DIR%/%BINARY_NAME%"
if errorlevel 1 exit /b 1

%ADB% shell "sh -c 'chmod 755 %REMOTE_LIB_DIR%/%BINARY_NAME%'"
if errorlevel 1 exit /b 1

echo Starting %BINARY_NAME% with default fast256 model
%ADB% shell "sh -c 'PID=%REMOTE_PID%; LOG=%REMOTE_LOG%; if [ -f $PID ]; then OLD=$(cat $PID 2>/dev/null); if [ x$OLD != x ] && kill -0 $OLD 2>/dev/null; then echo stopping existing pid=$OLD; kill -TERM $OLD 2>/dev/null; for i in 1 2 3; do kill -0 $OLD 2>/dev/null || break; sleep 1; done; if kill -0 $OLD 2>/dev/null; then kill -KILL $OLD 2>/dev/null; fi; fi; rm -f $PID; fi; export LD_LIBRARY_PATH=%REMOTE_LIB_DIR%; export ADSP_LIBRARY_PATH=%REMOTE_LIB_DIR%\;/vendor/lib/rfsa/adsp; export CDSP_LIBRARY_PATH=%REMOTE_LIB_DIR%; export QNN_SIGNED_PD=1; cd %REMOTE_LIB_DIR%; %REMOTE_LIB_DIR%/%BINARY_NAME%; for i in 1 2 3 4 5; do if [ -s $PID ]; then NEW=$(cat $PID 2>/dev/null); if [ x$NEW != x ] && kill -0 $NEW 2>/dev/null; then echo started pid=$NEW; exit 0; fi; fi; sleep 1; done; echo start failed; if [ -f $LOG ]; then tail -n 80 $LOG; fi; exit 1'"
if errorlevel 1 exit /b 1
exit /b 0
