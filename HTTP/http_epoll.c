/* http_epoll.c
 *
 * 基于 epoll 的 HTTP 服务器（由阻塞单线程版 http.c 改造而来）
 *
 * 改造要点：
 *   1. 所有 socket（监听 + 客户端）都设为非阻塞；
 *   2. 用 epoll 统一管理所有 fd 的可读 / 可写事件；
 *   3. 非阻塞 read 一次可能读不完整请求 → 每个连接维护读缓冲区，
 *      收到 "\r\n\r\n"（请求头结束）后才解析；
 *   4. 非阻塞 write 一次可能写不完 → 响应先写入连接的发送缓冲区，
 *      写不完时注册 EPOLLOUT，等可写了继续发；大文件分块流式发送；
 *   5. 默认水平触发(LT)，但读/写/accept 都按"循环到 EAGAIN"编写，
 *      想切边缘触发(ET) 只需在注册事件时加上 EPOLLET。
 *
 * 编译：gcc -O2 -Wall -o http_epoll http_epoll.c
 */
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <strings.h>
#include <signal.h>
#include <fcntl.h>
#include <errno.h>
#include <sys/epoll.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#define PORT       8080
#define BACKLOG    128
#define BUF_SIZE   8192
#define WEB_ROOT   "./www"
#define MAX_EVENTS 1024

/* ---------------- 每个连接的状态（epoll 模型的核心） ---------------- */
typedef struct {
    int  fd;
    int  is_listener;          /* 1 = 监听 socket，0 = 客户端连接 */

    /* 读侧：非阻塞 read 可能多次才凑齐一个完整请求头 */
    char rbuf[BUF_SIZE];
    int  rlen;                 /* rbuf 中已读字节数 */

    /* 写侧：响应先放这里，写不完就等 EPOLLOUT 继续 */
    char wbuf[BUF_SIZE];
    int  wlen;                 /* wbuf 中待发送字节数 */
    int  wsent;                /* wbuf 中已发送字节数 */

    /* 静态文件流式发送状态：不在内存里装整个文件 */
    FILE *fp;                  /* 正在发送的文件，NULL 表示没有 */
    long remaining;            /* 文件体剩余未读字节数 */

    int  closing;              /* 响应发完后关闭连接（Connection: close） */
} conn_t;

typedef struct {
    char method[16];
    char path[1024];
    char version[16];
    char host[256];
    long content_length;
} HttpRequest;

/* flush 的返回值 */
enum { FLUSH_DONE = 0, FLUSH_AGAIN = 1, FLUSH_CLOSE = 2 };

int  init_server_socket(void);
int  set_nonblocking(int fd);
int  parse_request(const char *buf, HttpRequest *req);
const char *get_mime_type(const char *path);

conn_t *conn_new(int fd, int is_listener);
void    conn_close(int epfd, conn_t *c);
int     conn_queue(conn_t *c, const char *data, int len);
int     conn_flush(conn_t *c);
void    queue_response_header(conn_t *c, int code, const char *msg,
                              const char *mime, long len);
void    queue_error(conn_t *c, int code, const char *msg);
void    serve_static_file(conn_t *c, const char *path, int head_only);
void    handle_dynamic_api(conn_t *c, const char *path);
void    process_request(int epfd, conn_t *c);
void    handle_accept(int epfd, int server_fd);
void    handle_read(int epfd, conn_t *c);
void    handle_write(int epfd, conn_t *c);

/* ---------------- 基础工具 ---------------- */

int set_nonblocking(int fd)
{
    int flags = fcntl(fd, F_GETFL, 0);
    if (flags < 0) return -1;
    return fcntl(fd, F_SETFL, flags | O_NONBLOCK);
}

int init_server_socket(void)
{
    int server_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (server_fd < 0) {
        perror("socket 创建失败");
        return -1;
    }

    /* 端口复用：避免服务器重启后端口处于 TIME_WAIT 导致 bind 失败 */
    int opt = 1;
    setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR,
               (const char *)&opt, sizeof(opt));

    struct sockaddr_in address;
    memset(&address, 0, sizeof(address));
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = INADDR_ANY;
    address.sin_port = htons(PORT);

    if (bind(server_fd, (struct sockaddr *)&address, sizeof(address)) < 0) {
        perror("bind 失败（端口可能被占用）");
        close(server_fd);
        return -1;
    }

    if (listen(server_fd, BACKLOG) < 0) {
        perror("listen 失败");
        close(server_fd);
        return -1;
    }

    /* 监听 socket 也必须非阻塞，否则 ET 模式下 accept 循环会卡死 */
    if (set_nonblocking(server_fd) < 0) {
        perror("fcntl 设置非阻塞失败");
        close(server_fd);
        return -1;
    }

    printf("HTTP 服务器已启动(epoll): http://localhost:%d (根目录 %s)\n",
           PORT, WEB_ROOT);
    return server_fd;
}

