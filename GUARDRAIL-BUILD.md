# Building the guardrail / access-control fork (Windows)

This fork of llama.cpp adds **access-control guardrails** on top of the upstream
engine:

- **Encrypted models only** — the loader refuses plain `.gguf`; models must be
  bundled with `gguf-encrypt` and opened with a passcode.
- **Server access control** — `llama-server` supports an IP/MAC allow-list and
  request/response logging.

It is built from an offline snapshot whose `ggml/` compute kernel was updated
from upstream `master`. This document covers how to build it (CPU, CUDA/GPU, and
a Visual Studio solution) and how to deploy it.

> Paths below use `C:\dev\cuda-11.3` and `C:\dev\onnxruntime` as the dependency
> locations. Adjust to wherever you place them.

---

## 1. Prerequisites

| Tool | Notes |
| --- | --- |
| **CMake** ≥ 3.18 | `ggml-cuda` requires 3.18+ for `CMAKE_CUDA_ARCHITECTURES`. |
| **Ninja** | For the command-line builds. |
| **Visual Studio** | CPU build works with any recent MSVC. **CUDA 11.3 needs the VS 2019 / `v142` toolset** (it does *not* support `v143`/VS 2022 or newer). |
| **ONNX Runtime SDK** | Required by the **server** (`server-context` links it). Use a build matching the vendored headers (`vendor/onnxruntime_*.h`, API v25 → ORT **1.25.x**). Provide `lib\onnxruntime.lib` + `lib\onnxruntime.dll`. |
| **CUDA Toolkit 11.3** | GPU build only. A portable (extracted) toolkit works — it must include `extras\visual_studio_integration\` for the VS solution. |

### Build-configuration notes (important)

These flags/fixes are required because of how the snapshot is wired:

- **`-DLLAMA_HTTPLIB=ON`** — `common/download.cpp` uses cpp-httplib
  unconditionally, but `common/CMakeLists.txt` only links it when `LLAMA_HTTPLIB`
  is set. Without this you get unresolved `httplib::*` symbols.
- **`-DCMAKE_WINDOWS_EXPORT_ALL_SYMBOLS=ON`** — `server-context`'s
  `output-guardrail.cpp` calls internal `unicode_*` functions that are not
  exported from `llama.dll`. This auto-exports them so the server links.
- **ONNX Runtime paths** — pass `-DONNXRUNTIME_LIBRARY=<...>\lib\onnxruntime.lib`
  and `-DONNXRUNTIME_DLL=<...>\lib\onnxruntime.dll` (and/or set
  `ONNXRUNTIME_ROOT`). The DLL lives in `lib\`, not `bin\`.
- `common_chat_template_direct_apply` (in `common/chat.cpp`) was missing from the
  snapshot export and has been restored; no action needed.

---

## 2. CPU build (server + CLI)

From a Developer Command Prompt (any recent MSVC):

```bat
set ONNXRUNTIME_ROOT=C:\dev\onnxruntime
cmake -S . -B build -G Ninja ^
  -DCMAKE_BUILD_TYPE=Release ^
  -DLLAMA_BUILD_SERVER=ON -DLLAMA_BUILD_WEBUI=ON ^
  -DLLAMA_HTTPLIB=ON -DCMAKE_WINDOWS_EXPORT_ALL_SYMBOLS=ON ^
  -DONNXRUNTIME_LIBRARY=%ONNXRUNTIME_ROOT%\lib\onnxruntime.lib ^
  -DONNXRUNTIME_DLL=%ONNXRUNTIME_ROOT%\lib\onnxruntime.dll
cmake --build build -j
```

Output: `build\bin\` (`llama-server.exe`, `llama-completion.exe`,
`gguf-encrypt.exe`, the `ggml*`/`llama`/`mtmd` DLLs, `onnxruntime.dll`).

The embedded Web UI (`-DLLAMA_BUILD_WEBUI=ON`) requires the prebuilt
`tools/server/public/index.html.gz`, which is present in the snapshot — no
Node/npm build is needed. Omit `-DLLAMA_BUILD_WEBUI=ON` to skip it (then `/`
returns 404; use `--path <dir>` for a custom UI).

---

## 3. CUDA / GPU build (RTX 3000-series, compute capability 8.6)

CUDA **11.3** requires the **VS 2019 (`v142`)** host compiler. Build everything
with that toolset (use a separate build dir from the CPU build):

```bat
set CUDA_PATH=C:\dev\cuda-11.3
set ONNXRUNTIME_ROOT=C:\dev\onnxruntime
REM From a VS 2019 (or VsDevCmd -vcvars_ver=14.29) x64 prompt:
cmake -S . -B build-cuda -G Ninja ^
  -DCMAKE_BUILD_TYPE=Release ^
  -DGGML_CUDA=ON -DCMAKE_CUDA_ARCHITECTURES=86 -DGGML_CUDA_NCCL=OFF ^
  -DLLAMA_BUILD_SERVER=ON -DLLAMA_BUILD_WEBUI=ON ^
  -DLLAMA_HTTPLIB=ON -DCMAKE_WINDOWS_EXPORT_ALL_SYMBOLS=ON ^
  -DCMAKE_CUDA_COMPILER=%CUDA_PATH%\bin\nvcc.exe ^
  -DONNXRUNTIME_LIBRARY=%ONNXRUNTIME_ROOT%\lib\onnxruntime.lib ^
  -DONNXRUNTIME_DLL=%ONNXRUNTIME_ROOT%\lib\onnxruntime.dll
