// SPDX-License-Identifier: MIT
// Copyright (c) 2025-2026 Robert S. (aka Robmaster)
// qoaenc: 16-bit PCM (interleaved, little endian) on stdin -> a .qoa file on
// stdout, with the reference encoder qoa.h (Dominic Szablewski, MIT).
// mkqoa.py builds and runs it; by hand:
//
//   cc -O2 -o qoaenc qoaenc.c -lm
//   ffmpeg -i song.flac -f s16le -ac 2 -ar 44100 - | ./qoaenc 2 44100 > song.qoa

#define QOA_IMPLEMENTATION
#define QOA_NO_STDIO
#include "qoa.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int main(int argc, char **argv) {
    if (argc != 3) {
        fprintf(stderr, "usage: qoaenc CHANNELS RATE < pcm_s16le > out.qoa\n");
        return 2;
    }
    qoa_desc q;
    memset(&q, 0, sizeof(q));
    q.channels = (unsigned int)atoi(argv[1]);
    q.samplerate = (unsigned int)atoi(argv[2]);
    if (q.channels < 1 || q.channels > QOA_MAX_CHANNELS || q.samplerate < 1 || q.samplerate > 0xffffff) {
        fprintf(stderr, "qoaenc: bad channels or rate\n");
        return 2;
    }
    size_t cap = 1 << 22, len = 0, r;
    unsigned char *pcm = malloc(cap);
    while (pcm && (r = fread(pcm + len, 1, cap - len, stdin)) > 0) {
        len += r;
        if (len == cap) {
            pcm = realloc(pcm, cap *= 2);
        }
    }
    if (!pcm) {
        fprintf(stderr, "qoaenc: out of memory\n");
        return 1;
    }
    q.samples = (unsigned int)(len / 2 / q.channels);
    if (q.samples == 0) {
        fprintf(stderr, "qoaenc: no samples\n");
        return 1;
    }
    unsigned int out_len;
    void *out = qoa_encode((const short *)pcm, &q, &out_len);
    if (!out || fwrite(out, 1, out_len, stdout) != out_len) {
        fprintf(stderr, "qoaenc: encoding failed\n");
        return 1;
    }
    return 0;
}
