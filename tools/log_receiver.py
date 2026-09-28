"""Receive ALVR PS4 client logs broadcast over UDP and print/save them.

Usage: python tools/log_receiver.py [port]
Logs are also appended to logs/ps4-YYYYmmdd-HHMMSS.log.
"""
import datetime
import os
import socket
import sys

port = int(sys.argv[1]) if len(sys.argv) > 1 else 9944
os.makedirs(os.path.join(os.path.dirname(__file__), "..", "logs"), exist_ok=True)
path = os.path.join(os.path.dirname(__file__), "..", "logs",
                    datetime.datetime.now().strftime("ps4-%Y%m%d-%H%M%S.log"))

sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
sock.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
sock.bind(("0.0.0.0", port))
print(f"Listening for PS4 logs on UDP {port} -> {os.path.abspath(path)}", flush=True)

expected = None
with open(path, "a", encoding="utf-8") as out:
    while True:
        data, (ip, _) = sock.recvfrom(4096)
        text = data.decode("utf-8", "replace")
        seq_s, _, line = text.partition("|")
        try:
            seq = int(seq_s)
        except ValueError:
            seq, line = None, text
        if seq is not None:
            if expected is not None and seq > expected:
                note = f"!! {seq - expected} log line(s) lost"
                print(note, flush=True)
                out.write(note + "\n")
            elif expected is not None and seq < expected:
                note = "== client restarted =="
                print(note, flush=True)
                out.write(note + "\n")
            expected = seq + 1
        msg = f"{ip} {line}"
        print(msg, flush=True)
        out.write(msg + "\n")
        out.flush()
