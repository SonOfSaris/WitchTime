@echo off
setlocal
cd /d "%~dp0"
call "C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars64.bat" >nul
if errorlevel 1 exit /b 1
if not exist out mkdir out
cl /nologo /O2 /W3 /EHsc /std:c++17 /MT codecheck.cpp /Fo:out\ /Fe:out\codecheck.exe /link user32.lib kernel32.lib
if errorlevel 1 exit /b 1
if not exist game_bytes.inc (
    echo tests\game_bytes.inc is missing: run python gen_bytes.py [path to CONTROLResonant.exe] first
    exit /b 1
)
cl /nologo /O2 /W3 /EHsc /std:c++17 /MT fakegame.cpp /Fo:out\ /Fe:out\fakegame.exe /link /MERGE:.patt=.text user32.lib kernel32.lib
if errorlevel 1 exit /b 1
cl /nologo /O2 /W3 /EHsc /std:c++17 /MT /DNO_PATTERN fakegame.cpp /Fo:out\nopattern_ /Fe:out\fakegame_nopattern.exe /link user32.lib kernel32.lib
exit /b %errorlevel%
