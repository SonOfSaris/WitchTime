@echo off
setlocal
cd /d "%~dp0"
call "C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars64.bat" >nul
if errorlevel 1 exit /b 1
if not exist out mkdir out
cl /nologo /O2 /W3 /EHsc /std:c++17 /MT /DWIN32_LEAN_AND_MEAN /Iminhook\include ^
   witchtime.cpp minhook\src\buffer.c minhook\src\hook.c minhook\src\trampoline.c minhook\src\hde\hde64.c ^
   /Fo:out\ /LD /link /OUT:out\witchtime.dll /IMPLIB:out\witchtime.lib user32.lib kernel32.lib
exit /b %errorlevel%
