"""HTTP/CLI integration against the actual webOS runtime over SSH."""

import base64
import contextlib
import json
import os
import secrets
import shlex
import signal
import socket
import subprocess
import sys
import time
import urllib.error
import urllib.parse
import urllib.request


class TvRuntime:
    def __init__(self, target):
        parsed = urllib.parse.urlsplit(target)
        if parsed.scheme != "http" or not parsed.hostname:
            raise ValueError("An explicit HTTP TV URL is required")
        self.host = parsed.hostname
        self.ssh = os.environ.get("WEBOS_SMOKE_SSH")
        if not self.ssh:
            raise ValueError("WEBOS_SMOKE_SSH is required for process lifecycle tests")
        directory = os.environ.get(
            "WEBOS_TEST_SERVICE_DIR",
            "/media/developer/apps/usr/palm/services/org.webosbrew.jetkvm.service",
        )
        self.binary = directory.rstrip("/") + "/webos-jetkvm"
        self.prefix = [
            "ssh",
            "-T",
            "-o",
            "BatchMode=yes",
            "-o",
            "ConnectTimeout=5",
            "-o",
            "LogLevel=ERROR",
            self.ssh,
        ]

    def shell(self, command, *, data=None):
        result = subprocess.run(
            [*self.prefix, command], input=data, capture_output=True, text=True, timeout=15
        )
        if result.returncode:
            raise RuntimeError("TV setup or cleanup command failed")
        return result.stdout.strip()

    def run(self, *arguments):
        return subprocess.run(
            [*self.prefix, "exec " + shlex.join([self.binary, *arguments])],
            capture_output=True,
            text=True,
            timeout=15,
        )

    def free_port(self):
        for _ in range(20):
            port = 30000 + secrets.randbelow(17000)
            try:
                with socket.create_connection((self.host, port), timeout=0.2):
                    pass
            except OSError:
                return port
        raise RuntimeError("Could not find an unused test port")

    @contextlib.contextmanager
    def server(self, config, port):
        command = "printf '%s\\n' $$; exec " + shlex.join(
            [self.binary, "--config", config, "--port", str(port)]
        )
        process = subprocess.Popen(
            [*self.prefix, command],
            stdin=subprocess.DEVNULL,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            text=True,
        )
        pid = None
        try:
            first_line = process.stdout.readline().strip()
            if not first_line.isdecimal():
                raise RuntimeError("TV did not report the test process PID")
            pid = int(first_line)
            assert pid > 1
            yield process, pid
        finally:
            if process.poll() is None and pid:
                self.stop(pid, signal.SIGTERM)
            try:
                process.communicate(timeout=5)
            except subprocess.TimeoutExpired:
                if pid:
                    self.stop(pid, signal.SIGKILL)
                process.kill()
                process.communicate(timeout=5)

    def stop(self, pid, stop_signal):
        # Restrict signals to the binary and PID this test started.
        command = (
            f"expected=$(readlink -f {shlex.quote(self.binary)}); "
            f"actual=$(readlink /proc/{pid}/exe); "
            f'[ "$actual" != "$expected" ] || kill -{int(stop_signal)} {pid}'
        )
        self.shell(command)


def ready(endpoint, process):
    for _ in range(80):
        try:
            urllib.request.urlopen(endpoint, timeout=0.3)
        except urllib.error.HTTPError as error:
            assert error.code == 401, "Server did not require authentication"
            return
        except OSError:
            if process.poll() is not None:
                raise AssertionError("TV server exited before readiness") from None
            time.sleep(0.05)
        else:
            raise AssertionError("Unauthenticated access was accepted")
    raise AssertionError("TV server never became ready")


def request(url, auth):
    return urllib.request.Request(url, headers={"Authorization": auth})


