"""Video bench for the PS4 client (Dev build): finds how fast the PS4 decodes and converts
the stream, and which encoder settings it copes with best.

Each test encodes a clip on the PC (ffmpeg + NVENC, with the settings ALVR 20.14.1 uses:
CBR, one-frame VBV, infinite GOP, no B-frames, CAVLC, quarter-resolution multipass,
spatial AQ, preset P4 low latency), sends it to the PS4 (TCP 9955), which plays it through
its real video pipeline (queue, hardware decoder, foveation expansion, headset display)
and sends the timings back. One parameter changes per test.

  python tools/video_bench.py                        quick plan, newest ALVR recording as source
  python tools/video_bench.py --plan full            every parameter
  python tools/video_bench.py --plan bitrate --fps 90
  python tools/video_bench.py --source clip.mp4 --frames 300 --ip 192.168.0.124
  python tools/video_bench.py --list                 show the plans

Source: an ALVR recording (dashboard, Debug tab, "Start recording": recording.*.h264 next
to session.json), which is real game content already in the foveated 1920x1056 layout, or
any video file (scaled). Without a source, a synthetic moving pattern is used (decoding
cost is not representative of games). The PS4 app must be running (Dev build, lobby,
SteamVR closed). Results: printed, and saved in logs/bench-<time>.json.
"""
import argparse
import glob
import json
import math
import os
import socket
import struct
import subprocess
import sys
import tempfile
import time

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
STREAMER_DIR = os.path.join(os.path.expanduser("~"), "Downloads", "ALVR-PS4_PC-Streamer")
PORT = 9955

# The PS4 client's defaults: 130 % (1248x1376 per eye as negotiated), foveation 0.5 / 2.
VIEW = (1248, 1376)
FFE = dict(center_x=0.5, center_y=0.5, shift_x=0.0, shift_y=0.0, ratio_x=2.0, ratio_y=2.0)


def compressed_size(n, center, shift, ratio):
    """One eye's size along an axis in the decoded frame (client/src/foveation.cpp)."""
    if not ratio > 1.0 or not center < 1.0:
        return (n + 31) // 32 * 32
    edge = n - center * n
    cs = 1.0 - math.ceil(edge / (ratio * 2.0)) * (ratio * 2.0) / n
    scale = cs + (1.0 - cs) / ratio
    return int(math.ceil(scale * n / 32.0) * 32)


def frame_size(ffe, view=VIEW):
    if ffe is None:
        return view[0] * 2, view[1]
    return (compressed_size(view[0], ffe["center_x"], ffe["shift_x"], ffe["ratio_x"]) * 2,
            compressed_size(view[1], ffe["center_y"], ffe["shift_y"], ffe["ratio_y"]))


# ---- H.264 Annex B -------------------------------------------------------------------------

def nal_units(data):
    """(type, bytes with start code) for each NAL unit."""
    out = []
    i, n = 0, len(data)
    starts = []
    while True:
        j = data.find(b"\x00\x00\x01", i)
        if j < 0:
            break
        s = j - 1 if j > 0 and data[j - 1] == 0 else j
        starts.append((s, j + 3))
        i = j + 3
    for k, (s, h) in enumerate(starts):
        e = starts[k + 1][0] if k + 1 < len(starts) else n
        if h < e:
            out.append((data[h] & 0x1F, b"\x00\x00\x00\x01" + data[h:e]))
    return out


def access_units(data):
    """config (SPS + PPS of the first IDR), [(is_idr, frame bytes without SPS/PPS)]."""
    config, frames, cur, cur_idr = b"", [], [], False
    for t, nal in nal_units(data):
        if t in (7, 8):
            # The SPS / PPS before the first frame (repeated ones are the same).
            if not frames and not cur and nal not in config:
                config += nal
            continue
        if t in (1, 5):
            first_mb_zero = nal[5] & 0x80 != 0
            if first_mb_zero and cur:
                frames.append((cur_idr, b"".join(cur)))
                cur, cur_idr = [], False
            cur.append(nal)
            cur_idr = cur_idr or t == 5
        # SEI (6), AUD (9) and the rest are left out, as the streamer sends none.
    if cur:
        frames.append((cur_idr, b"".join(cur)))
    while frames and not frames[0][0]:
        frames.pop(0)  # start on an IDR
    return config, frames


# ---- Encoding ------------------------------------------------------------------------------------

