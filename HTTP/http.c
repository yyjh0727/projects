#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <strings.h>
#include <signal.h>
#include <sys/time.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>


#define PORT      8080
#define BACKLOG   10
#define BUF_SIZE  8192
#define WEB_ROOT  "./www"

typedef struct {
    char method[16];
    char path[1024];
    char version[16];
    char host[256];
    long content_length;
} HttpRequest;

//初始化网络连接（socket / bind / listen）
//处理并发连接（本代码用单线程循环，多线程/epoll 见注释说明）
//解析 HTTP 请求（请求行 + 请求头）
//构建 HTTP 响应（状态行 + 响应头 + 空行 + 响应体）
//处理业务逻辑（静态文件服务 + 动态接口）
int  init_server_socket(void);
int  parse_request(const char *buf, HttpRequest *req);
void send_response_header(int fd, int code, const char *msg,
                          const char *mime, long len);
void send_error(int fd, int code, const char *msg);
void serve_static_file(int fd, const char *path,
                       int head_only);
void handle_dynamic_api(int fd, const char *path);
void handle_client(int client_fd);
const char *get_mime_type(const char *path);

int init_server_socket(void)
{
    int server_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (server_fd < 0) {
        perror("socket 创建失败");
        return -1;
    }

    //端口复用：避免服务器重启后端口处于 TIME_WAIT 导致 bind 失败
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

    printf("HTTP 服务器已启动: http://localhost:%d (根目录 %s)\n",
           PORT, WEB_ROOT);
    return server_fd;
}


//解析 HTTP 请求
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


//构建 HTTP 响应
void send_response_header(int fd, int code, const char *msg,
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
    send(fd, header, header_len, 0);
}

//错误响应（404、405 等）
void send_error(int fd, int code, const char *msg)
{
    char body[256];
    int body_len = snprintf(body, sizeof(body),
        "<html><body><h1>%d %s</h1></body></html>", code, msg);
    send_response_header(fd, code, msg, "text/html", body_len);
    send(fd, body, body_len, 0);
}

//第 5 步：处理业务逻辑 —— 静态文件服务
void serve_static_file(int fd, const char *path, int head_only)
{
    if (strstr(path, "..") != NULL) {      /* 路径中含 ".." 说明想跳出根目录 */
        send_error(fd, 403, "Forbidden");
        return;
    }

    char filepath[1200];
    if (strcmp(path, "/") == 0)
        snprintf(filepath, sizeof(filepath),
                 "%s/index.html", WEB_ROOT);
    else
        snprintf(filepath, sizeof(filepath),
                 "%s%s", WEB_ROOT, path);


    FILE *fp = fopen(filepath, "rb");
    if (fp == NULL) {
        send_error(fd, 404, "Not Found");
        return;
    }

    struct stat st;
    stat(filepath, &st);
    long filesize = (long)st.st_size;

    send_response_header(fd, 200, "OK", get_mime_type(filepath), filesize);

    if (head_only) { fclose(fp); return; }

    char file_buf[4096];
    size_t n;
    while ((n = fread(file_buf, 1, sizeof(file_buf), fp)) > 0)
        send(fd, file_buf, (int)n, 0);     /* 读多少发多少 */

    fclose(fp);
}


//处理业务逻辑 —— 动态接口
void handle_dynamic_api(int fd, const char *path)
{
    char body[512];
    int body_len;

    if (strcmp(path, "/api/hello") == 0) {
        body_len = snprintf(body, sizeof(body),
            "{\"message\": \"Hello from C HTTP Server\"}\n");
    } else {
        body_len = snprintf(body, sizeof(body),
            "{\"error\": \"api not found\"}");
        send_response_header(fd, 404, "Not Found",
                             "application/json", body_len);
        send(fd, body, body_len, 0);
        return;
    }
    send_response_header(fd, 200, "OK", "application/json", body_len);
    send(fd, body, body_len, 0);
}

//处理一个客户端连接
void handle_client(int client_fd)
{
    char buffer[BUF_SIZE] = {0};
    int n = recv(client_fd, buffer, BUF_SIZE - 1, 0);
    if (n <= 0) return;

    HttpRequest req;
    if (parse_request(buffer, &req) != 0) {
        send_error(client_fd, 400, "Bad Request");
        return;
    }
    printf("%s %s (Host: %s, Content-Length: %ld)\n",
           req.method, req.path, req.host, req.content_length);

    int head_only = (strcmp(req.method, "HEAD") == 0);

    if (strcmp(req.method, "GET") != 0 && !head_only) {
        send_error(client_fd, 405, "Method Not Allowed");
    } else if (strncmp(req.path, "/api/", 5) == 0) {
        handle_dynamic_api(client_fd, req.path);
    } else {
        serve_static_file(client_fd, req.path, head_only);
    }
}

const char *get_mime_type(const char *path)
{
    const char *dot = strrchr(path, '.');  //定位扩展名
    if (dot == NULL) return "application/octet-stream"; //无扩展名按二进制处理

    if (strcmp(dot, ".html") == 0) return "text/html";
    if (strcmp(dot, ".htm")  == 0) return "text/html";
    if (strcmp(dot, ".css")  == 0) return "text/css";
    if (strcmp(dot, ".js")   == 0) return "application/javascript";
    if (strcmp(dot, ".json") == 0) return "application/json";
    if (strcmp(dot, ".png")  == 0) return "image/png";
    if (strcmp(dot, ".jpg")  == 0) return "image/jpeg";
    if (strcmp(dot, ".gif")  == 0) return "image/gif";
    if (strcmp(dot, ".txt")  == 0) return "text/plain";
    return "application/octet-stream";     //未知类型按下载处理
}

int main(void)
{
    //日志行缓冲
    setvbuf(stdout, NULL, _IOLBF, 0);

    int server_fd = init_server_socket();
    if (server_fd < 0) return 1;

    signal(SIGPIPE, SIG_IGN);

    //处理并发连接
    //简单场景下，用 accept() 阻塞等待连接，处理完一个再接收下一个"
    for (;;) {
        struct sockaddr_in client_addr;
        socklen_t addr_len = sizeof(client_addr);

        //accept 阻塞等待，返回与客户端通信的新套接字
        int client_fd = accept(server_fd,
                               (struct sockaddr *)&client_addr, &addr_len);
        if (client_fd < 0) {
            perror("accept 失败");
            continue;
        }

        /* 设置收发超时客户端 5 秒无数据自动断开，
         * 防止慢速/异常连接把单线程服务器卡死 */
        struct timeval tv = {5, 0};
        setsockopt(client_fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
        setsockopt(client_fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));

        handle_client(client_fd);
        close(client_fd);
    }
    close(server_fd);
    return 0;
}
