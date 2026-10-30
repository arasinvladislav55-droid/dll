@echo off
chcp 65001 >nul
where cmake >nul 2>nul || (echo CMake not found. Install CMake and Visual Studio 2022 with "Desktop development with C++". & pause & exit /b 1)
cmake -B build -A x64 || (pause & exit /b 1)
cmake --build build --config Release || (pause & exit /b 1)
copy /y build\Release\uc_menu.dll uc_menu.dll
echo.
echo Done: uc_menu.dll
pause