def find_source():
    recs = sorted(glob.glob(os.path.join(STREAMER_DIR, "recording.*.h264")), key=os.path.getmtime)
    return recs[-1] if recs else None


def extract_segment(source, start, frames, out_path):
    """`frames` frames of an Annex B recording from the first IDR after `start` (0..1 of
    the file; the stream is CBR, so bytes follow time), written with their SPS / PPS.
    Returns the number of frames and where the IDR was found (0..1)."""
    size = os.path.getsize(source)
    # A recording without drops has few IDRs: search forward to the end of the file, then
    # from its beginning (which always starts with one). Chunks overlap by 8 bytes so a
    # start code across two chunks is not missed; the loop ends on the last chunk.
    CHUNK, data, i, found = 64 << 20, b"", -1, 0.0
    with open(source, "rb") as f:
        for p in (int(size * start), 0):
            while p < size:
                f.seek(p)
                chunk = f.read(CHUNK)
                k = chunk.find(b"\x00\x00\x01\x65")
                if k >= 0:
                    found = (p + k) / size
                    f.seek(max(0, p + k - 4096))
                    data = f.read(frames * 400 * 1024 + 4096)
                    i = data.find(b"\x00\x00\x01\x65")
                    break
                if len(chunk) < CHUNK:  # end of the file reached
                    break
                p += CHUNK - 8
            if i >= 0:
                break
    if i < 0:
        raise SystemExit("no IDR found in the recording")
    # SPS / PPS are repeated right before each IDR (repeatSPSPPS).
    j = data.rfind(b"\x00\x00\x01\x67", max(0, i - 4096), i)
    config, fr = access_units(data[j if j >= 0 else i:])
    if not config:
        with open(source, "rb") as f:
            config, _ = access_units(f.read(2 << 20))
    fr = fr[:frames]
    with open(out_path, "wb") as f:
        f.write(config)
        for _, d in fr:
            f.write(d)
    return len(fr), found


def decode_segment(source, seconds, frames, fps, out_path):
    """`frames` frames from `seconds` into a recording, when no IDR is near: ffmpeg decodes
    it from the start (slower), and keeps them almost lossless for the re-encoded tests."""
    subprocess.run(["ffmpeg", "-y", "-hide_banner", "-loglevel", "error", "-f", "h264", "-framerate", str(fps),
                    "-i", source, "-ss", f"{seconds:.2f}", "-frames:v", str(frames), "-c:v", "h264_nvenc",
                    "-preset", "p4", "-rc", "constqp", "-qp", "12", "-g", "100000", "-bf", "0", "-f", "h264",
                    out_path], check=True)


def encode(source, size, t, frames, fps, out_path):
    """Encodes `frames` frames of the source at `size` with test t's encoder settings."""
    w, h = size
    b = t.get("mbps", 60)
    bufsize = int(b * 1e6 / fps * 1.1)
    if source:
        inp = ["-f", "h264", "-framerate", str(fps), "-i", source] if source.endswith(".h264") else ["-i", source]
        vf = f"scale={w}:{h}:flags=bicubic,fps={fps}"
    else:
        inp = ["-f", "lavfi", "-i", f"testsrc2=size={w}x{h}:rate={fps}"]
        vf = "noise=alls=10:allf=t,format=yuv420p"
    cmd = ["ffmpeg", "-y", "-hide_banner", "-loglevel", "error"] + inp + [
        "-frames:v", str(frames), "-vf", vf, "-pix_fmt", "yuv420p",
        "-c:v", "h264_nvenc", "-preset", t.get("preset", "p4"), "-tune", t.get("tune", "ll"),
        "-profile:v", t.get("profile", "high"), "-coder", t.get("coder", "cavlc"),
        "-rc", "cbr", "-b:v", f"{b}M", "-maxrate", f"{b}M", "-bufsize", str(bufsize),
        "-multipass", t.get("multipass", "qres"), "-spatial-aq", str(t.get("aq", 1)),
        "-g", "100000", "-bf", "0", "-no-scenecut", "1", "-zerolatency", "1", "-forced-idr", "1",
        "-dpb_size", str(t.get("refs", 0)), "-color_range", "pc", "-f", "h264", out_path]
    if t.get("intra_refresh"):
        cmd[cmd.index("-dpb_size"):cmd.index("-dpb_size")] = ["-intra-refresh", "1"]
    if t.get("weighted_pred"):
        cmd[cmd.index("-dpb_size"):cmd.index("-dpb_size")] = ["-weighted_pred", "1"]
    subprocess.run(cmd, check=True)


