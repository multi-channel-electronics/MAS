#ifndef MCENETD_H
#define MCENETD_H

#include <mce_library.h>

/* protocol version */
#define MCENETD_PROTO_VERSION 1

/* ports */
#define MCENETD_CTLPORT 1111
#define MCENETD_DSPPORT 1221
#define MCENETD_CMDPORT 2112
#define MCENETD_DATPORT 2222

/* protocol operations
 * note: for variable-length message, MSGLEN includes the opcode and MSGLEN
 * itself in its count */

#define MCENETD_STOP  0x00
/* 00 <MSGLEN> <ERRCODE> ... */

#define MCENETD_HELLO 0x01
/* 01 <MAGIC1> <MAGIC2> <MAGIC3> <UDEPTH> <DEV_INDEX> */

#define MCENETD_READY 0x02
/* 02 <VERSION> <SER> <DDEPTH> <ENDPT> <FLAGS> */

#define MCENETD_CMDIOCTL 0x03
/* 03 <32-bit REQ> <32-bit ARG> */

#define MCENETD_IOCTLRET 0x04
/* 04 <MSGLEN> <32-bit RET> ... */

/* fixed-length message lengths, this count includes the opcode */
#define MCENETD_MSGLEN(op) ( \
    (op == MCENETD_HELLO) ? 6 : \
    (op == MCENETD_READY) ? 6 : \
    (op == MCENETD_CMDIOCTL) ? 9 : \
    0 ) /* zero implies a variable length message */

/* error responses */
#define MCENETD_ERR_INDEX   0x00 /* dev index out of range */
#define MCENETD_ERR_BALROG  0x01 /* too much recursion */
#define MCENETD_ERR_PENDING 0x02 /* an operation is already in progress */

/* protocol magic */
#define MCENETD_MAGIC1 0x3C
#define MCENETD_MAGIC2 0xEA
#define MCENETD_MAGIC3 0xE4

/* server flags */
#define MCENETD_F_MAS_OK 0x01 /* MAS initialised */
#define MCENETD_F_DSP_OK 0x02 /* DSP device available */
#define MCENETD_F_CMD_OK 0x04 /* CMD device available */
#define MCENETD_F_DAT_OK 0x08 /* DAT device available */
#define MCENETD_F_ACQ    0x10 /* acquisition in progress */

/* generic functions */
ssize_t mcenet_readmsg(int d, unsigned char *msg, size_t l);

#endif
