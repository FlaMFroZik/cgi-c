#!/usr/bin/env python3
"""tests/test_fcgi.py — самопроверка FastCGI-сервера без веб-сервера.

Запускает example/fcgi-server и два тестовых приложения из tests/ на
свободных портах и проверяет ответы на уровне протокола FastCGI:
что каждый запрос получает непустой корректный HTTP-ответ, что
Content-Length совпадает с телом, что работают keep-alive, HEAD,
загрузка файлов и запросы без завершающей записи STDIN.

    make test
"""
import os
import shutil
import socket
import struct
import subprocess
import sys
import tempfile
import time

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
SERVER = os.path.join(ROOT, "example", "fcgi-server")
EMPTY_APP = os.path.join(ROOT, "tests", "emptyapp")
CONTRACT_APP = os.path.join(ROOT, "tests", "contractapp")

# Типы записей FastCGI (версия 1).
BEGIN_REQUEST = 1
ABORT_REQUEST = 2
END_REQUEST = 3
PARAMS = 4
STDIN = 5
STDOUT = 6
STDERR = 7
UNKNOWN_TYPE = 11


class TestFailure(Exception):
    """Проверка не прошла."""


def check(condition, message):
    if not condition:
        raise TestFailure(message)


# ----------------------------------------------------------------------
# Минимальный FastCGI-клиент
# ----------------------------------------------------------------------

def record(rec_type, request_id, content=b""):
    """Собрать запись FastCGI."""
    return struct.pack(">BBHHBx", 1, rec_type, request_id,
                       len(content), 0) + content


def encode_pair(name, value):
    """Закодировать NAME=VALUE как в PARAMS."""
    def length(n):
        return bytes([n]) if n < 128 else struct.pack(">I", n | 0x80000000)

    name = name.encode() if isinstance(name, str) else name
    value = value.encode() if isinstance(value, str) else value
    return length(len(name)) + length(len(value)) + name + value


def recv_exact(sock, length):
    data = bytearray()
    while len(data) < length:
        chunk = sock.recv(length - len(data))
        if not chunk:
            raise TestFailure("сервер закрыл соединение, не отправив "
                              "END_REQUEST (получено %d байт)"
                              % len(data))
        data += chunk
    return bytes(data)


def read_response(sock):
    """Дочитать записи до END_REQUEST и собрать ответ."""
    stdout = b""
    stderr = b""
    while True:
        head = recv_exact(sock, 8)
        _, rec_type, _, length, padding = struct.unpack(">BBHHB", head[:7])
        content = recv_exact(sock, length) if length else b""
        if padding:
            recv_exact(sock, padding)
        if rec_type == STDOUT:
            stdout += content
        elif rec_type == STDERR:
            stderr += content
        elif rec_type == END_REQUEST:
            check(length == 8, "END_REQUEST длиной %d байт" % length)
            app_status, protocol_status = struct.unpack(">IB3x", content)
            return Response(stdout, stderr, app_status, protocol_status)


class Response:
    """Разобранный ответ приложения."""

    def __init__(self, stdout, stderr, app_status, protocol_status):
        self.raw = stdout
        self.stderr = stderr
        self.app_status = app_status
        self.protocol_status = protocol_status
        self.status = None
        self.content_length = None

        head, sep, body = stdout.partition(b"\r\n\r\n")
        check(sep == b"\r\n\r\n",
              "в ответе нет пустой строки после заголовков: %r"
              % stdout[:160])
        self.headers = head
        self.body = body

        for line in head.split(b"\r\n"):
            name, _, value = line.partition(b":")
            name = name.strip().lower()
            if name == b"status":
                try:
                    self.status = int(value.strip().split()[0])
                except (IndexError, ValueError):
                    raise TestFailure("плохая строка Status: %r" % line)
            elif name == b"content-length":
                try:
                    self.content_length = int(value.strip())
                except ValueError:
                    raise TestFailure("плохой Content-Length: %r" % line)


