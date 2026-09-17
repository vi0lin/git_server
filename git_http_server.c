/*
 * git_http_server.c
 *
 * A minimal HTTP server that serves Git repositories via the smart HTTP
 * protocol by spawning git-http-backend as a CGI process.
 *
 * Supports: git clone/fetch/pull/push over HTTP (protocol v0 and v2),
 *           gzip-compressed request bodies, chunked request bodies,
 *           "Expect: 100-continue", many concurrent clients.
 *
 * Build:
 *   gcc -O2 -Wall -Wextra -o git_http_server git_http_server.c
 *
 * Usage:
 *   git_http_server <port> <repo_root>
 *   git_http_server 8080 /srv/git
 *
 * Then clone with:
 *   git clone http://localhost:8080/myrepo.git
 */

#define _POSIX_C_SOURCE 200809L
#define _GNU_SOURCE      /* strncasecmp, strcasestr */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>
#include <errno.h>
#include <signal.h>
#include <time.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <sys/wait.h>
#include <poll.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <fcntl.h>
#include <ctype.h>
#include <stdint.h>
#include <stdarg.h>

#define BUF_SIZE        65536
#define MAX_HEADERS     64
#define MAX_HEADER_LEN  4096
#define MAX_PATH_LEN    4096
#define IO_TIMEOUT_SEC  600     /* drop connections that stall for 10 min */
#define INLINE_BODY_MAX 4096    /* smaller bodies always fit into an empty pipe */
#define LINGER_MS       3000    /* max wait for the client to close first    */

/* ─── Utility ────────────────────────────────────────────────────────────── */

static void die(const char *msg) {
    perror(msg);
    exit(1);
}

static void log_msg(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
static void log_msg(const char *fmt, ...) {
    char buf[1024];
    time_t t = time(NULL);
    struct tm tm;
    localtime_r(&t, &tm);
    size_t n = strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S ", &tm);
    va_list ap;
    va_start(ap, fmt);
    int m = vsnprintf(buf + n, sizeof(buf) - n - 1, fmt, ap);
    va_end(ap);
    if (m < 0) return;
    n += ((size_t)m < sizeof(buf) - n - 1) ? (size_t)m : sizeof(buf) - n - 2;
    buf[n++] = '\n';
    /* one write() per line so lines from concurrent processes don't mix */
    ssize_t ignored = write(STDOUT_FILENO, buf, n);
    (void)ignored;
}

/* URL-decode a *path* (no '+' -> ' ' conversion, that's only for queries) */
static void url_decode_path(char *dst, const char *src) {
    while (*src) {
        if (*src == '%' && isxdigit((unsigned char)src[1])
                        && isxdigit((unsigned char)src[2])) {
            char hex[3] = { src[1], src[2], '\0' };
            *dst++ = (char)strtol(hex, NULL, 16);
            src += 3;
        } else {
            *dst++ = *src++;
        }
    }
    *dst = '\0';
}

/* Write all bytes, retrying on EINTR */
static ssize_t write_all(int fd, const void *buf, size_t n) {
    const char *p = buf;
    size_t rem = n;
    while (rem > 0) {
        ssize_t w = write(fd, p, rem);
        if (w < 0) {
            if (errno == EINTR) continue;
            return -1;
        }
        p   += w;
        rem -= (size_t)w;
    }
    return (ssize_t)n;
}

/* ─── Buffered reader ────────────────────────────────────────────────────── */
/*
 * The old code read the socket one byte at a time. A buffered reader is
 * much faster, but the bytes that were read ahead (e.g. the beginning of
 * the request body) must not get lost – so everything goes through this.
 */

typedef struct {
    int    fd;
    char   buf[BUF_SIZE];
    size_t pos, len;
} Reader;

static void reader_init(Reader *r, int fd) {
    r->fd = fd; r->pos = r->len = 0;
}

static int reader_fill(Reader *r) {
    if (r->pos < r->len) return 1;
    for (;;) {
        ssize_t n = read(r->fd, r->buf, sizeof(r->buf));
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) return 0;
        r->pos = 0; r->len = (size_t)n;
        return 1;
    }
}

