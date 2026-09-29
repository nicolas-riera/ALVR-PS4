"""Upload the built .pkg to the PS4 over GoldHEN's FTP.

Usage: python tools/deploy.py [--stable] [ps4_ip] [port]
Uploads the Dev package ("ALVR PS4 (Dev)", title id ALVR00002) unless --stable is given.
The package lands in /data/pkg/, where GoldHEN's Package Installer lists it.
"""
import ftplib
import os
import sys

args = [a for a in sys.argv[1:] if a != "--stable"]
stable = len(args) != len(sys.argv) - 1
ip = args[0] if len(args) > 0 else "192.168.0.124"
port = int(args[1]) if len(args) > 1 else 2121
client_dir = os.path.join(os.path.dirname(__file__), "..", "client")
title = "ALVR00001" if stable else "ALVR00002"
pkg = os.path.join(client_dir, f"IV0000-{title}_00-ALVRPS4CLIENT000.pkg")
if not os.path.exists(pkg):
    sys.exit(f"{os.path.basename(pkg)} not found: build first (wsl -d Debian -- bash tools/build.sh"
             + (" VARIANT=stable)" if stable else ")"))

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
