#include <iostream>

#include <map>
#include <list>
#include <semaphore.h>
#include <sys/types.h>
#include <sys/sendfile.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <sys/socket.h>
#include <sys/ioctl.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <stdio.h>
#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <ev.h>

// Debug mode, a lot of debug print to std::cout
// #define HTTP_DEBUG

// send fd
ssize_t sock_fd_write(int sock, void *buf, ssize_t buflen, int fd);

// recv fd
ssize_t sock_fd_read(int sock, void *buf, ssize_t bufsize, int *fd);

// set nonblock mode for a descriptor
int set_nonblock(int fd);

// Workers storage: socket and status (is it free (true) or not (false))
std::map<int, bool> workers;

// Storage for slave sockets that are ready to be read but no free workers
std::list<int> ready_read_sockets;

// Semaphore to sync access to ready_read_sockets between forked processes
sem_t* locker;

// Arguments
char *host = 0, *port = 0, *dir = 0;

int safe_pop_front()
{
    // return -1, if ready_read_sockets is empty
    int fd;

    sem_wait(locker);

    if (ready_read_sockets.empty())
    {
        fd = -1;
    }
    else
    {
        fd = *ready_read_sockets.cbegin();
        ready_read_sockets.pop_front();
    }

    sem_post(locker);

    return fd;
}

void safe_push_back(int fd)
{
    sem_wait(locker);
    ready_read_sockets.push_back(fd);
    sem_post(locker);
}

void extract_path_from_http_get_request(std::string& path, const char* buf, ssize_t len)
{
    std::string request(buf, len);
    std::string s1(" ");
    std::string s2("?");

    // "GET "
    std::size_t pos1 = 4;

    std::size_t pos2 = request.find(s2, 4);
    if (pos2 == std::string::npos)
    {
        pos2 = request.find(s1, 4);
    }

    path = request.substr(4, pos2 - 4);
}


void slave_send_to_worker(struct ev_loop *loop, struct ev_io *w, int revents)
{
    // get slave socket
    int slave_socket = w->fd;

    // think we should stop watcher
    ev_io_stop(loop, w);

#ifdef HTTP_DEBUG
    std::cout << "slave_send_to_worker: got slave socket " << slave_socket << std::endl;
#endif

    // find a free worker and send slave socket to it
    for(auto it = workers.begin(); it != workers.end(); it++)
    {
        if ((*it).second)
        {
            // found free worker, it is busy from now
            (*it).second = false;

            char tmp[1];
            sock_fd_write((*it).first, tmp, sizeof(tmp), slave_socket);

#       ifdef HTTP_DEBUG
            std::cout << "slave_send_to_worker: sent slave socket " << slave_socket << " to worker" << std::endl;
#       endif

            return;
        }
    }

#ifdef HTTP_DEBUG
    std::cout << "slave_send_to_worker: no free workers, so call safe_push_back(" << slave_socket << ")" << std::endl;
#endif

    // add to queue for later processing
    safe_push_back(slave_socket);
}