/* ---------------- 连接对象管理 ---------------- */

conn_t *conn_new(int fd, int is_listener)
{
    conn_t *c = calloc(1, sizeof(conn_t));
    if (c == NULL) return NULL;
    c->fd = fd;
    c->is_listener = is_listener;
    return c;
}

/* 关闭连接：先从 epoll 摘除，再释放资源（顺序不能反） */
void conn_close(int epfd, conn_t *c)
{
    if (c == NULL) return;
    epoll_ctl(epfd, EPOLL_CTL_DEL, c->fd, NULL);
    if (c->fp != NULL) fclose(c->fp);
    close(c->fd);
    free(c);
}

/* 把数据追加到发送缓冲区 */
int conn_queue(conn_t *c, const char *data, int len)
{
    if (len > (int)sizeof(c->wbuf) - c->wlen) {
        /* 响应头 + 小 body 远大于 BUF_SIZE 的情况不会出现；
         * 大文件走 fp 流式发送，不进 wbuf */
        return -1;
    }
    memcpy(c->wbuf + c->wlen, data, len);
    c->wlen += len;
    return 0;
}

/* 尝试把待发数据真正发出去；大文件分块读入 wbuf 再发。
 * 返回 FLUSH_DONE（全部发完）/ FLUSH_AGAIN（socket 写满，等 EPOLLOUT）
 *      / FLUSH_CLOSE（出错，直接关连接） */
int conn_flush(conn_t *c)
{
    for (;;) {
        if (c->wsent < c->wlen) {
            /* wbuf 里还有没发完的数据，继续发 */
            int n = send(c->fd, c->wbuf + c->wsent,
                         c->wlen - c->wsent, MSG_NOSIGNAL);
            if (n > 0) {
                c->wsent += n;
            } else if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
                return FLUSH_AGAIN;          /* 内核写缓冲满了，等可写事件 */
            } else {
                return FLUSH_CLOSE;          /* 对端断开等错误 */
            }
        } else if (c->fp != NULL && c->remaining > 0) {
            /* wbuf 已发空，读下一块文件内容进来 */
            size_t want = c->remaining < (long)sizeof(c->wbuf)
                          ? (size_t)c->remaining : sizeof(c->wbuf);
            size_t r = fread(c->wbuf, 1, want, c->fp);
            if (r == 0) {                    /* 读文件出错，提前结束 */
                c->remaining = 0;
            } else {
                c->wlen = (int)r;
                c->wsent = 0;
                c->remaining -= (long)r;
            }
        } else {
            break;                           /* 没有更多数据要发 */
        }
    }
    if (c->fp != NULL) { fclose(c->fp); c->fp = NULL; }
    return c->closing ? FLUSH_CLOSE : FLUSH_DONE;
}

/* ---------------- HTTP 解析（与阻塞版相同） ---------------- */

int parse_request(const char *buf, HttpRequest *req)
{
    memset(req, 0, sizeof(HttpRequest));

    if (sscanf(buf, "%15s %1023s %15s",
               req->method, req->path, req->version) != 3) {
        return -1;
    }

    const char *line = strstr(buf, "\r\n");
    while (line != NULL) {
        line += 2;
        if (line[0] == '\r' && line[1] == '\n')
            break;
        if (strncasecmp(line, "Host:", 5) == 0) {
            sscanf(line + 5, " %255[^\r\n]", req->host);
        } else if (strncasecmp(line, "Content-Length:", 15) == 0) {
            req->content_length = atol(line + 15);
        }
        line = strstr(line, "\r\n");
    }
    return 0;
}

/* ---------------- 响应构建（不再直接 send，而是写入连接缓冲区） ---------------- */

void queue_response_header(conn_t *c, int code, const char *msg,
                           const char *mime, long len)
{
    char header[1024];
    int header_len = snprintf(header, sizeof(header),
        "HTTP/1.1 %d %s\r\n"
        "Content-Type: %s\r\n"
        "Content-Length: %ld\r\n"
        "Connection: close\r\n"
        "\r\n",
        code, msg, mime, len);
    conn_queue(c, header, header_len);
}

void queue_error(conn_t *c, int code, const char *msg)
{
    char body[256];
    int body_len = snprintf(body, sizeof(body),
        "<html><body><h1>%d %s</h1></body></html>", code, msg);
    queue_response_header(c, code, msg, "text/html", body_len);
    conn_queue(c, body, body_len);
    c->closing = 1;
}

