/* -*- mode: C; tab-width: 4; indent-tabs-mode: nil; c-basic-offset: 4 -*-
 *      vim: sw=4 ts=4 et tw=80
 */

#include <stdio.h>
#include <stdlib.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <fcntl.h>
#include <stdarg.h>

#include <mce_library.h>
#include <mcedsp.h>
#include <mce/dsp_errors.h>
#include <mce/data_ioctl.h>
#include "context.h"

#include "sdsu.h"

/* CMD subsystem */
int mcecmd_sdsu_connect(mce_context_t context)
{
    char dev_name[20] = "/dev/mce_cmd";
    sprintf(dev_name + 12, "%u", (unsigned)context->dev_num);

    C_cmd.fd = open(dev_name, O_RDWR);
    if (C_cmd.fd < 0)
        return -MCE_ERR_DEVICE;

    return 0;
}

int mcecmd_sdsu_disconnect(mce_context_t context)
{
    if (close(C_cmd.fd) < 0)
        return -MCE_ERR_DEVICE;

    return 0;
}

int mcecmd_sdsu_ioctl(mce_context_t context, unsigned long int req, ...)
{
    int ret;
    va_list ap;

    va_start(ap, req);
    ret = ioctl(context->cmd.fd, req, va_arg(ap, void*));
    va_end(ap);

    return ret;
}

ssize_t mcecmd_sdsu_read(mce_context_t context, void *buf, size_t count)
{
    return read(context->cmd.fd, buf, count);
}

ssize_t mcecmd_sdsu_write(mce_context_t context, const void *buf, size_t count)
{
    return write(context->cmd.fd, buf, count);
}

/* DSP subsystem */
int mcedsp_sdsu_connect(mce_context_t context)
{
    int fd;
    char dev_name[18] = "/dev/mce_dsp";
    sprintf(dev_name + 12, "%u", (unsigned)context->dev_num);

    fd = open(dev_name, O_RDWR);
    if (fd < 0) {
        return -DSP_ERR_DEVICE;
    }

    context->dsp.fd = fd;
    context->dsp.opened = 1;

    return 0;
}

int mcedsp_sdsu_disconnect(mce_context_t context)
{
    if (close(context->dsp.fd) < 0)
        return -DSP_ERR_DEVICE;
    context->dsp.fd = -1;

    return 0;
}

int mcedsp_sdsu_ioctl(mce_context_t context, unsigned long int req, ...)
{
    int ret;
    va_list ap;

    va_start(ap, req);
    ret = ioctl(context->dsp.fd, req, va_arg(ap, void*));
    va_end(ap);

    return ret;
}

ssize_t mcedsp_sdsu_read(mce_context_t context, void *buf, size_t count)
{
    return read(context->dsp.fd, buf, count);
}

ssize_t mcedsp_sdsu_write(mce_context_t context, const void *buf, size_t count)
{
    return write(context->dsp.fd, buf, count);
}

/* DATA subsystem */
int mcedata_sdsu_connect(mce_context_t context)
{
	void *map;
	int map_size;
    char dev_name[20] = "/dev/mce_data";
    sprintf(dev_name + 13, "%u", (unsigned)context->dev_num);

	C_data.fd = open(dev_name, O_RDWR);
    if (C_data.fd < 0)
        return -MCE_ERR_DEVICE;

	// Non-blocking reads allow us to timeout
	/* Hey!  This makes single go (and thus ramp) very slow!! */
/* 	if (fcntl(C_data.fd, F_SETFL, fcntl(C_data.fd, F_GETFL) | O_NONBLOCK)) */
/* 		return -MCE_ERR_DEVICE; */

	// Obtain buffer size for subsequent mmap
	map_size = ioctl(C_data.fd, DATADEV_IOCT_QUERY, QUERY_BUFSIZE);
	if (map_size > 0) {
		map = mmap(NULL, map_size, PROT_READ | PROT_WRITE,
			   MAP_SHARED, C_data.fd, 0);
		if (map != NULL) {
			C_data.map = map;
			C_data.map_size = map_size;
		}
	}

    C_data.dev_name = dev_name;

	return 0;
}

int mcedata_sdsu_disconnect(mce_context_t context)
{
    if (C_data.map != NULL)
		munmap(C_data.map, C_data.map_size);

	C_data.map_size = 0;
	C_data.map = NULL;

	if (close(C_data.fd) < 0)
		return -MCE_ERR_DEVICE;

    return 0;
}

int mcedata_sdsu_ioctl(mce_context_t context, unsigned long int req, ...)
{
    int ret;
    va_list ap;

    va_start(ap, req);
    ret = ioctl(context->data.fd, req, va_arg(ap, void*));
    va_end(ap);

    return ret;
}

ssize_t mcedata_sdsu_read(mce_context_t context, void *buf, size_t count)
{
    return read(context->data.fd, buf, count);
}

ssize_t mcedata_sdsu_write(mce_context_t context, const void *buf, size_t count)
{
    return write(context->data.fd, buf, count);
}