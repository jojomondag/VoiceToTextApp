@echo off
cd /d "%~dp0"
call "C:\Program Files\Microsoft Visual Studio\18\Community\VC\Auxiliary\Build\vcvarsall.bat" amd64 >nul 2>&1
REM CUDA 13.x's nvcc rejects MSVC versions newer than VS 2022. Allow the
REM unsupported MSVS 18 host compiler. NVCC_PREPEND_FLAGS applies even during
REM CMake's CUDA compiler-identification probe.
set "NVCC_PREPEND_FLAGS=-allow-unsupported-compiler"
echo === Configuring CMake === > "%~dp0build_log.txt" 2>&1
"C:\Program Files\Microsoft Visual Studio\18\Community\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe" -B "%~dp0build" -G Ninja -DCMAKE_BUILD_TYPE=Release -DVTT_CUDA=ON -DCMAKE_CUDA_FLAGS="-allow-unsupported-compiler" >> "%~dp0build_log.txt" 2>&1
if %errorlevel% neq 0 (
    echo CONFIG_FAILED >> "%~dp0build_log.txt"
    exit /b 1
)
echo === Building === >> "%~dp0build_log.txt" 2>&1
"C:\Program Files\Microsoft Visual Studio\18\Community\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe" --build "%~dp0build" --config Release >> "%~dp0build_log.txt" 2>&1
if %errorlevel% neq 0 (
    echo BUILD_FAILED >> "%~dp0build_log.txt"
    exit /b 1
)
echo BUILD_SUCCESS >> "%~dp0build_log.txt"
