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
#include <thread>
#include <mutex>

int set_nonblock(int fd) {
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

std::mutex m;
void log_error(const char *place) {
	m.lock();
    FILE *log = fopen("/tmp/log", "a");
    fprintf(log, "%s\n", strerror(errno));
    fclose(log);
	m.unlock();
}

void log(const char *msg) {
	m.lock();
    FILE *log = fopen("/tmp/log", "a");
    fprintf(log, "%s\n", msg);
    fclose(log);
	m.unlock();
}

// Arguments
char *host = 0, *port = 0, *dir = 0;

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

void process_slave_socket(int slave_socket) {
    char buf[1024];
    ssize_t recv_ret = recv(slave_socket, buf, sizeof(buf), MSG_NOSIGNAL);
    if (recv_ret < 0) {
        log_error("recv");
        close(slave_socket);
        return;
    }
    if (recv_ret == 0) {
        close(slave_socket);
        return;
    }
    buf[recv_ret]=0;

    // process http request, extract file path
    std::string path;
    extract_path_from_http_get_request(path, buf, recv_ret);

    // if path exists open and read file
    std::string full_path = std::string(dir) + path;
    char reply[1024];
	log(full_path.c_str());
    if (path.length() && path != "/" && access(full_path.c_str(), F_OK) != -1) {
        // file exists, get its size
        int fd = open(full_path.c_str(), O_RDONLY);
        int sz = lseek(fd, 0, SEEK_END);

        sprintf(reply, "HTTP/1.1 200 OK\r\n"
                       "Content-Type: text/html\r\n"
                       "Content-length: %d\r\n"
                       "Connection: close\r\n"
                       "\r\n", sz);

        ssize_t send_ret = send(slave_socket, reply, strlen(reply), MSG_NOSIGNAL);
        off_t offset = 0;
        while (offset < sz) {
            int s = sendfile(slave_socket, fd, &offset, sz - offset);
        }
        close(fd);
    } else {
        strcpy(reply, "HTTP/1.1 404 Not Found\r\n"
                      "Content-Type: text/html\r\n"
                      "Content-length: 0\r\n"
                      "Connection: close\r\n"
                      "\r\n");

        ssize_t send_ret = send(slave_socket, reply, strlen(reply), MSG_NOSIGNAL);
    }
    shutdown(slave_socket, SHUT_WR);
	while(recv(slave_socket, buf, sizeof(buf), MSG_NOSIGNAL) > 0);
	close(slave_socket);
}

void slave_send_to_worker(struct ev_loop *loop, struct ev_io *w, int revents) {
    int slave_socket = w->fd;
    ev_io_stop(loop, w);
	std::thread t(process_slave_socket, slave_socket);
	t.detach();
}

void master_accept_connection(struct ev_loop *loop, struct ev_io *w, int revents) {
    // create slave socket
    int slave_socket = accept(w->fd, 0, 0);
    if (slave_socket == -1) {
        log_error("accept");
        exit(3);
    }

    set_nonblock(slave_socket);

    struct ev_io *slave_watcher = new ev_io;
    ev_init(slave_watcher, slave_send_to_worker);
    ev_io_set(slave_watcher, slave_socket, EV_READ);
    ev_io_start(loop, slave_watcher);
}

int main(int argc, char* argv[]) {
	log("-----------------------------------------");
    if (daemon(0, 0) == -1) {
        log_error("daemon");
        exit(1);
    }
    int opt;
    while ((opt = getopt(argc, argv, "h:p:d:")) != -1) {
        switch(opt) {
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
    if (host == 0 || port == 0 || dir == 0) {
        printf("Usage: %s -h <host> -p <port> -d <folder>\n", argv[0]);
        exit(1);
    }

    struct ev_loop *loop = ev_default_loop(EVFLAG_FORKCHECK);
    int master_socket = socket(AF_INET, SOCK_STREAM, 0);
    if (master_socket == -1) {
        log_error("master_socket");
        exit(1);
    }
    set_nonblock(master_socket);

    struct sockaddr_in addr;
    addr.sin_family = AF_INET;
    addr.sin_port = htons(atoi(port));
    if (inet_pton(AF_INET, host, &(addr.sin_addr.s_addr)) != 1) {
        log_error("inet_aton");
        exit(2);
    }
    if (bind(master_socket, (struct sockaddr* )&addr, sizeof(addr)) == -1) {
        log_error("bind");
        exit(3);
    }
    listen(master_socket, SOMAXCONN);

    struct ev_io master_watcher;
    ev_init (&master_watcher, master_accept_connection);
    ev_io_set(&master_watcher, master_socket, EV_READ);
    ev_io_start(loop, &master_watcher);
    ev_loop(loop, 0);
    close(master_socket);
    return 0;
}