/* Read up to max bytes; returns 0 on EOF/error */
static ssize_t reader_read(Reader *r, char *dst, size_t max) {
    if (!reader_fill(r)) return 0;
    size_t n = r->len - r->pos;
    if (n > max) n = max;
    memcpy(dst, r->buf + r->pos, n);
    r->pos += n;
    return (ssize_t)n;
}

/*
 * Read one line, strip trailing CR/LF. Returns length (>= 0),
 * or -1 on EOF before any byte was read. Over-long lines are truncated.
 */
static ssize_t reader_line(Reader *r, char *dst, size_t max) {
    size_t i = 0;
    int got = 0;
    while (reader_fill(r)) {
        got = 1;
        char c = r->buf[r->pos++];
        if (c == '\n') break;
        if (i < max - 1) dst[i++] = c;
    }
    dst[i] = '\0';
    if (!got) return -1;
    while (i > 0 && dst[i-1] == '\r') dst[--i] = '\0';
    return (ssize_t)i;
}

/* ─── HTTP request parsing ───────────────────────────────────────────────── */

typedef struct {
    char method[16];
    char path[MAX_PATH_LEN];       /* URL path, decoded */
    char query[MAX_PATH_LEN];      /* query string, raw */
    char version[16];

    char content_type[256];
    long content_length;           /* -1 if absent */
    int  chunked;                  /* Transfer-Encoding: chunked */
    int  expect_continue;          /* Expect: 100-continue */
    char host[256];

    /* raw header lines, forwarded to the CGI as HTTP_* variables */
    char headers[MAX_HEADERS][MAX_HEADER_LEN];
    int  nheaders;
} HttpRequest;

static const char *header_value(const char *line, const char *name) {
    size_t n = strlen(name);
    if (strncasecmp(line, name, n) != 0 || line[n] != ':') return NULL;
    const char *v = line + n + 1;
    while (*v == ' ' || *v == '\t') v++;
    return v;
}

/* Returns 0 on success, -1 on malformed request */
static int parse_request(Reader *rd, HttpRequest *req) {
    memset(req, 0, sizeof(*req));
    req->content_length = -1;

    char line[MAX_HEADER_LEN];
    if (reader_line(rd, line, sizeof(line)) <= 0) return -1;

    char raw_path[MAX_PATH_LEN];
    if (sscanf(line, "%15s %4095s %15s", req->method, raw_path, req->version) != 3)
        return -1;

    char *q = strchr(raw_path, '?');
    if (q) {
        *q = '\0';
        snprintf(req->query, sizeof(req->query), "%s", q + 1);
    }
    url_decode_path(req->path, raw_path);

    for (;;) {
        ssize_t n = reader_line(rd, line, sizeof(line));
        if (n < 0) return -1;      /* connection closed inside headers */
        if (n == 0) break;         /* blank line = end of headers      */

        if (req->nheaders < MAX_HEADERS)
            snprintf(req->headers[req->nheaders++], MAX_HEADER_LEN, "%s", line);

        const char *v;
        if ((v = header_value(line, "Content-Type")))
            snprintf(req->content_type, sizeof(req->content_type), "%s", v);
        else if ((v = header_value(line, "Content-Length")))
            req->content_length = strtol(v, NULL, 10);
        else if ((v = header_value(line, "Transfer-Encoding")))
            req->chunked = (strcasestr(v, "chunked") != NULL);
        else if ((v = header_value(line, "Expect")))
            req->expect_continue = (strcasecmp(v, "100-continue") == 0);
        else if ((v = header_value(line, "Host")))
            snprintf(req->host, sizeof(req->host), "%.255s", v);
    }
    if (req->chunked) req->content_length = -1;  /* RFC 7230 3.3.3 */
    return 0;
}

/* ─── Send a plain HTTP error ────────────────────────────────────────────── */

static void send_error(int fd, int code, const char *msg) {
    char buf[512];
    int n = snprintf(buf, sizeof(buf),
        "HTTP/1.1 %d %s\r\n"
        "Content-Type: text/plain\r\n"
        "Content-Length: %zu\r\n"
        "Connection: close\r\n"
        "\r\n"
        "%s\n",
        code, msg, strlen(msg) + 1, msg);
    write_all(fd, buf, (size_t)n);
}

/* ─── Request body → git-http-backend stdin ─────────────────────────────── */

