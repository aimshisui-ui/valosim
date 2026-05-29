@echo off
setlocal enabledelayedexpansion

rem ============================================================
rem  VLR Manager - C++ build script (MSVC + ImGui DX11)
rem  Usage:
rem    build.bat            - release build of vlrgui + vlrtest + vlrmanager
rem    build.bat debug      - debug build (RTC1, /Zi)
rem    build.bat clean      - wipe build outputs
rem    build.bat test       - build + run vlrtest.exe
rem    build.bat run        - build + run vlrgui.exe
rem ============================================================

set ROOT=%~dp0
set BUILD=%ROOT%build
set INCLUDE_DIR=%ROOT%include
set SRC_DIR=%ROOT%src
set IMGUI_DIR=%ROOT%third_party\imgui
set IMGUI_BACKEND=%IMGUI_DIR%\backends

set ARG=%1
if "%ARG%"=="" set ARG=release

if /I "%ARG%"=="clean" (
    if exist "%BUILD%" rmdir /S /Q "%BUILD%"
    mkdir "%BUILD%"
    echo Build directory cleaned.
    pause
    exit /b 0
)

if not exist "%BUILD%" mkdir "%BUILD%"

set VCVARS="C:\Program Files (x86)\Microsoft Visual Studio\18\BuildTools\VC\Auxiliary\Build\vcvars64.bat"
if not exist %VCVARS% (
    echo ERROR: vcvars64.bat not found at %VCVARS%
    pause & exit /b 1
)
call %VCVARS% >nul
if errorlevel 1 ( echo MSVC env init failed & pause & exit /b 1 )

set MODE=release
if /I "%ARG%"=="debug" set MODE=debug

set CXXFLAGS=/nologo /std:c++17 /EHsc /W4 /WX- /permissive- /MP /DNOMINMAX /D_CRT_SECURE_NO_WARNINGS /D_WIN32_WINNT=0x0601 /I"%INCLUDE_DIR%" /I"%IMGUI_DIR%" /I"%IMGUI_BACKEND%"
set IMGUI_FLAGS=/nologo /std:c++17 /EHsc /W0 /MP /DNOMINMAX /D_CRT_SECURE_NO_WARNINGS /I"%IMGUI_DIR%" /I"%IMGUI_BACKEND%"

if /I "%MODE%"=="debug" (
    set CXXFLAGS=%CXXFLAGS% /Od /Zi /MDd /RTC1 /D_DEBUG
    set IMGUI_FLAGS=%IMGUI_FLAGS% /Od /Zi /MDd /D_DEBUG
    set LDFLAGS=/DEBUG
) else (
    set CXXFLAGS=%CXXFLAGS% /O2 /Oi /MD /DNDEBUG
    set IMGUI_FLAGS=%IMGUI_FLAGS% /O2 /MD /DNDEBUG
    set LDFLAGS=/OPT:REF /OPT:ICF
)

set CORE_SRC=^
 "%SRC_DIR%\Common.cpp" ^
 "%SRC_DIR%\Country.cpp" ^
 "%SRC_DIR%\Names.cpp" ^
 "%SRC_DIR%\NamesData.cpp" ^
 "%SRC_DIR%\Agent.cpp" ^
 "%SRC_DIR%\Coach.cpp" ^
 "%SRC_DIR%\Player.cpp" ^
 "%SRC_DIR%\Team.cpp" ^
 "%SRC_DIR%\Match.cpp" ^
 "%SRC_DIR%\MatchExport.cpp" ^
 "%SRC_DIR%\Series.cpp" ^
 "%SRC_DIR%\Tournament.cpp" ^
 "%SRC_DIR%\League.cpp" ^
 "%SRC_DIR%\SoloQ.cpp" ^
 "%SRC_DIR%\GameManager.cpp" ^
 "%SRC_DIR%\Goat.cpp" ^
 "%SRC_DIR%\FlagBitmaps.cpp" ^
 "%SRC_DIR%\LogoArt.cpp"

