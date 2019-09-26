@echo off
set APP_NAME=Pak_Decrypt.exe
call "C:\Program Files (x86)\Microsoft Visual Studio\2019\Community\Common7\Tools\VsDevCmd.bat" -arch=amd64 -host_arch=amd64
cd /d "%~dp0"

set CL=/nologo /errorReport:none /Gm- /GF /GS- /MP /MT /W4 /WX /wd4324 /wd4996 /D_CRT_SECURE_NO_DEPRECATE
set LINK=/errorReport:none /INCREMENTAL:NO

set CL=%CL% /Ox
rem set CL=%CL% /Od /Zi
rem set LINK=%LINK% /DEBUG

echo.
set APP_NAME=Pak_Decrypt
cl.exe %APP_NAME%.c /Fe%APP_NAME%.exe
if %ERRORLEVEL% neq 0 goto out
echo =^> %APP_NAME%.exe

echo.
set APP_NAME=Lxr_Decrypt
cl.exe %APP_NAME%.c puff.c /Fe%APP_NAME%
if %ERRORLEVEL% neq 0 goto out
echo =^> %APP_NAME%

:out
pause
