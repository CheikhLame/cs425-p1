#include "lab.h"

#include <errno.h>
#include <netdb.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>

int smtp_socket_connect(const char *host, const char *port)
{
    struct addrinfo hints;
    struct addrinfo *res = NULL;
    struct addrinfo *rp = NULL;
    int fd = -1;
    int rc;

    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;

    rc = getaddrinfo(host, port, &hints, &res);
    if (rc != 0) // GCOVR_EXCL_START
    {
        fprintf(stderr, "myapp: could not resolve %s:%s: %s\n", host, port,
                gai_strerror(rc));
        return -1;
    } // GCOVR_EXCL_STOP    

    for (rp = res; rp != NULL; rp = rp->ai_next)
    {
        fd = socket(rp->ai_family, rp->ai_socktype, rp->ai_protocol);
        if (fd == -1) // GCOVR_EXCL_START
        {
            continue;
        } 
        if (connect(fd, rp->ai_addr, rp->ai_addrlen) == 0)
        {
            break;
        }
        close(fd);
        fd = -1; 
    } // GCOVR_EXCL_STOP

    freeaddrinfo(res);

    if (fd == -1) // GCOVR_EXCL_START
    {
        fprintf(stderr, "myapp: could not connect to %s:%s: %s\n", host, port,
                strerror(errno));
        return -1;
    } // GCOVR_EXCL_STOP

    return fd;
}

static long socket_read(void *ctx, char *buf, size_t len)
{
    int fd = *(int *) ctx;
    return (long) recv(fd, buf, len, 0);
}

static long socket_write(void *ctx, const char *buf, size_t len)
{
    int fd = *(int *) ctx;
    return (long) send(fd, buf, len, 0);
}

smtp_transport_t smtp_socket_transport(int *fd)
{
    smtp_transport_t t;
    t.read = socket_read;
    t.write = socket_write;
    t.ctx = fd;
    return t;
}