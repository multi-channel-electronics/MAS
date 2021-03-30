/* -*- mode: C; tab-width: 4; indent-tabs-mode: nil; c-basic-offset: 4 -*-
 *      vim: sw=4 ts=4 et tw=80
 */
#include <unistd.h>
#include <stdlib.h>
#include <signal.h>
#include <errno.h>
#include <sys/types.h>
#include <unistd.h>
#include <string.h>

#include "mce/socks.h"
#include "mce/defaults.h"
#include <libconfig.h>

#include "context.h"
#include <libmaslog.h>

#define CONFIG_CLIENT "log_client"
#define CONFIG_LOGADDR "log_address"

#define CLIENT_NAME "\x1b" "LOG:client_name "


static int get_string(const mce_context_t *context, char *dest,
        config_setting_t *parent, const char *name)
{
    config_setting_t *set =
        config_setting_get_member(parent, name);
    if (set==NULL) {
        mcelib_warning(context, "%s: key '%s' not found in config file\n",
                __func__, name);
        return -1;
    }
    strcpy(dest, config_setting_get_string(set));
    return 0;
}


maslog_t *maslog_connect(mce_context_t *context, char *name)
{
    maslog_t *logger;

    if (context->mas_cfg == NULL) {
        mcelib_error(context, "No mas.cfg found.\n");
        return NULL;
    }

    config_setting_t *client = config_lookup(context->mas_cfg, CONFIG_CLIENT);

    char address[SOCKS_STR];
    if (get_string(context, address, client, CONFIG_LOGADDR)!=0)
        return NULL;

    int sock = massock_connect(address, -1);
    if (sock<0) {
        mcelib_error(context, "%s: could not connect to logger at %s\n",
                __func__, address);
        return NULL;
    }

    // Ignore pipe signals, we'll handle the errors (right?)
    signal(SIGPIPE, SIG_IGN);

    char cmd[SOCKS_STR];
    sprintf(cmd, "%c%s%s[%u/%li]", '0'+MASLOG_ALWAYS, CLIENT_NAME, name,
            (unsigned)context->fibre_card, (long)getpid());
    int sent = send(sock, cmd, strlen(cmd)+1, 0);
    if (sent != strlen(cmd)+1) {
        mcelib_warning(context, "%s: failed to send client name\n", __func__);
    }

    logger = (maslog_t*)malloc(sizeof(struct maslog_struct));
    if (logger == NULL)
        return NULL;

    logger->fd = sock;
    logger->context = context;

    return logger;
}

int maslog_print(maslog_t *logger, const char *str)
{
    return maslog_print_level(logger, str, MASLOG_ALWAYS);
}

int maslog_print_level(maslog_t *logger, const char *str, int level)
{
    if (logger==NULL || logger->fd<=0)
        return -1;

    char packet[2048];
    int idx = 0;

    while (*str != 0) {
        packet[idx++] = '0' + level;
        while (*str != 0 && *str != '\n') {
            packet[idx++] = *str++;
        }
        if (*str == '\n')
            str++;
        packet[idx++] = 0;
    }

    int sent = send(logger->fd, packet, idx, 0);
    if (sent != idx) {
        if (sent==0) {
            mcelib_warning(logger->context,
                    "%s: connection closed, no further logging.\n",
                    __func__);
            maslog_close(logger);
        } else if (sent<0) {
            mcelib_warning(logger->context,
                    "%s: pipe error, errno=%i, no further logging.\n",
                    __func__, errno);
            maslog_close(logger);
        } else {
            mcelib_warning(logger->context,
                    "%s: partial send, logging will continue.\n",
                    __func__);
        }
    }
    return (logger->fd > 0) ? 0 : -1;
}

int maslog_write(maslog_t *logger, const char *buf, int size)
{
    if (logger==NULL || logger->fd<=0)
        return -1;

    int sent = send(logger->fd, buf, size, 0);
    if (sent != size) {
        mcelib_error(logger->context,
                "%s: logging failed, (send error %i/%i)\n", __func__,
                sent, size);
        maslog_close(logger);
        return -1;
    }
    return 0;
}

int maslog_close(maslog_t *logger)
{
    if (logger == NULL)
        return -1;

    if (logger->fd <= 0) {
        free(logger);
        return -1;
    }

    int ret = close(logger->fd);

    if (ret)
        mcelib_error(logger->context, "%s: close error, errno=%i\n", __func__,
                ret);
    else
        free(logger);

    return ret;
}
