# 高性能 HTTP 服务器

C 语言从零实现的 HTTP/1.1 服务器。

## 功能

- TCP 套接字全流程：socket / bind / listen / accept
- SO_REUSEADDR 端口复用，避免 TIME_WAIT 导致重启失败
- HTTP 请求行 + 请求头解析（Host、Content-Length），支持 GET / HEAD
- 静态文件服务：按扩展名映射 MIME 类型（HTML/CSS/JS/JSON/图片等）
- 路径穿越防护：拦截 `..` 请求并返回 403
- 动态接口：`/api/hello` 返回 JSON，与静态路由分离
- 客户端 5 秒收发超时，防止慢速连接卡死服务器
- 忽略 SIGPIPE，防止对端断开导致进程退出

## 编译运行

```bash
gcc -o http_server http.c
./http_server
# 浏览器访问 http://localhost:8080
# 动态接口 http://localhost:8080/api/hello
```

## 已知局限与改进方向

- 当前为单线程 accept 串行模型，下一步计划用 epoll 改造为高并发版本
- 计划引入 sendfile 零拷贝优化静态文件传输