/* 静态文件服务：响应头进 wbuf，文件体交给 conn_flush 分块流式发送 */
void serve_static_file(conn_t *c, const char *path, int head_only)
{
    if (strstr(path, "..") != NULL) {      /* 路径中含 ".." 说明想跳出根目录 */
        queue_error(c, 403, "Forbidden");
        return;
    }

    char filepath[1200];
    if (strcmp(path, "/") == 0)
        snprintf(filepath, sizeof(filepath), "%s/index.html", WEB_ROOT);
    else
        snprintf(filepath, sizeof(filepath), "%s%s", WEB_ROOT, path);

    FILE *fp = fopen(filepath, "rb");
    if (fp == NULL) {
        queue_error(c, 404, "Not Found");
        return;
    }

    struct stat st;
    stat(filepath, &st);
    long filesize = (long)st.st_size;

    queue_response_header(c, 200, "OK", get_mime_type(filepath), filesize);

    if (head_only) {
        fclose(fp);
    } else {
        c->fp = fp;                        /* 文件体由 flush 阶段流式发送 */
        c->remaining = filesize;
    }
    c->closing = 1;
}

void handle_dynamic_api(conn_t *c, const char *path)
{
    char body[512];
    int body_len;

    if (strcmp(path, "/api/hello") == 0) {
        body_len = snprintf(body, sizeof(body),
            "{\"message\": \"Hello from C HTTP Server\"}\n");
        queue_response_header(c, 200, "OK", "application/json", body_len);
    } else {
        body_len = snprintf(body, sizeof(body),
            "{\"error\": \"api not found\"}");
        queue_response_header(c, 404, "Not Found",
                              "application/json", body_len);
    }
    conn_queue(c, body, body_len);
    c->closing = 1;
}

const char *get_mime_type(const char *path)
{
    const char *dot = strrchr(path, '.');  /* 定位扩展名 */
    if (dot == NULL) return "application/octet-stream";

    if (strcmp(dot, ".html") == 0) return "text/html";
    if (strcmp(dot, ".htm")  == 0) return "text/html";
    if (strcmp(dot, ".css")  == 0) return "text/css";
    if (strcmp(dot, ".js")   == 0) return "application/javascript";
    if (strcmp(dot, ".json") == 0) return "application/json";
    if (strcmp(dot, ".png")  == 0) return "image/png";
    if (strcmp(dot, ".jpg")  == 0) return "image/jpeg";
    if (strcmp(dot, ".gif")  == 0) return "image/gif";
    if (strcmp(dot, ".txt")  == 0) return "text/plain";
    return "application/octet-stream";     /* 未知类型按下载处理 */
}

/* ---------------- epoll 事件处理 ---------------- */

/* 请求头已完整，解析并构建响应 */
void process_request(int epfd, conn_t *c)
{
    HttpRequest req;
    if (parse_request(c->rbuf, &req) != 0) {
        queue_error(c, 400, "Bad Request");
    } else {
        printf("%s %s (Host: %s, Content-Length: %ld)\n",
               req.method, req.path, req.host, req.content_length);

        int head_only = (strcmp(req.method, "HEAD") == 0);

        if (strcmp(req.method, "GET") != 0 && !head_only) {
            queue_error(c, 405, "Method Not Allowed");
        } else if (strncmp(req.path, "/api/", 5) == 0) {
            handle_dynamic_api(c, req.path);
        } else {
            serve_static_file(c, req.path, head_only);
        }
    }

    /* 先尝试直接发，发不完再订阅 EPOLLOUT */
    int r = conn_flush(c);
    if (r == FLUSH_CLOSE) {
        conn_close(epfd, c);
        return;
    }
    if (r == FLUSH_AGAIN) {
        struct epoll_event ev;
        ev.events = EPOLLOUT;              /* 只关心可写，直到发完 */
        ev.data.ptr = c;
        epoll_ctl(epfd, EPOLL_CTL_MOD, c->fd, &ev);
    }
}

/* 监听 socket 可读 → 有新连接。
 * 循环 accept 到 EAGAIN：LT 下不是必须，但这样写之后
 * 给事件加 EPOLLET 切边缘触发也能直接正常工作 */
