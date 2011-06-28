/* -*- mode: C; tab-width: 4; indent-tabs-mode: nil; c-basic-offset: 4 -*-
 *      vim: sw=4 ts=4 et tw=80
 */
#include <mcenetd.h>
#include <mce_library.h>
#include <poll.h>
#include "context.h"

/* poll a socket for an event */
int mcenet_poll(int fd, short event, const char *name)
{
    struct pollfd pfd = { fd, event, 0 };

    int r = poll(&pfd, 1, 1000);
    if (r == -1) {
        perror("mcenet: poll");
        return 1;
    } else if (r == 0) {
        fprintf(stderr, "mcenet: timout waiting for %s\n", name);
        return 1;
    } else if (pfd.revents & POLLERR) {
        fprintf(stderr, "mcenet: error waiting for %s\n", name);
        return 1;
    } else if (pfd.revents & POLLHUP) {
        fprintf(stderr, "mcenet: connection dropped to %s\n", name);
        return 1;
    }

    return 0;
}

/* send a request on the control channel, the response is stored in message */
ssize_t mcenet_req(mce_context_t context, char *message, size_t len)
{
    ssize_t n;

    /* wait for the net.socket to become ready */
    if (mcenet_poll(context->net.sock, POLLOUT, context->dev_name))
        return -1;

    /* write the request */
    n = write(context->net.sock, message, len);
    if (n < len) {
        fprintf(stderr, "mcenet: short write to %s\n", context->dev_name);
        return -1;
    }

    /* wait for a response */
    if (mcenet_poll(context->net.sock, POLLIN, context->dev_name))
        return -1;

    /* read the response */
    n = read(context->net.sock, message, 256);

    return n;
}

/* initialise a connection */
int mcenet_hello(mce_context_t context)
{
    int l;
    char message[256];

    message[0] = MCENETD_HELLO;
    message[1] = MCENETD_MAGIC1;
    message[2] = MCENETD_MAGIC2;
    message[3] = MCENETD_MAGIC3;
    message[4] = context->net.udepth;
    message[5] = context->dev_num;
    
    l = mcenet_req(context, message, MCENETD_HELLO_L);

    if (l < 0)
        return 1;

    if (message[0] == MCENETD_STOP) {
        fprintf(stderr, "mcenet: server reports device %s not ready.\n",
                context->url);
        return 1;
    } else if (message[0] != MCENETD_READY) {
        fprintf(stderr, "mcenet: unexpected response (%02x) from server %s\n",
                message[0], context->url);
        return 1;
    } else if (l < MCENETD_READY_L) {
        fprintf(stderr, "mcenet: short read from %s.\n", context->dev_name);
        return 1;
    }

    /* otherwise we should be good to go; record the data */
    context->dev_endpoint = message[10];
    context->net.ddepth = message[1];
    context->net.token = *(uint64_t*)(message + 2);
    context->net.dsp_ok = message[11];
    context->net.cmd_ok = message[12];
    context->net.dat_ok = message[13];

    return 0;
}
