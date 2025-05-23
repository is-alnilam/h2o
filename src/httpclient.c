/*
 * Copyright (c) 2014-2019 DeNA Co., Ltd., Kazuho Oku, Fastly, Frederik
 *                         Deweerdt, Justin Zhu, Ichito Nagata, Grant Zhang,
 *                         Baodong Chen
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to
 * deal in the Software without restriction, including without limitation the
 * rights to use, copy, modify, merge, publish, distribute, sublicense, and/or
 * sell copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS
 * IN THE SOFTWARE.
 */
#include <errno.h>
#ifdef LIBC_HAS_BACKTRACE
#include <execinfo.h>
#endif
#include <getopt.h>
#include <netinet/in.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <openssl/opensslv.h>
#if !defined(LIBRESSL_VERSION_NUMBER) && OPENSSL_VERSION_NUMBER >= 0x30000000L
#include <openssl/provider.h>
#define LOAD_OPENSSL_PROVIDER 1
#endif
#include "picotls.h"
#include "picotls/openssl.h"
#include "quicly.h"
#include "h2o/hostinfo.h"
#include "h2o/httpclient.h"
#include "h2o/serverutil.h"

#define DEFAULT_IO_TIMEOUT 5000

static quicly_error_t save_http3_token_cb(quicly_save_resumption_token_t *self, quicly_conn_t *conn, ptls_iovec_t token);
static quicly_save_resumption_token_t save_http3_token = {save_http3_token_cb};
static int save_http3_ticket_cb(ptls_save_ticket_t *self, ptls_t *tls, ptls_iovec_t src);
static void add_header(h2o_iovec_t name, h2o_iovec_t value);
static ptls_save_ticket_t save_http3_ticket = {save_http3_ticket_cb};
static h2o_httpclient_connection_pool_t *connpool;
struct {
    const char *target; /* either URL or host:port when the method is CONNECT or CONNECT_UDP */
    const char *method;
    struct {
        h2o_iovec_t name;
        h2o_iovec_t value;
    } headers[256];
    size_t num_headers;
    size_t body_size;
    h2o_url_t *connect_to; /* when non-NULL, this property specifies the layer-4 address where the client should connect to */
} req = {NULL, "GET"};
static unsigned cnt_left = 1, concurrency = 1;
static int chunk_size = 10;
static h2o_iovec_t iov_filler;
static struct {
    h2o_socket_t *sock;
    int closed;
} std_in;
static int io_interval = 0, req_interval = 0;
static uint64_t io_timeout = DEFAULT_IO_TIMEOUT;
static int ssl_verify_none = 0;
static int exit_failure_on_http_errors = 0;
static int program_exit_status = EXIT_SUCCESS;
static h2o_socket_t *udp_sock = NULL;
static const char *upgrade_token = NULL;
static h2o_httpclient_forward_datagram_cb udp_write;
static struct sockaddr_in udp_sock_remote_addr;
static const ptls_key_exchange_algorithm_t *h3_key_exchanges[] = {
#if PTLS_OPENSSL_HAVE_X25519
    &ptls_openssl_x25519,
#endif
    &ptls_openssl_secp256r1, NULL};
static h2o_http3client_ctx_t h3ctx = {
    .tls =
        {
            .random_bytes = ptls_openssl_random_bytes,
            .get_time = &ptls_get_time,
            .key_exchanges = h3_key_exchanges,
            .cipher_suites = ptls_openssl_cipher_suites,
            .save_ticket = &save_http3_ticket,
        },
    .max_frame_payload_size = 16384,
};
static quicly_cid_plaintext_t h3_next_cid;
static const char *session_file = NULL;
static const char *progname; /* refers to argv[0] */

static h2o_httpclient_head_cb on_connect(h2o_httpclient_t *client, const char *errstr, h2o_iovec_t *method, h2o_url_t *url,
                                         const h2o_header_t **headers, size_t *num_headers, h2o_iovec_t *body,
                                         h2o_httpclient_proceed_req_cb *proceed_req_cb, h2o_httpclient_properties_t *props,
                                         h2o_url_t *origin);
static h2o_httpclient_body_cb on_head(h2o_httpclient_t *client, const char *errstr, h2o_httpclient_on_head_t *args);

static void load_session(const char *server_name, ptls_iovec_t *tls_session, quicly_transport_parameters_t *quic_tp,
                         ptls_iovec_t *quic_address_token)
{
    uint8_t buf[2048];
    size_t len;
    int ret;

    *tls_session = ptls_iovec_init(NULL, 0);
    *quic_address_token = ptls_iovec_init(NULL, 0);
    *quic_tp = (quicly_transport_parameters_t){};

    { /* read file */
        FILE *fp;
        if (session_file == NULL || (fp = fopen(session_file, "r")) == NULL)
            return;
        if ((len = fread(buf, 1, sizeof(buf), fp)) == 0) {
            fclose(fp);
            return;
        }
        fclose(fp);
    }

    const uint8_t *src = buf, *end = buf + len;
    ptls_decode_open_block(src, end, -1, {
        if (end - src != strlen(server_name) || memcmp(src, server_name, end - src) != 0) {
            ret = PTLS_ALERT_USER_CANCELED; /* any value would do, as the return value is compressed to bool */
            goto Exit;
        }
        src = end;
    });
    ptls_decode_open_block(src, end, -1, {
        if (src != end) {
            *tls_session = ptls_iovec_init(h2o_mem_alloc(end - src), end - src);
            memcpy(tls_session->base, src, end - src);
            src = end;
        }
    });
    ptls_decode_open_block(src, end, -1, {
        if (tls_session->base != NULL) {
            if ((ret = quicly_decode_transport_parameter_list(quic_tp, NULL, NULL, NULL, NULL, src, end)) != 0)
                goto Exit;
        }
        src = end;
    });
    ptls_decode_block(src, end, -1, {
        if (src != end) {
            *quic_address_token = ptls_iovec_init(h2o_mem_alloc(end - src), end - src);
            memcpy(quic_address_token->base, src, end - src);
            src = end;
        }
    });

    ret = 0;

Exit:
    if (ret != 0) {
        free(tls_session->base);
        *tls_session = ptls_iovec_init(NULL, 0);
        free(quic_address_token->base);
        *quic_address_token = ptls_iovec_init(NULL, 0);
    }
}

