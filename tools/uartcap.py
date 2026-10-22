import os, sys, termios, time, select
dev, secs, out = sys.argv[1], float(sys.argv[2]), sys.argv[3]
fd = os.open(dev, os.O_RDONLY | os.O_NOCTTY | os.O_NONBLOCK)
a = termios.tcgetattr(fd)
a[0] = 0; a[1] = 0; a[2] = termios.CS8 | termios.CREAD | termios.CLOCAL; a[3] = 0
a[4] = a[5] = termios.B115200
termios.tcsetattr(fd, termios.TCSANOW, a)
termios.tcflush(fd, termios.TCIFLUSH)
end = time.time() + secs; data = b''
while time.time() < end:
    r, _, _ = select.select([fd], [], [], 0.1)
    if r:
        try: data += os.read(fd, 4096)
        except BlockingIOError: pass
open(out, 'wb').write(data)