void process_slave_socket(int slave_socket)
{
    // recv from slave socket
    char buf[1024];
    ssize_t recv_ret = recv(slave_socket, buf, sizeof(buf), MSG_NOSIGNAL);
    if (recv_ret == -1)
    {
        std::cout << "do_work: recv return -1" << std::endl;
        return;
    }

#ifdef HTTP_DEBUG
    std::cout << "do_work: recv return " << recv_ret << std::endl;
    std::cout << "======== received message ========" << std::endl;
    std::cout << buf << std::endl;
    std::cout << "==================================" << std::endl;
#endif

    // process http request, extract file path
    std::string path;
    extract_path_from_http_get_request(path, buf, recv_ret);

    // if path exists open and read file
    std::string full_path = std::string(dir) + path;

#ifdef HTTP_DEBUG
    std::cout << "============ full path ===========" << std::endl;
    std::cout << full_path << std::endl;
    std::cout << "==================================" << std::endl; 
#endif

    char reply[1024];
    if (access(full_path.c_str(), F_OK) != -1)
    {
        // file exists, get its size
        int fd = open(full_path.c_str(), O_RDONLY);
        int sz = lseek(fd, 0, SEEK_END);;

        sprintf(reply, "HTTP/1.1 200 OK\r\n"
                       "Content-Type: text/html\r\n"
                       "Content-length: %d\r\n"
                       "Connection: close\r\n"
                       "\r\n", sz);

        ssize_t send_ret = send(slave_socket, reply, strlen(reply), MSG_NOSIGNAL);

#   ifdef HTTP_DEBUG
        std::cout << "do_work: send return " << send_ret << std::endl;
#   endif

        off_t offset = 0;
        while (offset < sz)
        {
            // think not the best solution
            offset = sendfile(slave_socket, fd, &offset, sz - offset);
        }

        close(fd);
    }
    else
    {
        strcpy(reply, "HTTP/1.1 404 Not Found\r\n"
                      "Content-Type: text/html\r\n"
                      "Content-length: 0\r\n"
                      "Connection: close\r\n"
                      "\r\n");

        ssize_t send_ret = send(slave_socket, reply, strlen(reply), MSG_NOSIGNAL);
/*#   ifdef HTTP_DEBUG
        std::cout << "do_work: send return " << send_ret << std::endl;
#   endif
        strcpy(reply, "<html>\n<head>\n<title>Not Found</title>\n</head>\r\n");
        send_ret = send(slave_socket, reply, strlen(reply), MSG_NOSIGNAL);
#   ifdef HTTP_DEBUG
        std::cout << "do_work: send return " << send_ret << std::endl;
#   endif
        strcpy(reply, "<body>\n<p>404 Request file not found.</p>\n</body>\n</html>\r\n");
        send_ret = send(slave_socket, reply, strlen(reply), MSG_NOSIGNAL);
#   ifdef HTTP_DEBUG
        std::cout << "do_work: send return " << send_ret << std::endl;
#   endif*/
    }
}

void do_work(struct ev_loop *loop, struct ev_io *w, int revents)
{
#ifdef HTTP_DEBUG
    std::cout << "do_work: from socket " << w->fd << std::endl;
#endif

    // get appropriate slave socket and read from it
    int slave_socket;
    char tmp[1];

    sock_fd_read(w->fd, tmp, sizeof(tmp), &slave_socket);
    if (slave_socket == -1)
    {
        std::cout << "do_work: slave_socket == -1" << std::endl;
        exit(4);
    }

#ifdef HTTP_DEBUG
    std::cout << "do_work: got slave socket " << slave_socket << std::endl;
#endif

    // do it
    process_slave_socket(slave_socket);

    // write back to paired socket to update worker status
    //sock_fd_write(w->fd, tmp, sizeof(tmp), slave_socket);
shutdown(slave_socket, SHUT_RDWR);
close(slave_socket);

#ifdef HTTP_DEBUG
    std::cout << "do_work: sent slave socket " << slave_socket << std::endl;
#endif
}


void set_worker_free(struct ev_loop *loop, struct ev_io *w, int revents)
{
    // get socket of the pair
    int fd = w->fd;

    char tmp[1];
    int slave_socket;

    sock_fd_read(fd, tmp, sizeof(tmp), &slave_socket);
#ifdef HTTP_DEBUG
    std::cout << "set_worker_free: got slave socket " << slave_socket << std::endl;
#endif

    // here we can restore watcher for the slave socket

    // complete all the work from the queue
    while ((slave_socket = safe_pop_front()) != -1)
    {
        process_slave_socket(slave_socket);
    }

    workers[fd] = true;
#ifdef HTTP_DEBUG    
    std::cout << "set_worker_free: worker associated with paired socket " << fd << " is free now" << std::endl;
#endif
}

