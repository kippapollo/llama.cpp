from pathlib import Path
import json
import os
import re
import signal
import shutil
import subprocess
import time

import pytest
import requests


REPO_ROOT = Path(__file__).resolve().parents[4]
BIN_DIR = REPO_ROOT / "build-ucrt" / "bin"
SERVER_BIN = BIN_DIR / "llama-server.exe"
CLI_BIN = BIN_DIR / "llama-cli.exe"
ENCRYPT_BIN = BIN_DIR / "gguf-encrypt.exe"
LOCAL_CODER_ROOT = Path(r"D:\apollo\work\llm\qwen\model\coder")
TEST_PASSCODE = "1111-2222-3333-4444-5555"


@pytest.fixture(scope="module")
def coder_source_root():
    if not LOCAL_CODER_ROOT.exists():
        pytest.skip(f"missing local model root: {LOCAL_CODER_ROOT}")
    return LOCAL_CODER_ROOT


def assert_built(binary: Path) -> None:
    assert binary.exists(), f"{binary} is not built"


def copy_model_tree(source_root: Path, dest_root: Path) -> Path:
    if dest_root.exists():
        shutil.rmtree(dest_root)
    shutil.copytree(source_root, dest_root)
    return dest_root


def snapshot_files(root: Path) -> list[str]:
    return sorted(
        path.relative_to(root).as_posix()
        for path in root.rglob("*")
        if path.is_file()
    )


def build_env(model_path: Path, passcode: str | None = None) -> dict[str, str]:
    env = os.environ.copy()
    env["LLAMA_MODEL_PATH"] = str(model_path)
    env.pop("LLAMA_PROMPT_GUARDRAIL_MODEL", None)
    env.pop("LLAMA_OUTPUT_GUARDRAIL_MODEL", None)
    if passcode is None:
        env.pop("LLAMA_MODEL_PASSCODE", None)
    else:
        env["LLAMA_MODEL_PASSCODE"] = passcode
    return env


def wait_for_health(process: subprocess.Popen[str], port: int, log_path: Path, timeout_s: int = 180) -> None:
    deadline = time.monotonic() + timeout_s
    url = f"http://127.0.0.1:{port}/health"

    while time.monotonic() < deadline:
        if process.poll() is not None:
            raise AssertionError(
                f"server exited early with code {process.returncode}\n"
                f"{log_path.read_text(encoding='utf-8', errors='replace')}"
            )
        try:
            response = requests.get(url, timeout=2)
            if response.status_code == 200:
                return
        except requests.RequestException:
            pass
        time.sleep(0.5)

    raise AssertionError(
        f"server did not become healthy within {timeout_s} seconds\n"
        f"{log_path.read_text(encoding='utf-8', errors='replace')}"
    )


def start_server(
    model_path: Path,
    port: int,
    log_path: Path,
    passcode: str | None = None,
    ctx_size: int = 2048,
    timeout_s: int = 180,
) -> tuple[subprocess.Popen[str], object]:
    assert_built(SERVER_BIN)

    log_file = log_path.open("w", encoding="utf-8", buffering=1)
    creationflags = 0
    if hasattr(subprocess, "CREATE_NEW_PROCESS_GROUP"):
        creationflags = subprocess.CREATE_NEW_PROCESS_GROUP
    process = subprocess.Popen(
        [str(SERVER_BIN), "--port", str(port), "--no-warmup", "--ctx-size", str(ctx_size)],
        cwd=REPO_ROOT,
        env=build_env(model_path, passcode),
        stdout=log_file,
        stderr=subprocess.STDOUT,
        text=True,
        creationflags=creationflags,
    )
    wait_for_health(process, port, log_path, timeout_s=timeout_s)
    return process, log_file


def stop_server(process: subprocess.Popen[str], log_file, timeout_s: int = 60) -> None:
    if process.poll() is None:
        if hasattr(signal, "CTRL_C_EVENT"):
            try:
                process.send_signal(signal.CTRL_BREAK_EVENT)
            except Exception:
                process.terminate()
        else:
            process.terminate()
        try:
            process.wait(timeout=timeout_s)
        except subprocess.TimeoutExpired:
            process.kill()
            process.wait(timeout=timeout_s)
    log_file.flush()
    log_file.close()


