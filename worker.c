#include "worker.h"

extern worker ** g_ppworker;
extern int g_workcount;

worker_t worker_create()
{
    worker_t pworker = (worker_t)malloc(sizeof(worker));

    if (pworker == NULL)
    {
        print_log(LOG_TYPE_ERR, "malloc worker error\n");
        return NULL;
    }

    pworker->tid = pthread_self();
    pworker->total_count = 0;
    pworker->closed_count = 0;
    pworker->neterr_count = 0;
    pworker->epfd = epoll_create(256);

    hash_table *ht = (hash_table *)malloc(sizeof(hash_table));
    pworker->pht = ht;
    ht_init(pworker->pht, HT_KEY_CONST|HT_VALUE_CONST, 0.05);

    //初始fd为-1，表示资源的长连接未建立或者资源掉线
    pworker->redis = connector_create(INVALID_ID, pworker, CONN_TYPE_REDIS, REDIS_IP, REDIS_PORT);

    return pworker;
}

void worker_close(worker_t pworker)
{
    //redis连接关闭
    //hash表的资源释放
    //连接关闭
    //ht_destroy
}

static void connect_redis_done(connector_t pconredis)
{
    int error = 0;
    socklen_t len = sizeof(int);

    if ((getsockopt(pconredis->sockfd, SOL_SOCKET, SO_ERROR, &error, &len)) == 0)
    {
        if (error == 0)
        {
            connector_sig_read(pconredis);
            connector_unsig_write(pconredis);
            pconredis->state = CONN_STATE_RUN;
        }
        else
        {
            connector_close(pconredis);
            print_log(LOG_TYPE_ERR, "connect redis error, ip %s, port %d, file = %s, line = %d", pconredis->ip, pconredis->port, __FILE__, __LINE__);
        }
    }
}

static void reids_heartbeat(connector_t pconredis)
{
    //失败的情况下设置连接状态pconredis->state == CONN_STATE_CLOSED;
    char *key = "contact_upload_9";
    char *field = "1984467097";
    char cmd[REDIS_CMD_LEN] = {'\0'};

    if (make_cmd(cmd, REDIS_CMD_LEN, 3, "hget", key, field) < 0)
    {
        print_log(LOG_TYPE_ERR, "hget %s %s error, file = %s, line = %d", key, field, __FILE__, __LINE__);
        return;
    }

    int len = strlen(cmd);
    buffer_write(pconredis->pwritebuf, cmd, len);
    connector_write(pconredis);
}

static void connect_redis(connector_t pconredis)
{
    int ret = 0;
    int sockfd = INVALID_ID;
    struct sockaddr_in redis_addr;

    if ((sockfd = socket(AF_INET, SOCK_STREAM, 0)) < 0)
    {
        print_log(LOG_TYPE_ERR, "socket, file = %s, line = %d", __FILE__, __LINE__);
        return;
    }

    setnonblock(sockfd);
    setreuse(sockfd);
    set_tcp_fastclose(sockfd);

    bzero(&redis_addr, sizeof(redis_addr));
    redis_addr.sin_family = AF_INET;
    redis_addr.sin_port = htons(pconredis->port);

    if (inet_pton(AF_INET, pconredis->ip, &redis_addr.sin_addr) < 0)
    {
        close(sockfd);
        print_log(LOG_TYPE_ERR, "inet_pton, file = %s, line = %d", __FILE__, __LINE__);
        return;
    }

    ret = connect(sockfd, (struct sockaddr *)&redis_addr, sizeof(redis_addr));

    if (ret == 0)
    {
        pconredis->sockfd = sockfd;
        connector_sig_read(pconredis);
        pconredis->state = CONN_STATE_RUN;
    }
    else if (errno == EINPROGRESS)
    {
        pconredis->sockfd = sockfd;
        connector_sig_write(pconredis);
        pconredis->state = CONN_STATE_CONNECTING;
    }
    else
    {
        close(sockfd);
        print_log(LOG_TYPE_ERR, "connect error, file = %s, line = %d", __FILE__, __LINE__);
    }
}

//资源的定时检查, 断开后会重新连接
void handle_time_check(worker_t pworker)
{
    connector_t pconredis = pworker->redis;

    if (pconredis->state == CONN_STATE_NONE || pconredis->state == CONN_STATE_CLOSED)
        connect_redis(pconredis);
    else
        reids_heartbeat(pconredis);
}

