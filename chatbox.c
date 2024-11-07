#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <signal.h>
#include <sys/socket.h>
#include <sys/ioctl.h>
#include <netinet/in.h>
#include <ctype.h>
#include <errno.h>
#include <arpa/inet.h>
#include "chatServer.h"

static int end_server = 0;

void intHandler(int sig) {
    end_server = 1;
}

int main(int argc, char *argv[]) {
    signal(SIGINT, intHandler);

    if (argc != 2) {
        printf("Usage: server <port>\n");
        return 1;
    }

    int port_num = atoi(argv[1]);
    if (port_num < 1 || port_num > 65535) {
        printf("Usage: server <port>\n");
        return 1;
    }

    conn_pool_t *pool = malloc(sizeof(conn_pool_t));
    if (pool == NULL) {
        perror("malloc");
        return 1;
    }
    initPool(pool);

    int socket_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (socket_fd < 0) {
        perror("socket");
        return 1;
    }

    int on = 1;
    if (ioctl(socket_fd, FIONBIO, &on) < 0) {
        perror("ioctl");
        return 1;
    }

    struct sockaddr_in serveraddr;
    memset(&serveraddr, 0, sizeof(serveraddr));
    serveraddr.sin_family = AF_INET;
    serveraddr.sin_addr.s_addr = htonl(INADDR_ANY);
    serveraddr.sin_port = htons(port_num);

    if (bind(socket_fd, (struct sockaddr *)&serveraddr, sizeof(serveraddr)) < 0) {
        perror("bind");
        return 1;
    }

    if (listen(socket_fd, 5) < 0) {
        perror("listen");
        return 1;
    }

    FD_SET(socket_fd, &pool->read_set);
    pool->maxfd = socket_fd;

    do {
        pool->ready_read_set = pool->read_set;
        pool->ready_write_set = pool->write_set;
        printf("Waiting on select()...\nMaxFd %d\n", pool->maxfd);
        pool->nready = select(pool->maxfd + 1, &pool->ready_read_set, &pool->ready_write_set, NULL, NULL);
        if (pool->nready < 0) {
            if (errno == EINTR) {
                continue;
            } else {
                perror("select");
                break;
            }
        }

        if (FD_ISSET(socket_fd, &pool->ready_read_set)) {
            struct sockaddr_in clientaddr;
            socklen_t addrlen = sizeof(clientaddr);
            int clientfd = accept(socket_fd, (struct sockaddr *)&clientaddr, &addrlen);
            if (clientfd < 0) {
                if (errno != EWOULDBLOCK) {
                    perror("accept");
                }
            } else {
                printf("New incoming connection on sd %d\n", clientfd);
                if (addConn(clientfd, pool) < 0) {
                    perror("addConn");
                    close(clientfd);
                }
            }
        }

        conn_t *conn, *next;
        for (conn = pool->conn_head; conn != NULL; conn = next) {
            next = conn->next;

            if (FD_ISSET(conn->fd, &pool->ready_read_set)) {
                printf("Descriptor %d is readable\n", conn->fd);
                char buffer[BUFFER_SIZE];
                int nbytes = read(conn->fd, buffer, sizeof(buffer));
                if (nbytes < 0) {
                    perror("read");
                } else if (nbytes == 0) {
                    printf("Connection closed for sd %d\n", conn->fd);
                    removeConn(conn->fd, pool);
                    continue;
                } else {
                    printf("%d bytes received from sd %d\n", nbytes, conn->fd);
                    for (int i = 0; i < nbytes; i++) {
                        buffer[i] = toupper(buffer[i]);
                    }
                    if (addMsg(conn->fd, buffer, nbytes, pool) < 0) {
                        perror("addMsg");
                    }
                }
            }
            if (FD_ISSET(conn->fd, &pool->ready_write_set)) {
                if (writeToClient(conn->fd, pool) < 0) {
                    perror("writeToClient");
                }
            }
        }
    } while (!end_server);

    conn_t *conn = pool->conn_head;
    while (conn != NULL) {
        conn_t *next_conn = conn->next;
        removeConn(conn->fd, pool);
        conn = next_conn;
    }
    free(pool);
    close(socket_fd);

    return 0;
}