static void save_session(ptls_t *tls, ptls_iovec_t *tls_session, const quicly_transport_parameters_t *quic_tp,
                         ptls_iovec_t *quic_address_token)
{
    if (session_file == NULL)
        return;

    const char *server_name = ptls_get_server_name(tls);
    struct {
        ptls_iovec_t tls_session, quic_address_token;
        quicly_transport_parameters_t quic_tp;
    } loaded;
    ptls_buffer_t buf;
    int ret;

    ptls_buffer_init(&buf, "", 0);

    /* load current data, or zero clear */
    load_session(server_name, &loaded.tls_session, &loaded.quic_tp, &loaded.quic_address_token);

    /* if new data is not supplied, point to the loaded values */
    if (tls_session == NULL)
        tls_session = &loaded.tls_session;
    if (quic_address_token == NULL)
        quic_address_token = &loaded.quic_address_token;
    if (quic_tp == NULL)
        quic_tp = &loaded.quic_tp;

    /* seralize the data */
    ptls_buffer_push_block(&buf, -1, { ptls_buffer_pushv(&buf, server_name, strlen(server_name)); });
    ptls_buffer_push_block(&buf, -1, { ptls_buffer_pushv(&buf, tls_session->base, tls_session->len); });
    ptls_buffer_push_block(&buf, -1, {
        if (tls_session->base != NULL &&
            (ret = quicly_encode_transport_parameter_list(&buf, quic_tp, NULL, NULL, NULL, NULL, 0)) != 0)
            goto Exit;
    });
    ptls_buffer_push_block(&buf, -1, { ptls_buffer_pushv(&buf, quic_address_token->base, quic_address_token->len); });

    {
        FILE *fp;
        if ((fp = fopen(session_file, "w")) == NULL) {
            fprintf(stderr, "failed to open file:%s:%s\n", session_file, strerror(errno));
            exit(EX_OSERR);
        }
        fwrite(buf.base, 1, buf.off, fp);
        fclose(fp);
    }

    free(loaded.tls_session.base);
    free(loaded.quic_address_token.base);
    ptls_buffer_dispose(&buf);
    return;

Exit:
    fprintf(stderr, "%s:out of memory\n", __FUNCTION__);
    exit(EX_SOFTWARE);
}

static int load_http3_session_cb(h2o_httpclient_ctx_t *ctx, struct sockaddr *server_addr, const char *server_name,
                                 ptls_iovec_t *address_token, ptls_iovec_t *session_ticket, quicly_transport_parameters_t *tp)
{
    load_session(server_name, session_ticket, tp, address_token);
    return 1;
}

static quicly_error_t save_http3_token_cb(quicly_save_resumption_token_t *self, quicly_conn_t *conn, ptls_iovec_t token)
{
    save_session(quicly_get_tls(conn), NULL, NULL, &token);
    return 0;
}

static int save_http3_ticket_cb(ptls_save_ticket_t *self, ptls_t *tls, ptls_iovec_t src)
{
    quicly_conn_t *conn = *ptls_get_data_ptr(tls);
    assert(quicly_get_tls(conn) == tls);

    save_session(tls, &src, quicly_get_remote_transport_parameters(conn), NULL);
    return 0;
}

static void add_header(h2o_iovec_t name, h2o_iovec_t value)
{
    if (req.num_headers >= sizeof(req.headers) / sizeof(req.headers[0])) {
        fprintf(stderr, "too many request headers\n");
        exit(EXIT_FAILURE);
    }

    req.headers[req.num_headers].name = name;
    req.headers[req.num_headers].value = value;
    ++req.num_headers;
}

struct st_timeout {
    h2o_timer_t timeout;
    void *ptr;
};

static void create_timeout(h2o_loop_t *loop, uint64_t delay_ticks, h2o_timer_cb cb, void *ptr)
{
    struct st_timeout *t = h2o_mem_alloc(sizeof(*t));
    *t = (struct st_timeout){{.cb = cb}, ptr};
    h2o_timer_link(loop, delay_ticks, &t->timeout);
}

static void on_exit_deferred(h2o_timer_t *entry)
{
    exit(1);
}

static void on_error(h2o_httpclient_ctx_t *ctx, h2o_mem_pool_t *pool, const char *fmt, ...)
{
    char errbuf[2048];
    va_list args;
    va_start(args, fmt);
    int errlen = vsnprintf(errbuf, sizeof(errbuf), fmt, args);
    va_end(args);
    fprintf(stderr, "%s: %.*s\n", progname, errlen, errbuf);

    /* defer using zero timeout to send pending GOAWAY frame */
    create_timeout(ctx->loop, 0, on_exit_deferred, NULL);

    h2o_mem_clear_pool(pool);
    free(pool);
}

static void stdin_on_read(h2o_socket_t *_sock, const char *err)
{
    assert(std_in.sock == _sock);

    h2o_socket_read_stop(std_in.sock);
    if (err != NULL)
        std_in.closed = 1;
    if (udp_sock != NULL)
        h2o_socket_read_stop(udp_sock);

    h2o_httpclient_t *client = std_in.sock->data;

    /* bail out if the client is not yet ready to receive data */
    if (client == NULL || client->write_req == NULL)
        return;

    if (client->write_req(client, h2o_iovec_init(std_in.sock->input->bytes, std_in.sock->input->size), std_in.closed) != 0) {
        fprintf(stderr, "write_req error\n");
        exit(1);
    }
    h2o_buffer_consume(&std_in.sock->input, std_in.sock->input->size);
}

