#!/usr/bin/env python3
"""Reproducible Redis 7 transaction/representation evidence for SwordFS #430.

Runs against an EXCLUSIVE, disposable Redis instance; FLUSHDB is destructive.
This is a protocol experiment, not a substitute for the future C++ TDD suite.
"""

import argparse
import math
import socket
import statistics
import threading
import time


class Redis:
    def __init__(self, host, port):
        self.socket = socket.create_connection((host, port), timeout=20)
        self.reader = self.socket.makefile("rb")

    def close(self):
        self.reader.close()
        self.socket.close()

    def send(self, *args):
        parts = [f"*{len(args)}\r\n".encode()]
        for arg in args:
            payload = str(arg).encode()
            parts.extend((f"${len(payload)}\r\n".encode(), payload, b"\r\n"))
        self.socket.sendall(b"".join(parts))

    def reply(self):
        header = self.reader.readline()
        if not header:
            raise ConnectionError("Redis connection closed")
        prefix, value = header[:1], header[1:-2]
        if prefix == b"+":
            return value.decode()
        if prefix == b":":
            return int(value)
        if prefix == b"-":
            raise RuntimeError(value.decode())
        if prefix == b"$":
            size = int(value)
            if size == -1:
                return None
            return self.reader.read(size + 2)[:-2]
        if prefix == b"*":
            count = int(value)
            return None if count == -1 else [self.reply() for _ in range(count)]
        raise ValueError(f"Unexpected RESP marker: {prefix!r}")

    def call(self, *args):
        self.send(*args)
        return self.reply()

    def pipeline(self, commands):
        # Send in batches so the server never needs to buffer all replies.
        for start in range(0, len(commands), 512):
            batch = commands[start : start + 512]
            for cmd in batch:
                self.send(*cmd)
            for _ in batch:
                self.reply()


def check(condition, message):
    if not condition:
        raise AssertionError(message)
    print(f"PASS {message}")


def watched_attempt(conn, key, lease, field, action, value=None):
    conn.call("WATCH", key, lease)
    assert conn.call("PTTL", lease) > 0
    conn.call("HGETALL", key)
    conn.call("MULTI")
    conn.call(action, key, field, *([value] if value is not None else []))


