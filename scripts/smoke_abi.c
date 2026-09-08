/* M1 ABI smoke test: links against the Rust staticlib and calls all five
 * exported symbols. Stubs return -1 (0 for is_in_data); parsing arrives M2+.
 * Build: gcc -Ireference scripts/smoke_abi.c target/debug/picohttpparser_rs.lib
 * Exit 0 = linked, called, stub values as expected.
 */
#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "picohttpparser.h"

int main(void)
{
    const char *method, *path, *msg;
    size_t method_len, path_len, msg_len, num_headers;
    int minor_version, status;
    struct phr_header headers[16];
    struct phr_chunked_decoder dec;
    char buf[64];
    size_t bufsz;
    const char req[] = "GET / HTTP/1.1\r\nHost: x\r\n\r\n";
    int r1, r2, r3, r4;
    ssize_t r5;

    memset(&dec, 0, sizeof(dec));
    num_headers = 16;
    r1 = phr_parse_request(req, sizeof(req) - 1, &method, &method_len, &path, &path_len, &minor_version,
                           headers, &num_headers, 0);
    num_headers = 16;
    r2 = phr_parse_response(req, sizeof(req) - 1, &minor_version, &status, &msg, &msg_len, headers,
                            &num_headers, 0);
    num_headers = 16;
    r3 = phr_parse_headers(req, sizeof(req) - 1, headers, &num_headers, 0);
    memcpy(buf, "5\r\nhello\r\n0\r\n\r\n", 15);
    bufsz = 15;
    r4 = phr_decode_chunked_is_in_data(&dec);
    r5 = phr_decode_chunked(&dec, buf, &bufsz);

    printf("request=%d response=%d headers=%d chunked=%d is_in_data=%d\n", r1, r2, r3, (int)r5, r4);
    assert(r1 == -1 && r2 == -1 && r3 == -1 && r5 == -1 && r4 == 0);
    puts("SMOKE-ABI: LINK+CALL OK (M1 stubs)");
    return 0;
}
