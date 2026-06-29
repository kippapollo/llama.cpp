# llama.cpp

This repository builds the `llama-cli` and `llama-server` command line tools from source, plus the supporting utilities under `tools/`.

## Command Line Usage

### `llama-cli`

Use `llama-cli` for one-shot prompts or interactive terminal use against a local GGUF model.

```powershell
llama-cli -m models\my-model.gguf -ngl all -p "Write a one-sentence summary of this repository."
```

If you want to run a model that is already on disk, `-m` is the most direct option. If you build with GPU support, `-ngl all` asks llama.cpp to offload as many layers as possible to the GPU.

### `llama-server`

Use `llama-server` to expose an HTTP API and the bundled web UI.

```powershell
llama-server -m models\my-model.gguf --host 127.0.0.1 --port 8080 --gpu-layers all
```

The server also supports the OpenAI-compatible API routes used by many client tools.

### Terminal Chat Clients

The repository includes simple terminal examples for talking to a running server:

```bash
API_URL=http://127.0.0.1:8080 bash tools/server/chat.sh
```

```bash
node tools/server/chat.mjs
```

[`tools/server/chat.sh`](./tools/server/chat.sh) requires `bash`, `curl`, and `jq`. [`tools/server/chat.mjs`](./tools/server/chat.mjs) requires Node.js.

## Example Config Files

### Build presets

[`CMakePresets.json`](./CMakePresets.json) contains ready-made Windows presets for GPU builds, including:

- `x64-windows-vulkan-release`
- `x64-windows-sycl-release`
- `x64-windows-sycl-release-f16`

These are the fastest way to build on Windows if your GPU stack matches one of the presets.

### Server access allow list

[`tools/server/access-allow.sample.csv`](./tools/server/access-allow.sample.csv) is the sample file for the `--access-allow-file` option.

Format:

```csv
alias,ip,mac1,mac2,...
```

Example:

```csv
office-desktop,192.168.1.10,00:11:22:33:44:55
laptop,192.168.1.11,aa:bb:cc:dd:ee:ff,aa:bb:cc:dd:ee:00
lab-box,10.0.0.20,12:34:56:78:9a:bc
```

Use it like this:

```powershell
Copy-Item tools\server\access-allow.sample.csv access-allow.csv
llama-server -m models\my-model.gguf --access-allow-file access-allow.csv
```

### Logging Config

Logging is configured with the same flags and environment variables in `llama-cli`, `llama-server`, and the other command line tools.

- `--log-file FNAME` or `LLAMA_LOG_FILE`
- `-lv, --verbosity, --log-verbosity N` or `LLAMA_LOG_VERBOSITY`
- `--log-prefix` or `LLAMA_LOG_PREFIX`
- `--log-timestamps` or `LLAMA_LOG_TIMESTAMPS`
- `--log-colors [on|off|auto]` or `LLAMA_LOG_COLORS`
- `--log-disable`

Example PowerShell session:

```powershell
$env:LLAMA_LOG_FILE = ".\logs\llama-server.log"
$env:LLAMA_LOG_VERBOSITY = "3"
$env:LLAMA_LOG_PREFIX = "1"
$env:LLAMA_LOG_TIMESTAMPS = "1"
$env:LLAMA_LOG_COLORS = "auto"

llama-server -m models\my-model.gguf --host 127.0.0.1 --port 8080 --gpu-layers all
```

Use the same environment variables with `llama-cli` if you want consistent logging for terminal runs.

### Request Logs

`llama-server` also supports request logging to a directory with `--log-dir`.

- The directory is created automatically if it does not exist.
- The server writes one JSONL file per day.
- Files are named `llama-server-YYYY-MM-DD.jsonl`.
- Each entry includes the timestamp, method, path, query string, status, request body, and response body.

Example:

```powershell
llama-server -m models\my-model.gguf --host 127.0.0.1 --port 8080 --gpu-layers all --log-dir .\logs\requests
```

`--log-file` is still the general application log. Use `--log-dir` when you want request and response body logs.

## Build Instructions

### Prerequisites

- CMake 3.26 or newer
- A C++17 compiler
- Ninja or Visual Studio Build Tools
- A local ONNX Runtime install for `llama-cli` and `llama-server`
- A GPU SDK if you want GPU acceleration

### Pick One GPU Backend

Enable only one GPU backend per build:

- `GGML_CUDA` for NVIDIA CUDA
- `GGML_HIP` for AMD ROCm/HIP
- `GGML_VULKAN` for Vulkan
- `GGML_SYCL` for SYCL
- `GGML_METAL` for macOS Metal

### Windows Builds

The repository already ships Windows release presets for Vulkan and SYCL. Use the preset that matches your GPU stack:

```powershell
cmake --preset x64-windows-vulkan-release
cmake --build build-x64-windows-vulkan-release --config Release
```

```powershell
cmake --preset x64-windows-sycl-release
cmake --build build-x64-windows-sycl-release --config Release
```

If you want CUDA or HIP, configure manually:

```powershell
cmake -S . -B build-cuda -G "Ninja Multi-Config" `
  -DGGML_CUDA=ON `
  -DCMAKE_CUDA_ARCHITECTURES=<your-arch>
cmake --build build-cuda --config Release
```

```powershell
cmake -S . -B build-hip -G "Ninja Multi-Config" `
  -DGGML_HIP=ON
cmake --build build-hip --config Release
```

If CMake cannot find ONNX Runtime, point one of `ONNXRUNTIME_ROOT`, `ONNXRUNTIME_DIR`, or `ONNXRUNTIME_PATH` at your local install.

### Notes

- Build from the repository root so the standard tool targets are enabled.
- `llama-cli`, `llama-server`, and the server chat scripts expect a GGUF model file.
- This README assumes the SDKs and dependencies are already present on the machine, which matters for offline builds.
