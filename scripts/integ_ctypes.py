"""M8 second consumer: drive all five parser entry points from Python via
ctypes against the release cdylib — no compiler, no C code, an independent
ABI path from the C harness. Asserts expected outcomes (known-good vectors);
prints a deterministic transcript. Exit nonzero on any failure."""

import ctypes
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
DLL = os.path.join(HERE, "..", "target", "release", "picohttpparser_rs.dll")


class Header(ctypes.Structure):
    _fields_ = [("name", ctypes.c_char_p),
                ("name_len", ctypes.c_size_t),
                ("value", ctypes.c_char_p),
                ("value_len", ctypes.c_size_t)]


class Decoder(ctypes.Structure):
    _fields_ = [("bytes_left_in_chunk", ctypes.c_size_t),
                ("consume_trailer", ctypes.c_char),
                ("_hex_count", ctypes.c_char),
                ("_state", ctypes.c_char),
                ("_total_read", ctypes.c_uint64),
                ("_total_overhead", ctypes.c_uint64)]


def fail(msg):
    print("FAIL: " + msg)
    sys.exit(1)


def s(ptr, n):
    # Zero-copy views are NOT NUL-terminated: slice by length, never .value.
    if not ptr:
        assert n == 0, "null pointer with nonzero length"
        return b""
    return ctypes.string_at(ptr, n)