class Connection:
    """Одно соединение с сервером: можно отправить несколько запросов."""

    def __init__(self, port, timeout=5.0):
        self.sock = socket.create_connection(("127.0.0.1", port), timeout)
        self.sock.settimeout(timeout)

    def send(self, rec_type, request_id, content=b""):
        self.sock.sendall(record(rec_type, request_id, content))

    def request(self, request_id, method="GET", uri="/", body=b"",
                extra=None, keep=False, terminate=True,
                content_length="auto"):
        """Отправить BEGIN_REQUEST + PARAMS + STDIN.

        content_length="auto" — подставить длину тела для POST/PUT/PATCH
        (как это делает веб-сервер); None — не передавать вовсе;
        число — передать явно.
        """
        params = {
            "REQUEST_METHOD": method,
            "REQUEST_URI": uri,
            "SERVER_PROTOCOL": "HTTP/1.1",
            "REMOTE_ADDR": "127.0.0.1",
        }
        if content_length == "auto":
            if body or method in ("POST", "PUT", "PATCH"):
                params["CONTENT_LENGTH"] = str(len(body))
        elif content_length is not None:
            params["CONTENT_LENGTH"] = str(content_length)
        if extra:
            params.update(extra)

        self.send(BEGIN_REQUEST, request_id,
                  struct.pack(">HB5x", 1, 1 if keep else 0))
        self.send(PARAMS, request_id,
                  b"".join(encode_pair(k, v) for k, v in params.items()))
        self.send(PARAMS, request_id)          # конец PARAMS
        if body:
            self.send(STDIN, request_id, body)
        if terminate:
            self.send(STDIN, request_id)       # конец STDIN

    def response(self):
        return read_response(self.sock)

    def close(self):
        try:
            self.sock.close()
        except OSError:
            pass


def expect_response(resp, status, body=None, contains=None, head=False):
    """Проверить код, заголовки и тело ответа."""
    check(resp.status == status,
          "статус %r, ожидался %d (ответ: %r)"
          % (resp.status, status, resp.raw[:200]))
    check(resp.protocol_status == 0,
          "protocolStatus %d, ожидался 0" % resp.protocol_status)
    if contains is not None:
        check(contains in resp.body,
              "в теле нет %r: %r" % (contains, resp.body[:200]))
    if resp.status not in (204, 304):
        check(resp.content_length is not None, "в ответе нет Content-Length")
        if head:
            check(resp.body == b"", "HEAD вернул непустое тело")
        else:
            check(resp.content_length == len(resp.body),
                  "Content-Length %d не совпадает с длиной тела %d"
                  % (resp.content_length, len(resp.body)))
    if body is not None:
        check(resp.body == body,
              "тело %r, ожидалось %r" % (resp.body[:160], body[:160]))


# ----------------------------------------------------------------------
# Запуск серверов
# ----------------------------------------------------------------------

def free_port():
    sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    try:
        sock.bind(("127.0.0.1", 0))
        return sock.getsockname()[1]
    finally:
        sock.close()


class Server:
    """Запущенный сервер: ждёт открытия порта и умеет корректно
    останавливаться (SIGTERM — мягкая остановка)."""

    def __init__(self, binary, root, args=("-q",)):
        self.port = free_port()
        self.log = tempfile.TemporaryFile()
        self.stopped = False
        self.proc = subprocess.Popen(
            [binary, "-p", str(self.port), "-r", root, *args],
            stdout=self.log, stderr=self.log)

        deadline = time.monotonic() + 10
        while time.monotonic() < deadline:
            if self.proc.poll() is not None:
                text = self.log_text()
                self.stop()
                raise TestFailure("%s завершился при запуске:\n%s"
                                  % (binary, text))
            try:
                probe = socket.create_connection(
                    ("127.0.0.1", self.port), 0.2)
                probe.close()
                return
            except OSError:
                time.sleep(0.05)

        self.stop()
        raise TestFailure("%s не открыл порт %d" % (binary, self.port))

    def log_text(self):
        self.log.seek(0)
        return self.log.read().decode("utf-8", "replace")

    def stop(self):
        if self.stopped:
            return
        self.stopped = True
        if self.proc.poll() is None:
            self.proc.terminate()
            try:
                self.proc.wait(timeout=5)
            except subprocess.TimeoutExpired:
                self.proc.kill()
                self.proc.wait()
        try:
            self.log.close()
        except OSError:
            pass


# ----------------------------------------------------------------------
# Проверки
# ----------------------------------------------------------------------

class Context:
    def __init__(self, root, port, empty_port, contract_port, big_data):
        self.root = root
        self.port = port
        self.empty_port = empty_port
        self.contract_port = contract_port
        self.big_data = big_data


def test_static_file(ctx):
    conn = Connection(ctx.port)
    try:
        conn.request(1, uri="/hello.txt")
        expect_response(conn.response(), 200, body=b"hello world\n")
    finally:
        conn.close()