# ---- Plans -------------------------------------------------------------------------------------------

def plans(fps):
    base = dict(mbps=80, ffe=dict(FFE), fps=fps)

    def t(name, **kw):
        d = dict(base)
        d["ffe"] = dict(base["ffe"])
        d.update(kw)
        d["name"] = name
        return d

    bitrate = [t(f"{b} Mbps", mbps=b) for b in (40, 60, 80, 100, 130, 160)]
    throughput = [t(f"max speed {b} Mbps", mbps=b, fps=0) for b in (60, 100, 160)]
    encoder = [
        t("CABAC 90 Mbps", mbps=90, coder="cabac"),
        t("CAVLC 90 Mbps", mbps=90),
        t("1 reference 90 Mbps", mbps=90, refs=1),
        t("Main profile 90 Mbps", mbps=90, profile="main"),
        t("Baseline profile 90 Mbps", mbps=90, profile="baseline"),
        t("preset P1 90 Mbps", mbps=90, preset="p1"),
        t("preset P7 90 Mbps", mbps=90, preset="p7"),
        t("ultra low latency 90 Mbps", mbps=90, tune="ull"),
        t("no AQ 90 Mbps", mbps=90, aq=0),
        t("intra refresh 90 Mbps", mbps=90, intra_refresh=True),
    ]
    size = []
    for c, r in ((0.5, 2.0), (0.45, 2.0), (0.4, 2.0), (0.5, 3.0), (0.4, 3.0), (0.35, 4.0)):
        ffe = dict(FFE, center_x=c, center_y=c, ratio_x=r, ratio_y=r)
        w, h = frame_size(ffe)
        size.append(t(f"foveation {c}/{r:g} ({w}x{h}) 90 Mbps", mbps=90, ffe=ffe))
    pipeline = [t(f"depth {d}", depth=d) for d in (1, 2, 3, 4)] + [t(f"{j} jobs", jobs=j) for j in (2, 4, 6, 8)]
    quick = [t("ALVR recording as it is", raw=True), t("80 Mbps (current)"), t("60 Mbps", mbps=60),
             t("120 Mbps", mbps=120), t("max speed 60 Mbps", fps=0), size[2], encoder[0]]
    pacing = [t("80 Mbps (current)")]  # one test: watch the "pacing" figures of the PS4 log
    return {"quick": quick, "pacing": pacing, "bitrate": bitrate + throughput, "encoder": encoder, "size": size,
            "pipeline": pipeline, "full": bitrate + throughput + encoder + size + pipeline}


# ---- PS4 link -------------------------------------------------------------------------------------------

def recv_all(sock, n):
    buf = b""
    while len(buf) < n:
        chunk = sock.recv(n - len(buf))
        if not chunk:
            raise ConnectionError("PS4 closed the connection")
        buf += chunk
    return buf


def run_test(sock, t, config, frames, loops):
    ffe = t["ffe"]
    header = dict(name=t["name"], view_w=VIEW[0], view_h=VIEW[1], full_range=1, fps=t["fps"], loops=loops,
                  depth=t.get("depth", 3), jobs=t.get("jobs", 4))
    if ffe:
        header.update(ffe_center_x=ffe["center_x"], ffe_center_y=ffe["center_y"], ffe_shift_x=ffe["shift_x"],
                      ffe_shift_y=ffe["shift_y"], ffe_ratio_x=ffe["ratio_x"], ffe_ratio_y=ffe["ratio_y"])
    hj = json.dumps(header).replace(" ", "").encode()
    parts = [b"ALVB", struct.pack("<I", len(hj)), hj, struct.pack("<I", len(config)), config,
             struct.pack("<I", len(frames))]
    for idr, data in frames:
        parts.append(struct.pack("<I", len(data) | (0x80000000 if idr else 0)))
        parts.append(data)
    sock.sendall(b"".join(parts))
    n, = struct.unpack("<I", recv_all(sock, 4))
    return json.loads(recv_all(sock, n))


