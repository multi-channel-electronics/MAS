/* -*- mode: C; tab-width: 4; indent-tabs-mode: nil; c-basic-offset: 4 -*-
 *      vim: sw=4 ts=4 et tw=80
 */
#ifndef _DSP_IOCTL_H_
#define _DSP_IOCTL_H_

#include <linux/ioctl.h>

/* IOCTL stuff */

#define DSPDEV_IOC_MAGIC 'p'
#define DSPDEV_IOCT_RESET  _IO(DSPDEV_IOC_MAGIC,  0)
#define DSPDEV_IOCT_SPEAK  _IO(DSPDEV_IOC_MAGIC,  1)
#define DSPDEV_IOCT_ERROR  _IO(DSPDEV_IOC_MAGIC,  2)
#define DSPDEV_IOCT_CORE   _IO(DSPDEV_IOC_MAGIC,  3)
/*#define DSPDEV_IOCT_CORE_IRQ _IO(DSPDEV_IOC_MAGIC, 4)*/
#define DSPDEV_IOCT_LATENCY _IO(DSPDEV_IOC_MAGIC,  5)
#define DSPDEV_IOCT_HARD_RESET  _IO(DSPDEV_IOC_MAGIC,  6)

#endif
