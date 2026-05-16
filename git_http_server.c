/*
 * git_http_server.c
 *
 * A minimal HTTP server that serves Git repositories via the smart HTTP
 * protocol by spawning git-http-backend as a CGI process.
 *
 * Supports: git clone/fetch/push over HTTP
 * SSH is handled by OpenSSH + git-shell (see README)
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
#define _DEFAULT_SOURCE  /* strncasecmp */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <signal.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <fcntl.h>
#include <ctype.h>

#define BACKLOG         32
#define BUF_SIZE        65536
#define MAX_HEADERS     64
#define MAX_HEADER_LEN  4096
#define MAX_PATH_LEN    4096

/* ─── Utility ────────────────────────────────────────────────────────────── */

static void die(const char *msg) {
    perror(msg);
    exit(1);
}

/* URL-decode src into dst (dst must be at least strlen(src)+1 bytes) */
static void url_decode(char *dst, const char *src) {
    while (*src) {
        if (*src == '%' && isxdigit((unsigned char)src[1])
                        && isxdigit((unsigned char)src[2])) {
            char hex[3] = { src[1], src[2], '\0' };
            *dst++ = (char)strtol(hex, NULL, 16);
            src += 3;
        } else if (*src == '+') {
            *dst++ = ' ';
            src++;
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

/* Read a line (up to maxlen-1 chars) from fd into buf; returns bytes read */
static ssize_t read_line(int fd, char *buf, size_t maxlen) {
    size_t i = 0;
    while (i < maxlen - 1) {
        char c;
        ssize_t r = read(fd, &c, 1);
        if (r <= 0) break;
        buf[i++] = c;
        if (c == '\n') break;
    }
    buf[i] = '\0';
    return (ssize_t)i;
}

/* ─── HTTP request parsing ───────────────────────────────────────────────── */

typedef struct {
    char method[16];
    char path[MAX_PATH_LEN];       /* URL path, decoded */
    char query[MAX_PATH_LEN];      /* query string, raw */
    char version[16];

    /* selected headers we care about */
    char content_type[256];
    long content_length;           /* -1 if absent */
    char user_agent[512];
    char host[256];

    /* raw header lines for forwarding */
    char headers[MAX_HEADERS][MAX_HEADER_LEN];
    int  nheaders;
} HttpRequest;

/* Returns 0 on success, -1 on malformed request */
static int parse_request(int fd, HttpRequest *req) {
    memset(req, 0, sizeof(*req));
    req->content_length = -1;

    /* Request line */
    char line[MAX_HEADER_LEN];
    if (read_line(fd, line, sizeof(line)) <= 0) return -1;

    char raw_path[MAX_PATH_LEN];
    if (sscanf(line, "%15s %4095s %15s", req->method, raw_path, req->version) != 3)
        return -1;

    /* Split path and query string */
    char *q = strchr(raw_path, '?');
    if (q) {
        *q = '\0';
        strncpy(req->query, q + 1, sizeof(req->query) - 1);
    }
    url_decode(req->path, raw_path);

    /* Headers */
    while (1) {
        ssize_t n = read_line(fd, line, sizeof(line));
        if (n <= 0) break;
        /* strip \r\n */
        while (n > 0 && (line[n-1] == '\n' || line[n-1] == '\r')) line[--n] = '\0';
        if (n == 0) break; /* blank line = end of headers */

        if (req->nheaders < MAX_HEADERS)
            snprintf(req->headers[req->nheaders++], MAX_HEADER_LEN, "%s", line);

        /* Parse known headers */
        if (strncasecmp(line, "Content-Type:", 13) == 0)
            snprintf(req->content_type, sizeof(req->content_type), "%s", line + 14);
        else if (strncasecmp(line, "Content-Length:", 15) == 0)
            req->content_length = atol(line + 16);
        else if (strncasecmp(line, "User-Agent:", 11) == 0)
            snprintf(req->user_agent, sizeof(req->user_agent), "%s", line + 12);
        else if (strncasecmp(line, "Host:", 5) == 0)
            snprintf(req->host, sizeof(req->host), "%s", line + 6);
    }
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

/* ─── CGI: spawn git-http-backend ────────────────────────────────────────── */

/*
 * Pipes request body to git-http-backend's stdin,
 * reads CGI response from its stdout, translates the CGI headers
 * into a proper HTTP response, and streams it back to the client.
 */
static void run_git_backend(int client_fd,
                             const HttpRequest *req,
                             const char *repo_root)
{
    /* stdin pipe:  parent writes request body  → child stdin  */
    /* stdout pipe: child writes CGI response   → parent reads */
    int stdin_pipe[2], stdout_pipe[2];
    if (pipe(stdin_pipe) < 0 || pipe(stdout_pipe) < 0) {
        send_error(client_fd, 500, "Internal Server Error (pipe)");
        return;
    }

    pid_t pid = fork();
    if (pid < 0) {
        send_error(client_fd, 500, "Internal Server Error (fork)");
        close(stdin_pipe[0]);  close(stdin_pipe[1]);
        close(stdout_pipe[0]); close(stdout_pipe[1]);
        return;
    }

    if (pid == 0) {
        /* ── Child: set up CGI environment and exec git-http-backend ── */
        close(stdin_pipe[1]);   /* close write end of stdin pipe  */
        close(stdout_pipe[0]);  /* close read end of stdout pipe  */

        dup2(stdin_pipe[0],  STDIN_FILENO);
        dup2(stdout_pipe[1], STDOUT_FILENO);
        close(stdin_pipe[0]);
        close(stdout_pipe[1]);

        /* Mandatory CGI variables for git-http-backend */
        setenv("GIT_PROJECT_ROOT",   repo_root,        1);
        setenv("GIT_HTTP_EXPORT_ALL","1",               1);
        setenv("PATH_INFO",           req->path,        1);
        setenv("QUERY_STRING",        req->query,       1);
        setenv("REQUEST_METHOD",      req->method,      1);
        setenv("SERVER_PROTOCOL",     req->version,     1);

        if (req->content_type[0])
            setenv("CONTENT_TYPE", req->content_type, 1);

        if (req->content_length >= 0) {
            char cl[32];
            snprintf(cl, sizeof(cl), "%ld", req->content_length);
            setenv("CONTENT_LENGTH", cl, 1);
        }

        if (req->host[0])
            setenv("HTTP_HOST", req->host, 1);

        if (req->user_agent[0])
            setenv("HTTP_USER_AGENT", req->user_agent, 1);

        /* Optional: basic auth forwarding */
        /* setenv("REMOTE_USER", "alice", 1); */

        /* Suppress git from trying to chdir into something weird */
        setenv("HOME", "/tmp", 1);

        execl("/usr/lib/git-core/git-http-backend",
              "git-http-backend", NULL);
        /* fallback path on some distros */
        execl("/usr/libexec/git-core/git-http-backend",
              "git-http-backend", NULL);
        perror("execl git-http-backend");
        _exit(1);
    }

    /* ── Parent: pump data in both directions ── */
    close(stdin_pipe[0]);   /* close read end  (child's stdin)  */
    close(stdout_pipe[1]);  /* close write end (child's stdout) */

    int write_fd  = stdin_pipe[1];
    int read_fd   = stdout_pipe[0];

    /* Forward request body → git-http-backend stdin */
    if (req->content_length > 0) {
        long remaining = req->content_length;
        char buf[BUF_SIZE];
        while (remaining > 0) {
            ssize_t to_read = (remaining < (long)sizeof(buf))
                              ? remaining : (long)sizeof(buf);
            ssize_t r = read(client_fd, buf, (size_t)to_read);
            if (r <= 0) break;
            if (write_all(write_fd, buf, (size_t)r) < 0) break;
            remaining -= r;
        }
    }
    close(write_fd);

    /*
     * Read CGI response from git-http-backend.
     * CGI format:
     *   Header: value\r\n
     *   ...\r\n
     *   \r\n
     *   <body>
     *
     * We must convert the CGI "Status:" header into an HTTP status line.
     */
    char cgi_headers[MAX_HEADERS][MAX_HEADER_LEN];
    int  ncgi = 0;
    int  status_code = 200;
    char status_msg[64] = "OK";

    /* Read CGI headers */
    char line[MAX_HEADER_LEN];
    while (1) {
        ssize_t n = read_line(read_fd, line, sizeof(line));
        if (n <= 0) break;
        while (n > 0 && (line[n-1] == '\n' || line[n-1] == '\r')) line[--n] = '\0';
        if (n == 0) break;

        if (strncasecmp(line, "Status:", 7) == 0) {
            sscanf(line + 8, "%d %63[^\r\n]", &status_code, status_msg);
        } else {
            if (ncgi < MAX_HEADERS)
                snprintf(cgi_headers[ncgi++], MAX_HEADER_LEN, "%s", line);
        }
    }

    /* Emit HTTP response status line */
    {
        char status_line[128];
        int slen = snprintf(status_line, sizeof(status_line),
            "HTTP/1.1 %d %s\r\n", status_code, status_msg);
        write_all(client_fd, status_line, (size_t)slen);
    }

    /* Emit headers */
    for (int i = 0; i < ncgi; i++) {
        write_all(client_fd, cgi_headers[i], strlen(cgi_headers[i]));
        write_all(client_fd, "\r\n", 2);
    }
    write_all(client_fd, "\r\n", 2);

    /* Stream body */
    {
        char buf[BUF_SIZE];
        ssize_t r;
        while ((r = read(read_fd, buf, sizeof(buf))) > 0)
            write_all(client_fd, buf, (size_t)r);
    }
    close(read_fd);

    /* Reap child */
    int wstatus;
    waitpid(pid, &wstatus, 0);
}

/* ─── Request validation ─────────────────────────────────────────────────── */

/* Reject path traversal attempts */
static int is_safe_path(const char *path) {
    if (strstr(path, "..")) return 0;
    if (strstr(path, "//")) return 0;
    return 1;
}

/* Only allow git smart-HTTP endpoints */
static int is_git_endpoint(const char *path) {
    return (strstr(path, "/info/refs")          != NULL ||
            strstr(path, "/git-upload-pack")    != NULL ||
            strstr(path, "/git-receive-pack")   != NULL ||
            strstr(path, "/objects/")           != NULL ||
            strstr(path, "/HEAD")               != NULL);
}

/* ─── Connection handler ─────────────────────────────────────────────────── */

static void handle_connection(int client_fd, const char *repo_root) {
    HttpRequest req;
    if (parse_request(client_fd, &req) < 0) {
        send_error(client_fd, 400, "Bad Request");
        return;
    }

    printf("[%s] %s %s%s%s\n",
           req.host, req.method, req.path,
           req.query[0] ? "?" : "", req.query);
    fflush(stdout);

    if (!is_safe_path(req.path)) {
        send_error(client_fd, 400, "Bad Request (path traversal)");
        return;
    }

    if (!is_git_endpoint(req.path)) {
        send_error(client_fd, 404, "Not Found");
        return;
    }

    if (strcmp(req.method, "GET")  != 0 &&
        strcmp(req.method, "POST") != 0) {
        send_error(client_fd, 405, "Method Not Allowed");
        return;
    }

    run_git_backend(client_fd, &req, repo_root);
}

/* ─── Main / accept loop ─────────────────────────────────────────────────── */

int main(int argc, char *argv[]) {
    if (argc != 3) {
        fprintf(stderr, "Usage: %s <port> <repo_root>\n", argv[0]);
        fprintf(stderr, "  e.g. %s 8080 /srv/git\n", argv[0]);
        return 1;
    }

    int port = atoi(argv[1]);
    const char *repo_root = argv[2];

    /* Ignore SIGPIPE so write() returns EPIPE instead of killing us */
    signal(SIGPIPE, SIG_IGN);
    /* Reap children automatically */
    signal(SIGCHLD, SIG_DFL);

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
    if (listen(srv, BACKLOG) < 0) die("listen");

    printf("git-http-server listening on port %d\n", port);
    printf("Serving repos from: %s\n", repo_root);
    printf("Clone with: git clone http://localhost:%d/<repo>.git\n\n", port);
    fflush(stdout);

    while (1) {
        struct sockaddr_in client_addr;
        socklen_t addrlen = sizeof(client_addr);
        int client_fd = accept(srv, (struct sockaddr *)&client_addr, &addrlen);
        if (client_fd < 0) {
            if (errno == EINTR) continue;
            perror("accept");
            continue;
        }

        char client_ip[INET_ADDRSTRLEN];
        inet_ntop(AF_INET, &client_addr.sin_addr, client_ip, sizeof(client_ip));

        pid_t pid = fork();
        if (pid < 0) {
            perror("fork");
            close(client_fd);
            continue;
        }
        if (pid == 0) {
            /* child handles connection */
            close(srv);
            handle_connection(client_fd, repo_root);
            close(client_fd);
            _exit(0);
        }
        /* parent: close client fd and keep accepting */
        close(client_fd);

        /* Reap any finished children (non-blocking) */
        while (waitpid(-1, NULL, WNOHANG) > 0);
    }

    close(srv);
    return 0;
}