def main():
    lib = ctypes.CDLL(DLL)

    # Argument order mirrors reference/picohttpparser.h: buf, len, method,
    # method_len, path, path_len, minor_version, headers, num_headers,
    # last_len.
    lib.phr_parse_request.argtypes = [ctypes.c_char_p, ctypes.c_size_t,
                                      ctypes.POINTER(ctypes.c_char_p),
                                      ctypes.POINTER(ctypes.c_size_t),
                                      ctypes.POINTER(ctypes.c_char_p),
                                      ctypes.POINTER(ctypes.c_size_t),
                                      ctypes.POINTER(ctypes.c_int),
                                      ctypes.POINTER(Header),
                                      ctypes.POINTER(ctypes.c_size_t),
                                      ctypes.c_size_t]
    lib.phr_parse_request.restype = ctypes.c_int

    req = b"GET /hello?a=1 HTTP/1.1\r\nHost: example.test\r\nX-Foo: bar\r\n\r\n"
    method, method_len = ctypes.c_char_p(), ctypes.c_size_t()
    path, path_len = ctypes.c_char_p(), ctypes.c_size_t()
    ver = ctypes.c_int()
    hdrs = (Header * 16)()
    nhdr = ctypes.c_size_t(16)
    ret = lib.phr_parse_request(req, len(req), ctypes.byref(method),
                                ctypes.byref(method_len), ctypes.byref(path),
                                ctypes.byref(path_len), ctypes.byref(ver),
                                hdrs, ctypes.byref(nhdr), 0)
    if ret != len(req):
        fail("request ret=%d want %d" % (ret, len(req)))
    method_b, path_b = s(method, method_len.value), s(path, path_len.value)
    print("ctypes: request method=%s path=%s version=1.%d headers=%d" % (
        method_b.decode(), path_b.decode(), ver.value, nhdr.value))
    if (method_b, path_b, ver.value, nhdr.value) != (b"GET", b"/hello?a=1", 1, 2):
        fail("request fields")
    if (s(hdrs[0].name, hdrs[0].name_len), s(hdrs[0].value, hdrs[0].value_len)) != (b"Host", b"example.test"):
        fail("request h0")
    if (s(hdrs[1].name, hdrs[1].name_len), s(hdrs[1].value, hdrs[1].value_len)) != (b"X-Foo", b"bar"):
        fail("request h1")

    lib.phr_parse_response.argtypes = [ctypes.c_char_p, ctypes.c_size_t,
                                       ctypes.POINTER(ctypes.c_int),
                                       ctypes.POINTER(ctypes.c_int),
                                       ctypes.POINTER(ctypes.c_char_p),
                                       ctypes.POINTER(ctypes.c_size_t),
                                       ctypes.POINTER(Header),
                                       ctypes.POINTER(ctypes.c_size_t),
                                       ctypes.c_size_t]
    lib.phr_parse_response.restype = ctypes.c_int
    resp = b"HTTP/1.1 404 Not Found\r\nContent-Length: 0\r\n\r\n"
    ver2, status = ctypes.c_int(), ctypes.c_int()
    msg, msg_len = ctypes.c_char_p(), ctypes.c_size_t()
    hdrs2 = (Header * 16)()
    nhdr2 = ctypes.c_size_t(16)
    ret = lib.phr_parse_response(resp, len(resp), ctypes.byref(ver2),
                                 ctypes.byref(status), ctypes.byref(msg),
                                 ctypes.byref(msg_len), hdrs2,
                                 ctypes.byref(nhdr2), 0)
    if ret != len(resp):
        fail("response ret=%d" % ret)
    msg_b = s(msg, msg_len.value)
    print("ctypes: response version=1.%d status=%d reason=%s headers=%d" % (
        ver2.value, status.value, msg_b.decode(), nhdr2.value))
    if (ver2.value, status.value, msg_b, nhdr2.value) != (1, 404, b"Not Found", 1):
        fail("response fields")
    if (s(hdrs2[0].name, hdrs2[0].name_len), s(hdrs2[0].value, hdrs2[0].value_len)) != (b"Content-Length", b"0"):
        fail("response h0")

    lib.phr_parse_headers.argtypes = [ctypes.c_char_p, ctypes.c_size_t,
                                      ctypes.POINTER(Header),
                                      ctypes.POINTER(ctypes.c_size_t),
                                      ctypes.c_size_t]
    lib.phr_parse_headers.restype = ctypes.c_int
    hblock = b"X-A: 1\r\nX-B: two\r\n\r\n"
    hdrs3 = (Header * 16)()
    nhdr3 = ctypes.c_size_t(16)
    ret = lib.phr_parse_headers(hblock, len(hblock), hdrs3, ctypes.byref(nhdr3), 0)
    if ret != len(hblock) or nhdr3.value != 2:
        fail("headers ret=%d count=%d" % (ret, nhdr3.value))
    print("ctypes: headers ret=%d count=%d v1=%s" % (
        ret, nhdr3.value, s(hdrs3[1].value, hdrs3[1].value_len).decode()))

    lib.phr_decode_chunked.argtypes = [ctypes.POINTER(Decoder), ctypes.c_char_p,
                                       ctypes.POINTER(ctypes.c_size_t)]
    lib.phr_decode_chunked.restype = ctypes.c_ssize_t
    lib.phr_decode_chunked_is_in_data.argtypes = [ctypes.POINTER(Decoder)]
    lib.phr_decode_chunked_is_in_data.restype = ctypes.c_int
    dec = Decoder()
    ctypes.memset(ctypes.byref(dec), 0, ctypes.sizeof(dec))
    dec.consume_trailer = b"1"
    cbody = ctypes.create_string_buffer(b"4\r\nWiki\r\n5;ext\r\npedia\r\n0\r\nT: v\r\n\r\nEXTRA")
    n = ctypes.c_size_t(ctypes.sizeof(cbody) - 1)
    ret = lib.phr_decode_chunked(ctypes.byref(dec), cbody, ctypes.byref(n))
    body = bytes(cbody.raw[:n.value])
    in_data = lib.phr_decode_chunked_is_in_data(ctypes.byref(dec))
    print("ctypes: chunked leftover=%d out=%d body=%s in_data=%d" % (
        ret, n.value, body.decode(), in_data))
    if ret != 5 or body != b"Wikipedia":
        fail("chunked ret=%d body=%r" % (ret, body))
    if in_data != 0:
        fail("chunked is_in_data=%d, want 0 after terminal chunk" % in_data)

    print("CTYPES OK")


if __name__ == "__main__":
    main()