def test_root_serves_index(ctx):
    conn = Connection(ctx.port)
    try:
        conn.request(1, uri="/")
        expect_response(conn.response(), 200, body=b"<html>index</html>")
    finally:
        conn.close()


def test_directory_listing(ctx):
    conn = Connection(ctx.port)
    try:
        conn.request(1, uri="/sub/")
        expect_response(conn.response(), 200,
                        contains=b"f.txt")
    finally:
        conn.close()


def test_directory_index(ctx):
    conn = Connection(ctx.port)
    try:
        conn.request(1, uri="/sub2/")
        expect_response(conn.response(), 200, body=b"sub index")
    finally:
        conn.close()


def test_not_found(ctx):
    conn = Connection(ctx.port)
    try:
        conn.request(1, uri="/nope.txt")
        resp = conn.response()
        expect_response(resp, 404)
        check(len(resp.body) > 0, "404 с пустым телом")
    finally:
        conn.close()


def test_head(ctx):
    conn = Connection(ctx.port)
    try:
        conn.request(1, method="HEAD", uri="/hello.txt")
        resp = conn.response()
        expect_response(resp, 200, head=True)
        check(resp.content_length == len(b"hello world\n"),
              "HEAD: Content-Length %r" % resp.content_length)
    finally:
        conn.close()


def test_head_listing(ctx):
    conn = Connection(ctx.port)
    try:
        conn.request(1, uri="/sub/", keep=True)
        listing = conn.response()
        expect_response(listing, 200)
        conn.request(2, method="HEAD", uri="/sub/")
        head = conn.response()
        expect_response(head, 200, head=True)
        check(head.content_length == len(listing.body),
              "HEAD: Content-Length %r, у списка %d"
              % (head.content_length, len(listing.body)))
    finally:
        conn.close()


def test_echo_binary(ctx):
    body = b"a\x00b\xffc"
    conn = Connection(ctx.port)
    try:
        conn.request(1, method="POST", uri="/echo", body=body)
        expect_response(conn.response(), 200, body=body)
    finally:
        conn.close()


def test_echo_empty(ctx):
    conn = Connection(ctx.port)
    try:
        conn.request(1, uri="/echo")
        resp = conn.response()
        expect_response(resp, 200)
        check(len(resp.body) > 0, "/echo вернул пустое тело")
    finally:
        conn.close()


def test_traversal(ctx):
    conn = Connection(ctx.port)
    try:
        conn.request(1, uri="/%2e%2e/%2e%2e/etc/passwd")
        expect_response(conn.response(), 403)
    finally:
        conn.close()


def test_method_not_allowed(ctx):
    conn = Connection(ctx.port)
    try:
        conn.request(1, method="DELETE", uri="/hello.txt")
        expect_response(conn.response(), 405)
    finally:
        conn.close()


def test_keep_alive(ctx):
    conn = Connection(ctx.port)
    try:
        conn.request(1, uri="/hello.txt", keep=True)
        expect_response(conn.response(), 200, body=b"hello world\n")
        conn.request(2, method="POST", uri="/echo", body=b"second")
        expect_response(conn.response(), 200, body=b"second")
    finally:
        conn.close()


def test_missing_stdin_terminator(ctx):
    """CONTENT_LENGTH=0, завершающей пустой записи STDIN нет.

    Раньше сервер ждал её до таймаута и закрывал соединение без
    ответа — для клиента это был пустой ответ.
    """
    conn = Connection(ctx.port, timeout=4.0)
    try:
        started = time.monotonic()
        conn.request(1, uri="/hello.txt", terminate=False,
                     content_length=0)
        expect_response(conn.response(), 200, body=b"hello world\n")
        elapsed = time.monotonic() - started
        check(elapsed < 3.0, "ответ пришёл только через %.1f с" % elapsed)
    finally:
        conn.close()


def test_late_stdin_terminator(ctx):
    """Запоздавшая пустая запись STDIN не ломает keep-alive."""
    conn = Connection(ctx.port)
    try:
        conn.request(1, uri="/hello.txt", keep=True, terminate=False,
                     content_length=0)
        expect_response(conn.response(), 200, body=b"hello world\n")
        conn.send(STDIN, 1)          # запоздавший конец STDIN
        conn.request(2, uri="/hello.txt")
        expect_response(conn.response(), 200, body=b"hello world\n")
    finally:
        conn.close()


def test_too_large(ctx):
    conn = Connection(ctx.port)
    try:
        conn.request(1, method="POST", uri="/echo",
                     body=b"x" * 5000, terminate=False)
        resp = conn.response()
        expect_response(resp, 413)
        check(len(resp.body) > 0, "413 с пустым телом")
    finally:
        conn.close()


