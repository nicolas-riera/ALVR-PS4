"""Upload the built .pkg to the PS4 over GoldHEN's FTP.

Usage: python tools/deploy.py [ps4_ip] [port]
The package lands in /data/pkg/, where GoldHEN's Package Installer lists it.
"""
import ftplib
import glob
import os
import sys

ip = sys.argv[1] if len(sys.argv) > 1 else "192.168.0.124"
port = int(sys.argv[2]) if len(sys.argv) > 2 else 2121
client_dir = os.path.join(os.path.dirname(__file__), "..", "client")
pkgs = glob.glob(os.path.join(client_dir, "*.pkg"))
if not pkgs:
    sys.exit("No .pkg found: build first (wsl -d Debian -- bash tools/build.sh)")
pkg = pkgs[0]

ftp = ftplib.FTP()
ftp.connect(ip, port, timeout=30)
ftp.login()
try:
    ftp.mkd("/data/pkg")
except ftplib.error_perm:
    pass
ftp.cwd("/data/pkg")
name = os.path.basename(pkg)
size = os.path.getsize(pkg)
with open(pkg, "rb") as f:
    ftp.storbinary(f"STOR {name}", f)
remote = ftp.size(name)
ftp.quit()
print(f"Uploaded {name} ({size} bytes) -> /data/pkg/{name}" + ("" if remote == size else f" SIZE MISMATCH remote={remote}"))