int initPool(conn_pool_t *pool) {
    pool->maxfd = -1;
    FD_ZERO(&pool->read_set);
    FD_ZERO(&pool->write_set);
    FD_ZERO(&pool->ready_read_set);
    FD_ZERO(&pool->ready_write_set);
    pool->nready = 0;
    pool->nr_conns = 0;
    pool->conn_head = NULL;
    return 0;
}

int addConn(int sd, conn_pool_t *pool) {
    conn_t *new_conn = malloc(sizeof(conn_t));
    if (new_conn == NULL) {
        perror("malloc");
        return -1;
    }

    new_conn->fd = sd;
    new_conn->prev = NULL;
    new_conn->next = pool->conn_head;
    new_conn->write_msg_head = NULL;
    new_conn->write_msg_tail = NULL;

    if (pool->conn_head)
        pool->conn_head->prev = new_conn;
    pool->conn_head = new_conn;
    pool->nr_conns++;

    FD_SET(sd, &pool->read_set);
    if (sd > pool->maxfd)
        pool->maxfd = sd;

    return 0;
}

int removeConn(int sd, conn_pool_t *pool) {
    conn_t *curr = pool->conn_head;
    conn_t *prev = NULL;

    while (curr != NULL && curr->fd != sd) {
        prev = curr;
        curr = curr->next;
    }

    if (curr == NULL) {
        return -1;
    }

    if (prev != NULL) {
        prev->next = curr->next;
    } else {
        pool->conn_head = curr->next;
    }
    if (curr->next != NULL) {
        curr->next->prev = prev;
    }

    msg_t *msg = curr->write_msg_head;
    while (msg != NULL) {
        msg_t *next_msg = msg->next;
        free(msg->message);
        free(msg);
        msg = next_msg;
    }

    printf("removing connection with sd %d \n", curr->fd);
    close(curr->fd);
    free(curr);
    pool->nr_conns--;
    FD_CLR(sd, &pool->read_set);
    FD_CLR(sd, &pool->write_set);

    if (pool->conn_head != NULL) {
        return pool->conn_head->fd;
    } else {
        return -1;
    }
}

int addMsg(int sd, char *buffer, int len, conn_pool_t *pool) {
    conn_t *conn;
    for (conn = pool->conn_head; conn != NULL; conn = conn->next) {
        if (conn->fd != sd) {
            msg_t *newmsg = malloc(sizeof(msg_t));
            if (newmsg == NULL) {
                perror("malloc");
                return -1;
            }

            newmsg->message = malloc(len);
            if (newmsg->message == NULL) {
                perror("malloc");
                free(newmsg);
                return -1;
            }

            memcpy(newmsg->message, buffer, len);
            newmsg->size = len;
            newmsg->next = NULL;

            if (conn->write_msg_tail) {
                conn->write_msg_tail->next = newmsg;
            } else {
                conn->write_msg_head = newmsg;
            }
            conn->write_msg_tail = newmsg;

            FD_SET(conn->fd, &pool->write_set);
            if (conn->fd > pool->maxfd)
                pool->maxfd = conn->fd;
        }
    }

    return 0;
}

int writeToClient(int sd, conn_pool_t *pool) {
    conn_t *conn;
    for (conn = pool->conn_head; conn != NULL; conn = conn->next) {
        if (conn->fd == sd) {
            msg_t *write_msg = conn->write_msg_head;
            while (write_msg) {
                int sent = write(sd, write_msg->message, write_msg->size);
                if (sent < 0) {
                    if (errno == EWOULDBLOCK) {
                        break;
                    } else {
                        perror("write");
                        return -1;
                    }
                }

                msg_t *next = write_msg->next;
                free(write_msg->message);
                free(write_msg);
                write_msg = next;
            }

            conn->write_msg_head = write_msg;
            if (write_msg == NULL) {
                FD_CLR(sd, &pool->write_set);
                conn->write_msg_tail = NULL;
            }

            break;
        }
    }

    return 0;
}