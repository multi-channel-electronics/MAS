/* -*- mode: C; tab-width: 4; indent-tabs-mode: nil; c-basic-offset: 4 -*-
 *      vim: sw=4 ts=4 et tw=80
 */
#ifndef _MCE_IOCTL_H_
#define _MCE_IOCTL_H_

/* IOCTL stuff */

#include <linux/ioctl.h>

#define MCEDEV_IOC_MAGIC 'r'

#define MCEDEV_IOCT_RESET             _IO(MCEDEV_IOC_MAGIC,  0)

#define MCEDEV_IOCT_QUERY             _IO(MCEDEV_IOC_MAGIC,  1)

#define MCEDEV_IOCT_HARDWARE_RESET    _IO(MCEDEV_IOC_MAGIC,  2)

#define MCEDEV_IOCT_INTERFACE_RESET   _IO(MCEDEV_IOC_MAGIC,  3)

#define MCEDEV_IOCT_LAST_ERROR        _IO(MCEDEV_IOC_MAGIC,  4)

#define MCEDEV_IOCT_GET               _IO(MCEDEV_IOC_MAGIC,  5)

#define MCEDEV_IOCT_SET               _IO(MCEDEV_IOC_MAGIC,  6)

/* Flags for MCEDEV_IOCT_SET */
#define MCEDEV_CLOSE_CLEANLY      (1 <<  0) /* Go to idle if user closes  */
#define MCEDEV_CLOSED_CHANNEL     (1 <<  1) /* Only writer can read reply */

#endif
