@echo off
REM ============================================================================
REM Regenerate the Visual Studio 2019 CUDA solution for this project.
REM
REM Produces build-vs2019\llama.cpp.sln targeting the CUDA backend
REM (sm_86 / RTX 3000-series, CUDA 11.3, v142 toolset) plus server + web UI.
REM
REM Requirements:
REM   - Visual Studio 2019 (v142 toolset)   -- CUDA 11.3 does NOT support v143
REM   - CMake on PATH
REM   - CUDA 11.3 toolkit WITH the VS MSBuild integration
REM     (extras\visual_studio_integration\MSBuildExtensions\CUDA 11.3.*)
REM   - ONNX Runtime SDK (lib\onnxruntime.lib + lib\onnxruntime.dll)
REM
REM Override the dependency locations by setting these before running:
REM   set CUDA_PATH=D:\cuda-11.3
REM   set ONNXRUNTIME_ROOT=D:\onnxruntime
REM   generate-vs2019.bat
REM ============================================================================
setlocal

if "%CUDA_PATH%"==""        set "CUDA_PATH=C:\dev\cuda-11.3"
if "%ONNXRUNTIME_ROOT%"=="" set "ONNXRUNTIME_ROOT=C:\dev\onnxruntime"
set "BUILD_DIR=%~dp0build-vs2019"

echo CUDA_PATH        = %CUDA_PATH%
echo ONNXRUNTIME_ROOT = %ONNXRUNTIME_ROOT%
echo BUILD_DIR        = %BUILD_DIR%
echo.

if not exist "%CUDA_PATH%\bin\nvcc.exe" (
  echo ERROR: nvcc not found at "%CUDA_PATH%\bin\nvcc.exe"
  echo        Set CUDA_PATH to a CUDA 11.3 toolkit root.
  exit /b 1
)
if not exist "%CUDA_PATH%\extras\visual_studio_integration\MSBuildExtensions\CUDA 11.3.props" (
  echo ERROR: CUDA VS MSBuild integration not found under "%CUDA_PATH%\extras".
  echo        The toolkit must include extras\visual_studio_integration.
  exit /b 1
)
if not exist "%ONNXRUNTIME_ROOT%\lib\onnxruntime.lib" (
  echo ERROR: onnxruntime.lib not found at "%ONNXRUNTIME_ROOT%\lib\onnxruntime.lib"
  echo        Set ONNXRUNTIME_ROOT to an ONNX Runtime SDK root.
  exit /b 1
)

REM Always start from a clean build directory so a previously cached toolset
REM (e.g. a different CUDA path or slash style) cannot block regeneration.
if exist "%BUILD_DIR%\CMakeCache.txt" (
  echo Removing existing build directory "%BUILD_DIR%"...
  rmdir /s /q "%BUILD_DIR%"
)

cmake -S "%~dp0." -B "%BUILD_DIR%" -G "Visual Studio 16 2019" -A x64 ^
  -T cuda="%CUDA_PATH%" ^
  -D GGML_CUDA=ON ^
  -D CMAKE_CUDA_ARCHITECTURES=86 ^
  -D GGML_CUDA_NCCL=OFF ^
  -D LLAMA_BUILD_SERVER=ON ^
  -D LLAMA_BUILD_WEBUI=ON ^
  -D CMAKE_WINDOWS_EXPORT_ALL_SYMBOLS=ON ^
  -D LLAMA_HTTPLIB=ON ^
  -D LLAMA_BUILD_TESTS=OFF ^
  -D ONNXRUNTIME_LIBRARY="%ONNXRUNTIME_ROOT%\lib\onnxruntime.lib" ^
  -D ONNXRUNTIME_DLL="%ONNXRUNTIME_ROOT%\lib\onnxruntime.dll"

if errorlevel 1 (
  echo.
  echo CMake generation FAILED.
  exit /b 1
)

echo.
echo Done. Open "%BUILD_DIR%\llama.cpp.sln" in Visual Studio 2019.
echo Build Release^|x64. Keep the v142 toolset -- CUDA 11.3 does not support v143.
endlocal