def protocol(host, port):
    a, b, c = (Redis(host, port) for _ in range(3))
    try:
        a.call("FLUSHDB")
        a.call("SET", "lease", "active", "PX", 600000)

        watched_attempt(a, "lock", "lease", "owner", "HSET", "EX")
        watched_attempt(b, "lock", "lease", "owner", "HSET", "EX")
        check(b.call("EXEC") == [1], "identical retry commits before delayed original")
        check(a.call("EXEC") is None, "delayed original aborts after real HSET retry")
        check(a.call("HGET", "lock", "owner") == b"EX", "retry retains one canonical owner state")

        # Simulate a TCP connection that loses the EXEC acknowledgement.
        # The caller cannot distinguish committed HSET from a lost command,
        # so it must replay the identical owner-state mutation under WATCH.
        a.call("DEL", "lock")
        uncertain = Redis(host, port)
        try:
            watched_attempt(uncertain, "lock", "lease", "owner", "HSET", "EX")
            uncertain.send("EXEC")  # do not read or trust its reply
            uncertain.socket.shutdown(socket.SHUT_RD)
        finally:
            uncertain.close()
        watched_attempt(b, "lock", "lease", "owner", "HSET", "EX")
        check(b.call("EXEC") in ([0], [1]),
              "same-request retry succeeds without relying on lost EXEC reply")
        check(a.call("HGET", "lock", "owner") == b"EX" and a.call("HLEN", "lock") == 1,
              "lost-reply replay converges to one authoritative owner value")

        a.call("WATCH", "lock")
        a.call("HGETALL", "lock")
        a.call("MULTI")
        a.call("PING")
        check(b.call("HSET", "lock", "owner", "EX") == 0, "same-value HSET updates no logical owner state")
        check(a.call("EXEC") is None, "same-value HSET still invalidates older WATCH")

        a.call("DEL", "lock")
        watched_attempt(a, "lock", "lease", "owner", "HSET", "EX")
        watched_attempt(b, "lock", "lease", "owner", "HDEL")
        check(b.call("EXEC") == [0], "absent-field HDEL does not modify the Hash")
        check(a.call("EXEC") == [1], "counterexample: old EXEC can create ghost lock after HDEL")

        a.call("DEL", "lock")
        watched_attempt(a, "lock", "lease", "owner", "HSET", "EX")
        b.call("WATCH", "lock", "lease")
        b.call("HGETALL", "lock")
        b.call("MULTI")
        # The ephemeral marker is absent before and after this EXEC. Its
        # creation/deletion still invalidates earlier WATCHes on the Hash.
        b.call("HSET", "lock", "\u0000fence", "1")
        b.call("HDEL", "lock", "owner")
        b.call("HDEL", "lock", "\u0000fence")
        check(b.call("EXEC") == [1, 0, 1], "transient fence + empty unlock commits without residue")
        check(a.call("EXEC") is None, "transient fence prevents late ghost grant")
        check(a.call("EXISTS", "lock") == 0, "normal no-op unlock retains no tombstone")

        watched_attempt(a, "lock", "lease", "owner", "HSET", "SH")
        b.call("PEXPIRE", "lease", 600000, "XX")
        check(a.call("EXEC") is None, "requester lease renewal invalidates grant")

        watched_attempt(a, "lock", "lease", "owner", "HSET", "SH")
        b.call("DEL", "lease")
        check(a.call("EXEC") is None, "requester lease revocation invalidates grant")

        a.call("SET", "lease", "active", "PX", 50)
        watched_attempt(a, "lock", "lease", "owner", "HSET", "SH")
        time.sleep(0.08)
        check(a.call("EXEC") is None, "requester lease expiry invalidates grant")

        a.call("SET", "lease", "active", "PX", 600000)
        a.call("SET", "holder-lease", "active", "PX", 600000)
        a.call("HSET", "lock", "other", "SH")
        a.call("WATCH", "lock", "lease")
        a.call("HGETALL", "lock")
        a.call("WATCH", "holder-lease")
        check(a.call("PTTL", "holder-lease") > 0, "holder lease watched before reading TTL")
        a.call("MULTI")
        a.call("HSET", "lock", "owner", "SH")
        b.call("PEXPIRE", "holder-lease", 600000, "XX")
        check(a.call("EXEC") is None, "holder lease renewal invalidates concurrent shared grant")

        batch_script = 'local r={} for i=1,#KEYS do r[i]=redis.call("PTTL",KEYS[i]) end return r'
        a.call("WATCH", "lease", "holder-lease")
        ttls = a.call("EVAL", batch_script, 2, "lease", "holder-lease")
        check(len(ttls) == 2 and all(ttl > 0 for ttl in ttls),
              "batched readonly PTTL preserves expiry evidence")
        a.call("MULTI")
        a.call("PING")
        b.call("PEXPIRE", "holder-lease", 600000, "XX")
        check(a.call("EXEC") is None, "batched PTTL retains WATCH on holder renewal")

        a.call("WATCH", "lock", "lease", "holder-lease")
        a.call("HGETALL", "lock")
        a.call("MULTI")
        a.call("PING")  # read-only GETLK validation transaction
        c.call("HSET", "lock", "other", "EX")
        check(a.call("EXEC") is None, "read-only GETLK snapshot detects a changed blocker")

        # Two non-compatible grants both observe an empty Hash. At most one
        # can commit without re-reading the other holder's authoritative lock.
        a.call("DEL", "lock")
        a.call("SET", "other-lease", "active", "PX", 600000)
        watched_attempt(a, "lock", "lease", "owner", "HSET", "EX:0:EOF")
        watched_attempt(b, "lock", "other-lease", "other", "HSET", "EX:0:EOF")
        check(b.call("EXEC") == [1], "first conflicting owner commits")
        check(a.call("EXEC") is None, "simultaneous conflicting owner must retry and recheck")

        a.call("DEL", "lock")
        watched_attempt(a, "lock", "lease", "owner", "HSET", "SH:0:EOF")
        watched_attempt(b, "lock", "other-lease", "other", "HSET", "SH:0:EOF")
        check(b.call("EXEC") == [1] and a.call("EXEC") is None,
              "simultaneous shared grants serialize through WATCH")
        watched_attempt(a, "lock", "lease", "owner", "HSET", "SH:0:EOF")
        check(a.call("EXEC") == [1] and a.call("HLEN", "lock") == 2,
              "shared retry allows both compatible owner records")

        # Stale GC must never remove a field that changed after its snapshot.
        a.call("WATCH", "lock")
        old_state = a.call("HGET", "lock", "other")
        check(old_state == b"SH:0:EOF", "cleanup has observed an exact old field")
        a.call("MULTI")
        a.call("HDEL", "lock", "other")
        b.call("HSET", "lock", "other", "EX:0:EOF")
        check(a.call("EXEC") is None and a.call("HGET", "lock", "other") == b"EX:0:EOF",
              "stale cleanup cannot delete newer owner state")

        a.call("DEL", "lock")
        for inode in range(128):
            a.call("HSET", f"lock:{inode}", "owner", "SH")
            a.call("HDEL", f"lock:{inode}", "owner")
        check(a.call("DBSIZE") == 3 and a.call("EXISTS", "lock") == 0,
              "healthy locks/unlocks leave no per-inode lock keys")

        a.call("DEL", "lock")
        a.pipeline([("HSET", "lock", f"stale:{i}", "SH") for i in range(1124)])
        a.call("HSET", "lock", "live-owner", "EX")
        check(a.call("HLEN", "lock") > 1024,
              "overlarge Hash is detected before authorization reads")
        for _ in range(20):
            if a.call("HLEN", "lock") <= 1024:
                break
            a.call("WATCH", "lock")
            _cursor, fields = a.call("HSCAN", "lock", "0", "COUNT", "128")
            stale = [fields[index] for index in range(0, len(fields), 2)
                     if fields[index].startswith(b"stale:")][:128]
            a.call("MULTI")
            for field in stale:
                a.call("HDEL", "lock", field.decode())
            check(a.call("EXEC") is not None, "bounded stale owner pass commits atomically")
        check(a.call("HLEN", "lock") <= 1024 and a.call("HGET", "lock", "live-owner") == b"EX",
              "bounded cleanup makes progress without deleting live lock")
    finally:
        for conn in (a, b, c):
            conn.close()


