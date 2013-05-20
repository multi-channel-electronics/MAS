/* -*- mode: C; tab-width: 4; indent-tabs-mode: nil; c-basic-offset: 4 -*-
 *      vim: sw=4 ts=4 et tw=80
 */
#define _GNU_SOURCE

#include <stdio.h>
#include <stdlib.h>
#include <errno.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <signal.h>
#include <linux/types.h>

#include "dspioctl.h"
#include "new_dsp.h"

typedef __s32 int32;

int fd = 0;

int _read_io(int cmd) {
    return ioctl (fd, cmd,0);
}

void _write_io(int cmd, int val) {
    ioctl(fd, cmd, val);
}

#define read_io(KEY) _read_io(DSPIOCT_R_ ## KEY)
#define write_io(KEY,VAL) _write_io(DSPIOCT_W_ ## KEY, VAL)


static inline int get_hrrq() { return read_io(HSTR) & 0x04; }
static inline int get_hf4()  { return read_io(HSTR) & 0x10; }


int write_cmd(struct dsp_command *cmd) {
    if (cmd->size != cmd->data[0] & 0xffff + 1) {
        printf("Bad packet!\n");
        return -1;
    }
    return ioctl(fd, DSPIOCT_COMMAND, cmd);
}

int read_reply(struct dsp_reply *reply) {
    return ioctl(fd, DSPIOCT_REPLY, reply);
}

void dump_regs() {
    printf(" HSTR: %06x\n", read_io(HSTR));
    printf(" HCVR: %06x\n", read_io(HCVR));
    printf(" HCTR: %06x\n", read_io(HCTR));
    printf("\n");
    
}

void mode_on() {
    printf("Raising HF2\n");
    int x = read_io(HCTR);
    write_io(HCTR, x | 0x20);
}

void mode_off() {
    printf("Lowering HF2\n");
    int x = read_io(HCTR);
    write_io(HCTR, x & ~0x20);
}

void int_handler(int sig) {
    if (sig == SIGINT) {
        printf("Stopping!\n");
        mode_off();
    }
    // Hand it up.
    signal(sig, SIG_DFL);
    raise(sig);
}

void exit_now(int err) {
    mode_off();
    exit(err);
}

#pragma push
#pragma pack(1)

#define DSP_STAT 10
#define MCE_DATA 11
#define MCE_REP  12

typedef struct {
    int type;
    int size;
    int32 *data;
} base_packet_t;

typedef struct {
    // Overlay for packet data.
    int32 stuff;
} dspstatus_t;
    
#pragma pop


/*
 * Packet reception
 */

void clear_hrrq() {
    while (get_hrrq()) {
        int i1 = read_io(HRXS);
        usleep(10);
    }
}


int32 read_pair() {
    /* printf( "read pair... (HTSR=%x)\n", read_io(HSTR)); */
    // Reconstruct 32 bit word from pair of hrxs reads.
    while (!get_hrrq());
    int i1 = read_io(HRXS);
    while (!get_hrrq());
    int i2 = read_io(HRXS);
    /* printf( "read pair... %i %i\n", i1, i2); */
    return (i1 & 0xffff) << 16 | (i2 & 0xffff);
}

int recv_packet(base_packet_t *packet, int uwait) {

    // Wait for packet start.
    while (!get_hrrq()) {
        usleep(10000);
        uwait -= 10000;
        if (uwait < 0)
            return -1;
    }

    // Get the first word
    
    int packet_info = read_pair();
    printf("packet_info: %x\n", packet_info);
    int ptype = (packet_info >> 16) & 0xff;
    int plen = (packet_info & 0xffff);

    // Get the rest of the words.
    packet->type = ptype;
    packet->size = plen;
    packet->data = (int32*)malloc(plen*sizeof(int32));
    printf("plen=%i\n", plen);
    for (int i=0; i<plen; i++) {
        printf("i=%i\n", i);
        packet->data[i] = read_pair();
    }

    return 0;
}

void summarize(base_packet_t *bp) {
    printf(" type = %i\n size = %i\n", bp->type, bp->size);
    if (bp->type == DSP_STAT) {
        for (int i=0; i<bp->size; i++) {
            printf("  payload %3i = %08x\n", i, bp->data[i]);
        }
    }
}

int main(void) {
    int x, y, z, i, j, k;
    fd = open("/dev/mce_test0", O_RDWR);
    printf("Hi. fd=%i\n\n", fd);
    if (fd<=0) 
        return -1;

    printf("On entry:\n");
	dump_regs();

    if (read_io(HCTR) & 0x20) {
        printf("We were still on; offing and exiting.\n");
        mode_off();
        dump_regs();
        return 0;
    }

    /* printf("Disabling prefetch\n"); */
    /* write_io(HCTR, read_io(HCTR) | 0x80); */

    
    if (0) {
        clear_hrrq();
        dump_regs();
        return 0;
    }

    //IOCT test...
    struct dsp_command cmd;
    struct dsp_reply rep;

    // Test responsiveness:
    if (get_hf4()) {
        printf("HF4 is high on entry, writing one word...\n");
        write_io(HTXR, 0x0);
        return 0;
    }

    mode_on();
    usleep(100000);
    if (!get_hf4()) {
        printf("No handshake!\n");
        mode_off();
        return 0;
    }

    mode_off();
    usleep(100000);
    if (get_hf4()) {
        printf("HF4 did not drop.\n");
        return 0;
    }

    dump_regs();


/*
 * Set up commanding.
 */

    // Kill this mode on exit?
    mode_on();
    // signal(SIGINT, int_handler);

    // Cause upload of DMA destination
    printf("Setting DMA REP_BUF\n");
    int err = ioctl(fd, DSPIOCT_SET_REP_BUF);
    printf(" err=%i\n", err);
    dump_regs();

/*
 * Send a test command
 */

    cmd.size=2;
    cmd.flags = DSP_EXPECT_DSP_REPLY;
    cmd.data[0] = cmd.size-1 + DSP_CMD_READ_X;
    cmd.data[1] = 0x3;
//    cmd.data[2] = 0xdead;       //       3
    err = write_cmd(&cmd);
    printf("write_cmd err=%i\n", err);
    usleep(500000);
    
    char line[256];
    /* printf("waiting for string > "); */
    /* fgets(line, 256, stdin); */

    ioctl(fd, DSPIOCT_DUMP_BUF);
    exit_now(0);
    
    /* printf("waiting for string > "); */
    /* fgets(line, 256, stdin); */

    dump_regs();
    exit_now(0);


    // Trigger an update.
    ioctl(fd, DSPIOCT_TRIGGER_FAKE);
    usleep(500000);

    while (1) {
        printf (" alive...\n");
        usleep(500000);
    }

    // Wait for packets?
    base_packet_t bp;
    while (0) {
        if (recv_packet(&bp, 1000000)==0) {
            printf("Message received [type=%i,size=%i].\n", bp.type, bp.size);
            break;
        } else 
            printf("Nothing yet.\n");
    }
    


    printf("stopping.\n");
    usleep(200000);
    // Shut down the test and pick up strays.
    mode_off();
    i = 0;
    while (get_hf4() || get_hrrq()) {
        printf("Waiting for hf4 to drop...\n");
        usleep(100000);
        while (get_hrrq()) {
            j = read_io(HRXS);
            i += 1;
            printf("  %x\n", j);
        }
    }
    printf(" %i strays\n", i);

    close(fd);
    return 0;
}
