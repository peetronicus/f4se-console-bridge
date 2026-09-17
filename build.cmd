@echo off
setlocal
if not defined F4SE_SOURCE_ROOT (
  echo Set F4SE_SOURCE_ROOT to an unmodified official F4SE source checkout.
  exit /b 2
)
set "CONFIG=%~1"
if not defined CONFIG set "CONFIG=Release"
cmake -S "%~dp0." -B "%~dp0build" -A x64 -DF4SE_SOURCE_ROOT="%F4SE_SOURCE_ROOT%"
if errorlevel 1 exit /b %errorlevel%
cmake --build "%~dp0build" --config "%CONFIG%" -- /m:2
exit /b %errorlevel%
