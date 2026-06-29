import time

import pytest
from openai import OpenAI
from utils import *

server = ServerPreset.tinyllama2()

TEST_API_KEY = "sk-this-is-the-secret-key"


def _prepare_access_allow_server(
    tmp_path,
    entries: list[tuple[str, str, list[str]]],
    host_ip: str | None = None,
) -> str:
    global server
    server = ServerPreset.tinyllama2()
    server.api_key = TEST_API_KEY

    host_ip = host_ip or get_primary_ipv4_address()
    if host_ip.startswith("127."):
        pytest.skip("No non-loopback IPv4 address available for access allow tests")

    server.server_host = host_ip

    allow_file = tmp_path / "access-allow.csv"
    allow_lines = []
    for alias, ip, macs in entries:
        if not macs:
            raise ValueError("access allow entries require at least one MAC address")
        allow_lines.append(f"{alias},{ip},{','.join(macs)}")

    allow_file.write_text("\n".join(allow_lines) + "\n", encoding="utf-8")
    server.access_allow_file = str(allow_file)
    return host_ip

@pytest.fixture(autouse=True)
def create_server():
    global server
    server = ServerPreset.tinyllama2()
    server.api_key = TEST_API_KEY


@pytest.mark.parametrize("endpoint", ["/health", "/models"])
def test_access_public_endpoint(endpoint: str):
    global server
    server.start()
    res = server.make_request("GET", endpoint)
    assert res.status_code == 200
    assert "error" not in res.body


@pytest.mark.parametrize("api_key", [None, "invalid-key"])
def test_incorrect_api_key(api_key: str):
    global server
    server.start()
    res = server.make_request("POST", "/completions", data={
        "prompt": "I believe the meaning of life is",
    }, headers={
        "Authorization": f"Bearer {api_key}" if api_key else None,
    })
    assert res.status_code == 401
    assert "error" in res.body
    assert res.body["error"]["type"] == "authentication_error"


def test_correct_api_key():
    global server
    server.start()
    res = server.make_request("POST", "/completions", data={
        "prompt": "I believe the meaning of life is",
    }, headers={
        "Authorization": f"Bearer {TEST_API_KEY}",
    })
    assert res.status_code == 200
    assert "error" not in res.body
    assert "content" in res.body


def test_correct_api_key_anthropic_header():
    global server
    server.start()
    res = server.make_request("POST", "/completions", data={
        "prompt": "I believe the meaning of life is",
    }, headers={
        "X-Api-Key": TEST_API_KEY,
    })
    assert res.status_code == 200
    assert "error" not in res.body
    assert "content" in res.body


def test_openai_library_correct_api_key():
    global server
    server.start()
    client = OpenAI(api_key=TEST_API_KEY, base_url=f"http://{server.server_host}:{server.server_port}")
    res = client.chat.completions.create(
        model="gpt-3.5-turbo",
        messages=[
            {"role": "system", "content": "You are a chatbot."},
            {"role": "user", "content": "What is the meaning of life?"},
        ],
    )
    assert len(res.choices) == 1


def test_access_allow_file_denies_wrong_mac_even_with_api_key(tmp_path):
    global server
    host_ip = get_primary_ipv4_address()
    if host_ip.startswith("127."):
        pytest.skip("No non-loopback IPv4 address available for access allow test")

    _prepare_access_allow_server(tmp_path, [
        ("local", host_ip, ["02:00:00:00:00:01"]),
    ], host_ip=host_ip)

    try:
        with pytest.raises(TimeoutError):
            server.start(timeout_seconds=10)
    finally:
        server.stop()


def test_access_allow_file_allows_matching_ip_and_mac(tmp_path):
    global server
    host_ip = get_primary_ipv4_address()
    if host_ip.startswith("127."):
        pytest.skip("No non-loopback IPv4 address available for access allow test")

    actual_mac = get_primary_mac_address()
    if actual_mac is None:
        pytest.skip("Primary MAC address is not available for access allow test")

    _prepare_access_allow_server(tmp_path, [
        ("local", host_ip, ["02:00:00:00:00:01", actual_mac]),
    ], host_ip=host_ip)

    server.start()
    res = server.make_request("POST", "/completions", data={
        "prompt": "I believe the meaning of life is",
        "n_predict": 1,
    }, headers={
        "Authorization": f"Bearer {TEST_API_KEY}",
    })
    assert res.status_code == 200
    assert "error" not in res.body
    assert "content" in res.body