void handle_accept(int epfd, int server_fd)
{
    for (;;) {
        struct sockaddr_in client_addr;
        socklen_t addr_len = sizeof(client_addr);
        int client_fd = accept(server_fd,
                               (struct sockaddr *)&client_addr, &addr_len);
        if (client_fd < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK)
                break;                     /* 没有更多新连接了 */
            perror("accept 失败");
            break;
        }

        set_nonblocking(client_fd);

        conn_t *c = conn_new(client_fd, 0);
        if (c == NULL) { close(client_fd); continue; }

        struct epoll_event ev;
        ev.events = EPOLLIN;               /* 想切 ET：EPOLLIN | EPOLLET */
        ev.data.ptr = c;                   /* ptr 挂连接状态，比 fd 可靠 */
        if (epoll_ctl(epfd, EPOLL_CTL_ADD, client_fd, &ev) < 0) {
            perror("epoll_ctl ADD 失败");
            conn_close(epfd, c);
        }
    }
}

/* 客户端连接可读 → 收数据，凑齐请求头后处理 */
void handle_read(int epfd, conn_t *c)
{
    for (;;) {
        if (c->rlen >= BUF_SIZE - 1) {     /* 缓冲区满还没看到头结束 */
            queue_error(c, 400, "Bad Request");
            conn_flush(c);
            conn_close(epfd, c);
            return;
        }
        int n = recv(c->fd, c->rbuf + c->rlen, BUF_SIZE - 1 - c->rlen, 0);
        if (n > 0) {
            c->rlen += n;
            c->rbuf[c->rlen] = '\0';

            /* 非阻塞 read 一次不一定拿到完整请求头，
             * 每次读到数据就检查 "\r\n\r\n"，凑齐才解析，
             * 否则继续读 / 等下一次 EPOLLIN */
            if (strstr(c->rbuf, "\r\n\r\n") != NULL) {
                process_request(epfd, c);  /* 可能关闭并释放 c */
                return;
            }
        } else if (n == 0) {
            conn_close(epfd, c);           /* 对端正常关闭 */
            return;
        } else {
            if (errno == EAGAIN || errno == EWOULDBLOCK)
                return;                    /* 数据读完了，等下次事件 */
            if (errno == EINTR)
                continue;                  /* 被信号打断，重试 */
            conn_close(epfd, c);           /* 真正的读错误 */
            return;
        }
    }
}

/* 客户端连接可写 → 继续发送剩余响应 */
void handle_write(int epfd, conn_t *c)
{
    int r = conn_flush(c);
    if (r == FLUSH_CLOSE) {
        conn_close(epfd, c);
    } else if (r == FLUSH_DONE) {
        /* 发完了但 closing == 0 的情况（预留 keep-alive 扩展）：
         * 把事件改回 EPOLLIN 等下一个请求 */
        struct epoll_event ev;
        ev.events = EPOLLIN;
        ev.data.ptr = c;
        epoll_ctl(epfd, EPOLL_CTL_MOD, c->fd, &ev);
    }
    /* FLUSH_AGAIN：还没发完，继续等下一次 EPOLLOUT */
}

int main(void)
{
    /* 日志行缓冲 */
    setvbuf(stdout, NULL, _IOLBF, 0);

    /* 写已关闭的连接会触发 SIGPIPE 让进程崩溃，必须忽略
     *（send 同时用了 MSG_NOSIGNAL，双保险） */
    signal(SIGPIPE, SIG_IGN);

    int server_fd = init_server_socket();
    if (server_fd < 0) return 1;

    int epfd = epoll_create1(0);
    if (epfd < 0) {
        perror("epoll_create1 失败");
        return 1;
    }

    /* 监听 socket 也包成 conn_t，统一用 data.ptr 处理 */
    conn_t *listener = conn_new(server_fd, 1);
    if (listener == NULL) return 1;

    struct epoll_event ev;
    ev.events = EPOLLIN;
    ev.data.ptr = listener;
    if (epoll_ctl(epfd, EPOLL_CTL_ADD, server_fd, &ev) < 0) {
        perror("epoll_ctl 注册监听 socket 失败");
        return 1;
    }

    struct epoll_event events[MAX_EVENTS];
    for (;;) {
        int n = epoll_wait(epfd, events, MAX_EVENTS, -1);
        if (n < 0) {
            if (errno == EINTR) continue;  /* 被信号打断 */
            perror("epoll_wait 失败");
            break;
        }

        for (int i = 0; i < n; i++) {
            conn_t *c = events[i].data.ptr;

            if (c->is_listener) {
                handle_accept(epfd, c->fd);
                continue;
            }

            /* 出错或对端挂断：直接回收连接 */
            if (events[i].events & (EPOLLERR | EPOLLHUP)) {
                conn_close(epfd, c);
                continue;
            }

            if (events[i].events & EPOLLIN)
                handle_read(epfd, c);      /* 可能在内部关闭并 free(c) */
            else if (events[i].events & EPOLLOUT)
                handle_write(epfd, c);
        }
    }

    conn_close(epfd, listener);
    close(epfd);
    return 0;
}