static size_t build_capsule_header(uint8_t *header_buf, size_t payload_len)
{
    uint8_t *p = header_buf;
    *p++ = 0; /* Datagram Capsule Type */
    p = quicly_encodev(p, (uint64_t)payload_len);
    return p - header_buf;
}

static void tunnel_on_udp_sock_read(h2o_socket_t *sock, const char *err)
{
    uint8_t buf[1500];
    struct iovec vec;
    struct msghdr mess = {};
    ssize_t rret;

    size_t context_id_len;
    if (strcmp(req.method, "CONNECT-UDP") == 0) {
        // No context id for draft03.
        context_id_len = 0;
    } else {
        context_id_len = 1;
        buf[0] = 0; // Context ID 0 used for UDP packets.
    }

    /* read one UDP datagram, or return */
    do {
        vec.iov_base = buf + context_id_len;
        vec.iov_len = sizeof(buf) - context_id_len;
        mess.msg_name = &udp_sock_remote_addr;
        mess.msg_namelen = sizeof(udp_sock_remote_addr);
        mess.msg_iov = &vec;
        mess.msg_iovlen = 1;
    } while ((rret = recvmsg(h2o_socket_get_fd(sock), &mess, 0)) == -1 && errno == EINTR);
    if (rret == -1)
        return;

    h2o_httpclient_t *client = std_in.sock->data;

    /* drop datagram if the connection is not ready */
    if (client == NULL || client->write_req == NULL)
        return;

    /* send the datagram directly or encapsulated on the stream */
    if (udp_write != NULL) {
        h2o_iovec_t datagram = h2o_iovec_init(buf, context_id_len + rret);
        udp_write(client, &datagram, 1);
    } else {
        /* append UDP chunk to the input buffer of stdin read socket! */
        uint8_t header_buf[3];
        h2o_buffer_append(&std_in.sock->input, header_buf, build_capsule_header(header_buf, context_id_len + rret));
        h2o_buffer_append(&std_in.sock->input, buf, context_id_len + rret);
        /* pretend as if we read from stdin */
        stdin_on_read(std_in.sock, NULL);
    }
}

static void tunnel_on_udp_read(h2o_httpclient_t *client, h2o_iovec_t *datagrams, size_t num_datagrams)
{
    int is_draft03 = strcmp(req.method, "CONNECT-UDP") == 0;

    for (size_t i = 0; i != num_datagrams; ++i) {
        if (udp_sock != NULL) {
            /* connected to client via UDP; decode and forward the UDP payload */
            struct iovec udp_payload;
            if (is_draft03) {
                udp_payload = (struct iovec){datagrams[i].base, datagrams[i].len};
            } else {
                const uint8_t *src = (uint8_t *)datagrams[i].base;
                /* Skip datagrams with context id != 0, rfc9298 section 5. TODO: error-close the connection upon decoding failure?
                 */
                if (ptls_decode_quicint(&src, src + datagrams[i].len) != 0)
                    continue;
                udp_payload = (struct iovec){.iov_base = (void *)src,
                                             .iov_len = datagrams[i].len - (src - (const uint8_t *)datagrams[i].base)};
            }
            struct msghdr mess = {.msg_name = &udp_sock_remote_addr,
                                  .msg_namelen = sizeof(udp_sock_remote_addr),
                                  .msg_iov = &udp_payload,
                                  .msg_iovlen = 1};
            sendmsg(h2o_socket_get_fd(udp_sock), &mess, 0);
        } else {
            /* connected to client via capsule stream; encode and forward (TODO make it atomic write) */
            uint8_t header_buf[3];
            fwrite(header_buf, 1, build_capsule_header(header_buf, datagrams[i].len), stdout);
            fwrite(datagrams[i].base, 1, datagrams[i].len, stdout);
            fflush(stdout);
        }
    }
}

static void stdin_proceed_request(h2o_httpclient_t *client, const char *errstr)
{
    if (errstr == NULL && !std_in.closed) {
        h2o_socket_read_start(std_in.sock, stdin_on_read);
        if (udp_sock != NULL)
            h2o_socket_read_start(udp_sock, tunnel_on_udp_sock_read);
    }
}

