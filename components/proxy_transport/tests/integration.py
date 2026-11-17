"""Run against a built test client; uses only loopback and temporary certificates."""
import hashlib
import pathlib
import select
import socket
import ssl
import subprocess
import sys
import tempfile
import threading


def listener():
    sock = socket.socket()
    sock.bind(("127.0.0.1", 0))
    sock.listen()
    sock.settimeout(8)
    return sock


def exact(sock, count):
    result = b""
    while len(result) < count:
        data = sock.recv(count - len(result))
        if not data:
            raise EOFError()
        result += data
    return result


with tempfile.TemporaryDirectory() as directory:
    root = pathlib.Path(directory)
    cert, key = root / "cert.pem", root / "key.pem"
    subprocess.run(["openssl", "req", "-x509", "-newkey", "rsa:2048", "-nodes",
                    "-keyout", str(key), "-out", str(cert), "-days", "1",
                    "-subj", "/CN=localhost", "-addext", "subjectAltName=DNS:localhost"],
                   check=True, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    for mode in ("domain", "ipv4", "bad-certificate", "skip-certificate", "large-first-write"):
        origin, proxy = listener(), listener()
        context = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
        context.load_cert_chain(cert, key)
        names, errors = [], []
        context.set_servername_callback(lambda sock, name, ctx: names.append(name))

        def serve_origin():
            try:
                raw, _ = origin.accept()
                with context.wrap_socket(raw, server_side=True) as conn:
                    request = b""
                    while b"\r\n\r\n" not in request:
                        request += conn.recv(4096)
                    conn.sendall(b"HTTP/1.1 200 OK\r\nContent-Length: 16\r\nConnection: close\r\n\r\nnested TLS works")
            except Exception as error:
                errors.append(error)

        def serve_proxy():
            try:
                raw, _ = proxy.accept()
                with context.wrap_socket(raw, server_side=True) as conn:
                    assert exact(conn, 58) == hashlib.sha224(b"test-password").hexdigest().encode() + b"\r\n"
                    assert exact(conn, 1) == b"\x01"
                    atyp = exact(conn, 1)
                    if mode == "ipv4":
                        assert atyp == b"\x01" and exact(conn, 4) == socket.inet_aton("127.0.0.1")
                    else:
                        assert atyp == b"\x03"
                        assert exact(conn, exact(conn, 1)[0]) == b"localhost"
                    assert int.from_bytes(exact(conn, 2), "big") == origin.getsockname()[1]
                    assert exact(conn, 2) == b"\r\n"
                    assert conn.pending() > 0, "first TLS record must contain application payload"
                    if mode == "large-first-write":
                        assert exact(conn, 4096) == b"x" * 4096
                        conn.sendall(b"OK")
                        return
                    with socket.create_connection(origin.getsockname()) as target:
                        while True:
                            ready = [conn] if conn.pending() else select.select([conn, target], [], [], 5)[0]
                            if not ready:
                                raise TimeoutError()
                            for source in ready:
                                data = source.recv(4096)
                                if not data:
                                    return
                                (target if source is conn else conn).sendall(data)
            except ssl.SSLError as error:
                if mode != "bad-certificate":
                    errors.append(error)
            except Exception as error:
                errors.append(error)

        threads = [threading.Thread(target=serve_proxy)]
        if mode not in ("bad-certificate", "large-first-write"):
            threads.append(threading.Thread(target=serve_origin))
        for thread in threads:
            thread.start()
        result = subprocess.run([sys.argv[1], str(proxy.getsockname()[1]),
                                 str(origin.getsockname()[1]), str(cert), mode], timeout=10)
        for thread in threads:
            thread.join(timeout=10)
            assert not thread.is_alive()
        origin.close()
        proxy.close()
        assert result.returncode == 0, mode
        assert not errors, (mode, errors)
        assert names == (["wrong.example"] if mode == "bad-certificate" else
                         ["wrong.example", "localhost"] if mode == "skip-certificate" else
                         ["localhost"] if mode == "large-first-write" else
                         ["localhost", "localhost"]), names
        print(mode, "PASS")