def percentile(values, percentile_value):
    return sorted(values)[min(len(values) - 1, math.ceil(percentile_value * len(values)) - 1)]


def benchmark(host, port):
    conn = Redis(host, port)
    try:
        print("\nlayout,stale_owners,redis_keys,bytes_used,scan_median_ms,scan_p99_ms,payload_bytes")
        for count in (1, 32, 1000, 100000):
            for layout in ("flat", "generation-grouped"):
                conn.call("FLUSHDB")
                commands = []
                if layout == "flat":
                    commands = [("HSET", "lock", f"g:{i}:owner", "SH:0:EOF") for i in range(count)]
                else:
                    for i in range(count):
                        commands.extend((("SADD", "lock:gens", i), ("HSET", f"lock:g:{i}", "owner", "SH:0:EOF")))
                conn.pipeline(commands)
                samples = []
                payload = 0
                for _ in range(10):
                    t0 = time.perf_counter()
                    if layout == "flat":
                        result = conn.call("HGETALL", "lock")
                        payload = sum(len(s) for s in result)
                    else:
                        generations = conn.call("SMEMBERS", "lock:gens")
                        # Represent complete discovery, not optimistic partial scanning.
                        keys = [("HGETALL", f"lock:g:{i.decode()}") for i in generations]
                        payload = sum(len(s) for s in generations)
                        for start in range(0, len(keys), 512):
                            batch = keys[start : start + 512]
                            for cmd in batch:
                                conn.send(*cmd)
                            for _ in batch:
                                for element in conn.reply():
                                    payload += len(element)
                    samples.append((time.perf_counter() - t0) * 1000)
                redis_keys = conn.call("DBSIZE")
                used = conn.call("INFO", "memory")
                memory_used = int(next(line.split(":")[1] for line in used.decode().splitlines() if line.startswith("used_memory:")))
                print(f"{layout},{count},{redis_keys},{memory_used},{statistics.median(samples):.2f},"
                      f"{percentile(samples, 0.99):.2f},{payload}", flush=True)
    finally:
        conn.close()