def row(r, t, size):
    if not r.get("ok"):
        return f"{t['name']:<40} ERROR: {r.get('error')}"
    d, c, l = r["decode"], r["convert"], r["latency"]
    return (f"{t['name']:<40} {size[0]}x{size[1]:<5} {r['kb_per_frame']:6.0f} KB  "
            f"dec {d['avg']:5.2f} p90 {d['p90']:5.1f} p99 {d['p99']:5.1f} max {d['max']:5.1f}  "
            f"conv {c['avg']:4.2f}  lat {l['avg']:5.1f} p99 {l['p99']:5.1f}  "
            f"{r['fps_out']:5.1f} fps  drop {r['dropped'] + r['replaced']}")


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--ip", default="192.168.0.124")
    ap.add_argument("--source", help="ALVR recording (.h264) or video file (default: newest ALVR recording)")
    ap.add_argument("--synthetic", action="store_true", help="synthetic moving pattern instead of a recording")
    ap.add_argument("--plan", default="quick")
    ap.add_argument("--fps", type=float, default=90.0, help="playback rate on the PS4 (0 = as fast as possible)")
    ap.add_argument("--frames", type=int, default=300)
    ap.add_argument("--loops", type=int, default=2)
    ap.add_argument("--start", type=float, default=0.5,
                    help="where the clip starts in an ALVR recording, 0..1 of its length (default: the middle)")
    ap.add_argument("--depth", type=int, help="decoder pipeline depth for every test (default: each test's, 3)")
    ap.add_argument("--mbps", type=float, help="bitrate of every test")
    ap.add_argument("--jobs", type=int, help="conversion jobs for every test (default: each test's, 4)")
    ap.add_argument("--list", action="store_true")
    a = ap.parse_args()
    all_plans = plans(a.fps)
    if a.list:
        for k, v in all_plans.items():
            print(k + ":")
            for t in v:
                print("   ", t["name"])
        return
    source = None if a.synthetic else (a.source or find_source())
    print("source:", source or "synthetic pattern (not representative of game content)")
    tests = all_plans[a.plan]
    for t in tests:
        if a.depth:
            t.setdefault("depth", a.depth)
        if a.mbps:
            t["mbps"] = a.mbps
        if a.jobs:
            t.setdefault("jobs", a.jobs)
    results = []
    raw_ok = True
    tmp = tempfile.mkdtemp(prefix="alvr-bench-")
    if source and source.endswith(".h264"):
        # A clip of the recording, starting on an IDR: the source of every test.
        seg = os.path.join(tmp, "segment.h264")
        n, found = extract_segment(source, a.start, a.frames, seg)
        if abs(found - a.start) > 0.02:
            # No IDR near the start (a recording without drops has one, at its beginning):
            # decode up to the position instead; the recording's own frames cannot be sent.
            frame_bytes = os.path.getsize(seg) / max(n, 1)
            seconds = a.start * os.path.getsize(source) / frame_bytes / (a.fps or 90)
            print(f"no IDR near {a.start:.0%}: decoding the recording up to {seconds:.0f} s...")
            decode_segment(source, seconds, a.frames, int(a.fps or 90), seg)
            raw_ok = False
        print(f"clip: {a.frames} frames from {a.start:.0%} of {os.path.basename(source)}")
        source = seg
    sock = socket.create_connection((a.ip, PORT), timeout=10)
    sock.settimeout(120)
    print(f"connected to {a.ip}:{PORT}, {len(tests)} tests, {a.frames} frames x {a.loops}\n")
    try:
        for i, t in enumerate(tests):
            size = frame_size(t["ffe"])
            if t.get("raw"):  # the recording's own frames (real ALVR encoder output)
                if not source or not source.endswith(".h264") or not raw_ok:
                    continue
                config, frames = access_units(open(source, "rb").read())
                frames = frames[:a.frames]
            else:
                clip = os.path.join(tmp, f"t{i}.h264")
                encode(source, size, t, a.frames, int(a.fps) if a.fps else 90, clip)
                config, frames = access_units(open(clip, "rb").read())
            r = run_test(sock, t, config, frames, a.loops)
            if r.get("ok"):  # measured here: the PS4 total also counts earlier tests
                r["kb_per_frame"] = sum(len(d) for _, d in frames) / len(frames) / 1024
            r["test"] = t
            r["frame_size"] = size
            results.append(r)
            print(row(r, t, size), flush=True)
    finally:
        sock.close()
        out = os.path.join(ROOT, "logs", time.strftime("bench-%Y%m%d-%H%M%S.json"))
        os.makedirs(os.path.dirname(out), exist_ok=True)
        json.dump(dict(source=source, frames=a.frames, loops=a.loops, results=results), open(out, "w"), indent=1)
        print("\nsaved", out)


if __name__ == "__main__":
    main()