def main():
    if len(sys.argv) != 2:
        raise ValueError("Usage: http_server_test.py http://TV:47990 (WEBOS_SMOKE_SSH required)")
    tv = TvRuntime(sys.argv[1])
    help_result = tv.run("--help")
    assert help_result.returncode == 0
    assert "--test-h264" not in help_result.stdout and "--test-auth" not in help_result.stdout
    for option, value in [
        ("--port", "-1"),
        ("--port", "47991trailing"),
        ("--port", "4295015287"),
        ("--bitrate", "8000suffix"),
        ("--bitrate", "4294975296"),
    ]:
        malformed = tv.run(option, value)
        assert malformed.returncode != 0, "Malformed numeric argument was accepted"
        assert "Expected an unsigned decimal number" in malformed.stderr
    for arguments in [("--test-h264", "/nonexistent.h264"), ("--test-auth",)]:
        assert tv.run(*arguments).returncode != 0, "Removed runtime option was accepted"

    folder = tv.shell("umask 077; mktemp -d /tmp/webos-jetkvm-http-XXXXXX")
    assert folder.startswith("/tmp/webos-jetkvm-http-") and "\n" not in folder
    try:
        config = folder + "/config.json"
        password = secrets.token_hex(16)
        tv.shell(
            "umask 077; cat > " + shlex.quote(config),
            data=json.dumps({"password": password, "autostart": False, "resolution": "1080p"}),
        )
        port = tv.free_port()
        missing = tv.run("--config", folder + "/missing.json", "--port", str(port))
        assert missing.returncode != 0 and "Set a password" in missing.stderr
        endpoint = f"http://{tv.host}:{port}/api/status"
        auth = "Basic " + base64.b64encode(("jetkvm:" + password).encode()).decode()
        with tv.server(config, port) as (first, _):
            ready(endpoint, first)
            with urllib.request.urlopen(request(endpoint, auth), timeout=1) as response:
                status = json.load(response)
            assert status["application"] == "org.webosbrew.jetkvm"
            assert status["source"] == "Nyx hardware H.264"
            duplicate = tv.run("--config", config, "--port", str(port))
            assert duplicate.returncode != 0, "Second server shared the HTTP port"
            assert "Cannot bind HTTP port" in duplicate.stderr
            with urllib.request.urlopen(request(endpoint, auth), timeout=1) as response:
                assert json.load(response)["application"] == "org.webosbrew.jetkvm"
            try:
                urllib.request.urlopen(request(endpoint, "Basic invalid"), timeout=1)
                raise AssertionError("Wrong password was accepted")
            except urllib.error.HTTPError as error:
                assert error.code == 401
            for origin, expected in [
                (f"http://{tv.host}:{port}", 101),
                ("http://foreign.invalid", 403),
                ("", 403),
            ]:
                with socket.create_connection((tv.host, port), timeout=2) as client:
                    headers = [
                        "GET /webrtc/signaling/client HTTP/1.1",
                        f"Host: {tv.host}:{port}",
                        "Connection: Upgrade",
                        "Upgrade: websocket",
                        "Sec-WebSocket-Version: 13",
                        "Sec-WebSocket-Key: AAAAAAAAAAAAAAAAAAAAAA==",
                        "Authorization: " + auth,
                    ]
                    if origin:
                        headers.append("Origin: " + origin)
                    client.sendall(("\r\n".join(headers) + "\r\n\r\n").encode())
                    status_line = client.recv(4096).split(b"\r\n", 1)[0]
                    assert int(status_line.split()[1]) == expected, status_line
            for method, action in [
                ("GET", "video"),
                ("POST", "offer"),
                ("POST", "answer"),
                ("POST", "disconnect"),
                ("POST", "video"),
            ]:
                legacy = request(f"http://{tv.host}:{port}/api/{action}", auth)
                legacy.method = method
                if method == "POST":
                    legacy.data = b"{}"
                try:
                    urllib.request.urlopen(legacy, timeout=1)
                    raise AssertionError("Legacy browser API exists")
                except urllib.error.HTTPError as error:
                    assert error.code == 404
        assert first.returncode == 0, "Server did not shut down cleanly"
        for stop_signal in (signal.SIGTERM, signal.SIGINT):
            with tv.server(config, port) as (process, pid):
                ready(endpoint, process)
                with socket.create_connection((tv.host, port), timeout=2) as client:
                    headers = [
                        "GET /webrtc/signaling/client HTTP/1.1",
                        f"Host: {tv.host}:{port}",
                        f"Origin: http://{tv.host}:{port}",
                        "Connection: Upgrade",
                        "Upgrade: websocket",
                        "Sec-WebSocket-Version: 13",
                        "Sec-WebSocket-Key: AAAAAAAAAAAAAAAAAAAAAA==",
                        "Authorization: " + auth,
                    ]
                    client.sendall(("\r\n".join(headers) + "\r\n\r\n").encode())
                    assert b"101" in client.recv(4096).split(b"\r\n", 1)[0]
                    tv.stop(pid, stop_signal)
                    process.communicate(timeout=5)
                    assert process.returncode == 0, "Shutdown with an open WebSocket failed"
    finally:
        tv.shell("rm -rf -- " + shlex.quote(folder))
    print("Passed on webOS: CLI, authentication, duplicate port, origin, legacy APIs and shutdown")


if __name__ == "__main__":
    main()