def run_cli(model_path: Path, prompt: str, passcode: str | None = None) -> subprocess.CompletedProcess[str]:
    assert_built(CLI_BIN)
    process = subprocess.Popen(
        [
            str(CLI_BIN),
            "-p",
            prompt,
            "--single-turn",
            "--n-predict",
            "0",
            "--ctx-size",
            "1",
            "--batch-size",
            "1",
            "--ubatch-size",
            "1",
        ],
        cwd=REPO_ROOT,
        env=build_env(model_path, passcode),
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
        text=True,
    )

    deadline = time.monotonic() + 600
    last_progress = time.monotonic()

    while True:
        remaining = deadline - time.monotonic()
        if remaining <= 0:
            process.terminate()
            try:
                stdout, _ = process.communicate(timeout=30)
            except subprocess.TimeoutExpired:
                process.kill()
                stdout, _ = process.communicate(timeout=30)
            raise subprocess.TimeoutExpired(process.args, 600, output=stdout)

        try:
            stdout, _ = process.communicate(timeout=min(15.0, remaining))
            return subprocess.CompletedProcess(process.args, process.returncode, stdout)
        except subprocess.TimeoutExpired:
            if time.monotonic() - last_progress >= 15.0:
                print("cli smoke: waiting for process exit...")
                last_progress = time.monotonic()


def encrypt_bundle(input_root: Path, bundle_path: Path) -> subprocess.CompletedProcess[str]:
    assert_built(ENCRYPT_BIN)
    return subprocess.run(
        [
            str(ENCRYPT_BIN),
            "--in",
            str(input_root),
            "--out",
            str(bundle_path),
            "--passcode",
            TEST_PASSCODE,
        ],
        cwd=REPO_ROOT,
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
        text=True,
        timeout=900,
    )


def wait_for_log_contains(
    log_path: Path,
    fragments: list[str],
    timeout_s: int = 30,
    progress_label: str | None = None,
) -> str:
    deadline = time.monotonic() + timeout_s
    last_text = ""
    last_progress = time.monotonic()

    while time.monotonic() < deadline:
        last_text = log_path.read_text(encoding="utf-8", errors="replace")
        if all(fragment in last_text for fragment in fragments):
            return last_text
        if progress_label is not None and time.monotonic() - last_progress >= 15.0:
            print(f"{progress_label}: waiting for startup logs...")
            last_progress = time.monotonic()
        time.sleep(0.5)

    raise AssertionError(f"log did not contain expected fragments: {fragments}\n{last_text}")


def parse_bundle_root(log_text: str) -> Path:
    match = re.search(r"resolved model root='([^']+)' mode=bundle model='([^']+)'", log_text)
    assert match, log_text
    return Path(match.group(1))


def parse_output_guardrail_root(log_text: str) -> Path:
    match = re.search(r"output guardrail: selected model root = (.+)", log_text)
    assert match, log_text
    return Path(match.group(1).strip())


def assert_classifier_log(log_text: str, model_root: Path) -> None:
    embedding = str(model_root / "embedding.gguf")
    samples = str(model_root / "prompt-guardrail-samples.json")
    assert f"prompt guardrail mode=sample-similarity model='{embedding}' samples='{samples}'" in log_text, log_text


def prepare_bundle(tmp_path, coder_source_root) -> tuple[Path, Path, list[str]]:
    plain_root = copy_model_tree(coder_source_root, tmp_path / "bundle-source")
    bundle_path = tmp_path / "coder.model"
    before_files = snapshot_files(plain_root)

    encrypt_result = encrypt_bundle(plain_root, bundle_path)
    assert encrypt_result.returncode == 0, encrypt_result.stdout
    assert bundle_path.exists()

    return plain_root, bundle_path, before_files


def remove_optional_sidecars(model_root: Path) -> None:
    sidecar_files = [
        model_root / "embedding.gguf",
        model_root / "prompt-guardrail-samples.json",
        model_root / "system-prompt.txt",
    ]
    for path in sidecar_files:
        if path.exists():
            path.unlink()

    ner_dir = model_root / "ner"
    if ner_dir.exists():
        shutil.rmtree(ner_dir)


def corrupt_embedding_sidecar(model_root: Path) -> None:
    embedding_path = model_root / "embedding.gguf"
    assert embedding_path.exists(), embedding_path
    embedding_path.write_bytes(b"not a gguf model")


def test_plain_directory_server_startup(tmp_path, coder_source_root):
    plain_root = copy_model_tree(coder_source_root, tmp_path / "plain-coder")
    before_files = snapshot_files(plain_root)

    server_log = tmp_path / "plain-server.log"
    process, log_file = start_server(plain_root, 8123, server_log)
    try:
        log_text = wait_for_log_contains(server_log, ["resolved model root='", "mode=directory"])
        assert "resolved model root='" in log_text
        assert "mode=directory" in log_text
        assert_classifier_log(log_text, plain_root)
    finally:
        stop_server(process, log_file)

    after_files = snapshot_files(plain_root)
    assert after_files == before_files, after_files


