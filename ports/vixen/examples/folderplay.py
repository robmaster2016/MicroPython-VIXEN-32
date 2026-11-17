# SPDX-License-Identifier: MIT
# Copyright (c) 2025-2026 Robert S. (aka Robmaster)
# folderplay.py -- plays the music folder: copy WAV, QOA and SID files to
# /sd/vixen-32/music/, then
#
#   import folderplay                    # every file, in name order, with pictures
#   folderplay.play()                    # once more (a second import does nothing)
#   folderplay.play(shuffle=True, repeat=True)
#   folderplay.play("/sd/other", sid_seconds=120)
#
# Keys while it plays (keyboard or SSH console):
#
#   SPACE   pause / play on
#   n       next track
#   p       previous track (on the first: it starts again)
#   RUN/STOP (Ctrl-C) ends it, the music stops with it.
#
# The pictures are sound_demo's (rosette for WAV/QOA, harmonograph for SID;
# LVGL build), without LVGL the files just play. A SID tune never ends by
# itself, so it plays sid_seconds. Files the sound module does not take are
# skipped.

import os
import sys

sys.modules["sound_demo_quiet"] = True      # the functions, not the show
import sound_demo                           # noqa: E402

MUSIC = "/sd/vixen-32/music"
KINDS = (".wav", ".qoa", ".sid")


def files(folder=MUSIC):
    """The playable files in folder, sorted by name (case ignored)."""
    try:
        names = os.listdir(folder)
    except OSError:
        return []
    except UnicodeError:
        # a firmware without Latin-1 names: list as bytes, skip what is not UTF-8
        names = []
        for b in os.listdir(folder.encode()):
            try:
                names.append(b.decode())
            except UnicodeError:
                print("skipped (name is not UTF-8):", b)
    names = [n for n in names if n.lower().endswith(KINDS) and not n.startswith(".")]
    names.sort(key=lambda n: n.lower())
    return [folder + "/" + n for n in names]


def _shuffle(lst):
    import random
    for i in range(len(lst) - 1, 0, -1):
        j = random.getrandbits(16) % (i + 1)
        lst[i], lst[j] = lst[j], lst[i]


def play(folder=MUSIC, shuffle=False, repeat=False, sid_seconds=None, fps=25):
    """Play every WAV, QOA and SID file in folder. sid_seconds: how long a SID
    tune plays (default sound_demo.SID_SECONDS)."""
    while True:
        todo = files(folder)
        if not todo:
            print("no music in", folder, "- copy .wav, .qoa or .sid files there")
            return
        if shuffle:
            _shuffle(todo)
        print("%d file(s) in %s" % (len(todo), folder))
        for i, path in enumerate(todo):
            print("%2d. %s" % (i + 1, path.rsplit("/", 1)[-1]))
        print("SPACE pause/play, n next, p previous, RUN/STOP ends")
        sound_demo.art(*todo, sid_seconds=sid_seconds, fps=fps)
        if not repeat:
            return


play()
