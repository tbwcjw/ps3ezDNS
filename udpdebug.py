import socket
import sys
import time

sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)

IP = "0.0.0.0"
PORT = 18194
for n in range(2):
    try:
        sock.bind((IP, PORT))
        ip, port = sock.getsockname()
        print(f"udp bound on {ip}. listening to {port}")
        break
    except OSError as e:
        import subprocess
        subprocess.run(["sudo", "fuser", "-k", f"18194/udp"])
        time.sleep(1)
else:
    print("no such luck")
    sys.exit(1)

while True:
    data, addr = sock.recvfrom(4096)
    ip, port = addr
    print(f"{ip}:{port}: {data}")
