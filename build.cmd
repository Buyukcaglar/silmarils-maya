@echo off
rem build.cmd [MAYA_DATA folder] - builds maya64.exe and maya_cli.exe into build\win (Release, Ninja).
rem Uses clang-cl when LLVM is installed, else MSVC's cl.  Needs Visual Studio (2022 or later), CMake, Ninja, Python 3.
setlocal
set "ROOT=%~dp0"
set "DATA=%~1"
if "%DATA%"=="" set "DATA=%ROOT%_extracted\amiga\Maya\data"

if defined VCINSTALLDIR goto have_vs
set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
if exist "%VSWHERE%" for /f "usebackq delims=" %%i in (`"%VSWHERE%" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do set "VS=%%i"
if not defined VS set "VS=%ProgramFiles%\Microsoft Visual Studio\18\Professional"
call "%VS%\VC\Auxiliary\Build\vcvars64.bat" >nul || exit /b 1
:have_vs

set "CC=cl"
where clang-cl >nul 2>nul && set "CC=clang-cl"
if exist "%ProgramFiles%\LLVM\bin\clang-cl.exe" set "CC=%ProgramFiles%\LLVM\bin\clang-cl.exe"

cmake -G Ninja -S "%ROOT%." -B "%ROOT%build\win" -DCMAKE_BUILD_TYPE=Release "-DCMAKE_C_COMPILER=%CC%" "-DMAYA_DATA=%DATA%" || exit /b 1
cmake --build "%ROOT%build\win" || exit /b 1
echo.
echo Built: %ROOT%build\win\maya64.exe
