from datetime import date
import json

from utils import *


def _daily_log_path(log_dir: Path) -> Path:
    return log_dir / f"llama-server-{date.today().isoformat()}.jsonl"


def _read_log_entries(log_dir: Path) -> list[dict]:
    log_path = _daily_log_path(log_dir)
    assert log_path.exists(), f"expected log file at {log_path}"
    return [
        json.loads(line)
        for line in log_path.read_text(encoding="utf-8").splitlines()
        if line.strip()
    ]


def test_log_dir_writes_completion_prompt_and_result(tmp_path):
    server = ServerPreset.tinyllama2()
    log_dir = tmp_path / "logs"
    server.log_dir = str(log_dir)

    server.start()
    res = server.make_request("POST", "/completions", data={
        "prompt": "Write one short sentence about logging.",
        "n_predict": 1,
    })
    server.stop()

    assert res.status_code == 200
    assert "content" in res.body

    entries = _read_log_entries(log_dir)
    matching = [
        entry for entry in entries
        if entry.get("method") == "POST" and entry.get("path") == "/completions"
    ]
    assert matching, "expected a log entry for /completions"

    entry = matching[-1]
    assert "Write one short sentence about logging." in entry["request_body"]
    assert res.body["content"] in entry["response_body"]


def test_log_dir_writes_streaming_prompt_and_result(tmp_path):
    server = ServerPreset.tinyllama2()
    log_dir = tmp_path / "logs"
    server.log_dir = str(log_dir)

    server.start()
    list(server.make_stream_request("POST", "/completions", data={
        "stream": True,
        "max_tokens": 1,
        "prompt": "Say hello.",
    }))
    server.stop()

    entries = _read_log_entries(log_dir)
    matching = [
        entry for entry in entries
        if entry.get("method") == "POST" and entry.get("path") == "/completions"
    ]
    assert matching, "expected a log entry for /completions"

    entry = matching[-1]
    assert "Say hello." in entry["request_body"]
    assert entry["response_body"]
