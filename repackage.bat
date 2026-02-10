@echo off
rem Repack all .zip files in this directory using 7-Zip with maximum compression.
rem Usage: place this batch in the folder with zip files and run it.
setlocal

set "SEVENZ=C:\Program Files\7-Zip\7z.exe"
if not exist "%SEVENZ%" (
  echo "%SEVENZ%" was not found. Please install 7-Zip at that location or update this script.
  exit /b 1
)

echo wTF is happening
for %%F in (*.zip) do (
  call :repack "%%F"
)

endlocal
exit /b 0

:repack
set "file=%~1"
set "name=%~n1"
set "tmpdir=%TEMP%\repack_%name%_%RANDOM%"
if exist "%tmpdir%" rmdir /s /q "%tmpdir%"
mkdir "%tmpdir%"
echo Extracting "%file%" to "%tmpdir%"...
"%SEVENZ%" x -y "%file%" -o"%tmpdir%"
if errorlevel 1 (
  echo Extraction failed for "%file%". Cleaning up.
  rmdir /s /q "%tmpdir%"
  exit /b 1
)
echo Recreating "%file%" with maximum compression...
set "bak=%file%.bak"
move /Y "%file%" "%bak%" >nul 2>&1
"%SEVENZ%" a -tzip -mx=9 "%file%" "%tmpdir%\*"
if errorlevel 1 (
  echo Compression failed for "%file%". Restoring original.
  move /Y "%bak%" "%file%" >nul 2>&1
  rmdir /s /q "%tmpdir%"
  exit /b 1
) else (
  del /Q "%bak%" >nul 2>&1
  rmdir /s /q "%tmpdir%"
  echo Done: "%file%"
)
exit /b 0