pid_t create_worker()
{
    int sp[2];
    if (socketpair(AF_LOCAL, SOCK_STREAM, 0, sp) == -1)
    {
        printf("socketpair error, %s\n", strerror(errno));
        exit(1);
    }

    // get default loop
    struct ev_loop* loop = EV_DEFAULT;

    auto pid = fork();

    if (pid)
    {
        // parent, use socket 0
        close(sp[1]);

        // save worker socket and set free status
        workers.insert(std::pair<int, bool>(sp[0], true));

        // to detect the worker finished work with a socket
        struct ev_io* half_watcher = new ev_io;
        ev_init(half_watcher, set_worker_free);
        ev_io_set(half_watcher, sp[0], EV_READ);
        ev_io_start(loop, half_watcher);

#   ifdef HTTP_DEBUG
        std::cout << "Worker with pid " << std::to_string(pid)
                  << " associated with socket " << std::to_string(sp[0])
                  << " in parent" << std::endl;
#   endif
    }
    else
    {
        // child, use socket 1
        close(sp[0]);

        // we use EVFLAG_FORKCHECK instead of
        // ev_default_fork();

        // create watcher for paired socket
        struct ev_io worker_watcher;
        ev_init(&worker_watcher, do_work);
        ev_io_set(&worker_watcher, sp[1], EV_READ);
        ev_io_start(loop, &worker_watcher);

#   ifdef HTTP_DEBUG
        std::cout << "Worker associated with socket " << std::to_string(sp[1]) << " in child" << std::endl;
#   endif

        // wait for events, run loop in a child
        ev_loop(loop, 0);
    }

    return pid;
}

void master_accept_connection(struct ev_loop *loop, struct ev_io *w, int revents)
{
    // create slave socket
    int slave_socket = accept(w->fd, 0, 0);
    if (slave_socket == -1)
    {
        printf("accept error, %s\n", strerror(errno));
        exit(3);
    }

    set_nonblock(slave_socket);

    // create watcher for a slave socket
    struct ev_io* slave_watcher = new ev_io;
    ev_init(slave_watcher, slave_send_to_worker);
    ev_io_set(slave_watcher, slave_socket, EV_READ);
    ev_io_start(loop, slave_watcher);
#ifdef HTTP_DEBUG
    std::cout << "master_accept_connection: slave socket is " << slave_socket << std::endl;
#endif
}


int main(int argc, char* argv[])
{
    // we want to be a daemon
    if (daemon(0, 0) == -1)
    {
        std::cout << "daemon error" << std::endl;
        exit(1);
    }

    // Allocate semaphore and initialize it as shared
    locker = new sem_t;
    sem_init(locker, 1, 1);

#ifdef HTTP_DEBUG
    std::cout << "main begin, parent pid is " << getpid() << std::endl;
#endif


    int opt;
    while ((opt = getopt(argc, argv, "h:p:d:")) != -1)
    {
        switch(opt)
        {
            case 'h':
                host = optarg;
                break;
            case 'p':
                port = optarg;
                break;
            case 'd':
                dir = optarg;
                break;
            default:
                printf("Usage: %s -h <host> -p <port> -d <folder>\n", argv[0]);
                exit(1);
        }
    }

    if (host == 0 || port == 0 || dir == 0)
    {
        printf("Usage: %s -h <host> -p <port> -d <folder>\n", argv[0]);
        exit(1);
    }

    //--------------------------------------------------------------------//


    // Our event loop
    struct ev_loop *loop = ev_default_loop(EVFLAG_FORKCHECK);


    //---------------- Create 3 workers --------------------//

    if (create_worker() == 0)
    {
        // worker 1 process
        printf("Worker 1 is about to return\n");
        return 0;
    }


    if (create_worker() == 0)
    {
        // worker 2 process
        printf("Worker 2 is about to return\n");
        return 0;
    }

    if (create_worker() == 0)
    {
        // worker 3 process
        printf("Worker 3 is about to return\n");
        return 0;
    }

    //------------------------------------------------------//


    // Master socket, think non-blocking
    int master_socket = socket(AF_INET, SOCK_STREAM, 0);
    if (master_socket == -1)
    {
        printf("socket error, %s\n", strerror(errno));
        exit(1);
    }
    set_nonblock(master_socket);

    struct sockaddr_in addr;
    addr.sin_family = AF_INET;
    addr.sin_port = htons(atoi(port));

    if (inet_pton(AF_INET, host, &(addr.sin_addr.s_addr)) != 1)
    {
        printf("inet_aton error\n");
        exit(2);
    }

    if (bind(master_socket, (struct sockaddr* )&addr, sizeof(addr)) == -1)
    {
        printf("bind return -1, %s\n", strerror(errno));
        exit(3);
    }


    listen(master_socket, SOMAXCONN);

#ifdef HTTP_DEBUG
    std::cout << "master socket is " << master_socket << std::endl;
#endif

    // Master watcher
    struct ev_io master_watcher;
    ev_init (&master_watcher, master_accept_connection);
    ev_io_set(&master_watcher, master_socket, EV_READ);
    ev_io_start(loop, &master_watcher);

    // Start loop
    ev_loop(loop, 0);

    close(master_socket);

    return 0;
}


