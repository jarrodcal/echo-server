A Simple Server Demo

一个简单的后台网络服务样例，未借助任何第三方库(除glibc)。功能是接收客户端连接，访问Redis获得数据直接返回给客户端。
系统包含线程架构，缓冲区，网络连接，日志，和Redis资源长连接模块, 运用到哈希表和链表基本数据结构，简单可依赖。

采用Master+Workers+辅助线程的架构，Master线程接收客户端连接，Workers处理和客户端的实际交互，辅助线程包含写日志，查看系统状态。
主线程和工作线程都采用异步非阻塞的模式，EPOLL边缘触发。
访问Redis为长连接且定时查看连接状态，定时发送心跳包，未采用第三方客户端和Redis通信，直接解析redis通信协议。

@功能: 客户端输入uid，服务端输出uid$gdid\
@协议：文本协议，"$(len)\r\n(uid)"
@流程: 每一次客户端请求创建一个连接，并保存在哈希表中(方便后续系统拓展)，构造reids协议异步请求，获得数据后返回给客户端
@例子：输入：$10\r\n123456789, 输出：123456789$adfa899ad2，返回uid同时是为了便于客户端进行数据校验

TODO：检查心跳结果，查看连接的状态, 定义一些常量，占用系统资源监控，连接池，工作线程创建新的连接，信号处理，从文件读取配置，改为守护进程模式, 看场景实现RC4加密功能，通信协议改为变体长度的二进制版本, jemalloc代替glibc，调整链表结构为linux内核的实现方式

使用方法：
Server: make ./sever-demo &
Client: php client.php