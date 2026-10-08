#!/usr/bin/env python3
"""A deliberately slow RTSP client, used to test server-side adaptive bitrate.

It plays a stream with RTP over TCP but reads the socket at a limited speed,
like a viewer on a slow network. The server falls behind schedule, notices
it, and switches the session to a lower quality. Watch it happen in the
server log or in the "RTSP sessions" panel of the web page.

Usage:
    python3 scripts/slow_rtsp_client.py rtsp://127.0.0.1:8554/videos/movie.mp4 --kbps 1000 --seconds 30
"""
import argparse
import socket
import time
from urllib.parse import urlparse


def request(sock, method, url, cseq, extra=""):
    message = f"{method} {url} RTSP/1.0\r\nCSeq: {cseq}\r\n{extra}\r\n"
    sock.sendall(message.encode())
    data = b""
    while b"\r\n\r\n" not in data:
        chunk = sock.recv(4096)
        if not chunk:
            raise RuntimeError("connection closed")
        data += chunk
    head, _, rest = data.partition(b"\r\n\r\n")
    headers = {}
    for line in head.decode().split("\r\n")[1:]:
        name, _, value = line.partition(":")
        headers[name.strip().lower()] = value.strip()
    length = int(headers.get("content-length", "0"))
    while len(rest) < length:
        rest += sock.recv(4096)
    status = head.decode().split("\r\n")[0]
    return status, headers, rest[:length].decode(errors="replace")


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("url")
    parser.add_argument("--kbps", type=int, default=1000, help="reading speed in kilobits per second")
    parser.add_argument("--seconds", type=int, default=30, help="how long to play")
    args = parser.parse_args()

    target = urlparse(args.url)
    sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    sock.setsockopt(socket.SOL_SOCKET, socket.SO_RCVBUF, 32 * 1024)  # small buffer = congestion shows quickly
    sock.connect((target.hostname, target.port or 554))

    status, headers, sdp = request(sock, "DESCRIBE", args.url, 1, "Accept: application/sdp\r\n")
    print(status)
    base = headers.get("content-base", args.url + "/")
    tracks = [line.split("control:")[1] for line in sdp.splitlines() if line.startswith("a=control:trackID")]
    session = ""
    for index, track in enumerate(tracks):
        extra = f"Transport: RTP/AVP/TCP;unicast;interleaved={2 * index}-{2 * index + 1}\r\n"
        if session:
            extra += f"Session: {session}\r\n"
        status, headers, _ = request(sock, "SETUP", base + track, 2 + index, extra)
        session = headers.get("session", "").split(";")[0]
        print(status, "->", headers.get("transport"))
    status, headers, _ = request(sock, "PLAY", base, 10, f"Session: {session}\r\nRange: npt=0.000-\r\n")
    print(status, headers.get("rtp-info", ""))

    bytes_per_second = args.kbps * 1000 / 8
    started = time.time()
    received = 0
    while time.time() - started < args.seconds:
        chunk = sock.recv(4096)
        if not chunk:
            break
        received += len(chunk)
        # Sleep so that the average reading speed matches --kbps.
        ahead = received / bytes_per_second - (time.time() - started)
        if ahead > 0:
            time.sleep(ahead)
    print(f"received {received / 1e6:.2f} MB in {time.time() - started:.1f} s "
          f"({received * 8 / 1000 / (time.time() - started):.0f} kbps)")
    sock.sendall(f"TEARDOWN {base} RTSP/1.0\r\nCSeq: 99\r\nSession: {session}\r\n\r\n".encode())
    sock.close()


if __name__ == "__main__":
    main()