static void start_request(h2o_httpclient_ctx_t *ctx)
{
    h2o_mem_pool_t *pool;
    h2o_url_t *target_uri;
    const char *upgrade_to = NULL;

    /* allocate memory pool */
    pool = h2o_mem_alloc(sizeof(*pool));
    h2o_mem_init_pool(pool);

    /* parse URL, or host:port if CONNECT */
    target_uri = h2o_mem_alloc_pool(pool, *target_uri, 1);
    *target_uri = (h2o_url_t){};

    if (strcmp(req.method, "CONNECT-UDP") == 0 || (strcmp(req.method, "CONNECT") == 0 && upgrade_token == NULL)) {
        /* Traditional CONNECT, either creating a TCP tunnel or a UDP tunnel in the style of masque draft-03).
         * Authority section of target is set to host:port, and `upgrade_to` specifies traditional CONNECT. When masque is used,
         * scheme and path are set accordingly. */
        if (h2o_url_init(target_uri, NULL, h2o_iovec_init(req.target, strlen(req.target)), h2o_iovec_init(NULL, 0)) != 0 ||
            target_uri->_port == 0 || target_uri->_port == 65535) {
            on_error(ctx, pool, "CONNECT target should be in the form of host:port: %s", req.target);
            return;
        }
        if (strcmp(req.method, "CONNECT-UDP") == 0) {
            target_uri->scheme = &H2O_URL_SCHEME_MASQUE;
            target_uri->path = h2o_iovec_init(H2O_STRLIT("/"));
        }
        upgrade_to = h2o_httpclient_upgrade_to_connect;
    } else {
        /* An ordinary request or extended CONNECT. Both of them talks to origin specified by the target URI */
        if (h2o_url_parse(pool, req.target, SIZE_MAX, target_uri) != 0) {
            on_error(ctx, pool, "unrecognized type of URL: %s", req.target);
            return;
        }
        upgrade_to = upgrade_token;
    }

    /* initiate the request */
    if (connpool == NULL) {
        connpool = h2o_mem_alloc(sizeof(*connpool));
        h2o_socketpool_t *sockpool = h2o_mem_alloc(sizeof(*sockpool));
        h2o_socketpool_target_t *target = h2o_socketpool_create_target(req.connect_to != NULL ? req.connect_to : target_uri, NULL);
        h2o_socketpool_init_specific(sockpool, 10, &target, 1, NULL);
        h2o_socketpool_set_timeout(sockpool, io_timeout);
        h2o_socketpool_register_loop(sockpool, ctx->loop);
        h2o_httpclient_connection_pool_init(connpool, sockpool);

        /* obtain root */
        char *root, *crt_fullpath;
        if ((root = getenv("H2O_ROOT")) == NULL)
            root = H2O_TO_STR(H2O_ROOT);
#define CA_PATH "/share/h2o/ca-bundle.crt"
        crt_fullpath = h2o_mem_alloc(strlen(root) + strlen(CA_PATH) + 1);
        sprintf(crt_fullpath, "%s%s", root, CA_PATH);
#undef CA_PATH

        SSL_CTX *ssl_ctx = SSL_CTX_new(SSLv23_client_method());
        SSL_CTX_load_verify_locations(ssl_ctx, crt_fullpath, NULL);
        if (ssl_verify_none) {
            SSL_CTX_set_verify(ssl_ctx, SSL_VERIFY_NONE, NULL);
        } else {
            SSL_CTX_set_verify(ssl_ctx, SSL_VERIFY_PEER | SSL_VERIFY_FAIL_IF_NO_PEER_CERT, NULL);
        }
        h2o_socketpool_set_ssl_ctx(sockpool, ssl_ctx);
        SSL_CTX_free(ssl_ctx);
    }
    h2o_httpclient_connect(std_in.sock != NULL ? (h2o_httpclient_t **)&std_in.sock->data : NULL, pool, target_uri, ctx, connpool,
                           target_uri, upgrade_to, on_connect);
}

static void on_next_request(h2o_timer_t *entry)
{
    struct st_timeout *t = H2O_STRUCT_FROM_MEMBER(struct st_timeout, timeout, entry);
    h2o_httpclient_ctx_t *ctx = t->ptr;
    free(t);

    start_request(ctx);
}

static void print_headers(h2o_header_t *headers, size_t num_headers)
{
    for (size_t i = 0; i != num_headers; ++i) {
        const char *name = headers[i].orig_name;
        if (name == NULL)
            name = headers[i].name->base;
        fprintf(stderr, "%.*s: %.*s\n", (int)headers[i].name->len, name, (int)headers[i].value.len, headers[i].value.base);
    }
}

static int on_body(h2o_httpclient_t *client, const char *errstr, h2o_header_t *trailers, size_t num_trailers)
{
    if (errstr != NULL) {
        if (udp_sock != NULL)
            h2o_socket_read_stop(udp_sock);
        if (errstr != h2o_httpclient_error_is_eos) {
            on_error(client->ctx, client->pool, errstr);
            return -1;
        }
    }

    fwrite((*client->buf)->bytes, 1, (*client->buf)->size, stdout);
    fflush(stdout);
    h2o_buffer_consume(&(*client->buf), (*client->buf)->size);

    if (errstr == h2o_httpclient_error_is_eos) {
        h2o_mem_clear_pool(client->pool);
        free(client->pool);
        --cnt_left;
        if (cnt_left >= concurrency) {
            /* next attempt */
            ftruncate(fileno(stdout), 0); /* ignore error when stdout is a tty */
            create_timeout(client->ctx->loop, req_interval, on_next_request, client->ctx);
        }
    }

    if (num_trailers != 0) {
        print_headers(trailers, num_trailers);
        fprintf(stderr, "\n");
        fflush(stderr);
    }

    return 0;
}

static void print_status_line(int version, int status, h2o_iovec_t msg)
{
    if (exit_failure_on_http_errors && status >= 400)
        program_exit_status = EXIT_FAILURE;

    fprintf(stderr, "HTTP/%d", (version >> 8));
    if ((version & 0xff) != 0) {
        fprintf(stderr, ".%d", version & 0xff);
    }
    fprintf(stderr, " %d", status);
    if (msg.len != 0) {
        fprintf(stderr, " %.*s\n", (int)msg.len, msg.base);
    } else {
        fprintf(stderr, "\n");
    }
}

static int on_informational(h2o_httpclient_t *client, int version, int status, h2o_iovec_t msg, h2o_header_t *headers,
                            size_t num_headers)
{
    print_status_line(version, status, msg);
    print_headers(headers, num_headers);
    fprintf(stderr, "\n");
    fflush(stderr);
    return 0;
}

h2o_httpclient_body_cb on_head(h2o_httpclient_t *client, const char *errstr, h2o_httpclient_on_head_t *args)
{
    if (errstr != NULL && errstr != h2o_httpclient_error_is_eos) {
        on_error(client->ctx, client->pool, errstr);
        return NULL;
    }

    print_status_line(args->version, args->status, args->msg);
    print_headers(args->headers, args->num_headers);
    fprintf(stderr, "\n");
    fflush(stderr);

    if (errstr == h2o_httpclient_error_is_eos) {
        on_error(client->ctx, client->pool, "no body");
        return NULL;
    }

    if (200 <= args->status && args->status <= 299) {
        udp_write = args->forward_datagram.write_;
        if (args->forward_datagram.read_ != NULL)
            *args->forward_datagram.read_ = tunnel_on_udp_read;
    }

    return on_body;
}

