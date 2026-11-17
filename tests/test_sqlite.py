# SPDX-License-Identifier: MIT
# Copyright (c) 2025-2026 Robert S. (aka Robmaster)
# vx: sd   (run_tests.sh mounts a RAM /sd with the card's semantics first)
# sqlite3: the module (deps/micropython-sqlite3) on the VIXEN OS layer
# (vixen_sqlite.c): memory on the gc heap, databases in RAM, written back
# whole through "-vxtmp" + rename on every commit.
import gc
import os
import sqlite3

fails = 0


def check(name, ok):
    global fails
    print(("ok   " if ok else "FAIL ") + name)
    if not ok:
        fails += 1


def raises(exc, fn, *args):
    try:
        fn(*args)
    except exc:
        return True
    except Exception as e:
        print("     got", type(e).__name__, e)
    return False


def exists(p):
    try:
        os.stat(p)
        return True
    except OSError:
        return False


def read(p):
    with open(p, "rb") as f:
        return f.read()


check("version", sqlite3.sqlite_version == "3.54.1")
check("threadsafety", sqlite3.threadsafety == 0)

# ---- in memory: types, parameters, rows, errors ------------------------
db = sqlite3.connect(":memory:")
db.execute("CREATE TABLE t (i INTEGER, r REAL, s TEXT, b BLOB, n)")
big = 2**40 + 7
db.execute("INSERT INTO t VALUES (?, ?, ?, ?, ?)", (big, 1.5, "Grüße", b"\x00\xff", None))
db.execute("INSERT INTO t VALUES (:i, :r, :s, :b, :n)",
           {"i": -(2**62), "r": -0.25, "s": "", "b": b"", "n": 3})
rows = db.execute("SELECT * FROM t ORDER BY r DESC").fetchall()
check("64-bit int round trip", rows[0][0] == big and rows[1][0] == -(2**62))
check("real", rows[0][1] == 1.5 and rows[1][1] == -0.25)
check("text utf-8", rows[0][2] == "Grüße")
check("blob", rows[0][3] == b"\x00\xff" and isinstance(rows[0][3], bytes))
check("null", rows[0][4] is None)
check("sum of int64", db.execute("SELECT SUM(i) FROM t").fetchone()[0] == big - 2**62)
row = db.execute("SELECT i AS a, s AS b FROM t LIMIT 1").fetchone()
check("row by name", row["a"] == row[0] and row.keys() == ["a", "b"])
db.execute("CREATE TABLE u (k TEXT PRIMARY KEY)")
db.execute("INSERT INTO u VALUES ('x')")
check("IntegrityError", raises(sqlite3.IntegrityError, db.execute, "INSERT INTO u VALUES ('x')"))
check("OperationalError", raises(sqlite3.OperationalError, db.execute, "SELEKT 1"))
check("date functions", db.execute("SELECT date('now')").fetchone()[0].startswith("2000-01-01"))
check("random()", db.execute("SELECT random() != random()").fetchone()[0] == 1)
db.close()

# ---- on the card ---------------------------------------------------------
os.mkdir("/sd/db")
P = "/sd/db/test.db"
db = sqlite3.connect(P)
check("connect creates the file", exists(P))
db.execute("CREATE TABLE kv (k INTEGER PRIMARY KEY, v TEXT)")
db.executemany("INSERT INTO kv (v) VALUES (?)", [("row %d" % i,) for i in range(2000)])
db.commit()
check("file is a database", read(P)[:16] == b"SQLite format 3\x00")
check("no -vxtmp left", not exists(P + "-vxtmp"))
check("no journal on the card", not exists(P + "-journal"))
size1 = os.stat(P)[6]

# uncommitted work never reaches the card
db.execute("DELETE FROM kv")
check("commit pending", db.execute("SELECT COUNT(*) FROM kv").fetchone()[0] == 0)
check("card unchanged", os.stat(P)[6] == size1)
db.rollback()
check("rollback", db.execute("SELECT COUNT(*) FROM kv").fetchone()[0] == 2000)

# two connections to one file share the data and lock each other out
db2 = sqlite3.connect(P)
db.execute("INSERT INTO kv (v) VALUES ('new')")   # write transaction open
check("reader blocked by writer", raises(sqlite3.OperationalError, db2.execute, "SELECT COUNT(*) FROM kv"))
db.commit()
check("other connection sees commit", db2.execute("SELECT COUNT(*) FROM kv").fetchone()[0] == 2001)
db2.close()

# closing with live statements (an unfinished SELECT) still closes the file
cur = db.cursor()
cur.execute("SELECT * FROM kv")
cur.fetchone()
db.close()
check("cursor dead after close", raises(sqlite3.ProgrammingError, cur.fetchone))

db = sqlite3.connect(P)
check("reopen reads back", db.execute("SELECT v FROM kv WHERE k = 1").fetchone()[0] == "row 0")
check("integrity_check", db.execute("PRAGMA integrity_check").fetchone()[0] == "ok")
db.close()

# power cut between remove and rename: only the complete -vxtmp is left
good = read(P)
os.rename(P, P + "-vxtmp")
db = sqlite3.connect(P)
check("recovers -vxtmp", db.execute("SELECT COUNT(*) FROM kv").fetchone()[0] == 2001)
db.close()
check("-vxtmp renamed back", exists(P) and not exists(P + "-vxtmp"))

# power cut while writing -vxtmp: the database is the old one
with open(P + "-vxtmp", "wb") as f:
    f.write(b"half a database")
db = sqlite3.connect(P)
check("ignores partial -vxtmp", db.execute("SELECT COUNT(*) FROM kv").fetchone()[0] == 2001)
db.close()
check("partial -vxtmp removed", not exists(P + "-vxtmp"))

# relative names are fixed at connect(), a later chdir does not move them
os.chdir("/sd/db")
cwd = os.getcwd()   # the RAM /sd ignores chdir within it: "/sd"
db = sqlite3.connect("rel.db")
os.chdir("/")
db.execute("CREATE TABLE x (a)")
db.execute("INSERT INTO x VALUES (1)")
db.commit()
db.close()
check("relative path", exists(cwd.rstrip("/") + "/rel.db") and not exists("/rel.db"))

check("bad directory", raises(sqlite3.OperationalError, sqlite3.connect, "/sd/nodir/x.db"))

# VACUUM rewrites and truncates the whole file
db = sqlite3.connect(P)
db.execute("VACUUM")
check("vacuum", db.execute("SELECT COUNT(*) FROM kv").fetchone()[0] == 2001)
db.close()

# connections that are opened and closed give their memory back
gc.collect()
free0 = gc.mem_free()
for i in range(20):
    db = sqlite3.connect(P)
    c = db.cursor()
    c.execute("SELECT * FROM kv")
    c.fetchmany(5)
    db.execute("SELECT * FROM kv").fetchone()
    db.close()
gc.collect()
leak = free0 - gc.mem_free()
print("     memory after 20 connections: %d bytes" % leak)
check("no leak", leak < 16384)

print("TEST OK" if fails == 0 else "TEST FAILED (%d)" % fails)