void * worker_loop(void *param)
{
    worker_t pworker = (worker_t)param;

    int nfds = 0;
    int timeout = 100;
    struct epoll_event evs[4096];
    connector_t pconn = NULL;
    int i;

    while (1)
    {
        nfds = epoll_wait(pworker->epfd, evs, 4096, timeout);

        if (nfds == -1)
        {
            if (errno == EINTR)
                continue;

            print_log(LOG_TYPE_ERR, "worker epoll_wait error, epfd = %d, errno = %d", pworker->epfd, errno);
            break;
        }

        for (i = 0; i < nfds; i++)
        {
            pconn = (connector_t)evs[i].data.ptr;
            
            if (evs[i].events & EPOLLIN)
            {
                worker_handle_read(pconn, evs[i].events);
            }
            if (evs[i].events & EPOLLOUT)
            {
                worker_handle_write(pconn);
            }
            if ((evs[i].events & EPOLLERR) || (evs[i].events & EPOLLHUP))
            {
                print_log(LOG_TYPE_DEBUG, "EPOLLERR or EPOLLHUP occure");
                pworker->neterr_count++;

                connector_close(pconn);
            }
            if (evs[i].events & EPOLLRDHUP)
            {
                //可以在应用层面（写缓冲区）检查数据是否已经完全发出，server发出去，系统层面会在close后根据SO_LINGER进行相关的处理
                print_log(LOG_TYPE_DEBUG, "EPOLLRDHUP occure");
                pworker->closed_count++;

                connector_close(pconn);
            }
        }

        handle_time_check(pworker);
    }

    return NULL;
}

void create_worker_system(int count)
{
    int i;

    pthread_t tid;
    pthread_attr_t attr;

    pthread_attr_init(&attr);
    pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);

    for (i=0; i<count; i++)
    {
        g_ppworker[i] = worker_create();

        if (pthread_create(&tid, &attr, worker_loop, (void *)g_ppworker[i]) != 0)
        {
            print_log(LOG_TYPE_ERR, "create work thread error");
            pthread_attr_destroy(&attr);
            return;
        }

        pthread_attr_destroy(&attr);
    }
}

void worker_handle_read(connector_t pconn, int event)
{
    switch (pconn->type)
    {
        case CONN_TYPE_CLIENT:
            channel_handle_client_read(pconn, event);
            break;
        
        case CONN_TYPE_REDIS:
            channel_handle_redis_read(pconn, event);
            break;
    }
}

void worker_handle_write(connector_t pconn)
{
    switch (pconn->type)
    {
        case CONN_TYPE_CLIENT:
            channel_handle_client_write(pconn);
            break;
        
        case CONN_TYPE_REDIS:
            channel_handle_redis_write(pconn);
            break;
    }
}

void channel_handle_client_read(connector_t pconn, int event)
{
    if (connector_read(pconn, event) > 0)
    {
        //进行消息解析和业务处理
        if (buffer_readable(pconn->preadbuf) > 0)
        {
            char *data = buffer_get_read(pconn->preadbuf);
            size_t len = strlen(data);
            buffer_read(pconn->preadbuf, len, TRUE);

            print_log(LOG_TYPE_DEBUG, "Read msg %s", data);
            memcpy(pconn->uid, data, len);

            int len2 = sizeof(connector_t);
            ht_insert(pconn->pworker->pht, data, len+1, pconn, len2);

            size_t value_size;
            connector_t phashcon = (connector_t)ht_get(pconn->pworker->pht, data, len+1, &value_size);
            print_log(LOG_TYPE_DEBUG, "In hash table ip %s, port %d, uid %s", phashcon->ip, phashcon->port, phashcon->uid);
            ht_remove(pconn->pworker->pht, data, len+1);

            buffer_write(pconn->pwritebuf, data, len);
            connector_write(pconn);
        }
    }
}

void channel_handle_redis_read(connector_t pconn, int event)
{
    connector_read(pconn, event);

    if (buffer_readable(pconn->preadbuf) > 0)
    {
        char *data = buffer_get_read(pconn->preadbuf);
        print_log(LOG_TYPE_DEBUG, "Msg is %s", data);
        int len = strlen(data);
        buffer_read(pconn->preadbuf, len, TRUE);
    }
}

void channel_handle_client_write(connector_t pconn)
{
    connector_unsig_write(pconn);
    connector_write(pconn);
}

void channel_handle_redis_write(connector_t pconn)
{
    //以非阻塞模式设置套接口，去connect Redis，当异步返回时会设置套接字为可写状态，此时Epoll返回写通知事件，可能是有数据也可能是连接状态的返回。
    //通过con的state判断是哪种情况，通过调用getsockopt来得到返回的连接状态（连接成功和失败都会返回可写通知）

    if (pconn->state == CONN_STATE_CONNECTING || pconn->state == CONN_STATE_CLOSED)
    {
        connect_redis_done(pconn);
    }
    else
    {
        connector_unsig_write(pconn);
        connector_write(pconn);
    }
}