def contention(host, port):
    """Small local Redis command-level proxy for grant/GETLK read amplification."""
    writer, interferer = Redis(host, port), Redis(host, port)
    try:
        print("\nfields,renewal_or_writer_abort_rate,attempt_p99_ms,readonly_fanout_p99_ms")
        for count in (1, 32, 1000):
            writer.call("FLUSHDB")
            writer.call("SET", "requester", "active", "PX", 600000)
            writer.call("SET", "holder", "active", "PX", 600000)
            intervals = "".join(f"{index * 4096}-{index * 4096 + 2047}," for index in range(128))
            writer.pipeline([("HSET", "lock", f"generation:1:owner:{i}", intervals) for i in range(count)])
            times, aborts = [], 0
            for trial in range(100):
                start = time.perf_counter()
                writer.call("WATCH", "lock", "requester")
                writer.call("HGETALL", "lock")
                writer.call("WATCH", "holder")
                writer.call("PTTL", "holder")
                writer.call("PTTL", "requester")
                writer.call("MULTI")
                writer.call("HSET", "lock", "request", intervals)
                if trial % 4 == 0:
                    interferer.call("PEXPIRE", "holder", 600000, "XX")
                if trial % 10 == 0:
                    interferer.call("HSET", "lock", "interferer", trial)
                if writer.call("EXEC") is None:
                    aborts += 1
                times.append((time.perf_counter() - start) * 1000)

            # Concurrent read-only watched snapshots without writers.
            readonly_samples, errors = [], []

            def reader_work():
                conn = Redis(host, port)
                try:
                    for _ in range(50):
                        start = time.perf_counter()
                        conn.call("WATCH", "lock", "requester")
                        conn.call("HGETALL", "lock")
                        conn.call("MULTI")
                        conn.call("PING")
                        if conn.call("EXEC") != ["PONG"]:
                            raise AssertionError("read-only snapshot aborted without writers")
                        readonly_samples.append((time.perf_counter() - start) * 1000)
                except Exception as error:
                    errors.append(error)
                finally:
                    conn.close()

            threads = [threading.Thread(target=reader_work) for _ in range(4)]
            for thread in threads:
                thread.start()
            for thread in threads:
                thread.join()
            if errors:
                raise errors[0]
            print(f"{count},{aborts / 100:.2f},{percentile(times, 0.99):.2f},"
                  f"{percentile(readonly_samples, 0.99):.2f}", flush=True)
    finally:
        writer.close()
        interferer.close()


def lease_batch(host, port):
    conn = Redis(host, port)
    script = 'local r={} for i=1,#KEYS do r[i]=redis.call("PTTL",KEYS[i]) end return r'
    try:
        print("\nholders,per-key-watch-pttl-median-ms,batch-watch-eval-median-ms")
        for count in (32, 128, 512, 1024):
            conn.call("FLUSHDB")
            conn.pipeline([("SET", f"lease:{i}", "active", "PX", 600000) for i in range(count)] +
                          [("HSET", "lock", f"{i+1}:42", "P1|R,0,100") for i in range(count)])
            keys = [f"lease:{i}" for i in range(count)]
            separate, batched = [], []
            for _ in range(20):
                started = time.perf_counter()
                conn.call("WATCH", "lock")
                conn.call("HLEN", "lock")
                conn.call("HGETALL", "lock")
                for key in keys:
                    conn.call("WATCH", key)
                    conn.call("PTTL", key)
                conn.call("MULTI")
                conn.call("PING")
                assert conn.call("EXEC") == ["PONG"]
                separate.append((time.perf_counter() - started) * 1000)

                started = time.perf_counter()
                conn.call("WATCH", "lock")
                conn.call("HLEN", "lock")
                conn.call("HGETALL", "lock")
                conn.call("WATCH", *keys)
                assert len(conn.call("EVAL", script, count, *keys)) == count
                conn.call("MULTI")
                conn.call("PING")
                assert conn.call("EXEC") == ["PONG"]
                batched.append((time.perf_counter() - started) * 1000)
            print(f"{count},{statistics.median(separate):.2f},{statistics.median(batched):.2f}", flush=True)
    finally:
        conn.close()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--host", default="127.0.0.1")
    parser.add_argument("--port", type=int, default=16379)
    parser.add_argument("--benchmark", action="store_true")
    parser.add_argument("--contention", action="store_true")
    parser.add_argument("--lease-batch", action="store_true")
    args = parser.parse_args()
    protocol(args.host, args.port)
    if args.benchmark:
        benchmark(args.host, args.port)
    if args.contention:
        contention(args.host, args.port)
    if args.lease_batch:
        lease_batch(args.host, args.port)


if __name__ == "__main__":
    main()