def test_access_allow_file_allows_loopback_without_mac_resolution(tmp_path):
    global server
    server = ServerPreset.tinyllama2()
    server.server_host = "127.0.0.1"
    server.wait_for_health = False

    allow_file = tmp_path / "access-allow.csv"
    allow_file.write_text(
        "local,127.0.0.1,02:00:00:00:00:01\n",
        encoding="utf-8",
    )
    server.access_allow_file = str(allow_file)

    server.start()
    try:
        deadline = time.time() + 120
        res = None
        while time.time() < deadline:
            res = server.make_request("GET", "/health")
            if res.status_code != 503:
                break
            time.sleep(1)

        assert res is not None
        assert res.status_code == 200
        assert "error" not in res.body
    finally:
        server.stop()


def test_access_allow_file_reloads_after_mtime_change(tmp_path):
    global server
    host_ip = get_primary_ipv4_address()
    if host_ip.startswith("127."):
        pytest.skip("No non-loopback IPv4 address available for access allow test")

    actual_mac = get_primary_mac_address()
    if actual_mac is None:
        pytest.skip("Primary MAC address is not available for access allow test")

    server = ServerPreset.tinyllama2()
    server.api_key = TEST_API_KEY
    server.server_host = host_ip

    allow_file = tmp_path / "access-allow.csv"
    allow_file.write_text(
        f"local,{host_ip},{actual_mac}\n",
        encoding="utf-8",
    )
    server.access_allow_file = str(allow_file)

    server.start()
    try:
        allowed = server.make_request("POST", "/completions", data={
            "prompt": "I believe the meaning of life is",
            "n_predict": 1,
        }, headers={
            "Authorization": f"Bearer {TEST_API_KEY}",
        })
        assert allowed.status_code == 200

        time.sleep(1.1)
        allow_file.write_text(
            f"local,{host_ip},02:00:00:00:00:01\n",
            encoding="utf-8",
        )

        denied = server.make_request("POST", "/completions", data={
            "prompt": "I believe the meaning of life is",
            "n_predict": 1,
        }, headers={
            "Authorization": f"Bearer {TEST_API_KEY}",
        })
        assert denied.status_code == 403
        assert "error" in denied.body
    finally:
        server.stop()


@pytest.mark.parametrize("origin,cors_header,cors_header_value", [
    ("localhost", "Access-Control-Allow-Origin", "localhost"),
    ("web.mydomain.fr", "Access-Control-Allow-Origin", "web.mydomain.fr"),
    ("origin", "Access-Control-Allow-Credentials", "true"),
    ("web.mydomain.fr", "Access-Control-Allow-Methods", "GET, POST"),
    ("web.mydomain.fr", "Access-Control-Allow-Headers", "*"),
])
def test_cors_options(origin: str, cors_header: str, cors_header_value: str):
    global server
    server.start()
    res = server.make_request("OPTIONS", "/completions", headers={
        "Origin": origin,
        "Access-Control-Request-Method": "POST",
        "Access-Control-Request-Headers": "Authorization",
    })
    assert res.status_code == 200
    assert cors_header in res.headers
    assert res.headers[cors_header] == cors_header_value


@pytest.mark.parametrize(
    "media_path, image_url, success",
    [
        (None,             "file://mtmd/test-1.jpeg",    False), # disabled media path, should fail
        ("../../../tools", "file://mtmd/test-1.jpeg",    True),
        ("../../../tools", "file:////mtmd//test-1.jpeg", True),  # should be the same file as above
        ("../../../tools", "file://mtmd/notfound.jpeg",  False), # non-existent file
        ("../../../tools", "file://../mtmd/test-1.jpeg", False), # no directory traversal
    ]
)
def test_local_media_file(media_path, image_url, success,):
    server = ServerPreset.tinygemma3()
    server.media_path = media_path
    server.start()
    res = server.make_request("POST", "/chat/completions", data={
        "max_tokens": 1,
        "messages": [
            {"role": "user", "content": [
                {"type": "text", "text": "test"},
                {"type": "image_url", "image_url": {
                    "url": image_url,
                }},
            ]},
        ],
    })
    if success:
        assert res.status_code == 200
    else:
        assert res.status_code == 400