set IMGUI_SRC=^
 "%IMGUI_DIR%\imgui.cpp" ^
 "%IMGUI_DIR%\imgui_draw.cpp" ^
 "%IMGUI_DIR%\imgui_tables.cpp" ^
 "%IMGUI_DIR%\imgui_widgets.cpp" ^
 "%IMGUI_BACKEND%\imgui_impl_win32.cpp" ^
 "%IMGUI_BACKEND%\imgui_impl_dx11.cpp"

pushd "%BUILD%"

echo === Compiling ImGui (cached objs) ===
cl %IMGUI_FLAGS% /c %IMGUI_SRC%
if errorlevel 1 ( popd & exit /b 1 )

echo === Compiling vlrtest.exe (%MODE%) ===
cl %CXXFLAGS% /c %CORE_SRC% "%SRC_DIR%\smoke_test.cpp"
if errorlevel 1 ( popd & exit /b 1 )
link /nologo /out:vlrtest.exe %LDFLAGS% Common.obj Country.obj Names.obj NamesData.obj Agent.obj Coach.obj Player.obj Team.obj Match.obj MatchExport.obj Series.obj Tournament.obj League.obj SoloQ.obj GameManager.obj Goat.obj FlagBitmaps.obj smoke_test.obj
if errorlevel 1 ( popd & exit /b 1 )

echo === Compiling vlrmanager.exe (console fallback) ===
cl %CXXFLAGS% /c "%SRC_DIR%\main.cpp"
if errorlevel 1 ( popd & exit /b 1 )
link /nologo /out:vlrmanager.exe %LDFLAGS% Common.obj Country.obj Names.obj NamesData.obj Agent.obj Coach.obj Player.obj Team.obj Match.obj MatchExport.obj Series.obj Tournament.obj League.obj SoloQ.obj GameManager.obj Goat.obj FlagBitmaps.obj main.obj
if errorlevel 1 ( popd & exit /b 1 )

echo === Compiling vlrgui.exe (ImGui DX11) ===
cl %CXXFLAGS% /c "%SRC_DIR%\gui_main.cpp"
if errorlevel 1 ( popd & exit /b 1 )
link /nologo /out:vlrgui.exe /SUBSYSTEM:WINDOWS /ENTRY:wWinMainCRTStartup %LDFLAGS% Common.obj Country.obj Names.obj NamesData.obj Agent.obj Coach.obj Player.obj Team.obj Match.obj MatchExport.obj Series.obj Tournament.obj League.obj SoloQ.obj GameManager.obj Goat.obj FlagBitmaps.obj LogoArt.obj gui_main.obj imgui.obj imgui_draw.obj imgui_tables.obj imgui_widgets.obj imgui_impl_win32.obj imgui_impl_dx11.obj user32.lib gdi32.lib shell32.lib comdlg32.lib d3d11.lib dxgi.lib d3dcompiler.lib
if errorlevel 1 ( popd & exit /b 1 )

echo === Compiling Play.exe (auto-build launcher) ===
cl %CXXFLAGS% /c "%SRC_DIR%\launcher.cpp"
if errorlevel 1 ( popd & exit /b 1 )
link /nologo /out:Play.exe /SUBSYSTEM:WINDOWS /ENTRY:wWinMainCRTStartup %LDFLAGS% launcher.obj user32.lib shell32.lib
if errorlevel 1 ( popd & exit /b 1 )

popd

rem Place Play.exe at the project root so it's the natural one-click entry
rem point. It's small (~50KB) and self-contained.
copy /Y "%BUILD%\Play.exe" "%ROOT%Play.exe" >nul

echo.
echo Build complete:
echo   %BUILD%\vlrgui.exe       (ImGui UI - the real game)
echo   %BUILD%\vlrtest.exe      (smoke tests)
echo   %BUILD%\vlrmanager.exe   (console fallback)

if /I "%ARG%"=="test" ( "%BUILD%\vlrtest.exe" & pause & exit /b %errorlevel% )
if /I "%ARG%"=="run"  ( start "" "%BUILD%\vlrgui.exe" & exit /b 0 )

echo.
echo Tip: double-click Play.exe (root or Desktop) to launch the game directly.
pause
exit /b 0