/* Copy exactly `len` bytes (or until EOF if len < 0) */
static int copy_body_plain(Reader *rd, int out, long len) {
    char buf[BUF_SIZE];
    while (len != 0) {
        size_t want = sizeof(buf);
        if (len > 0 && (long)want > len) want = (size_t)len;
        ssize_t r = reader_read(rd, buf, want);
        if (r <= 0) return len < 0 ? 0 : -1;
        if (write_all(out, buf, (size_t)r) < 0) return -1;
        if (len > 0) len -= r;
    }
    return 0;
}

/* Decode "Transfer-Encoding: chunked" (git uses it for bodies > 1 MiB) */
static int copy_body_chunked(Reader *rd, int out) {
    char line[256];
    char buf[BUF_SIZE];
    for (;;) {
        if (reader_line(rd, line, sizeof(line)) < 0) return -1;
        char *end;
        unsigned long size = strtoul(line, &end, 16);   /* ignores ;ext */
        if (end == line) return -1;
        if (size == 0) {
            /* trailers until blank line */
            ssize_t n;
            while ((n = reader_line(rd, line, sizeof(line))) > 0) ;
            return 0;
        }
        while (size > 0) {
            size_t want = size < sizeof(buf) ? size : sizeof(buf);
            ssize_t r = reader_read(rd, buf, want);
            if (r <= 0) return -1;
            if (write_all(out, buf, (size_t)r) < 0) return -1;
            size -= (unsigned long)r;
        }
        if (reader_line(rd, line, sizeof(line)) != 0) return -1; /* CRLF */
    }
}

/* ─── CGI: spawn git-http-backend ────────────────────────────────────────── */

/* Export every request header as HTTP_<NAME> like any CGI server does.
 * This is what makes gzip bodies (HTTP_CONTENT_ENCODING) and protocol v2
 * (HTTP_GIT_PROTOCOL) work. */
static void export_headers(const HttpRequest *req) {
    for (int i = 0; i < req->nheaders; i++) {
        const char *line  = req->headers[i];
        const char *colon = strchr(line, ':');
        if (!colon || colon == line) continue;

        char name[256] = "HTTP_";
        size_t k = 5;
        for (const char *p = line; p < colon && k < sizeof(name) - 1; p++) {
            unsigned char c = (unsigned char)*p;
            if (isalnum(c))      name[k++] = (char)toupper(c);
            else if (c == '-')   name[k++] = '_';
            else { k = 0; break; }          /* weird header name: skip */
        }
        if (k <= 5) continue;
        name[k] = '\0';

        /* passed separately / must not be forwarded */
        if (!strcmp(name, "HTTP_CONTENT_TYPE")   ||
            !strcmp(name, "HTTP_CONTENT_LENGTH") ||
            !strcmp(name, "HTTP_PROXY")          ||   /* httpoxy */
            !strcmp(name, "HTTP_AUTHORIZATION"))
            continue;

        const char *v = colon + 1;
        while (*v == ' ' || *v == '\t') v++;
        setenv(name, v, 1);
    }
}

static void exec_backend(void) {
    execl("/usr/lib/git-core/git-http-backend", "git-http-backend", (char *)NULL);
    execl("/usr/libexec/git-core/git-http-backend", "git-http-backend", (char *)NULL);
    execlp("git", "git", "http-backend", (char *)NULL);
    perror("exec git-http-backend");
    _exit(127);
}

static long now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec * 1000L + ts.tv_nsec / 1000000L;
}

static void describe_exit(int wstatus, char *buf, size_t n) {
    if (WIFEXITED(wstatus))        snprintf(buf, n, "exit=%d", WEXITSTATUS(wstatus));
    else if (WIFSIGNALED(wstatus)) snprintf(buf, n, "signal=%d", WTERMSIG(wstatus));
    else                           snprintf(buf, n, "status=%d", wstatus);
}