cmake --build build-cuda -j
```

- `CMAKE_CUDA_ARCHITECTURES=86` targets the RTX 3060 (and the 3000 series).
- `GGML_CUDA_NCCL=OFF` — NCCL is not available on Windows.
- The CUDA runtime DLLs (`cudart64_110.dll`, `cublas64_11.dll`,
  `cublasLt64_11.dll`) are **not** auto-staged; copy them from
  `%CUDA_PATH%\bin\` next to the executables for deployment.

> The CUDA binaries cannot start on a machine without an NVIDIA driver:
> `ggml-cuda.dll` imports `nvcuda.dll` (the driver), so they only run on the
> target GPU box.

---

## 4. Visual Studio 2019 solution

A generator script produces a `.sln` for building in the IDE:

```bat
REM defaults: CUDA_PATH=C:\dev\cuda-11.3, ONNXRUNTIME_ROOT=C:\dev\onnxruntime
generate-vs2019.bat
```

This writes `build-vs2019\llama.cpp.sln`. Open it in **Visual Studio 2019**,
select **Release | x64**, and build `llama-server` / `llama-completion`.

- Keep the **`v142`** toolset — do **not** let VS 2022 retarget to `v143`
  (CUDA 11.3 will not build under it).
- The CUDA MSBuild integration is loaded from
  `%CUDA_PATH%\extras\visual_studio_integration\` — so the toolkit folder must
  include `extras\` (copy the whole toolkit; don't trim it).
- The generated solution hardcodes absolute paths. To move it to another machine,
  either replicate the exact paths or re-run `generate-vs2019.bat` there with
  `CUDA_PATH` / `ONNXRUNTIME_ROOT` set.

---

## 5. Preparing a model (encryption)

The loader refuses plain `.gguf`. Encrypt a model (or model directory) into a
bundle:

```bat
gguf-encrypt.exe --in model\coder.gguf --out coder-enc.gguf ^
  --passcode 1234-5678-9012-3456-7890
```

A model directory may also contain `system-prompt.txt` and
`prompt-guardrail-samples.json`; if present they are preserved in the bundle.
The passcode format is five groups of four digits.

---

## 6. Running

The decrypt passcode is read from the `LLAMA_MODEL_PASSCODE` environment
variable.

```bat
set LLAMA_MODEL_PASSCODE=1234-5678-9012-3456-7890

REM CLI:
llama-completion.exe -m coder-enc.gguf -ngl 99 -p "def add(a,b):" -n 64 -no-cnv

REM Server (GPU, web UI at /, access control + logging):
llama-server.exe -m coder-enc.gguf -ngl 99 --host 127.0.0.1 --port 8080 ^
  --access-allow-file access-allow.csv --log-dir logs
```

- `-ngl 99` offloads all layers to the GPU (CUDA build).
- **`--access-allow-file <csv>`** — IP/MAC allow-list. Format per line:
  `alias,ip,mac1[,mac2...]`. The server only allows a request when the source IP
  is listed *and* the resolved MAC matches. **Loopback (`127.0.0.1` / `::1`) is
  always allowed** and bypasses the list. An invalid file aborts startup.
- **`--log-dir <dir>`** — writes request/response records to
  `llama-server-<date>.jsonl`.
- **`--path <dir>`** — serve a custom Web UI instead of the embedded one.
- **`--tool-force-mode`** — when a request **provides tools**, run it raw against the
  model: **no prompt-guardrail classification/refusal, no forced system prompt, and
  no output guardrail**. Client/agent system instructions are passed through as-is.
  This is intended for agent clients (Continue, Cline, etc.) where the guardrail/forced
  prompt would otherwise suppress tool calling. Requests **without** tools, and all
  requests when the flag is omitted, keep every guardrail. The access-allow list and
  request logging still apply regardless of this flag.

> Note on agent tool-calling: streaming tool-call responses no longer abort with
> *"output guardrail failed: Invalid diff: now finding less tool calls!"* — the
> streaming diff tolerates transient partial-parse states. Reliable file *writing*
> still depends on the model's tool-calling strength; weaker coder models may print
> code in chat or create empty files under heavy agent context.

---

## 7. Deployment checklist (target GPU machine, Windows 10/11 x64)

Copy the executables plus their runtime DLLs (the `build*\bin` folder), and
ensure the target has:

| Requirement | Provided by |
| --- | --- |
| NVIDIA driver ≥ **465.89** (`nvcuda.dll`) | GPU driver install (not shippable) |
| **VC++ 2015–2022 x64 Redistributable** | `MSVCP140.dll` / `VCRUNTIME140*.dll` |
| `cudart64_110.dll`, `cublas64_11.dll`, `cublasLt64_11.dll` | CUDA 11.3 toolkit (redistributable) |
| `onnxruntime.dll`, `onnxruntime_providers_shared.dll` | bundled (server) |
| `ggml*.dll`, `llama.dll`, `mtmd.dll` | build output |

The binaries target a minimum OS of Windows Vista-era (PE OS version 6.00), so
they run on **Windows 10** as well as 11.
