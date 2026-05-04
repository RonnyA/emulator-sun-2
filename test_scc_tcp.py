"""
Test the SCC-TCP pipe end-to-end.

Two parts:
  1. Verify the *output* path: client connects, server greets via the
     ringbuf-based scc_tcp_send_byte path.  If we see the greeting,
     ringbuf + drain + send() works.
  2. Verify the *input* path: client sends "hello\\n", we check that
     scc-tcp recv'd it via the trace counter ("recv N bytes from
     client").  This proves recv → input_buf works.  We can't easily
     prove scc_tcp_poll → scc_in_push without booting SunOS, but the
     trace will tell us the byte made it across.
"""
import socket, subprocess, time, os, sys, signal

ROOT = r"E:\Dev\Emulators\68k\emulator-sun-2"
SIM  = os.path.join(ROOT, "sim", "sim.exe")
PORT = 9912

env = os.environ.copy()
env["SCC_TCP_TRACE"] = "1"

print("=== launching sim with --scc-tcp ===")
proc = subprocess.Popen(
    [SIM, "-q",
     "--scc-tcp=" + str(PORT),
     "--no-kbd",
     "--prom=" + os.path.join(ROOT, "media/rom/sun2-multi-rev-R.bin"),
     "--disk=" + os.path.join(ROOT, "media/disk/my-sun2-s3.2-disk.img"),
     "--tape=" + os.path.join(ROOT, "media/tape/tape3.2")],
    cwd=ROOT,
    env=env,
    stdout=subprocess.PIPE,
    stderr=subprocess.STDOUT,
    creationflags=getattr(subprocess, "CREATE_NEW_PROCESS_GROUP", 0),
)
# Give sim a few seconds to set up SDL + pcap + listen socket.
for _ in range(20):
    time.sleep(0.25)
    try:
        probe = socket.create_connection(("127.0.0.1", PORT), timeout=0.1)
        probe.close()
        break
    except OSError:
        pass

print("=== connecting to localhost:%d ===" % PORT)
s = socket.socket()
s.settimeout(3)
s.connect(("127.0.0.1", PORT))

# 1. Read whatever the server sends in 45 seconds — give the PROM time
#    to time out the keyboard probe AND finish RAM self-test (which on
#    a real Sun-2 took up to 30s for 4MB).
data = b""
end = time.time() + 45
while time.time() < end:
    try:
        chunk = s.recv(4096)
        if not chunk: break
        data += chunk
    except socket.timeout:
        pass
print("RX from server: %d bytes" % len(data))
print("   first 100:   %r" % data[:100])

# 2. Send a probe. Server should recv it (we'll see in trace) and push
#    it into scc_in_push (no observable side-effect from this side
#    since SunOS isn't reading, but the trace proves it).
probe = b"probe-from-test\r\n"
s.send(probe)
print("TX to server:   %d bytes  %r" % (len(probe), probe))
time.sleep(1)

# 3. Read for another 2 seconds in case anything else flows.
end = time.time() + 2
extra = b""
while time.time() < end:
    try:
        chunk = s.recv(4096)
        if not chunk: break
        extra += chunk
    except socket.timeout:
        pass
print("RX more:        %d bytes  %r" % (len(extra), extra[:200]))

s.close()

# Stop the sim and dump the trace output.
print("=== shutting down sim ===")
try:
    proc.send_signal(signal.SIGTERM)
except Exception:
    proc.terminate()
try:
    out, _ = proc.communicate(timeout=3)
except subprocess.TimeoutExpired:
    proc.kill()
    out, _ = proc.communicate()

print("=== sim stderr/stdout (filtered for scc-tcp) ===")
sys.stdout.reconfigure(encoding="utf-8", errors="replace")
text = out.decode("utf-8", errors="replace")
for line in text.splitlines():
    if "scc-tcp" in line or "scc:" in line:
        print(line)

# Count interesting events
n_send_byte = sum(1 for ln in text.splitlines() if "scc-tcp: send_byte" in ln)
n_recv      = sum(1 for ln in text.splitlines() if "scc-tcp: recv "    in ln)
n_send_tcp  = sum(1 for ln in text.splitlines() if "scc-tcp: send "    in ln)
n_poll      = sum(1 for ln in text.splitlines() if "scc-tcp: poll"     in ln)

print()
print("=== summary ===")
print("scc_wr_data called for ch=0/1 (scc_tcp_send_byte fired):", n_send_byte)
print("TCP recv events                                        :", n_recv)
print("TCP send events                                        :", n_send_tcp)
print("scc_in_push events (input path delivered to SunOS)     :", n_poll)