static void run_git_backend(int client_fd, Reader *rd, const HttpRequest *req,
                            const char *repo_root, const char *client_ip,
                            int server_port)
{
    long t_start = now_ms();
    int stdin_pipe[2], stdout_pipe[2];
    if (pipe(stdin_pipe) < 0) {
        send_error(client_fd, 500, "Internal Server Error (pipe)");
        return;
    }
    if (pipe(stdout_pipe) < 0) {
        close(stdin_pipe[0]); close(stdin_pipe[1]);
        send_error(client_fd, 500, "Internal Server Error (pipe)");
        return;
    }

    pid_t backend = fork();
    if (backend < 0) {
        send_error(client_fd, 500, "Internal Server Error (fork)");
        close(stdin_pipe[0]);  close(stdin_pipe[1]);
        close(stdout_pipe[0]); close(stdout_pipe[1]);
        return;
    }

    if (backend == 0) {
        /* ── git-http-backend process ── */
        dup2(stdin_pipe[0],  STDIN_FILENO);
        dup2(stdout_pipe[1], STDOUT_FILENO);
        close(stdin_pipe[0]);  close(stdin_pipe[1]);
        close(stdout_pipe[0]); close(stdout_pipe[1]);
        close(client_fd);

        export_headers(req);

        setenv("GIT_PROJECT_ROOT",    repo_root,     1);
        setenv("GIT_HTTP_EXPORT_ALL", "1",           1);
        setenv("GATEWAY_INTERFACE",   "CGI/1.1",     1);
        setenv("PATH_INFO",           req->path,     1);
        setenv("QUERY_STRING",        req->query,    1);
        setenv("REQUEST_METHOD",      req->method,   1);
        setenv("SERVER_PROTOCOL",     req->version,  1);
        setenv("REMOTE_ADDR",         client_ip,     1);
        char port[16];
        snprintf(port, sizeof(port), "%d", server_port);
        setenv("SERVER_PORT", port, 1);

        unsetenv("CONTENT_TYPE");
        unsetenv("CONTENT_LENGTH");
        if (req->content_type[0])
            setenv("CONTENT_TYPE", req->content_type, 1);
        if (req->content_length >= 0) {
            char cl[32];
            snprintf(cl, sizeof(cl), "%ld", req->content_length);
            setenv("CONTENT_LENGTH", cl, 1);
        }
        /* chunked: no CONTENT_LENGTH -> git-http-backend reads until EOF */

        setenv("HOME", "/tmp", 1);
        exec_backend();
    }

    close(stdin_pipe[0]);
    close(stdout_pipe[1]);

    /*
     * Feed the request body from a separate process. Previously the whole
     * body was written first and the response read afterwards, which can
     * deadlock once the backend fills its 64 KiB output pipe while we are
     * still blocked writing its input. With a separate feeder both
     * directions flow independently.
     */
    int has_body = req->chunked || req->content_length > 0;
    pid_t feeder = -1;

    int body_ok = 1;

    if (has_body) {
        if (req->expect_continue)
            write_all(client_fd, "HTTP/1.1 100 Continue\r\n\r\n", 25);
    }

    if (has_body && !req->chunked && req->content_length <= INLINE_BODY_MAX) {
        /* small body (ls-refs, short negotiations): fits into the empty
         * pipe, so no deadlock is possible and we save one process */
        body_ok = copy_body_plain(rd, stdin_pipe[1], req->content_length) == 0;
    } else if (has_body) {
        feeder = fork();
        if (feeder == 0) {
            close(stdout_pipe[0]);
            int rc = req->chunked
                   ? copy_body_chunked(rd, stdin_pipe[1])
                   : copy_body_plain(rd, stdin_pipe[1], req->content_length);
            close(stdin_pipe[1]);
            _exit(rc == 0 ? 0 : 1);
        }
        if (feeder < 0) {
            /* fall back to inline copy */
            body_ok = (req->chunked
                       ? copy_body_chunked(rd, stdin_pipe[1])
                       : copy_body_plain(rd, stdin_pipe[1], req->content_length)) == 0;
        }
    }
    close(stdin_pipe[1]);   /* backend sees EOF once the feeder is done */

    /* ── Read CGI headers from backend ── */
    Reader out;
    reader_init(&out, stdout_pipe[0]);

    char resp[BUF_SIZE];
    size_t rlen = 0;
    int  status_code = 200;
    char status_msg[64] = "OK";
    int  got_headers = 0;
    char line[MAX_HEADER_LEN];
    char hdrs[MAX_HEADERS * 256];
    size_t hlen = 0;

    for (;;) {
        ssize_t n = reader_line(&out, line, sizeof(line));
        if (n < 0) break;                 /* backend died before headers end */
        if (n == 0) { got_headers = 1; break; }

        const char *v;
        if ((v = header_value(line, "Status"))) {
            if (sscanf(v, "%d %63[^\r\n]", &status_code, status_msg) < 1)
                status_code = 500;
        } else if (hlen + (size_t)n + 2 < sizeof(hdrs)) {
            memcpy(hdrs + hlen, line, (size_t)n);
            hlen += (size_t)n;
            hdrs[hlen++] = '\r'; hdrs[hlen++] = '\n';
        }
    }

    long sent = 0;
    int  client_ok = 1;

    if (!got_headers) {
        status_code = 502;
        send_error(client_fd, 502, "Bad Gateway (git-http-backend failed)");
    } else {
        rlen = (size_t)snprintf(resp, sizeof(resp),
                   "HTTP/1.1 %d %s\r\nConnection: close\r\n",
                   status_code, status_msg);
        int ok = write_all(client_fd, resp, rlen) >= 0 &&
                 write_all(client_fd, hdrs, hlen) >= 0 &&
                 write_all(client_fd, "\r\n", 2) >= 0;

        /* stream body (starting with already-buffered bytes) */
        ssize_t r;
        while (ok && (r = reader_read(&out, resp, sizeof(resp))) > 0) {
            ok = write_all(client_fd, resp, (size_t)r) >= 0;
            if (ok) sent += r;
        }
        client_ok = ok;
        /* keep draining so the backend never blocks on a dead client */
        while ((r = reader_read(&out, resp, sizeof(resp))) > 0) ;
    }
    close(stdout_pipe[0]);

    int bstatus = 0;
    waitpid(backend, &bstatus, 0);

    if (feeder > 0) {
        /* the backend is gone; a feeder still running has nothing left to do */
        int fstatus = 0;
        if (waitpid(feeder, &fstatus, WNOHANG) == 0) {
            kill(feeder, SIGTERM);
            waitpid(feeder, &fstatus, 0);
        } else if (!(WIFEXITED(fstatus) && WEXITSTATUS(fstatus) == 0)) {
            body_ok = 0;
        }
    }

    char bdesc[32];
    describe_exit(bstatus, bdesc, sizeof(bdesc));
    int backend_ok = WIFEXITED(bstatus) && WEXITSTATUS(bstatus) == 0;

    log_msg("%s %s %s%s%s -> %d  %ld bytes  %ld ms%s%s%s%s%s",
            client_ip, req->method, req->path,
            req->query[0] ? "?" : "", req->query, status_code,
            sent, now_ms() - t_start,
            backend_ok ? "" : "  BACKEND ", backend_ok ? "" : bdesc,
            body_ok    ? "" : "  REQUEST-BODY-INCOMPLETE",
            client_ok  ? "" : "  CLIENT-DISCONNECTED",
            req->chunked ? "  (chunked)" : "");
}