def test_plain_directory_without_sidecars_starts_without_guardrails(tmp_path, coder_source_root):
    plain_root = copy_model_tree(coder_source_root, tmp_path / "plain-coder-no-sidecars")
    remove_optional_sidecars(plain_root)

    server_log = tmp_path / "plain-no-sidecars-server.log"
    process, log_file = start_server(plain_root, 8126, server_log)
    try:
        log_text = wait_for_log_contains(
            server_log,
            ["resolved model root='", "mode=directory", "prompt guardrail mode=disabled", "output guardrail mode=disabled"],
        )
        assert "resolved model root='" in log_text
        assert "mode=directory" in log_text
    finally:
        stop_server(process, log_file)


def test_guardrail_refusal_short_circuits_chat_completions(tmp_path, coder_source_root):
    plain_root = copy_model_tree(coder_source_root, tmp_path / "plain-coder-refusal")

    server_log = tmp_path / "plain-refusal-server.log"
    process, log_file = start_server(plain_root, 8128, server_log)
    try:
        wait_for_log_contains(
            server_log,
            ["resolved model root='", "prompt guardrail mode=sample-similarity", "output guardrail: backend startup complete"],
            timeout_s=600,
            progress_label="guardrail refusal",
        )

        response = requests.post(
            "http://127.0.0.1:8128/v1/chat/completions",
            json={
                "model": "coder",
                "messages": [
                    {"role": "user", "content": "What is the capital of Canada?"},
                ],
                "max_tokens": 16,
                "stream": False,
            },
            timeout=120,
        )
        assert response.status_code == 200, response.text
        body = response.json()
        assert body["choices"][0]["message"]["role"] == "assistant"
        assert "I can only assist with software, IT, and coding requests." in body["choices"][0]["message"]["content"]
        assert body["choices"][0]["finish_reason"] == "stop"

        log_text = wait_for_log_contains(
            server_log,
            ["classify_locked: route=nontech", "guardrail refused request, returning refusal response without generation"],
            timeout_s=60,
            progress_label="guardrail refusal request",
        )
        assert "duplicate result received" not in log_text, log_text
    finally:
        stop_server(process, log_file)


def test_guardrail_stream_true_request_does_not_duplicate_results(tmp_path, coder_source_root):
    plain_root = copy_model_tree(coder_source_root, tmp_path / "plain-coder-stream-true")

    request_payload = {
        "model": "coder",
        "messages": [
            {"role": "user", "content": "Write a Python solution for this algorithm interview problem and explain the approach."},
        ],
        "temperature": 0,
        "max_tokens": 8,
        "seed": 42,
    }

    baseline_server_log = tmp_path / "plain-baseline-server.log"
    baseline_process, baseline_log_file = start_server(plain_root, 8129, baseline_server_log)
    try:
        wait_for_log_contains(
            baseline_server_log,
            ["resolved model root='", "prompt guardrail mode=sample-similarity", "output guardrail: backend startup complete"],
            timeout_s=600,
            progress_label="guardrail baseline",
        )

        deadline = time.monotonic() + 180
        while True:
            baseline_response = requests.post(
                "http://127.0.0.1:8129/v1/chat/completions",
                json={**request_payload, "stream": False},
                timeout=120,
            )
            if baseline_response.status_code == 200:
                break
            if baseline_response.status_code != 503 or time.monotonic() >= deadline:
                break
            time.sleep(1)

        assert baseline_response.status_code == 200, baseline_response.text
        expected_content = baseline_response.json()["choices"][0]["message"]["content"]
    finally:
        stop_server(baseline_process, baseline_log_file)

    server_log = tmp_path / "plain-stream-true-server.log"
    process, log_file = start_server(plain_root, 8130, server_log)
    try:
        wait_for_log_contains(
            server_log,
            ["resolved model root='", "prompt guardrail mode=sample-similarity", "output guardrail: backend startup complete"],
            timeout_s=600,
            progress_label="guardrail stream override",
        )

        response = requests.post(
            "http://127.0.0.1:8130/v1/chat/completions",
            json={**request_payload, "stream": True},
            stream=True,
            timeout=120,
        )
        assert response.status_code == 200, response.text
        assert response.headers.get("Content-Type", "").startswith("text/event-stream"), response.headers

        assembled_content = []
        saw_initial_chunk = False
        last_finish_reason = None
        chunk_count = 0

        for line_bytes in response.iter_lines():
            if not line_bytes:
                continue

            line = line_bytes.decode("utf-8")
            if line == "data: [DONE]":
                break
            if not line.startswith("data: "):
                continue

            chunk = json.loads(line[6:])
            chunk_count += 1

            if chunk["choices"]:
                assert len(chunk["choices"]) == 1, chunk
                choice = chunk["choices"][0]
                delta = choice["delta"]
                if not saw_initial_chunk:
                    assert delta["role"] == "assistant"
                    assert delta["content"] is None
                    saw_initial_chunk = True
                else:
                    assert "role" not in delta
                    if delta.get("content") is not None:
                        assembled_content.append(delta["content"])

                if choice["finish_reason"] is not None:
                    last_finish_reason = choice["finish_reason"]
            else:
                assert "usage" in chunk, chunk

        assert chunk_count > 0, "expected streamed SSE chunks"
        assert saw_initial_chunk, "expected initial assistant role chunk"
        assert "".join(assembled_content) == expected_content
        assert last_finish_reason in {"stop", "length"}

        probe = requests.get("http://127.0.0.1:8130/v1/models", timeout=30)
        assert probe.status_code == 200, probe.text

        log_text = wait_for_log_contains(
            server_log,
            ["classify_locked: route=tech"],
            timeout_s=120,
            progress_label="guardrail stream override request",
        )
        assert "duplicate result received" not in log_text, log_text
    finally:
        stop_server(process, log_file)