def test_upload_multipart(ctx):
    body = (
        b"--BND\r\n"
        b'Content-Disposition: form-data; name="file"; '
        b'filename="t.txt"\r\n'
        b"Content-Type: text/plain\r\n"
        b"\r\n"
        b"uploaded!\r\n"
        b"--BND--\r\n"
    )
    conn = Connection(ctx.port)
    try:
        conn.request(1, method="POST", uri="/upload", body=body,
                     extra={"CONTENT_TYPE":
                            "multipart/form-data; boundary=BND"})
        expect_response(conn.response(), 200, contains=b"t.txt")
    finally:
        conn.close()

    path = os.path.join(ctx.root, "uploads", "t.txt")
    check(os.path.exists(path), "файл не сохранён: %s" % path)
    with open(path, "rb") as file:
        check(file.read() == b"uploaded!",
              "содержимое загруженного файла не совпало")


def test_big_file(ctx):
    conn = Connection(ctx.port, timeout=10.0)
    try:
        conn.request(1, uri="/big.bin")
        expect_response(conn.response(), 200, body=ctx.big_data)
    finally:
        conn.close()


def test_empty_file(ctx):
    conn = Connection(ctx.port)
    try:
        conn.request(1, uri="/empty.txt")
        expect_response(conn.response(), 200, body=b"")
    finally:
        conn.close()


def test_app_without_response(ctx):
    """Обработчик ничего не написал — библиотека отвечает 500."""
    conn = Connection(ctx.empty_port)
    try:
        conn.request(1, uri="/")
        resp = conn.response()
        expect_response(resp, 500)
        check(len(resp.body) > 0, "запасной 500 пришёл с пустым телом")
        check(resp.app_status == 500,
              "appStatus %d, ожидался 500" % resp.app_status)
    finally:
        conn.close()


def test_data_is_string(ctx):
    conn = Connection(ctx.contract_port)
    try:
        conn.request(1, uri="/")
        resp = conn.response()
        expect_response(resp, 200, contains=b"data=ok size=0 nul=ok")
    finally:
        conn.close()


def test_data_is_string_with_body(ctx):
    conn = Connection(ctx.contract_port)
    try:
        conn.request(1, method="POST", uri="/", body=b"hello")
        expect_response(conn.response(), 200,
                        contains=b"data=ok size=5 nul=ok")
    finally:
        conn.close()


def test_data_with_nul_bytes(ctx):
    conn = Connection(ctx.contract_port)
    try:
        conn.request(1, method="POST", uri="/", body=b"a\x00b\xffc")
        expect_response(conn.response(), 200,
                        contains=b"data=ok size=5 nul=ok")
    finally:
        conn.close()


def test_long_printf(ctx):
    """fcgi_printf() не обрезает длинную строку на 4095 байтах."""
    conn = Connection(ctx.contract_port, timeout=10.0)
    try:
        conn.request(1, uri="/long")
        resp = conn.response()
        expect_response(resp, 200, body=b"x" * 9999)
    finally:
        conn.close()


def test_unknown_record_type(ctx):
    sock = socket.create_connection(("127.0.0.1", ctx.port), 5.0)
    sock.settimeout(5.0)
    try:
        sock.sendall(record(200, 0, b"x"))
        head = recv_exact(sock, 8)
        _, rec_type, request_id, length, _ = struct.unpack(">BBHHB",
                                                           head[:7])
        content = recv_exact(sock, length) if length else b""
        check(rec_type == UNKNOWN_TYPE,
              "тип ответа %d, ожидался UNKNOWN_TYPE" % rec_type)
        check(request_id == 0, "requestId %d, ожидался 0" % request_id)
        check(length == 8, "длина UNKNOWN_TYPE %d, ожидалась 8" % length)
        check(len(content) == 8 and content[0] == 200,
              "содержимое UNKNOWN_TYPE: %r" % content)
    finally:
        sock.close()


