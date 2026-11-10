PC tests for the sound module's 6502 core and SID player
=========================================================

Both include the port's headers directly; nothing of MicroPython is needed.

    cc -O2 -o cputest cputest.c
    ./cputest 6502_functional_test.bin
    # from github.com/Klaus2m5/6502_65C02_functional_tests (bin_files/):
    # load at 0, start $0400, success = PC stuck at $3469 -> "PASS"

    cc -O2 -o sidplay sidplay.c
    ./sidplay tune.sid [song] [seconds]
    SIDLOG=writes.txt ./sidplay tune.sid     # every SID write: time_ms reg value

`sidplay` prints the load/init/play addresses, the interrupt setup after init,
calls per second by source (frame / raster / CIA) and note starts per voice --
enough to see whether a tune runs and at what rate.
