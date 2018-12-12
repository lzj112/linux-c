//关闭非活动连接

#include <sys/socket.h>
#include <netinet/in.h>
#include <sys/types.h>
#include <arpa/inet.h>
#include <sys/epoll.h>
#include <sys/time.h>
#include <pthread.h>
#include <assert.h>
#include <unistd.h>
#include <string.h>
#include <stdlib.h>
#include <signal.h>
#include <errno.h>
#include <stdio.h>
#include <fcntl.h>
#include "lst_timer.h"

#define FD_LIMIT 65535
#define MAX_EVENT_NUMBER 1024
#define TIMESLOT 5

static int pipefd[2];
//利用升序链表来管理定时器
static sort_timer_lst timer_lst;
static int epollfd = 0;

int setnonblocking(int fd) 
{
    int old_option = fcntl(fd, F_GETFL);
    int new_option = old_option | O_NONBLOCK;
    fcntl(fd, F_SETFL, new_option);
    return old_option;
}

void addfd(int epollfd, int fd) 
{
    epoll_event event;
    event.data.fd = fd;
    event.events = EPOLLIN | EPOLLET;
    epoll_ctl(epollfd, EPOLL_CTL_ADD, fd, &event);
    setnonblocking(fd); 
}

void sig_handler(int sig) 
{
    int save_errno = errno;
    int msg = sig;
    send(pipefd[1], (char*)&msg, 1, 0);
    errno = save_errno;
}

void addsig(int sig) 
{
    struct sigaction sa;
    memset(&sa, '\0', sizeof(sa));
    sa.sa_handler = sig_handler;
    sa.sa_flags |= SA_RESTART;
    sigfillset(&sa.sa_mask);
    assert(sigaction(sig, &sa, NULL) != -1);
}

void timer_handler() 
{
    //定时处理任务，实际上就是调用tick函数
    timer_lst.tick();
    //因为一次alarm调用只会引起一次SIGALRM信号，所以我们要重新定时
    //以不断触发SIGALRM信号
    alarm(TIMESLOT);
}

//定时回调函数，他删除非活动连接socket上的注册事件，并关闭之
void cb_func(client_data* user_data) 
{
    epoll_ctl(epollfd, EPOLL_CTL_DEL, user_data->sockfd, 0);
    assert(user_data);

    close(user_data->sockfd);
    printf("close fd %d\n", user_data->sockfd);
}

int main(int argc, char* argv[]) 
{
    if (argc <= 2) 
    {
        printf("errno\n");
        return 1;
    }

    const char* ip = argv[1];
    int port = atoi(argv[2]);

    int ret = 0;
    sockaddr_in address;
    bzero(&address, sizeof(address));
    address.sin_family = AF_INET;
    address.sin_port = htons(port);
    inet_pton(AF_INET, ip, &address.sin_addr);

    int listenfd = socket(PF_INET, SOCK_STREAM, 0);
    assert(listenfd >= 0);

    ret = bind(listenfd, (sockaddr*)&address, sizeof(address));
    assert(ret != -1);

    ret = listen(listenfd, 5);
    assert(ret != -1);

    epoll_event events[MAX_EVENT_NUMBER];
    int epollfd = epoll_create(5);
    assert(epollfd != -1);
    addfd(epollfd, listenfd);

    ret = socketpair(PF_UNIX, SOCK_STREAM, 0, pipefd);
    assert(ret != -1);
    setnonblocking(pipefd[1]); //写端
    addfd(epollfd, pipefd[0]); //读端

    //设置信号处理函数
    addsig(SIGALRM);
    addsig(SIGTERM);
    bool stop_server = false;

    client_data* users = new client_data[FD_LIMIT]; //连接数据
    bool timeout = false;
    alarm(TIMESLOT); //定时  5s后发送信号SIGALRM

    while (!stop_server) 
    {
        int number = epoll_wait(epollfd, events, MAX_EVENT_NUMBER, -1);
        if ((number < 0) && (errno != EINTR)) 
        {
            printf("epoll failure\n");
            break;
        }

        for (int i = 0; i < number; i++) 
        {
            int sockfd = events[i].data.fd;
            //处理新的客户端连接
            if (sockfd == listenfd) 
            {
                struct sockaddr_in client_address;
                socklen_t client_addrlength = sizeof(client_address);
                int connfd = accept(listenfd, (struct sockaddr*)&client_address, 
                                    &client_addrlength);
                addfd(epollfd, connfd);
                users[connfd].address = client_address;
                users[connfd].sockfd = connfd;

                //创建定时器，设置回调函数与超时时间，然后绑定定时器与用户数据
                //最后将定时器添加到链表timer_lst中
                util_timer* timer = new util_timer;
                timer->user_data = &users[connfd];
                timer->cb_func = cb_func;
                time_t cur = time(NULL);
                timer->expire = cur + 3 * TIMESLOT;
                users[connfd].timer = timer;
                timer_lst.add_timer(timer);
            }
            //处理信号
            else if ((sockfd == pipefd[0]) && (events[i].events & EPOLLIN)) 
            {
                int sig;
                char signals[1024];
                ret = recv(pipefd[0], signals, sizeof(signals), 0);
                if (ret == -1) 
                {
                    //handler the error
                    continue;
                }
                else if (ret == 0) 
                {
                    continue;
                }
                else 
                {
                    for (int i = 0; i < ret; i++) 
                    {
                        switch(signals[i]) 
                        {
                            case SIGALRM:
                            {
                                //用timeout变量标记有定时任务需要处理，但不立即处理
                                //定时任务。这是因为定时任务的优先级不是很高，我们优先
                                //处理其他更重要的任务
                                timeout = true;
                                break;
                            }
                            case SIGTERM:
                            {
                                stop_server = true;
                            }
                        }
                    }
                }
            }
            //处理客户连接上接收到的数据
            else if (events[i].events & EPOLLIN) 
            {
                memset(users[sockfd].buf, '\0', BUFFER_SIZE);
                ret = recv(sockfd, users[sockfd].buf, BUFFER_SIZE-1, 0);
                printf("get %d bytes of client data %s from %d\n", ret, users[sockfd].buf, sockfd);
                util_timer* timer = users[sockfd].timer;
                if (ret < 0) 
                {
                    //如果发生读错误，则关闭连接，并移除其对应的定时器
                    if (errno != EAGAIN) 
                    {
                        cb_func(&users[sockfd]);  
                        if (timer) 
                        {
                            timer_lst.del_timer(timer);
                        }

                    }
                }
                else if (ret == 0) 
                {
                    //如果对方已经关闭连接，则我们也关闭连接，并移除对应的定时器
                    cb_func(&users[sockfd]);
                    if (timer) 
                    {
                        timer_lst.del_timer(timer);
                    }
                }
                else 
                {
                    //如果某个对应的客户连接上有数据可读，则我们要调整该连接对应的定时器
                    //以延迟该连接被关闭的时间
                    if (timer) 
                    {
                        time_t cur = time(NULL);
                        timer->expire = cur + 3 * TIMESLOT;
                        printf("adjust timer once\n");
                        timer_lst.adjust_timer(timer);
                    }
                }
            }
            else 
            {
                //others
            }
        }
        //最后处理定时事件，因为I/O事件有更高的优先级，当然这样做将导致
        //定时任务不能精确的按照预期的时间执行
        if (timeout) 
        {
            timer_handler();
            timeout = false;
        }
    }

    close(listenfd);
    close(pipefd[0]);
    close(pipefd[1]);
    delete[] users;
    return 0;
}