def test_plain_directory_cli_startup(tmp_path, coder_source_root):
    plain_root = copy_model_tree(coder_source_root, tmp_path / "plain-coder")
    result = run_cli(plain_root, "What is the capital of Canada?")
    assert result.returncode == 0, result.stdout
    assert "output guardrail: backend startup complete" in result.stdout, result.stdout
    assert "I can only assist with software, IT, and coding requests." in result.stdout, result.stdout
    selected_root = parse_output_guardrail_root(result.stdout)
    assert selected_root == plain_root
    assert selected_root.exists()


def test_bundle_server_startup_and_cleanup(tmp_path, coder_source_root):
    plain_root, bundle_path, before_files = prepare_bundle(tmp_path, coder_source_root)
    server_log = tmp_path / "bundle-server.log"
    process, log_file = start_server(bundle_path, 8124, server_log, TEST_PASSCODE, timeout_s=900)
    temp_root = None
    try:
        log_text = wait_for_log_contains(
            server_log,
            ["resolved model root='", "mode=bundle", "prompt guardrail mode=sample-similarity"],
            timeout_s=600,
            progress_label="bundle server",
        )
        temp_root = parse_bundle_root(log_text)
        assert temp_root.exists()
        assert_classifier_log(log_text, temp_root)
        assert snapshot_files(temp_root) == before_files
    finally:
        stop_server(process, log_file)

    assert temp_root is not None
    deadline = time.monotonic() + 10
    while time.monotonic() < deadline and temp_root.exists():
        time.sleep(0.25)
    assert not temp_root.exists(), f"decrypted temp root was not removed: {temp_root}"


def test_bundle_cli_startup(tmp_path, coder_source_root):
    _, bundle_path, _ = prepare_bundle(tmp_path, coder_source_root)
    result = run_cli(bundle_path, "What is the capital of Canada?", TEST_PASSCODE)
    assert result.returncode == 0, result.stdout
    assert "output guardrail: backend startup complete" in result.stdout, result.stdout
    assert "I can only assist with software, IT, and coding requests." in result.stdout, result.stdout
    temp_root = parse_output_guardrail_root(result.stdout)
    assert not temp_root.exists(), f"decrypted temp root was not removed: {temp_root}"


@pytest.mark.parametrize(
    ("mutator", "expected_fragment"),
    [
        ("malformed_samples", "failed to initialize prompt guardrail"),
        ("empty_samples", "failed to initialize prompt guardrail"),
    ],
)
def test_guardrail_missing_assets_fail_to_start(tmp_path, coder_source_root, mutator, expected_fragment):
    model_root = copy_model_tree(coder_source_root, tmp_path / f"broken-{mutator}")

    if mutator == "malformed_samples":
        (model_root / "prompt-guardrail-samples.json").write_text("{", encoding="utf-8")
    elif mutator == "empty_samples":
        (model_root / "prompt-guardrail-samples.json").write_text(
            '{"instruction":"Classify coding-related prompts.","positive_examples":[],"negative_examples":[]}',
            encoding="utf-8",
        )
    else:
        raise AssertionError(mutator)

    result = subprocess.run(
        [str(SERVER_BIN), "--port", "8125"],
        cwd=REPO_ROOT,
        env=build_env(model_root),
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
        text=True,
        timeout=180,
    )

    assert result.returncode != 0, result.stdout
    assert expected_fragment in result.stdout, result.stdout


def test_guardrail_failed_embedding_load_continues_without_prompt_guardrail(tmp_path, coder_source_root):
    model_root = copy_model_tree(coder_source_root, tmp_path / "broken-embedding")
    corrupt_embedding_sidecar(model_root)

    server_log = tmp_path / "broken-embedding-server.log"
    process, log_file = start_server(model_root, 8130, server_log)
    try:
        log_text = wait_for_log_contains(
            server_log,
            ["prompt guardrail initialization failed", "prompt guardrail mode=disabled", "output guardrail: backend startup complete"],
            timeout_s=600,
            progress_label="broken embedding fallback",
        )
    finally:
        stop_server(process, log_file)
