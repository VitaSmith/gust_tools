@rem This test script performs unpack and repack of G1T
@rem textures and validates that the data matches.
@rem The texture test files can be downloaded from:
@rem http://vitasmith.rpc1.org/gust_tools/test_g1t.7z
@rem
@echo off
setlocal EnableDelayedExpansion
call build.cmd g1t
if %ERRORLEVEL% neq 0 goto err

set list=type_01_sw type_09_ps4 type_10_psv type_12_psv type_12_psv_2 type_21_sw type_3c_3ds type_3d_3ds type_45_3ds type_59_win type_5b_win type_5f_win type_62_ps4 type_62_ps4_2

for %%a in (%list%) do (
   if exist %%a.g1t.bak move /y %%a.g1t.bak %%a.g1t >NUL 2>&1
)

for %%a in (%list%) do (
   echo | set /p PrintName=* %%a.g1t... 
   gust_g1t.exe %%a.g1t >NUL 2>&1
   if !ERRORLEVEL! neq 0 goto err
   gust_g1t.exe %%a >NUL 2>&1
   if !ERRORLEVEL! neq 0 goto err
   fc /b %%a.g1t %%a.g1t.bak >NUL 2>&1
   if !ERRORLEVEL! neq 0 goto err
   echo 	[PASS]
)

echo [ALL TESTS PASSED]
goto out

:err
echo 	[FAIL]

:out
for %%a in (%list%) do (
   if exist %%a.g1t.bak move /y %%a.g1t.bak %%a.g1t >NUL 2>&1
)