/*
 * Close the connection gracefully. close() on a socket that still has
 * unread input makes the kernel send a TCP RST, and an RST can destroy the
 * tail of the response that is still travelling to the client ->
 * "the remote end hung up unexpectedly". So: send FIN, let the client read
 * everything and close its side, then close ours.
 */
static void graceful_close(int fd) {
    shutdown(fd, SHUT_WR);
    char buf[4096];
    long deadline = now_ms() + LINGER_MS;
    for (;;) {
        long left = deadline - now_ms();
        if (left <= 0) break;
        struct pollfd pfd = { .fd = fd, .events = POLLIN };
        int pr = poll(&pfd, 1, (int)left);
        if (pr < 0 && errno == EINTR) continue;
        if (pr <= 0) break;
        ssize_t n = read(fd, buf, sizeof(buf));
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) break;                  /* client closed: done */
    }
    close(fd);
}

/* ─── Request validation ─────────────────────────────────────────────────── */

static int is_safe_path(const char *path) {
    if (path[0] != '/')     return 0;
    if (strstr(path, "..")) return 0;
    if (strstr(path, "//")) return 0;
    return 1;
}

static int is_git_endpoint(const char *path) {
    return (strstr(path, "/info/refs")          != NULL ||
            strstr(path, "/git-upload-pack")    != NULL ||
            strstr(path, "/git-receive-pack")   != NULL ||
            strstr(path, "/objects/")           != NULL ||
            strstr(path, "/HEAD")               != NULL);
}

/* ─── Connection handler ─────────────────────────────────────────────────── */

