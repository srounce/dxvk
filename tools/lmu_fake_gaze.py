#!/usr/bin/env python3
# Fake LMU gaze writer for testing d3d11.lmuFoveationGaze (M1-1).
# Writes /dev/shm/lmu_gaze in the D3D11GazeShm layout and sweeps the focus
# centre so you can watch it track with d3d11.lmuFoveationDebug=True.
#
#   uint32 sequence; uint32 valid; float lX,lY,rX,rY; double timestampNs;  (32 bytes)
#
# Usage: python3 lmu_fake_gaze.py                 # slow circular sweep
#        python3 lmu_fake_gaze.py --static X Y     # fixed point
#        add --conv C to converge the eyes (left = x+C, right = x-C); default 0.15
#
# The two eyes get different X so the focus areas converge, as a real per-eye
# gaze tracker would. --conv 0 makes both eyes identical.

import mmap, os, struct, sys, time, math

PATH = "/dev/shm/lmu_gaze"
SIZE = 32
FMT = "<IIffffd"

def main():
    argv = sys.argv[1:]
    conv = 0.15
    if "--conv" in argv:
        i = argv.index("--conv")
        conv = float(argv[i + 1])
        del argv[i:i + 2]

    static = None
    if len(argv) == 3 and argv[0] == "--static":
        static = (float(argv[1]), float(argv[2]))

    fd = os.open(PATH, os.O_CREAT | os.O_RDWR, 0o644)
    os.ftruncate(fd, SIZE)
    mm = mmap.mmap(fd, SIZE)

    seq = 0
    t0 = time.monotonic()
    print(f"writing {PATH}; Ctrl-C to stop")
    try:
        while True:
            t = time.monotonic() - t0
            if static:
                x, y = static
            else:
                x = 0.55 * math.sin(t * 1.2)
                y = 0.35 * math.sin(t * 0.8)
            lx, rx = x + conv, x - conv   # converge the eyes toward the nose
            seq += 1                      # odd: write in progress
            mm.seek(0); mm.write(struct.pack(FMT, seq, 1, lx, y, rx, y, t * 1e9))
            seq += 1                      # even: done
            mm.seek(0); mm.write(struct.pack("<I", seq))
            time.sleep(1.0 / 120.0)
    except KeyboardInterrupt:
        pass
    finally:
        mm.close(); os.close(fd)

if __name__ == "__main__":
    main()