ssize_t
sock_fd_write(int sock, void *buf, ssize_t buflen, int fd)
{
    ssize_t     size;
    struct msghdr   msg;
    struct iovec    iov;
    union {
        struct cmsghdr  cmsghdr;
        char        control[CMSG_SPACE(sizeof (int))];
    } cmsgu;
    struct cmsghdr  *cmsg;

    iov.iov_base = buf;
    iov.iov_len = buflen;

    msg.msg_name = NULL;
    msg.msg_namelen = 0;
    msg.msg_iov = &iov;
    msg.msg_iovlen = 1;

    if (fd != -1) {
        msg.msg_control = cmsgu.control;
        msg.msg_controllen = sizeof(cmsgu.control);

        cmsg = CMSG_FIRSTHDR(&msg);
        cmsg->cmsg_len = CMSG_LEN(sizeof (int));
        cmsg->cmsg_level = SOL_SOCKET;
        cmsg->cmsg_type = SCM_RIGHTS;

#   ifdef HTTP_DEBUG
        std::cout << "passing fd " << fd << std::endl;
#   endif
        *((int *) CMSG_DATA(cmsg)) = fd;
    } else {
        msg.msg_control = NULL;
        msg.msg_controllen = 0;
#   ifdef HTTP_DEBUG
        std::cout << "not passing fd " << fd << std::endl;
#   endif
    }

    size = sendmsg(sock, &msg, 0);

    if (size < 0)
        perror ("sendmsg");
    return size;
}

ssize_t
sock_fd_read(int sock, void *buf, ssize_t bufsize, int *fd)
{
    ssize_t     size;

    if (fd) {
        struct msghdr   msg;
        struct iovec    iov;
        union {
            struct cmsghdr  cmsghdr;
            char        control[CMSG_SPACE(sizeof (int))];
        } cmsgu;
        struct cmsghdr  *cmsg;

        iov.iov_base = buf;
        iov.iov_len = bufsize;

        msg.msg_name = NULL;
        msg.msg_namelen = 0;
        msg.msg_iov = &iov;
        msg.msg_iovlen = 1;
        msg.msg_control = cmsgu.control;
        msg.msg_controllen = sizeof(cmsgu.control);
        size = recvmsg (sock, &msg, 0);
        if (size < 0) {
            perror ("recvmsg");
            exit(1);
        }
        cmsg = CMSG_FIRSTHDR(&msg);
        if (cmsg && cmsg->cmsg_len == CMSG_LEN(sizeof(int))) {
            if (cmsg->cmsg_level != SOL_SOCKET) {
                fprintf (stderr, "invalid cmsg_level %d\n",
                     cmsg->cmsg_level);
                exit(1);
            }
            if (cmsg->cmsg_type != SCM_RIGHTS) {
                fprintf (stderr, "invalid cmsg_type %d\n",
                     cmsg->cmsg_type);
                exit(1);
            }

            *fd = *((int *) CMSG_DATA(cmsg));
#       ifdef HTTP_DEBUG
            std::cout << "received fd " << *fd << std::endl;
#       endif
        } else
            *fd = -1;
    } else {
        size = read (sock, buf, bufsize);
        if (size < 0) {
            perror("read");
            exit(1);
        }
    }
    return size;
}

int set_nonblock(int fd)
{
    int flags;
#if defined(O_NONBLOCK)
    if (-1 == (flags = fcntl(fd, F_GETFL, 0)))
        flags = 0;
    return fcntl(fd, F_SETFL, flags | O_NONBLOCK);
#else
    flags = 1;
    return ioctl(fd, FIONBIO, &flags);
#endif
}
