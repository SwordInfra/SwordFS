#!/usr/bin/env python3
"""#424 Redis primitive evidence; run only on a disposable loopback Redis."""

import argparse
import socket
import struct
import uuid


class Redis:
    def __init__(self, port):
        self.sock = socket.create_connection(("127.0.0.1", port), timeout=10)
        self.stream = self.sock.makefile("rb")

    def close(self):
        self.stream.close()
        self.sock.close()

    def call(self, *args):
        parts = [f"*{len(args)}\r\n".encode()]
        for arg in args:
            value = arg if isinstance(arg, bytes) else str(arg).encode()
            parts.extend((b"$" + str(len(value)).encode() + b"\r\n", value, b"\r\n"))
        self.sock.sendall(b"".join(parts))
        return self.reply()

    def reply(self):
        header = self.stream.readline()
        if not header:
            raise ConnectionError("Redis connection closed")
        kind, value = header[:1], header[1:-2]
        if kind == b"+":
            return value.decode()
        if kind == b":":
            return int(value)
        if kind == b"-":
            raise RuntimeError(value.decode())
        if kind == b"$":
            size = int(value)
            if size == -1:
                return None
            body = self.stream.read(size)
            assert self.stream.read(2) == b"\r\n"
            return body
        if kind == b"*":
            size = int(value)
            return None if size == -1 else [self.reply() for _ in range(size)]
        raise ValueError(header)


def record(pos, length, slice_id):
    # Real RedisSliceChunkMetadata wire format (24 bytes).
    return struct.pack("<IQIII", pos, slice_id, length if slice_id else 0, 0, length)


def visible(redis, key):
    eof = int(redis.call("GET", key + ":eof"))
    data = bytearray(eof)
    for raw in redis.call("LRANGE", key + ":list", 0, -1):
        pos, slice_id, size, off, length = struct.unpack("<IQIII", raw)
        assert size == (length if slice_id else 0) and off == 0
        stop = min(eof, pos + length)
        if pos < stop:
            data[pos:stop] = bytes([ord("A") + slice_id - 1 if slice_id else 0]) * (stop - pos)
    return bytes(data).replace(b"\x00", b".").decode()


def seed(redis, key, eof):
    assert redis.call("SET", key + ":eof", eof) == "OK"
    assert redis.call("RPUSH", key + ":list", record(0, eof, 1)) == 1


def run(port):
    a, b = Redis(port), Redis(port)
    root = "swordfs-424-probe:" + uuid.uuid4().hex
    try:
        key = root + ":ordinary"
        seed(a, key, 12)
        a.call("RPUSHX", key + ":list", record(0, 4, 2))
        b.call("RPUSHX", key + ":list", record(4, 4, 3))
        assert visible(a, key) == "BBBBCCCCAAAA"
        b.call("RPUSHX", key + ":list", record(2, 4, 4))
        assert visible(a, key) == "BBDDDDCCAAAA"
        print("PASS: normal disjoint writes coexist; overlaps follow LIST order")

        key = root + ":failed-eof"
        seed(a, key, 8)
        a.call("WATCH", key + ":eof")
        a.call("RPUSHX", key + ":list", record(8, 4, 2))
        b.call("SET", key + ":eof", 10)
        a.call("MULTI")
        a.call("SET", key + ":eof", 12)
        assert a.call("EXEC") is None
        # Even B's partial grow to 10 immediately reveals two bytes of
        # A's uncommitted private record, before any later grow to 12.
        assert visible(a, key) == "AAAAAAAABB"
        b.call("SET", key + ":eof", 12)
        assert visible(a, key) == "AAAAAAAABBBB"
        print("COUNTEREXAMPLE: failed EOF CAS can reveal unacknowledged appended tail after grow")

        key = root + ":failed-zero"
        seed(a, key, 4)
        a.call("WATCH", key + ":eof")
        assert a.call("GET", key + ":eof") == b"4"
        b.call("RPUSHX", key + ":list", record(4, 4, 3))
        b.call("SET", key + ":eof", 8)
        a.call("RPUSHX", key + ":list", record(4, 4, 0))
        a.call("MULTI")
        a.call("SET", key + ":eof", 8)
        assert a.call("EXEC") is None
        assert visible(a, key) == "AAAA...."
        print("COUNTEREXAMPLE: failed stale zero-cover masks committed concurrent extension")

        # JuiceFS-style small WATCH+MULTI: queue private record and inode
        # EOF together, so competing inode changes abort both operations.
        # This validates the normal conflict path, not EXEC runtime errors.
        key = root + ":joint-eof"
        seed(a, key, 8)
        a.call("WATCH", key + ":eof")
        assert a.call("GET", key + ":eof") == b"8"
        b.call("SET", key + ":eof", 10)
        a.call("MULTI")
        a.call("RPUSH", key + ":list", record(8, 4, 2))
        a.call("SET", key + ":eof", 12)
        assert a.call("EXEC") is None
        b.call("SET", key + ":eof", 12)
        assert visible(a, key) == "AAAAAAAA...."
        print("PASS: narrow WATCH append+EOF conflict leaves no unpublished tail")

        key = root + ":joint-cover"
        seed(a, key, 4)
        a.call("WATCH", key + ":eof")
        b.call("RPUSHX", key + ":list", record(4, 4, 3))
        b.call("SET", key + ":eof", 8)
        a.call("MULTI")
        a.call("RPUSH", key + ":list", record(4, 4, 0))
        a.call("SET", key + ":eof", 8)
        assert a.call("EXEC") is None
        assert visible(a, key) == "AAAACCCC"
        print("PASS: narrow WATCH zero-cover conflict preserves remote extension")

        key = root + ":joint-shrink"
        seed(a, key, 8)
        a.call("WATCH", key + ":eof")
        a.call("MULTI")
        a.call("RPUSH", key + ":list", record(4, 4, 0))
        a.call("SET", key + ":eof", 4)
        assert a.call("EXEC") == [2, "OK"]
        b.call("SET", key + ":eof", 8)
        assert visible(a, key) == "AAAA...."
        print("PASS: narrow normal shrink+cover preserves zeroes after grow")

        key = root + ":aba-after"
        seed(a, key, 8)
        b.call("SET", key + ":eof", 4)
        b.call("RPUSHX", key + ":list", record(4, 4, 0))
        b.call("SET", key + ":eof", 8)
        a.call("RPUSHX", key + ":list", record(6, 2, 2))
        assert visible(a, key) == "AAAA..BB"
        print("OBSERVATION: post-shrink/grow append may be legitimate later write")

        key = root + ":aba-before"
        seed(a, key, 8)
        a.call("RPUSHX", key + ":list", record(6, 2, 2))
        b.call("SET", key + ":eof", 4)
        b.call("RPUSHX", key + ":list", record(4, 4, 0))
        b.call("SET", key + ":eof", 8)
        assert visible(a, key) == "AAAA...."
        print("PASS: pre-shrink committed tail stays erased after grow")
    finally:
        a.close()
        b.close()


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--port", type=int, required=True)
    parser.add_argument("--confirm-disposable", action="store_true")
    args = parser.parse_args()
    if not args.confirm_disposable:
        parser.error("must confirm disposable loopback Redis instance")
    run(args.port)