static size_t *filler_remaining_bytes(h2o_httpclient_t *client)
{
    return (size_t *)&client->data;
}

static void filler_on_io_timeout(h2o_timer_t *entry)
{
    struct st_timeout *t = H2O_STRUCT_FROM_MEMBER(struct st_timeout, timeout, entry);
    h2o_httpclient_t *client = t->ptr;
    free(t);

    h2o_iovec_t vec = iov_filler;
    if (vec.len > *filler_remaining_bytes(client))
        vec.len = *filler_remaining_bytes(client);
    *filler_remaining_bytes(client) -= vec.len;
    client->write_req(client, vec, *filler_remaining_bytes(client) == 0);
}

static void filler_proceed_request(h2o_httpclient_t *client, const char *errstr)
{
    if (errstr != NULL) {
        on_error(client->ctx, client->pool, errstr);
        return;
    }
    if (*filler_remaining_bytes(client) > 0)
        create_timeout(client->ctx->loop, io_interval, filler_on_io_timeout, client);
}

h2o_httpclient_head_cb on_connect(h2o_httpclient_t *client, const char *errstr, h2o_iovec_t *_method, h2o_url_t *url,
                                  const h2o_header_t **headers, size_t *num_headers, h2o_iovec_t *body,
                                  h2o_httpclient_proceed_req_cb *proceed_req_cb, h2o_httpclient_properties_t *props,
                                  h2o_url_t *origin)
{
    h2o_headers_t headers_vec = {NULL};
    size_t i;
    if (errstr != NULL) {
        on_error(client->ctx, client->pool, errstr);
        return NULL;
    }

    *_method = h2o_iovec_init(req.method, strlen(req.method));
    *url = *(h2o_url_t *)client->data;
    for (i = 0; i != req.num_headers; ++i)
        h2o_add_header_by_str(client->pool, &headers_vec, req.headers[i].name.base, req.headers[i].name.len, 1, NULL,
                              req.headers[i].value.base, req.headers[i].value.len);
    *body = h2o_iovec_init(NULL, 0);
    *proceed_req_cb = NULL;

    if (client->upgrade_to != NULL) {
        *proceed_req_cb = stdin_proceed_request;
        if (std_in.sock->input->size != 0) {
            body->len = std_in.sock->input->size;
            body->base = h2o_mem_alloc_pool(client->pool, char, body->len);
            memcpy(body->base, std_in.sock->input->bytes, body->len);
            h2o_buffer_consume(&std_in.sock->input, body->len);
        }
    } else if (req.body_size > 0) {
        *filler_remaining_bytes(client) = req.body_size;
        char *clbuf = h2o_mem_alloc_pool(client->pool, char, sizeof(H2O_UINT32_LONGEST_STR) - 1);
        size_t clbuf_len = sprintf(clbuf, "%zu", req.body_size);
        h2o_add_header(client->pool, &headers_vec, H2O_TOKEN_CONTENT_LENGTH, NULL, clbuf, clbuf_len);
        *proceed_req_cb = filler_proceed_request;
    }

    *headers = headers_vec.entries;
    *num_headers = headers_vec.size;
    client->informational_cb = on_informational;
    return on_head;
}

static void usage(const char *progname)
{
    fprintf(stderr,
            "Usage: %s [options] <url>\n"
            "Options:\n"
            "  -2 <ratio>   HTTP/2 ratio (between 0 and 100)\n"
            "  -3 <ratio>   HTTP/3 ratio (between 0 and 100)\n"
            "  -b <size>    size of request body (in bytes; default: 0)\n"
            "  -C <concurrency>\n"
            "               sets the number of requests run at once (default: 1)\n"
            "  -c <size>    size of body chunk (in bytes; default: 10)\n"
            "  -d <delay>   request interval (in msec; default: 0)\n"
            "  -f           returns an error if an HTTP response code is 400 or greater.\n"
            "  -H <name:value>\n"
            "               adds a request header\n"
            "  -i <delay>   I/O interval between sending chunks (in msec; default: 0)\n"
            "  -k           skip peer verification\n"
            "  -m <method>  request method (default: GET). When method is CONNECT,\n"
            "               \"host:port\" should be specified in place of URL.\n"
            "  -o <path>    file to which the response body is written (default: stdout)\n"
            "  -s <session-file>\n"
            "               file to read / write session information (atm HTTP/3 only)\n"
            "  -t <times>   number of requests to send the request (default: 1)\n"
            "  -W <bytes>   receive window size (HTTP/3 only)\n"
            "  -x <URL>     specifies the host and port to connect to. When the scheme is\n"
            "               set to HTTP, cleartext TCP is used. When the scheme is HTTPS,\n"
            "               TLS is used and the provided hostname is used for peer.\n"
            "               verification\n"
            "  -X <local-udp-port>\n"
            "               specifies that the tunnel being created is a CONNECT-UDP tunnel\n"
            "  --initial-udp-payload-size <bytes>\n"
            "               specifies the udp payload size of the initial message (default:\n"
            "               %" PRIu16 ")\n"
            "  --max-udp-payload-size <bytes>\n"
            "               specifies the max_udp_payload_size transport parameter to send\n"
            "               (default: %" PRIu64 ")\n"
            " --io-timeout <milliseconds>\n"
            "               specifies the timeout for I/O operations (default: 5000ms)\n"
            "  -h, --help   prints this help\n"
            "\n",
            progname, quicly_spec_context.initial_egress_max_udp_payload_size,
            quicly_spec_context.transport_params.max_udp_payload_size);
}