static void handle_connection(int client_fd, const char *repo_root,
                              const char *client_ip, int server_port)
{
    struct timeval tv = { .tv_sec = IO_TIMEOUT_SEC, .tv_usec = 0 };
    setsockopt(client_fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    setsockopt(client_fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));

    /* heap instead of stack: HttpRequest is ~270 KiB */
    HttpRequest *req = malloc(sizeof(*req));
    Reader      *rd  = malloc(sizeof(*rd));
    if (!req || !rd) { send_error(client_fd, 500, "Out of memory"); return; }
    reader_init(rd, client_fd);

    if (parse_request(rd, req) < 0) {
        log_msg("%s -> 400 (malformed request, timeout or connection closed early: %s)",
                client_ip, errno ? strerror(errno) : "eof");
        send_error(client_fd, 400, "Bad Request");
        return;
    }
    if (!is_safe_path(req->path)) {
        log_msg("%s %s %s -> 400 (unsafe path)", client_ip, req->method, req->path);
        send_error(client_fd, 400, "Bad Request (path traversal)");
        return;
    }
    if (!is_git_endpoint(req->path)) {
        log_msg("%s %s %s -> 404", client_ip, req->method, req->path);
        send_error(client_fd, 404, "Not Found");
        return;
    }
    if (strcmp(req->method, "GET") != 0 && strcmp(req->method, "POST") != 0) {
        log_msg("%s %s %s -> 405", client_ip, req->method, req->path);
        send_error(client_fd, 405, "Method Not Allowed");
        return;
    }

    run_git_backend(client_fd, rd, req, repo_root, client_ip, server_port);
}

/* ─── Main / accept loop ─────────────────────────────────────────────────── */

static void on_sigchld(int sig) {
    (void)sig;
    int saved = errno;
    while (waitpid(-1, NULL, WNOHANG) > 0) ;
    errno = saved;
}

int main(int argc, char *argv[]) {
    if (argc != 3) {
        fprintf(stderr, "Usage: %s <port> <repo_root>\n", argv[0]);
        fprintf(stderr, "  e.g. %s 8080 /srv/git\n", argv[0]);
        return 1;
    }

    int port = atoi(argv[1]);
    const char *repo_root = argv[2];

    signal(SIGPIPE, SIG_IGN);

    /* Reap finished connection handlers immediately (no zombies) */
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = on_sigchld;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = SA_RESTART | SA_NOCLDSTOP;
    sigaction(SIGCHLD, &sa, NULL);

    int srv = socket(AF_INET, SOCK_STREAM, 0);
    if (srv < 0) die("socket");

    int opt = 1;
    setsockopt(srv, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    struct sockaddr_in addr = {
        .sin_family      = AF_INET,
        .sin_port        = htons((uint16_t)port),
        .sin_addr.s_addr = INADDR_ANY,
    };
    if (bind(srv, (struct sockaddr *)&addr, sizeof(addr)) < 0) die("bind");
    if (listen(srv, SOMAXCONN) < 0) die("listen");

    printf("git-http-server listening on port %d\n", port);
    printf("Serving repos from: %s\n", repo_root);
    printf("Clone with: git clone http://localhost:%d/<repo>.git\n\n", port);
    fflush(stdout);

    for (;;) {
        struct sockaddr_in client_addr;
        socklen_t addrlen = sizeof(client_addr);
        int client_fd = accept(srv, (struct sockaddr *)&client_addr, &addrlen);
        if (client_fd < 0) {
            if (errno == EINTR || errno == ECONNABORTED) continue;
            perror("accept");
            if (errno == EMFILE || errno == ENFILE) sleep(1);
            continue;
        }

        char client_ip[INET_ADDRSTRLEN];
        inet_ntop(AF_INET, &client_addr.sin_addr, client_ip, sizeof(client_ip));

        pid_t pid = fork();
        if (pid < 0) {
            perror("fork");
            send_error(client_fd, 503, "Service Unavailable");
            close(client_fd);
            continue;
        }
        if (pid == 0) {
            /* the handler waits for its own children itself */
            signal(SIGCHLD, SIG_DFL);
            close(srv);
            errno = 0;
            handle_connection(client_fd, repo_root, client_ip, port);
            graceful_close(client_fd);
            _exit(0);
        }
        close(client_fd);
    }
}
