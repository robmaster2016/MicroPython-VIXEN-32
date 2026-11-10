/* SPDX-License-Identifier: MIT
 * Copyright (c) 2025-2026 Robert S. (aka Robmaster) */
/* PC test of the SID player (vx_sidplay.h, the code vixen_sound.c runs).
 *
 *   cc -O2 -o sidplay sidplay.c && ./sidplay tune.sid [song] [seconds]
 *
 * Plays the tune against a simulated clock and prints what reached the SID:
 * writes per register, note starts (gate 0->1) per voice, calls per second.
 * With SIDLOG=file every write goes there as "time_ms reg value". */
#include <stdio.h>
#include <stdlib.h>

static FILE *logf;
static double now_ms;
static unsigned long writes[32], gates[3];
static unsigned char ctrl[3];

static void sidp_out(unsigned reg, unsigned v) {
    writes[reg]++;
    if (logf) {
        fprintf(logf, "%.3f %u %u\n", now_ms, reg, v);
    }
}
static void sidp_out_ctrl(unsigned reg, unsigned v) {
    unsigned voice = reg / 7u;
    if ((v & 1u) && !(ctrl[voice] & 1u)) {
        gates[voice]++;
    }
    ctrl[voice] = (unsigned char)v;
    sidp_out(reg, v);
}

#include "../../vx_sidplay.h"

static unsigned char ram[65536], io[4096];

int main(int argc, char **argv) {
    if (argc < 2) {
        fprintf(stderr, "usage: %s tune.sid [song] [seconds]\n", argv[0]);
        return 2;
    }
    static unsigned char file[65536 + 0x7C + 2];
    FILE *f = fopen(argv[1], "rb");
    size_t n = f ? fread(file, 1, sizeof(file), f) : 0;
    int song = argc > 2 ? atoi(argv[2]) : 0;
    double secs = argc > 3 ? atof(argv[3]) : 10.0;
    if (getenv("SIDLOG")) {
        logf = fopen(getenv("SIDLOG"), "w");
    }
    sidp_hdr_t h;
    if (n < 0x76 || sidp_parse(&h, file) || n < h.off + 2u) {
        puts("not a SID file");
        return 1;
    }
    unsigned pos = h.off, load = h.load;
    if (!load) {
        load = file[pos] | (file[pos + 1] << 8);
        pos += 2;
    }
    unsigned len = (unsigned)n - pos;
    if (len > 0x10000u - load) {
        len = 0x10000u - load;
    }
    memcpy(ram + load, file + pos, len);
    song = sidp_setup(&h, ram, io, load, len, song);
    if (song < 0) {
        puts("no such song");
        return 1;
    }
    printf("%s %.32s / %.32s  load $%04X-$%04X init $%04X play $%04X song %d/%d %s\n",
        h.rsid ? "RSID" : "PSID", file + 0x16, file + 0x36, load, load + len - 1, sidp.init, sidp.play,
        song, h.songs, ((h.flags >> 2) & 3u) == 2u ? "NTSC" : "PAL");
    uint64_t now = 0;
    sidp_start((uint32_t)(getenv("WRAP") ? 0xFFFFFFFFu - 8000000u : 0u));
    printf("after init: pc=$%04X cia_mask=%02X vic_mask=%02X $0314=$%04X $FFFE=$%04X $01=%02X latch=$%04X d012=%02X\n",
        sidp.cpu.pc, sidp.cia_mask, sidp.vic_mask, ram[0x314] | ram[0x315] << 8, ram[0xFFFE] | ram[0xFFFF] << 8,
        ram[1], io[0xC04] | io[0xC05] << 8, io[0x12]);
    uint32_t base = (uint32_t)(getenv("WRAP") ? 0xFFFFFFFFu - 8000000u : 0u);  /* WRAP=1: clock wraps after 2 s */
    for (; now < (uint64_t)(secs * SIDP_UNITS_PER_S); now += 400) {   /* poll every 100 us */
        now_ms = now / 4000.0;
        if (sidp_poll((uint32_t)now + base)) {
            printf("STUCK at %.1f ms (pc=$%04X)\n", now_ms, sidp.cpu.pc);
            break;
        }
    }
    unsigned long calls = sidp.ncalls[0] + sidp.ncalls[1] + sidp.ncalls[2];
    printf("calls %lu (%.1f/s; frame %lu raster %lu cia %lu)\n", calls, calls / secs,
        (unsigned long)sidp.ncalls[0], (unsigned long)sidp.ncalls[1], (unsigned long)sidp.ncalls[2]);
    printf("note starts per voice: %lu %lu %lu\n", gates[0], gates[1], gates[2]);
    printf("writes per reg:");
    for (int r = 0; r <= 24; r++) {
        printf(" %lu", writes[r]);
    }
    printf("\n");
    return 0;
}