#if !H2O_USE_LIBUV
h2o_socket_t *create_udp_socket(h2o_loop_t *loop, uint16_t port)
{
    int fd;
    struct sockaddr_in sin;
    if ((fd = socket(PF_INET, SOCK_DGRAM, 0)) == -1) {
        perror("failed to create UDP socket");
        exit(EXIT_FAILURE);
    }
    sin.sin_family = AF_INET;
    sin.sin_addr.s_addr = htonl(0);
    sin.sin_port = htons(port);
    if (bind(fd, (void *)&sin, sizeof(sin)) != 0) {
        perror("failed to bind bind UDP socket");
        exit(EXIT_FAILURE);
    }
    return h2o_evloop_socket_create(loop, fd, H2O_SOCKET_FLAG_DONT_READ);
}
#endif

static void on_sigfatal(int signo)
{
    fprintf(stderr, "received fatal signal %d\n", signo);

    h2o_set_signal_handler(signo, SIG_DFL);

#ifdef LIBC_HAS_BACKTRACE
    void *frames[128];
    int framecnt = backtrace(frames, sizeof(frames) / sizeof(frames[0]));
    backtrace_symbols_fd(frames, framecnt, 2);
#endif
}

int main(int argc, char **argv)
{
    progname = argv[0];

    h2o_set_signal_handler(SIGABRT, on_sigfatal);
    h2o_set_signal_handler(SIGBUS, on_sigfatal);
    h2o_set_signal_handler(SIGFPE, on_sigfatal);
    h2o_set_signal_handler(SIGILL, on_sigfatal);
    h2o_set_signal_handler(SIGSEGV, on_sigfatal);

    h2o_multithread_queue_t *queue;
    h2o_multithread_receiver_t getaddr_receiver;
    h2o_httpclient_ctx_t ctx = {
        .getaddr_receiver = &getaddr_receiver,
        .max_buffer_size = 128 * 1024,
        .http2 = {.max_concurrent_streams = 100},
        .http3 = &h3ctx,
    };
    int opt;

    SSL_load_error_strings();
    SSL_library_init();
    OpenSSL_add_all_algorithms();

    /* When using OpenSSL >= 3.0, load legacy provider so that blowfish can be used for 64-bit QUIC CIDs. */
#if LOAD_OPENSSL_PROVIDER
    OSSL_PROVIDER_load(NULL, "legacy");
    OSSL_PROVIDER_load(NULL, "default");
#endif

    quicly_amend_ptls_context(&h3ctx.tls);
    h3ctx.quic = quicly_spec_context;
    h3ctx.quic.transport_params.max_streams_uni = 10;
    h3ctx.quic.transport_params.max_datagram_frame_size = 1500;
    h3ctx.quic.receive_datagram_frame = &h2o_httpclient_http3_on_receive_datagram_frame;
    h3ctx.quic.tls = &h3ctx.tls;
    h3ctx.quic.save_resumption_token = &save_http3_token;
    {
        uint8_t random_key[PTLS_SHA256_DIGEST_SIZE];
        h3ctx.tls.random_bytes(random_key, sizeof(random_key));
        h3ctx.quic.cid_encryptor = quicly_new_default_cid_encryptor(
            &ptls_openssl_bfecb, &ptls_openssl_aes128ecb, &ptls_openssl_sha256, ptls_iovec_init(random_key, sizeof(random_key)));
        assert(h3ctx.quic.cid_encryptor != NULL);
        ptls_clear_memory(random_key, sizeof(random_key));
    }
    h3ctx.quic.stream_open = &h2o_httpclient_http3_on_stream_open;
    h3ctx.load_session = load_http3_session_cb;

#if H2O_USE_LIBUV
    ctx.loop = uv_loop_new();
#else
    ctx.loop = h2o_evloop_create();
#endif

#if H2O_USE_LIBUV
#else
    { /* initialize QUIC context */
        int fd;
        struct sockaddr_in sin;
        if ((fd = socket(PF_INET, SOCK_DGRAM, 0)) == -1) {
            perror("failed to create UDP socket");
            exit(EXIT_FAILURE);
        }
        memset(&sin, 0, sizeof(sin));
        if (bind(fd, (void *)&sin, sizeof(sin)) != 0) {
            perror("failed to bind bind UDP socket");
            exit(EXIT_FAILURE);
        }
        h2o_socket_t *sock = h2o_evloop_socket_create(ctx.loop, fd, H2O_SOCKET_FLAG_DONT_READ);
        h2o_quic_init_context(&h3ctx.h3, ctx.loop, sock, &h3ctx.quic, &h3_next_cid, NULL,
                              h2o_httpclient_http3_notify_connection_update, 1 /* use_gso */, NULL);
    }
#endif

    enum {
        OPT_INITIAL_UDP_PAYLOAD_SIZE = 0x100,
        OPT_MAX_UDP_PAYLOAD_SIZE,
        OPT_DISALLOW_DELAYED_ACK,
        OPT_ACK_FREQUENCY,
        OPT_IO_TIMEOUT,
        OPT_HTTP3_MAX_FRAME_PAYLOAD_SIZE,
        OPT_UPGRADE,
    };
    struct option longopts[] = {{"initial-udp-payload-size", required_argument, NULL, OPT_INITIAL_UDP_PAYLOAD_SIZE},
                                {"max-udp-payload-size", required_argument, NULL, OPT_MAX_UDP_PAYLOAD_SIZE},
                                {"disallow-delayed-ack", no_argument, NULL, OPT_DISALLOW_DELAYED_ACK},
                                {"ack-frequency", required_argument, NULL, OPT_ACK_FREQUENCY},
                                {"io-timeout", required_argument, NULL, OPT_IO_TIMEOUT},
                                {"http3-max-frame-payload-size", required_argument, NULL, OPT_HTTP3_MAX_FRAME_PAYLOAD_SIZE},
                                {"upgrade", required_argument, NULL, OPT_UPGRADE},
                                {"help", no_argument, NULL, 'h'},
                                {NULL}};
    const char *optstring = "t:m:o:b:x:X:C:c:d:H:i:fk2:W:s:h3:"
#ifdef __GNUC__
                            ":" /* for backward compatibility, optarg of -3 is optional when using glibc */
#endif
        ;
    while ((opt = getopt_long(argc, argv, optstring, longopts, NULL)) != -1) {
        switch (opt) {
        case 't':
            if (sscanf(optarg, "%u", &cnt_left) != 1 || cnt_left < 1) {
                fprintf(stderr, "count (-t) must be a number greater than zero\n");
                exit(EXIT_FAILURE);
            }
            break;
        case 'm':
            req.method = optarg;
            break;
        case 'o':
            if (freopen(optarg, "w", stdout) == NULL) {
                fprintf(stderr, "failed to open file:%s:%s\n", optarg, strerror(errno));
                exit(EXIT_FAILURE);
            }
            break;
        case 'b':
            req.body_size = atoi(optarg);
            if (req.body_size <= 0) {
                fprintf(stderr, "body size must be greater than 0\n");
                exit(EXIT_FAILURE);
            }
            break;
        case 'x': {
            h2o_mem_pool_t pool;
            h2o_mem_init_pool(&pool);
            req.connect_to = h2o_mem_alloc(sizeof(*req.connect_to));
            /* we can leak pool and `req.connect_to`, as they are globals allocated only once in `main` */
            if (h2o_url_parse(&pool, optarg, strlen(optarg), req.connect_to) != 0) {
                fprintf(stderr, "invalid server URL specified for -x\n");
                exit(EXIT_FAILURE);
            }
        } break;
        case 'X': {
#if H2O_USE_LIBUV
            fprintf(stderr, "-X is not supported by the libuv backend\n");
            exit(EXIT_FAILURE);
#else
            uint16_t udp_port;
            if (sscanf(optarg, "%" SCNu16, &udp_port) != 1) {
                fprintf(stderr, "failed to parse optarg of -X\n");
                exit(EXIT_FAILURE);
            }
            udp_sock = create_udp_socket(ctx.loop, udp_port);
            h2o_socket_read_start(udp_sock, tunnel_on_udp_sock_read);
            h3ctx.quic.initial_egress_max_udp_payload_size = 1400; /* increase initial UDP payload size so that we'd have room to
                                                                    * carry ordinary QUIC packets. */
#endif
        } break;
        case 'C':
            if (sscanf(optarg, "%u", &concurrency) != 1 || concurrency < 1) {
                fprintf(stderr, "concurrency (-C) must be a number greather than zero");
                exit(EXIT_FAILURE);
            }
            break;
        case 'c':
            chunk_size = atoi(optarg);
            if (chunk_size <= 0) {
                fprintf(stderr, "chunk size must be greater than 0\n");
                exit(EXIT_FAILURE);
            }
            break;
        case 'd':
            req_interval = atoi(optarg);
            break;
        case 'H': {
            const char *colon, *value_start;
            if ((colon = strchr(optarg, ':')) == NULL) {
                fprintf(stderr, "no `:` found in -H\n");
                exit(EXIT_FAILURE);
            }
            for (value_start = colon + 1; *value_start == ' ' || *value_start == '\t'; ++value_start)
                ;
            /* lowercase the header field name (HTTP/2: RFC 9113 Section 8.2, HTTP/3: RFC 9114 Section 4.2) */
            h2o_iovec_t name = h2o_strdup(NULL, optarg, colon - optarg);
            h2o_strtolower(name.base, name.len);
            add_header(name, h2o_iovec_init(value_start, strlen(value_start)));
        } break;
        case 'i':
            io_interval = atoi(optarg);
            break;
        case 'k':
            ssl_verify_none = 1;
            break;
        case '2':
            if (!strcasecmp(optarg, "f")) {
                ctx.protocol_selector.ratio.http2 = 100;
                ctx.force_cleartext_http2 = 1;
            } else if (sscanf(optarg, "%" SCNd8, &ctx.protocol_selector.ratio.http2) != 1 ||
                       !(0 <= ctx.protocol_selector.ratio.http2 && ctx.protocol_selector.ratio.http2 <= 100)) {
                fprintf(stderr, "failed to parse HTTP/2 ratio (-2)\n");
                exit(EXIT_FAILURE);
            }
            break;
        case '3':
#if H2O_USE_LIBUV
            fprintf(stderr, "HTTP/3 is currently not supported by the libuv backend.\n");
            exit(EXIT_FAILURE);
#else
            if (optarg == NULL) {
                /* parse the optional argument (glibc extension; see above) */
                if (optind < argc && ('0' <= argv[optind][0] && argv[optind][0] <= '9') &&
                    sscanf(argv[optind], "%" SCNd8, &ctx.protocol_selector.ratio.http3) == 1) {
                    ++optind;
                } else {
                    ctx.protocol_selector.ratio.http3 = 100;
                }
            } else {
                if (sscanf(optarg, "%" SCNd8, &ctx.protocol_selector.ratio.http3) != 1)
                    ctx.protocol_selector.ratio.http3 = -1;
            }
            if (!(0 <= ctx.protocol_selector.ratio.http3 && ctx.protocol_selector.ratio.http3 <= 100)) {
                fprintf(stderr, "failed to parse HTTP/3 ratio (-3)\n");
                exit(EXIT_FAILURE);
            }
#endif
            break;
        case 'W': {
            uint64_t v;
            if (sscanf(optarg, "%" SCNu64, &v) != 1) {
                fprintf(stderr, "failed to parse HTTP/3 receive window size (-W)\n");
                exit(EXIT_FAILURE);
            }
            h3ctx.quic.transport_params.max_stream_data.uni = v;
            h3ctx.quic.transport_params.max_stream_data.bidi_local = v;
            h3ctx.quic.transport_params.max_stream_data.bidi_remote = v;
        } break;
        case 'f':
            exit_failure_on_http_errors = 1;
            break;
        case 's':
            session_file = optarg;
            break;
        case 'h':
            usage(argv[0]);
            exit(0);
            break;
        case OPT_INITIAL_UDP_PAYLOAD_SIZE:
            if (sscanf(optarg, "%" SCNu16, &h3ctx.quic.initial_egress_max_udp_payload_size) != 1) {
                fprintf(stderr, "failed to parse --initial-udp-payload-size\n");
                exit(EXIT_FAILURE);
            }
            break;
        case OPT_MAX_UDP_PAYLOAD_SIZE:
            if (sscanf(optarg, "%" SCNu64, &h3ctx.quic.transport_params.max_udp_payload_size) != 1) {
                fprintf(stderr, "failed to parse --max-udp-payload-size\n");
                exit(EXIT_FAILURE);
            }
            break;
        case OPT_DISALLOW_DELAYED_ACK:
            h3ctx.quic.transport_params.min_ack_delay_usec = UINT64_MAX;
            break;
        case OPT_ACK_FREQUENCY: {
            double f;
            if (sscanf(optarg, "%lf", &f) != 1 || !(0 <= f && f <= 1)) {
                fprintf(stderr, "failed to parse --ack-frequency\n");
                exit(EXIT_FAILURE);
            }
            h3ctx.quic.ack_frequency = (uint16_t)(f * 1024);
        } break;
        case OPT_IO_TIMEOUT:
            if (sscanf(optarg, "%" SCNu64, &io_timeout) != 1) {
                fprintf(stderr, "failed to parse --io-timeout\n");
                exit(EXIT_FAILURE);
            }
            break;
        case OPT_HTTP3_MAX_FRAME_PAYLOAD_SIZE:
            if (sscanf(optarg, "%" SCNu64, &h3ctx.max_frame_payload_size) != -1) {
                fprintf(stderr, "failed to parse --http3-max-frame-payload-size\n");
                exit(EXIT_FAILURE);
            }
            break;
        case OPT_UPGRADE:
            upgrade_token = optarg;
            break;
        default:
            exit(EXIT_FAILURE);
            break;
        }
    }
    argc -= optind;
    argv += optind;

    ctx.io_timeout = io_timeout;
    ctx.connect_timeout = io_timeout;
    ctx.first_byte_timeout = io_timeout;
    ctx.keepalive_timeout = io_timeout;

    if (ctx.protocol_selector.ratio.http2 + ctx.protocol_selector.ratio.http3 > 100) {
        fprintf(stderr, "sum of the use ratio of HTTP/2 and HTTP/3 is greater than 100\n");
        exit(EXIT_FAILURE);
    }

    int is_connect = 0;
    if ((strcmp(req.method, "CONNECT") == 0 && upgrade_token == NULL) || strcmp(req.method, "CONNECT-UDP") == 0) {
        /* traditional CONNECT */
        if (req.connect_to == NULL) {
            fprintf(stderr, "CONNECT method must be accompanied by either `-x` or `--upgrade`\n");
            exit(EXIT_FAILURE);
        }
        is_connect = 1;
    } else if (upgrade_token != NULL) {
        /* masque using extended CONNECT (RFC 9298) */
        if (strcmp(req.method, "GET") == 0) {
            if (ctx.protocol_selector.ratio.http2 != 0 || ctx.protocol_selector.ratio.http3 != 0) {
                fprintf(stderr, "extended CONNECT with GET cannot be used on H2/H3; specify `-2 0 -3 0`\n");
                exit(EXIT_FAILURE);
            }
        } else if (strcmp(req.method, "CONNECT") == 0) {
            if (ctx.protocol_selector.ratio.http2 < 0 ||
                ctx.protocol_selector.ratio.http2 + ctx.protocol_selector.ratio.http3 != 100) {
                fprintf(stderr,
                        "extended CONNECT using CONNECT method cannot be used on H1; specify `-2 100` or a mixture of H2 and H2\n");
                exit(EXIT_FAILURE);
            }
        }
        is_connect = 1;
    }
    if (is_connect) {
#if H2O_USE_LIBUV
        std_in.sock = h2o_uv__poll_create(ctx.loop, 0, (uv_close_cb)free);
#else
        std_in.sock = h2o_evloop_socket_create(ctx.loop, 0, 0);
#endif
        h2o_socket_read_start(std_in.sock, stdin_on_read);
    }

    if (argc < 1) {
        fprintf(stderr, "no URL\n");
        exit(EXIT_FAILURE);
    }
    req.target = argv[0];

    if (req.body_size != 0) {
        iov_filler.base = h2o_mem_alloc(chunk_size);
        memset(iov_filler.base, 'a', chunk_size);
        iov_filler.len = chunk_size;
    }

    /* setup context */
    queue = h2o_multithread_create_queue(ctx.loop);
    h2o_multithread_register_receiver(queue, ctx.getaddr_receiver, h2o_hostinfo_getaddr_receiver);

    /* setup the first request(s) */
    for (unsigned i = 0; i < concurrency && i < cnt_left; ++i)
        start_request(&ctx);

    while (cnt_left != 0) {
#if H2O_USE_LIBUV
        uv_run(ctx.loop, UV_RUN_ONCE);
#else
        h2o_evloop_run(ctx.loop, INT32_MAX);
#endif
    }

#if H2O_USE_LIBUV
/* libuv path currently does not support http3 */
#else
    if (ctx.protocol_selector.ratio.http3 > 0) {
        h2o_quic_close_all_connections(&ctx.http3->h3);
        while (h2o_quic_num_connections(&ctx.http3->h3) != 0) {
            h2o_evloop_run(ctx.loop, INT32_MAX);
        }
    }
#endif

    if (req.connect_to != NULL)
        free(req.connect_to);

    return program_exit_status;
}
