# 高性能 HTTP 服务器

C 语言从零实现的 HTTP/1.1 服务器，包含两个并发模型版本：

| 文件 | 并发模型 | 说明 |
|------|---------|------|
| `http.c` | 阻塞式单线程串行 | 基础版：一个连接处理完才 accept 下一个 |
| `http_epoll.c` | epoll I/O 多路复用 | 高并发版：单线程事件驱动，实测 50 并发 200 请求全部成功 |

## 功能

- TCP 套接字全流程：socket / bind / listen / accept
- SO_REUSEADDR 端口复用，避免 TIME_WAIT 导致重启失败
- HTTP 请求行 + 请求头解析（Host、Content-Length），支持 GET / HEAD
- 静态文件服务：按扩展名映射 MIME 类型（HTML/CSS/JS/JSON/图片等）
- 路径穿越防护：拦截 `..` 请求并返回 403
- 动态接口：`/api/hello` 返回 JSON，与静态路由分离
- 忽略 SIGPIPE，防止对端断开导致进程退出

## epoll 版架构（http_epoll.c）

```
                    ┌─────────────────────────────┐
                    │        epoll_wait            │
                    └──────────────┬───────────────┘
              ┌────────────────────┼────────────────────┐
              ▼                    ▼                    ▼
      监听 socket 可读       连接可读 EPOLLIN      连接可写 EPOLLOUT
      handle_accept()        handle_read()         handle_write()
      循环 accept            读入 rbuf 直到         conn_flush()
      到 EAGAIN              "\r\n\r\n" 凑齐       分块流式发文件
                             → process_request
```

核心设计：

- **全链路非阻塞**：监听 socket 和客户端连接都设 O_NONBLOCK，epoll 统一接管 accept/read/write
- **每连接状态机 `conn_t`**：读缓冲区（凑齐 `\r\n\r\n` 才解析）、写缓冲区（写不完注册 EPOLLOUT 续发）、文件流式发送状态（8KB 分块，内存占用与文件大小无关）
- **`epoll_event.data.ptr` 挂连接状态**，避免 fd 复用导致的状态错乱
- **默认水平触发(LT)**，但 accept/read/write 均按"循环到 EAGAIN"编写，注册事件时加 `EPOLLET` 即可切换边缘触发

## 编译运行

```bash
# 阻塞版
gcc -o http_server http.c
./http_server

# epoll 版（仅 Linux）
gcc -O2 -Wall -o http_epoll http_epoll.c
./http_epoll
# 浏览器访问 http://localhost:8080
# 动态接口 http://localhost:8080/api/hello
```

## 实测验证（Ubuntu 24.04，gcc -O2 -Wall -Wextra）

| 测试项 | 结果 |
|--------|------|
| 编译 | 零警告零错误 |
| GET / 、/api/hello、404、HEAD | 全部正确 |
| 路径穿越 `/../etc/passwd` | 拦截，返回 403 |
| 5 MB 大文件下载 | 逐字节比对一致 |
| 并发：200 请求 / 50 并发 | 全部返回 200，无一失败 |

## 改进方向

- 引入 sendfile 零拷贝优化静态文件传输
- 支持 HTTP keep-alive 长连接（代码已预留扩展点）
- 加定时器回收空闲连接