def test_abort_request(ctx):
    """На ABORT_REQUEST приходит END_REQUEST, соединение не рвётся."""
    sock = socket.create_connection(("127.0.0.1", ctx.port), 5.0)
    sock.settimeout(5.0)
    try:
        sock.sendall(record(BEGIN_REQUEST, 1,
                            struct.pack(">HB5x", 1, 1)))  # keep-alive
        sock.sendall(record(PARAMS, 1,
                            encode_pair("REQUEST_METHOD", "GET") +
                            encode_pair("REQUEST_URI", "/")))
        sock.sendall(record(PARAMS, 1))
        sock.sendall(record(ABORT_REQUEST, 1))
        while True:
            head = recv_exact(sock, 8)
            _, rec_type, _, length, padding = struct.unpack(">BBHHB",
                                                            head[:7])
            content = recv_exact(sock, length) if length else b""
            if padding:
                recv_exact(sock, padding)
            if rec_type == END_REQUEST:
                app_status, protocol_status = struct.unpack(">IB3x",
                                                            content)
                check(protocol_status == 0,
                      "protocolStatus %d" % protocol_status)
                check(app_status == 0, "appStatus %d" % app_status)
                return
    finally:
        sock.close()


TESTS = [
    ("статический файл", test_static_file),
    ("GET / отдаёт index.html", test_root_serves_index),
    ("список файлов в папке", test_directory_listing),
    ("index.html внутри папки", test_directory_index),
    ("404 с непустым телом", test_not_found),
    ("HEAD без тела", test_head),
    ("HEAD списка файлов", test_head_listing),
    ("/echo с двоичным телом", test_echo_binary),
    ("/echo с пустым телом", test_echo_empty),
    ("выход за пределы root", test_traversal),
    ("405 Method Not Allowed", test_method_not_allowed),
    ("keep-alive: два запроса", test_keep_alive),
    ("нет завершающей записи STDIN", test_missing_stdin_terminator),
    ("запоздавшая запись STDIN", test_late_stdin_terminator),
    ("413 с непустым телом", test_too_large),
    ("загрузка multipart/form-data", test_upload_multipart),
    ("большой файл", test_big_file),
    ("пустой файл", test_empty_file),
    ("обработчик без ответа → 500", test_app_without_response),
    ("data: пустое тело — строка", test_data_is_string),
    ("data: тело — строка", test_data_is_string_with_body),
    ("data: двоичное тело", test_data_with_nul_bytes),
    ("fcgi_printf без обрезки", test_long_printf),
    ("UNKNOWN_TYPE", test_unknown_record_type),
    ("ABORT_REQUEST", test_abort_request),
]


# ----------------------------------------------------------------------
# Подготовка и запуск
# ----------------------------------------------------------------------

def build_root(tmp):
    root = os.path.join(tmp, "www")
    os.makedirs(os.path.join(root, "sub"))
    os.makedirs(os.path.join(root, "sub2"))

    def write(name, data):
        with open(os.path.join(root, name), "wb") as file:
            file.write(data)

    write("index.html", b"<html>index</html>")
    write("hello.txt", b"hello world\n")
    write("empty.txt", b"")
    write("big.bin", bytes(range(256)) * 800)
    write(os.path.join("sub", "f.txt"), b"sub file\n")
    write(os.path.join("sub2", "index.html"), b"sub index")
    return root


def main():
    for path in (SERVER, EMPTY_APP, CONTRACT_APP):
        if not os.path.exists(path):
            print("нет %s — сначала выполните make" % path, file=sys.stderr)
            return 2

    tmp = tempfile.mkdtemp(prefix="fcgi-test-")
    servers = []
    try:
        root = build_root(tmp)
        big_data = bytes(range(256)) * 800

        main_server = Server(SERVER, root, args=("-q", "-m", "4096"))
        servers.append(main_server)
        empty_server = Server(EMPTY_APP, root)
        servers.append(empty_server)
        contract_server = Server(CONTRACT_APP, root)
        servers.append(contract_server)

        ctx = Context(root, main_server.port, empty_server.port,
                      contract_server.port, big_data)

        failed = 0
        for name, test in TESTS:
            try:
                test(ctx)
                print("  ok   %s" % name)
            except Exception as exc:  # noqa: BLE001 — тест-раннер
                failed += 1
                print("  FAIL %s: %s" % (name, exc))

        if failed:
            print("\n%d из %d проверок провалено" % (failed, len(TESTS)))
            for index, server in enumerate(servers):
                print("\n--- журнал сервера %d (порт %d) ---"
                      % (index + 1, server.port))
                print(server.log_text()[-4000:])
            return 1

        print("\nвсе %d проверок прошли" % len(TESTS))
        return 0
    finally:
        for server in servers:
            server.stop()
        shutil.rmtree(tmp, ignore_errors=True)


if __name__ == "__main__":
    sys.exit(main())
