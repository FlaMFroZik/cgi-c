#!/usr/bin/env python3
"""fcgi-client.py — маленький FastCGI-клиент для проверки
приложения без веб-сервера.

Говорит протокол FastCGI (TCP или Unix-сокет), отправляет
один или несколько запросов и печатает ответ: переменные
STDERR, статус END_REQUEST и тело STDOUT.

Примеры:
    ./example/fcgi-client.py 'GET /'
    ./example/fcgi-client.py -p 9000 'POST /upload' --data @file.bin
    ./example/fcgi-client.py -s /tmp/app.sock 'GET /?x=1' --keep
    ./example/fcgi-client.py 'GET /a' 'GET /b'      # два запроса подряд
"""
import argparse
import socket
import struct
import sys

# Типы записей (FastCGI, версия 1)
BEGIN_REQUEST = 1
ABORT_REQUEST = 2
END_REQUEST = 3
PARAMS = 4
STDIN = 5
STDOUT = 6
STDERR = 7
GET_VALUES = 9
GET_VALUES_RESULT = 10
UNKNOWN_TYPE = 11

RESPONDER = 1
KEEP_CONN = 1

MAX_CONTENT = 65535  # максимум данных в одной записи


def record(rec_type, request_id, content=b""):
    """Собрать запись FastCGI."""
    assert len(content) <= MAX_CONTENT
    head = struct.pack(
        ">BBHHBx",  # версия, тип, requestId, длина, padding, резерв
        1, rec_type, request_id, len(content), 0)
    return head + content


def pair(name, value):
    """Закодировать пару NAME=VALUE как в PARAMS."""
    out = b""
    for item in (name, value):
        item = item.encode()
        out += (struct.pack(">I", len(item) | 0x80000000)
                if len(item) >= 128 else bytes([len(item)]))
    return out + name.encode() + value.encode()


def begin_request(request_id, keep_conn=False):
    role = struct.pack(">H", RESPONDER)
    flags = KEEP_CONN if keep_conn else 0
    return record(BEGIN_REQUEST, request_id, role + bytes([flags]) + b"\0" * 5)


def send_params(sock, request_id, params):
    blob = b"".join(pair(k, v) for k, v in params.items())
    while blob:
        chunk, blob = blob[:MAX_CONTENT], blob[MAX_CONTENT:]
        sock.sendall(record(PARAMS, request_id, chunk))
    sock.sendall(record(PARAMS, request_id))  # пустая — конец потока


def send_stdin(sock, request_id, data):
    while data:
        chunk, data = data[:MAX_CONTENT], data[MAX_CONTENT:]
        sock.sendall(record(STDIN, request_id, chunk))
    sock.sendall(record(STDIN, request_id))  # конец потока


def read_records(sock):
    """Читать записи, пока не придёт END_REQUEST.
    Возвращает (stdout, stderr, app_status, protocol_status)."""
    stdout, stderr = bytearray(), bytearray()
    app_status, protocol_status = 0, 0
    while True:
        head = recv_exact(sock, 8)
        if head is None:
            break
        version, rec_type, request_id, length, padding = \
            struct.unpack(">BBHHB", head[:7])
        content = recv_exact(sock, length) if length else b""
        if padding:
            recv_exact(sock, padding)
        if content is None:
            break
        if rec_type == STDOUT:
            stdout += content
        elif rec_type == STDERR:
            sys.stderr.write("[app stderr] " + content.decode(
                "utf-8", "replace"))
        elif rec_type == END_REQUEST:
            # appStatus (4 байта), protocolStatus (1), резерв (3)
            app_status, protocol_status = struct.unpack(">IB3x", content)
            break
    return bytes(stdout), bytes(stderr), app_status, protocol_status


def recv_exact(sock, length):
    buf = bytearray()
    while len(buf) < length:
        chunk = sock.recv(length - len(buf))
        if not chunk:
            return None
        buf += chunk
    return bytes(buf)


def parse_target(text):
    """'METHOD URI' -> (method, uri); URI без метода -> GET."""
    if " " in text:
        method, uri = text.split(" ", 1)
        return method.upper(), uri
    return "GET", text


def main():
    ap = argparse.ArgumentParser(description="FastCGI test client")
    ap.add_argument("requests", nargs="+", help="запросы: 'METHOD URI'")
    ap.add_argument("-H", "--host", default="127.0.0.1")
    ap.add_argument("-p", "--port", type=int, default=9000)
    ap.add_argument("-s", "--socket", help="Unix-сокет вместо TCP")
    ap.add_argument("--data", help="тело запроса; '@-' — из stdin, "
                                   "'@file' — из файла")
    ap.add_argument("--keep", action="store_true",
                    help="не закрывать соединение между запросами "
                         "(FCGI_KEEP_CONN)")
    ap.add_argument("-v", "--verbose", action="store_true",
                    help="печатать каждую полученную запись")
    args = ap.parse_args()

    if args.socket:
        sock = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
        sock.connect(args.socket)
    else:
        sock = socket.create_connection((args.host, args.port))

    body = b""
    if args.data:
        if args.data == "@-":
            body = sys.stdin.buffer.read()
        elif args.data.startswith("@"):
            with open(args.data[1:], "rb") as f:
                body = f.read()
        else:
            body = args.data.encode()

    for request_id, target in enumerate(args.requests, start=1):
        method, uri = parse_target(target)
        params = {
            "REQUEST_METHOD": method,
            "REQUEST_URI": uri,
            "SCRIPT_NAME": "",
            "SERVER_PROTOCOL": "HTTP/1.1",
            "REMOTE_ADDR": "127.0.0.1",
        }
        query = uri.partition("?")[2]
        if query:
            params["QUERY_STRING"] = query
        if body:
            params["CONTENT_TYPE"] = "application/octet-stream"
            params["CONTENT_LENGTH"] = str(len(body))

        sock.sendall(begin_request(request_id, args.keep))
        send_params(sock, request_id, params)
        send_stdin(sock, request_id, body)

        stdout, stderr, app_status, protocol_status = read_records(sock)
        if args.verbose:
            print("---", file=sys.stderr)
        sys.stdout.buffer.write(stdout)
        sys.stdout.flush()
        print("\n--- app_status: %d, protocol_status: %d"
              % (app_status, protocol_status), file=sys.stderr)
        if not args.keep and len(args.requests) > 1:
            break  # без KEEP_CONN сервер закроет соединение

    sock.close()


if __name__ == "__main__":
    main()
