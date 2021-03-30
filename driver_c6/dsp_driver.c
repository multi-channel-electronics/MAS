/* -*- mode: C; tab-width: 4; indent-tabs-mode: nil; c-basic-offset: 4 -*-
 *      vim: sw=4 ts=4 et tw=80
 */
/*
   dsp_driver.c

   Contains all PCI related code, including register definitions and
   lowest level i/o routines.
   */

#include <linux/version.h>
#include <linux/kernel.h>
#include <linux/pci.h>
#include <linux/interrupt.h>
#include <linux/sched.h>
#include <linux/delay.h>

#include "mce_options.h"
#include "autoversion.h"

#ifdef REALTIME
#include <rtai.h>
#endif

#ifndef OLD_KERNEL
#  include <linux/dma-mapping.h>
/* Newer 2.6 kernels use IRQF_SHARED instead of SA_SHIRQ */
#  ifdef IRQF_SHARED
#    define IRQ_FLAGS IRQF_SHARED
#  else
#    define IRQ_FLAGS SA_SHIRQ
#  endif
#else
#  define IRQ_FLAGS 0
#endif

#if LINUX_VERSION_CODE >= KERNEL_VERSION(4,15,0)
#    define NEW_TIMER
#endif

#include "dsp_driver.h"
#include "mce/ioctl.h"

/*
 *  dsp_driver.c includes
 */

#include <linux/module.h>
#include <linux/init.h>

#include <linux/slab.h>
#include <linux/fs.h>
#include <linux/errno.h>
#include <linux/types.h>
#include <linux/proc_fs.h>
#include <linux/fcntl.h>
#include <asm/uaccess.h>

#include "mce_driver.h"
#include "dsp_ops.h"
#include "proc.h"

MODULE_LICENSE("GPL");
MODULE_AUTHOR ("Matthew Hasselfield");


/*
   Each command is mapped to a particular DSP host control vector
   address.  Those can be firmware specific, but aren't so far.
   */

enum dsp_vector_type {
    VECTOR_STANDARD, // 4 words into htxr
    VECTOR_QUICK,    // no htxr, no reply expected
};

struct dsp_vector {
    u32 key;
    u32 vector;
    enum dsp_vector_type type;
};

#define NUM_DSP_CMD 17

static struct dsp_vector dsp_vector_set[NUM_DSP_CMD] = {
    {DSP_WRM, 0x0078, VECTOR_STANDARD},
    {DSP_RDM, 0x007A, VECTOR_STANDARD},
    {DSP_VER, 0x007A, VECTOR_STANDARD},
    {DSP_GOA, 0x007C, VECTOR_STANDARD},
    {DSP_STP, 0x007E, VECTOR_STANDARD},
    {DSP_RST, 0x0080, VECTOR_STANDARD},
    {DSP_CON, 0x0082, VECTOR_STANDARD},
    {DSP_HST, 0x0084, VECTOR_STANDARD},
    {DSP_RCO, 0x0086, VECTOR_STANDARD},
    {DSP_QTS, 0x0088, VECTOR_STANDARD},
    {DSP_INT_RST, HCVR_INT_RST, VECTOR_QUICK},
    {DSP_INT_DON, HCVR_INT_DON, VECTOR_QUICK},
    {DSP_INT_RPC, HCVR_INT_RPC, VECTOR_QUICK},
    {DSP_SYS_ERR, HCVR_SYS_ERR, VECTOR_QUICK},
    {DSP_SYS_RST, HCVR_SYS_RST, VECTOR_QUICK},
};


typedef struct {

    u32 code;
    dsp_handler handler;
    unsigned long data;

} dsp_handler_entry;

#define MAX_HANDLERS 16

/* Here's the deal with the states.  When a DSP command is issued,
 * IDLE -> CMD.  At this time the commander may request that RESERVE
 * be set.  The CMD bit is cleared once the DSP reply is received (or
 * times out).  The RESERVE bit must be cleared by whomever set it (or
 * someone else).  When either of CMD or RESERVE are set, no DSP
 * commands can be issued and send_command will return -EAGAIN.
 *
 * High priority tasks include the clearing of the quiet-RP buffer
 * (RPC) and the sending of frame buffer updates to the DSP (INFORM).
 * When these flags (PRIORITY) are set, the driver sets a tasklet to
 * accomplish them and in the meantime blocks any other DSP commands.
 *
 */

typedef enum {
    /* These are bit flags.  Except IDLE.  It's nothin'. */
    DDAT_IDLE     = 0x00,
    DDAT_CMD      = 0x01, /* Command in progress; awaiting reply */
    DDAT_RESERVE  = 0x02, /* Commander is reserved by some agent */
    DDAT_INFORM   = 0x10, /* QT system wants to write grant to DSP */
    DDAT_RPC      = 0x20, /* RP flag needs to be cleared ASAP */
    DDAT_DSP_INT  = 0x40, /* Oustanding internal DSP command, e.g. QTS. */
    /* Masks */
    DDAT_BUSY     = 0x0F, /* Commander unavailable, for anything. */
    DDAT_PRIORITY = 0xF0, /* Outstanding priority tasks */
} dsp_state_t;


/* This structure helps provide a blocking commander that can service
   other driver levels. */

struct dsp_local {
    wait_queue_head_t queue;
    dsp_command *cmd;
    dsp_message *msg;
    int flags;
#define   LOCAL_CMD 0x01
#define   LOCAL_REP 0x02
#define   LOCAL_ERR 0x08
};


/* Mode bits in DSP firmware (U0105+) - in particular, we want to set
 * up NOIRQ and HANDSHAKE before issuing any DSP commands that will
 * interrupt and reply.  */

#define DSP_MODE_APP          0x0001
#define DSP_MODE_MCE          0x0002
#define DSP_MODE_QUIETDA      0x0004
#define DSP_MODE_QUIETRP      0x0008

struct dsp_dev_t {

    int enabled;
    int card;

    struct pci_dev *pci;
    dsp_reg_t *dsp;
    int hcvr_bits;

    int comm_mode;
    irq_handler_t int_handler;

    struct tasklet_struct handshake_tasklet;
    struct timer_list tim_poll;

    struct dsp_local local;

    struct timer_list tim_dsp;

    spinlock_t lock;
    int cmd_count;
    int rep_count;

    volatile dsp_state_t state;
    int version;
    char version_string[32];

    int n_handlers;
    dsp_handler_entry handlers[MAX_HANDLERS];

    dsp_command last_command;
    dsp_callback callback;

    /* Priority stuff */
    int inform_tail;
    struct tasklet_struct priority_tasklet;

} dsp_dev[MAX_CARDS];


/* Internal prototypes */

void  dsp_driver_remove(struct pci_dev *pci);

int   dsp_driver_probe(struct pci_dev *pci, const struct pci_device_id *id);

static int dsp_quick_command(struct dsp_dev_t *dev, u32 vector);

/* Data for PCI enumeration */

static const struct pci_device_id pci_ids[] = {
    { PCI_DEVICE(DSP_VENDORID, DSP_DEVICEID) },
    { 0 },
};

static struct pci_driver pci_driver = {
    .name = "mce_dsp",
    .id_table = pci_ids,
    .probe = dsp_driver_probe,
    .remove = dsp_driver_remove,
};


/* DSP register wrappers */

static inline int dsp_read_hrxs(dsp_reg_t *dsp) {
    return ioread32((void*)&(dsp->htxr_hrxs));
}

static inline int dsp_read_hstr(dsp_reg_t *dsp) {
    return ioread32((void*)&(dsp->hstr));
}

static inline int dsp_read_hcvr(dsp_reg_t *dsp) {
    return ioread32((void*)&(dsp->hcvr));
}

static inline int dsp_read_hctr(dsp_reg_t *dsp) {
    return ioread32((void*)&(dsp->hctr));
}

static inline void dsp_write_htxr(dsp_reg_t *dsp, u32 value) {
    iowrite32(value, (void*)&(dsp->htxr_hrxs));
}

static inline void dsp_write_hcvr(dsp_reg_t *dsp, u32 value) {
    iowrite32(value, (void*)&(dsp->hcvr));
}

static inline void dsp_write_hctr(dsp_reg_t *dsp, u32 value) {
    iowrite32(value, (void*)&(dsp->hctr));
}


#define DDAT_LOCK    spin_lock_irqsave(&dev->lock, irqflags)
#define DDAT_UNLOCK  spin_unlock_irqrestore(&dev->lock, irqflags)

void dsp_ack_int_or_schedule(unsigned long data)
{
    struct dsp_dev_t *dev = (struct dsp_dev_t*)data;
    /* Check that DSP has dropped HF3 */
    if (dsp_read_hstr(dev->dsp) & HSTR_HC3) {
        PRINT_ERR(dev->card, "Rescheduling int ack.");
        tasklet_schedule(&dev->handshake_tasklet);
    } else {
        dsp_write_hctr(dev->dsp, dev->comm_mode);
    }
}


irqreturn_t pci_int_handler(int irq, void *dev_id, struct pt_regs *regs)
{
    /* Note that the regs argument is deprecated in newer kernels,
       do not use it.  It is left here for compatibility with
       -2.6.18                                                    */

    struct dsp_dev_t *dev = NULL;
    dsp_reg_t *dsp = NULL;
    dsp_message msg;
    int i=0, j=0, k=0;
    int n = sizeof(msg) / sizeof(u32);
    int s = 0;

    for(k=0; k<MAX_CARDS; k++) {
        dev = dsp_dev + k;
        if (dev_id == dev) {
            dsp = dev->dsp;
            break;
        }
    }

    if (dev_id != dev)
        return IRQ_NONE;

    //Verify handshake bit
    if ( !(dsp_read_hstr(dsp) & HSTR_HC3) ) {
        /* not our interrupt -- another device interrupted on our
           shared IRQ */
        return IRQ_NONE;
    }

    // Interrupt hand-shaking changed in U0105.
    if (dev->comm_mode & DSP_PCI_MODE_HANDSHAKE) {
        // Raise HF0 to acknowledge that IRQ is being handled.
        // DSP will lower INTA and then HF3, and wait for HF0 to fall.
        dsp_write_hctr(dev->dsp, dev->comm_mode | HCTR_HF0);
    } else {
        // Host command to clear INTA
        dsp_quick_command(dev, HCVR_INT_RST);
    }

    // Read data into dsp_message structure
    while ( i<n && ((s=dsp_read_hstr(dsp)) & HSTR_HRRQ) ) {
        ((u32*)&msg)[i++] = dsp_read_hrxs(dsp) & DSP_DATAMASK;
    }
    if (i<n)
        PRINT_ERR(dev->card, "incomplete message %i/%i (last HSTR=%#x).\n",
                i, n, s);

    // We are done with the DSP, so release it.
    if (dev->comm_mode & DSP_PCI_MODE_HANDSHAKE) {
        dsp_ack_int_or_schedule((unsigned long)dev);
    } else {
        // Host command to clear HF3
        dsp_quick_command(dev, HCVR_INT_DON);
    }

    // Discover message handler
    for (j=0; j < dev->n_handlers; j++) {
        if ( (dev->handlers[j].code == msg.type) &&
                (dev->handlers[j].handler != NULL) ) {
            dev->handlers[j].handler(&msg, dev->handlers[j].data);
        }
    }

    PRINT_INFO(dev->card, "ok\n");
    return IRQ_HANDLED;
}


/*
 * This will handle REP interrupts from the DSP, which correspond to
 * replies to DSP commands.
 */
int dsp_reply_handler(dsp_message *msg, unsigned long data)
{
    unsigned long irqflags;
    struct dsp_dev_t *dev = (struct dsp_dev_t *)data;
    dsp_callback callback = NULL;
    int complain = 0;

    DDAT_LOCK;
    if (dev->state & DDAT_CMD) {
        PRINT_INFO(dev->card, "REP received, calling back.\n");
        // Store a copy of the callback address before going to IDLE
        callback = dev->callback;
    } else {
        PRINT_ERR(dev->card,
                "unexpected REP received [state=%i, %i %i].\n",
                dev->state, dev->cmd_count, dev->rep_count);
    }
    DDAT_UNLOCK;

    /* This is probably the best place to check packet consistency. */
    if (msg->command != dev->last_command.command) {
        PRINT_ERR(dev->card, "reply does not match command.\n");
        complain = 1;
    }
    if (msg->reply != DSP_ACK) {
        PRINT_ERR(dev->card, "reply was not ACK.\n");
        complain = 1;
    }
    if (complain) {
        dsp_command *cmd = &dev->last_command;
        PRINT_ERR(dev->card, "command %#06x %#06x %#06x %#06x\n",
                cmd->command, cmd->args[0], cmd->args[1], cmd->args[2]);
        PRINT_ERR(dev->card, "reply   %#06x %#06x %#06x %#06x\n",
                msg->type, msg->command, msg->reply, msg->data);
    }

    if (callback != NULL) {
        int card = dev - dsp_dev;
        callback(0, msg, card);
    }

    DDAT_LOCK;
    dev->state &= ~DDAT_CMD;
    dev->rep_count++;
    DDAT_UNLOCK;
    return 0;
}

/* This will handle HEY interrupts from the DSP, which are generic
 * communications from the DSP typically used for debugging.
 */
int dsp_hey_handler(dsp_message *msg, unsigned long data)
{
    PRINT_ERR(((struct dsp_dev_t *)data)->card,
            "dsp HEY received: %06x %06x %06x\n",
            msg->command, msg->reply, msg->data);
    return 0;
}

void dsp_timeout(
#ifdef NEW_TIMER
        struct timer_list *t
#else
        unsigned long data
#endif
        )
{
#ifdef NEW_TIMER
    struct dsp_dev_t *dev = from_timer(dev, t, tim_dsp);
#else
    struct dsp_dev_t *dev = (struct dsp_dev_t*)data;
#endif
    unsigned long irqflags;

    DDAT_LOCK;
    if (dev->state & DDAT_CMD) {
        dev->state &= ~DDAT_CMD;
        DDAT_UNLOCK;

        PRINT_ERR(dev->card, "dsp reply timed out!\n");
        if (dev->callback != NULL) {
            int card = dev - dsp_dev;
            dev->callback(-DSP_ERR_TIMEOUT, NULL, card);
        }
    } else {
        DDAT_UNLOCK;
        PRINT_INFO(dev->card, "timer ignored\n");
    }
}


/*
   DSP command sending framework

   Modules issue dsp commands and register a callback function with

   dsp_send_command_now

   This looks up the vector address by calling dsp_lookup_vector,
   registers the callback in the device structure, and issues the
   command to the correct vector by calling
   dsp_send_command_now_vector.
   */
static int dsp_quick_command(struct dsp_dev_t *dev, u32 vector)
{
    PRINT_INFO(dev->card, "sending vector %#x\n", vector);
    if (dsp_read_hcvr(dev->dsp) & HCVR_HC) {
        PRINT_ERR(dev->card, "HCVR blocking\n");
        return -EAGAIN;
    }
    dsp_write_hcvr(dev->dsp, vector | dev->hcvr_bits | HCVR_HC);
    return 0;
}


struct dsp_vector *dsp_lookup_vector(dsp_command *cmd, int card)
{
    int i;
    for (i = 0; i < NUM_DSP_CMD; i++)
        if (dsp_vector_set[i].key == cmd->command)
            return dsp_vector_set+i;

    PRINT_ERR(card, "could not identify command %#x\n", cmd->command);
    return NULL;
}


int dsp_send_command_now_vector(struct dsp_dev_t *dev, u32 vector,
        dsp_command *cmd)
{
    int i = 0;
    int n = sizeof(dsp_command) / sizeof(u32);
    int s = 0;

    // DSP may block while HCVR interrupts in some cases.
    if (dsp_read_hcvr(dev->dsp) & HCVR_HC) {
        PRINT_ERR(dev->card, "HCVR blocking\n");
        return -EAGAIN;
    }

    // HSTR must be ready to receive
    if ( !((s=dsp_read_hstr(dev->dsp)) & HSTR_TRDY) ) {
        /* DSP56301 errata ED46: such a read may sometimes return
         * the value of HCVR instead of HSTR... hopefully this will
         * help us catch that. */
        PRINT_ERR(dev->card, "HSTR not ready to transmit (%#x)!\n", s);
        return -EIO;
    }

    //Write bytes and interrupt
    while ( i<n && ((s=dsp_read_hstr(dev->dsp)) & HSTR_HTRQ)!=0 )
        dsp_write_htxr(dev->dsp, ((u32*)cmd)[i++]);

    if (i<n) {
        PRINT_ERR(dev->card, "HTXR filled up during write! HSTR=%#x\n", s);
        return -EIO;
    }

    dsp_write_hcvr(dev->dsp, vector | dev->hcvr_bits | HCVR_HC);
    return 0;
}

int dsp_send_command_now(struct dsp_dev_t *dev, dsp_command *cmd)
{
    struct dsp_vector *vect = dsp_lookup_vector(cmd, dev->card);

    PRINT_INFO(dev->card, "cmd=%06x\n", cmd->command);

    if (vect==NULL)
        return -ERESTARTSYS;

    switch (vect->type) {
        case VECTOR_STANDARD:
            memcpy(&dev->last_command, cmd, sizeof(*cmd));
            return dsp_send_command_now_vector(dev, vect->vector, cmd);
        case VECTOR_QUICK:
            // FIXME: these don't reply so they'll always time out.
            return dsp_quick_command(dev, vect->vector);
    }

    PRINT_ERR(dev->card,
            "unimplemented vector command type %06x for command %06x\n",
            vect->type, cmd->command);
    return -ERESTARTSYS;
}


/*
 * DSP command sending framework
 *
 * - The most straight-forward approach to sending DSP commands is to
 *   call dsp_command with pointers to the full dsp_command structure
 *   and an empty dsp_message structure.
 *
 * - A non-blocking version of the above that allows the caller to
 *   specify a callback routine that will be run upon receipt of the
 *   DSP ack/err interrupt message is exposed as dsp_command_nonblock.
 *   The calling module must notify the dsp module that the message has
 *   been processed by calling dsp_clear_commflags; this cannot be done
 *   from within the callback routine!
 *
 * - The raw, no-semaphore-obtaining, no-flag-checking-or-setting,
 *   non-invalid-command-rejecting command issuer is dsp_command_now.
 *   Don't use it.  Only I'm allowed to use it.
 */


/* For better or worse, this routine returns linux error codes.
 *
 * The callback error codes are 0 (success, msg will be non-null) or
 * DSP_ERR_TIMEOUT (failure, msg will be null).
 *
 * The "reserve" option can be a combination of:
 *   DSP_REQ_RESERVE - execute the command (if possible) and also
 *                     reserve the DSP (while awaiting an MCE reply,
 *                     for example).
 *   DSP_REQ_PRIORITY - execute as high-priority (should only be used
 *                      by the priority tasklet).
 */

int dsp_send_command(dsp_command *cmd, dsp_callback callback, int card,
        dsp_request_t reserve)
{
    struct dsp_dev_t *dev = dsp_dev + card;
    unsigned long irqflags;
    int err = 0;

    DDAT_LOCK;

    // Locked due to command, reservation, or pending priority command?
    if ((dev->state & DDAT_BUSY) ||
            ((dev->state & DDAT_PRIORITY) && (reserve != DSP_REQ_PRIORITY))
       ) {
        DDAT_UNLOCK;
        return -EAGAIN;
    }

    PRINT_INFO(card, "send %i\n", dev->cmd_count+1);
    if ((err = dsp_send_command_now(dev, cmd)) == 0) {
        dev->state |= DDAT_CMD;
        // Commander reserves DSP?
        if (reserve == DSP_REQ_RESERVE)
            dev->state |= DDAT_RESERVE;
        dev->cmd_count++;
        dev->callback = callback;
        mod_timer(&dev->tim_dsp, jiffies + DSP_DEFAULT_TIMEOUT);
    }

    PRINT_INFO(card, "returning [%i]\n", err);
    DDAT_UNLOCK;
    return err;
}


void dsp_unreserve(int card)
{
    unsigned long irqflags;
    struct dsp_dev_t *dev = dsp_dev + card;
    DDAT_LOCK;
    dev->state &= ~DDAT_RESERVE;
    DDAT_UNLOCK;
}


int dsp_send_command_wait_callback(int error, dsp_message *msg, int card)
{
    unsigned long irqflags;
    struct dsp_dev_t *dev = dsp_dev + card;

    if (dev->local.flags != LOCAL_CMD) {
        PRINT_ERR(card, "unexpected flags, cmd=%x rep=%x err=%x\n",
                dev->local.flags & LOCAL_CMD,
                dev->local.flags & LOCAL_REP,
                dev->local.flags & LOCAL_ERR);
        return -1;
    }
    memcpy(dev->local.msg, msg, sizeof(*dev->local.msg));
    DDAT_LOCK;
    dev->local.flags |= LOCAL_REP;
    wake_up_interruptible(&dev->local.queue);
    DDAT_UNLOCK;

    return 0;
}


int dsp_send_command_wait(dsp_command *cmd,
        dsp_message *msg, int card)
{
    unsigned long irqflags;
    struct dsp_dev_t *dev = dsp_dev + card;
    int err = 0;

    PRINT_INFO(card, "entry\n");

    DDAT_LOCK;
    // Loop until we can write in our message
    while (dev->local.flags != 0) {
        DDAT_UNLOCK;
        if (wait_event_interruptible(dev->local.queue,
                    dev->local.flags == 0)) {
            return -ERESTARTSYS;
        }
        DDAT_LOCK;
    }
    if (!(dev->state & DDAT_PRIORITY))
        tasklet_schedule(&dev->priority_tasklet);
    dev->state |= DDAT_DSP_INT;
    dev->local.cmd = cmd;
    dev->local.msg = msg;
    dev->local.flags = LOCAL_CMD;
    DDAT_UNLOCK;

    PRINT_INFO(card, "commanded, waiting\n");
    if (wait_event_interruptible(dev->local.queue,
                dev->local.flags != LOCAL_CMD)) {
        dev->local.flags = 0;
        return -ERESTARTSYS;
    }

    // Get spin lock, clear flags, wake up sleepers again.
    DDAT_LOCK;
    err = (dev->local.flags & LOCAL_ERR) ? -EIO : 0;
    dev->local.flags = 0;
    wake_up_interruptible(&dev->local.queue);
    DDAT_UNLOCK;

    PRINT_INFO(card, "returning %x\n", err);
    return err;
}

int dsp_grant_now_callback( int error, dsp_message *msg, int card)
{
    if (error != 0 || msg==NULL) {
        PRINT_ERR(card, "error or NULL message.\n");
    }
    return 0;
}

int dsp_clear_RP_now(struct dsp_dev_t *dev)
{
    unsigned long irqflags;
    dsp_command cmd = { DSP_INT_RPC, {0, 0, 0} };
    int err;
    DDAT_LOCK;
    if  (dev->state & DDAT_CMD)
        err = -EAGAIN;
    else
        err = dsp_send_command_now(dev, &cmd);
    DDAT_UNLOCK;
    return err;
}


/* This function runs from a tasklet and is responsible for running
 * top-priority DSP actions, such as clearing the RPQ buffer and
 * writing QT inform packets to the DSP.  As long as priority events
 * are outstanding, no other commands can be processed.  */

void dsp_priority_task(unsigned long data)
{
    int card = (int)data;
    struct dsp_dev_t *dev = dsp_dev+card;
    unsigned long irqflags;
    int err = 0;
    int success_mask = ~0;
    if (dev->state & DDAT_RPC) {
        err = dsp_clear_RP_now(dev);
        success_mask = ~DDAT_RPC;
    } else if (dev->state & DDAT_DSP_INT) {
        err = dsp_send_command(dev->local.cmd, &dsp_send_command_wait_callback,
                card, DSP_REQ_PRIORITY);
        success_mask = ~DDAT_DSP_INT;
    } else if (dev->state & DDAT_INFORM) {
        dsp_command cmd = { DSP_QTS, { DSP_QT_TAIL, dev->inform_tail, 0 } };
        err = dsp_send_command(&cmd, dsp_grant_now_callback, card, DSP_REQ_PRIORITY);
        success_mask = ~DDAT_INFORM;
    } else {
        PRINT_ERR(card, "extra schedule.\n");
        return;
    }

    DDAT_LOCK;
    // Only retry on -EAGAIN, which is not considered to be an error.
    if (err == -EAGAIN) {
        err = 0;
    } else {
        dev->state &= success_mask;
        err = 0;
    }
    // Reschedule if there are any priority tasks
    if (dev->state & DDAT_PRIORITY)
        tasklet_schedule(&dev->priority_tasklet);
    DDAT_UNLOCK;

    if (err != 0)
        PRINT_ERR(card, "weird error calling dsp_grant_now (task %i)\n",
                ~success_mask);
}


int dsp_request_grant(int card, int new_tail)
{
    struct dsp_dev_t *dev = dsp_dev + card;
    unsigned long irqflags;

    DDAT_LOCK;
    // Only reanimate the tasklet if it's not active.
    if (!(dev->state & DDAT_PRIORITY))
        tasklet_schedule(&dev->priority_tasklet);
    dev->state |= DDAT_INFORM;
    dev->inform_tail = new_tail;
    DDAT_UNLOCK;

    return 0;
}

/* dsp_clear_RP will set the priority flag so that RP is cleared at
 * the earliest opportunity.
 */

void dsp_request_clear_RP(int card)
{
    unsigned long irqflags;
    struct dsp_dev_t *dev = dsp_dev + card;
    DDAT_LOCK;
    if (!(dev->state & DDAT_PRIORITY))
        tasklet_schedule(&dev->priority_tasklet);
    dev->state |= DDAT_RPC;
    DDAT_UNLOCK;
}

/*
 *  Initialization and clean-up
 */


int dsp_query_version(int card)
{
    struct dsp_dev_t *dev = dsp_dev + card;
    int err = 0;
    int u0107 = 0;
    u32 hctr;
    dsp_command cmd = { DSP_VER, {0,0,0} };
    dsp_message msg;

    /* Before trying the following, try the U0107 handshake */
    hctr = dsp_read_hctr(dev->dsp);
    dsp_write_hctr(dev->dsp, hctr | HCTR_HF2);

    /* Wait 1ms or so, and check for HC4. */
    usleep_range(1000, 10000);
    if ((dsp_read_hstr(dev->dsp) & HSTR_HC4) != 0) {
        /* successful U0107 handshake, assume that's what it is */
        u0107 = 1;
    }

    /* deal with U0107 */
    if (u0107) {
        PRINT_ERR(card, "successful U0107 handshake from DSP\n");
        strcpy(dev->version_string, "U0107");
        dev->version = DSP_U0107;

        /* restore baseline to de-handshake U0107 */
        dsp_write_hctr(dev->dsp, DSP_PCI_MODE_BASE);

        /* Wait 1ms or so and check */
        usleep_range(1000, 10000);
        if (dsp_read_hstr(dev->dsp) & HSTR_HC4) {
            PRINT_ERR(card, "card did not de-hand-shake.\n");
            PRINT_ERR(card, "HSTR: %#10x\n", dsp_read_hstr(dev->dsp));
            PRINT_ERR(card, "HCTR: %#10x\n", dsp_read_hctr(dev->dsp));
            PRINT_ERR(card, "HCVR: %#10x\n", dsp_read_hcvr(dev->dsp));
        }

        return 0;
    }

    strcpy(dev->version_string,  "<=U0103");
    dev->version = 0;
    if ( (err=dsp_send_command_wait(&cmd, &msg, card)) != 0 )
        return err;

    dev->version = DSP_U0103;

    if (msg.reply == DSP_ACK) {

        dev->version_string[0] = msg.data >> 16;
        sprintf(dev->version_string+1, "%02i%02i",
                (msg.data >> 8) & 0xff, msg.data & 0xff);

        dev->version = msg.data;
    }

    PRINT_ERR(card, "discovered PCI card DSP code version %s\n",
            dev->version_string);
    return 0;
}

void dsp_timer_function(
#ifdef NEW_TIMER
        struct timer_list *t
#else
        unsigned long data
#endif
        )
{
#ifdef NEW_TIMER
    struct dsp_dev_t *dev = from_timer(dev, t, tim_poll);
#else
    struct dsp_dev_t *dev = (struct dsp_dev_t *)data;
#endif
    PRINT_INFO(dev->card, "entry\n");
    pci_int_handler(0, dev, NULL);
    mod_timer(&dev->tim_poll, jiffies + DSP_POLL_JIFFIES);
}


int dsp_set_latency(int card, int value)
{
    /* If argument 'value' is positive, that value is loaded into
       the DSP as the size of PCI bursts.  If 'value' is 0 or
       negative, the burst size is obtained from the PCI
       configuration registers (that's the right way). */

    struct dsp_dev_t *dev = dsp_dev + card;
    dsp_command cmd0 = { DSP_QTS, { DSP_QT_BURST, 0, 0 }};
    dsp_command cmd1 = { DSP_WRM, { DSP_C6_MEMX, 0, 0 }};
    dsp_command cmd2 = { DSP_WRM, { DSP_C6_MEMP, 0, 0 }};
    dsp_message rep;
    char c;

    // Get latency value or use value passed in
    if (((int)value) <= 0) {
        // User wants us to use the bus value -- most likely.
        c = 0;
        pci_read_config_byte(dev->pci, PCI_LATENCY_TIMER, &c);
        PRINT_INFO(card, "PCI latency is %#x\n", (int)c);
    } else {
        c = (char)value;
        PRINT_INFO(card, "obtained user latency %#x\n", (int)c);
    }
    if (c <= 0) {
        PRINT_ERR(card, "bad latency value %#x\n", (int)c);
        return -1;
    }

    /* As of U0106, this is easy */
    if (dev->version >= DSP_U0106) {
        cmd0.args[1] = (int)(c-4);
        dsp_send_command_wait(&cmd0, &rep, card);
        return (rep.reply == DSP_ACK) ? 0 : -1;
    }

    /* Hacks for older versions; modify temporary and
       semi-permanent storage of PCI_BURST_SIZE */
    cmd1.args[2] = (int)c;
    cmd2.args[2] = (int)c;
    switch (dev->version) {
        case DSP_U0104:
            cmd1.args[1] = 0x5b;
            cmd2.args[1] = 0x6ad;
            break;
        case DSP_U0105:
            cmd1.args[1] = 0x29;
            cmd2.args[1] = 0x6c7;
            break;
        default:
            PRINT_ERR(card, "can't set latency for DSP version %#06x\n",
                    dev->version);
            return -1;
    }
    dsp_send_command_wait(&cmd1, &rep, card);
    if (rep.reply == DSP_ACK)
        dsp_send_command_wait(&cmd2, &rep, card);
    return (rep.reply == DSP_ACK) ? 0 : -1;
}


/*
   Memory allocation

   This is lame, but DMA-able memory likes a pci device in old kernels.
   */


void* dsp_allocate_dma(ssize_t size, unsigned long* bus_addr_p)
{
#ifdef OLD_KERNEL
    //FIX_ME: mce will call this with out card info currently
    return pci_alloc_consistent(dev->pci, size, bus_addr_p);
#else
    return dma_alloc_coherent(NULL, size, (dma_addr_t*)bus_addr_p,
            GFP_KERNEL);
#endif
}

void  dsp_free_dma(void* buffer, int size, unsigned long bus_addr)
{
#ifdef OLD_KERNEL
    //FIX_ME: mce will call this with out card info currently
    pci_free_consistent (dev->pci, size, buffer, bus_addr);
#else
    dma_free_coherent(NULL, size, buffer, bus_addr);
#endif
}

int dsp_pci_flush()
{
    //FIX ME: no current called, needs card info
    struct dsp_dev_t *dev = dsp_dev;
    dsp_reg_t *dsp = dev->dsp;

    int count = 0, tmp;

    while ((dsp_read_hstr(dsp) & HSTR_HRRQ) && (count++ < PCI_MAX_FLUSH))
    {
        tmp = dsp_read_hrxs(dsp);
        if (count<4)
            PRINT_INFO(dev->card, " %x", tmp);
        else if (count==4)
            PRINT_INFO(dev->card, " ...");
    }

    PRINT_INFO(dev->card, "\n");

    if (dsp_read_hstr(dsp) & HSTR_HRRQ) {
        PRINT_ERR(dev->card, "could not empty HRXS!\n");
        return -1;
    }

    return 0;
}


int dsp_pci_remove_handler(struct dsp_dev_t *dev)
{
    struct pci_dev *pci = dev->pci;

    if (dev->int_handler==NULL) {
        PRINT_INFO(dev->card, "no handler installed\n");
        return 0;
    }

#ifdef REALTIME
    rt_disable_irq(pci->irq);
    rt_free_global_irq(pci->irq);
#else
    free_irq(pci->irq, dev);
#endif
    dev->int_handler = NULL;

    return 0;
}


int dsp_pci_set_handler(int card, irq_handler_t handler,
        char *dev_name)
{
    struct dsp_dev_t *dev = dsp_dev + card;
    struct pci_dev *pci = dev->pci;
    int err = 0;
    int cfg_irq = 0;

    if (pci==NULL || dev==NULL) {
        PRINT_ERR(card, "Null pointers! pci=%lx dev=%lx\n",
                (long unsigned)pci, (long unsigned)dev);
        return -ERESTARTSYS;
    }

    pci_read_config_byte(pci, PCI_INTERRUPT_LINE, (char*)&cfg_irq);
    PRINT_INFO(card, "pci has irq %i and config space has irq %i\n",
            pci->irq, cfg_irq);

    // Free existing handler
    if (dev->int_handler!=NULL)
        dsp_pci_remove_handler(dev);

#ifdef REALTIME
    PRINT_ERR(card, "request REALTIME irq %#x\n", pci->irq);
    rt_disable_irq(pci->irq);
    err = rt_request_global_irq(pci->irq, (void*)handler);
#else
    PRINT_INFO(card, "requesting irq %#x\n", pci->irq);
    err = request_irq(pci->irq, handler, IRQ_FLAGS, dev_name, dev);
#endif

    if (err!=0) {
        PRINT_ERR(card, "irq request failed with error code %#x\n",
                -err);
        return err;
    }

#ifdef REALTIME
    rt_startup_irq(pci->irq);
    rt_enable_irq(pci->irq);
#endif

    dev->int_handler = handler;
    return 0;
}


//Can be called in the future to clear a handler, not in use atm
int dsp_clear_handler(u32 code, int card)
{
    struct dsp_dev_t *dev = dsp_dev + card;
    int i = 0;

    PRINT_INFO(card, "entry\n");

    for (i=0; i<dev->n_handlers; i++) {
        if (dev->handlers[i].code == code)
            break;
    }

    if (i>=dev->n_handlers)
        return -1;

    dev->n_handlers--;

    // Move entries i+1 to i.
    for ( ; i<dev->n_handlers; i++) {
        memcpy(dev->handlers + i, dev->handlers + i + 1,
                sizeof(*dev->handlers));
    }

    // Clear the removed entry
    memset(dev->handlers + dev->n_handlers, 0, sizeof(*dev->handlers));

    PRINT_INFO(card, "ok\n");
    return 0;
}


int dsp_set_msg_handler(u32 code, dsp_handler handler, unsigned long data,
        int card)
{
    struct dsp_dev_t *dev = dsp_dev + card;
    int i;

    PRINT_INFO(card, "entry (%X: %p: %lX; %i)\n", code, handler, data, card);

    // Replace handler if it exists
    for (i=0; i<dev->n_handlers; i++) {
        if (dev->handlers[i].code == code) {
            dev->handlers[i].handler = handler;
            dev->handlers[i].data = data;
            return 0;
        }
    }

    // Add to end of list
    if (i < MAX_HANDLERS) {
        dev->handlers[i].code = code;
        dev->handlers[i].handler = handler;
        dev->handlers[i].data = data;
        dev->n_handlers++;
        PRINT_INFO(card, "ok\n");
        return 0;
    }

    PRINT_ERR(card, "no available handler slots\n");
    return -1;
}


/*
 *  IOCTL, for what it's worth...
 */

int dsp_driver_ioctl(unsigned int iocmd, unsigned long arg, int card)
{
    struct dsp_dev_t *dev = dsp_dev + card;

    switch(iocmd) {

        case DSPDEV_IOCT_SPEAK:
            PRINT_IOCT(card, "state=%#x\n", dev->state);
            break;

        case DSPDEV_IOCT_CORE:
            if (dev->pci == NULL || dev->dsp == NULL) {
                PRINT_IOCT(card, "dev->pci=%p dev->dsp=%p\n",
                        dev->pci, dev->dsp);
                return 0;
            }
            PRINT_IOCT(card, "hstr=%#06x hctr=%#06x\n",
                    dsp_read_hstr(dev->dsp), dsp_read_hctr(dev->dsp));
            break;

        case DSPDEV_IOCT_LATENCY:
            return dsp_set_latency(card, (int)arg);

        case DSPDEV_IOCT_HARD_RESET:
            // Vector int to hard reset the card
            dsp_quick_command(dev, HCVR_SYS_RST);
            return 0;

        default:
            PRINT_ERR(card, "I don't handle iocmd=%ui\n", iocmd);
            return -1;
    }

    return 0;

}


int dsp_proc(struct seq_file *sfile, void *data)
{
    int card = (unsigned long)data;
    struct dsp_dev_t *dev = dsp_dev + card;

    PRINT_INFO(card, "card = %d\n", card);
    if (dev->pci == NULL)
        return 0;
    seq_printf(sfile, "    %-15s %25s\n", "bus address:",
            pci_name(dev->pci));
    seq_printf(sfile, "    %-15s %25s\n",
            "interrupt:",
            (dev->comm_mode & DSP_PCI_MODE_NOIRQ) ? "polling" : "enabled");
    seq_printf(sfile, "    %-32s %#08x\n    %-32s %#08x\n"
            "    %-32s %#08x\n",
            "hstr:", dsp_read_hstr(dev->dsp),
            "hctr:", dsp_read_hctr(dev->dsp),
            "hcvr:", dsp_read_hcvr(dev->dsp));
    seq_printf(sfile, "    %-20s %20s\n",
            "firmware version:", dev->version_string);
    seq_printf(sfile, "    %-30s ", "state:");

    if (dev->state & DDAT_CMD)
        seq_printf(sfile, "commanded");
    else
        seq_printf(sfile, "     idle");

    if (dev->state & DDAT_RESERVE)
        seq_printf(sfile, " (reserved)\n");
    else
        seq_printf(sfile, "\n");
    return 0;
}


int dsp_configure(struct pci_dev *pci)
{
    int err = 0;
    int card;
    struct dsp_dev_t *dev;

    PRINT_INFO(NOCARD, "(%p) entry\n", pci);

    // Find a free slot in dsp_dev array; this defines the card id
    if (pci==NULL) {
        PRINT_ERR(NOCARD, "Called with NULL pci_dev!\n");
        return -EPERM;
    }
    for (card=0; card<MAX_CARDS; card++) {
        dev = dsp_dev + card;
        if(dev->pci == NULL)
            break;
    }
    if (dev->pci != NULL) {
        PRINT_ERR(NOCARD, "too many cards, dsp_dev[] is full.\n");
        return -EPERM;
    }
    PRINT_INFO(card, "%s: card = %i\n", __FUNCTION__, card);

    // Initialize device structure
    memset(dev, 0, sizeof(*dev));
    dev->pci = pci;
    dev->card = card;

    tasklet_init(&dev->handshake_tasklet,
            dsp_ack_int_or_schedule, (unsigned long)dev);
    spin_lock_init(&dev->lock);

    init_waitqueue_head(&dev->local.queue);

#ifdef NEW_TIMER
    timer_setup(&dev->tim_dsp, dsp_timeout, 0);
#else
    init_timer(&dev->tim_dsp);
    dev->tim_dsp.function = dsp_timeout;
    dev->tim_dsp.data = (unsigned long)dev;
#endif
    dev->state = DDAT_IDLE;

    // Data granting task
    tasklet_init(&dev->priority_tasklet, dsp_priority_task, (unsigned long)card);

    // PCI paperwork
    err = pci_enable_device(dev->pci);
    if (err)
        goto fail;

    // This should be done before regions / ioremap
    pci_set_master(dev->pci);

    if (pci_request_regions(dev->pci, DEVICE_NAME)!=0) {
        PRINT_ERR(card, "pci_request_regions failed.\n");
        err = -1;
        goto fail;
    }
    dev->dsp = (dsp_reg_t *)ioremap_nocache(pci_resource_start(dev->pci, 0) &
            PCI_BASE_ADDRESS_MEM_MASK,
            sizeof(*dev->dsp));
    if (dev->dsp==NULL) {
        PRINT_ERR(card, "Could not map PCI registers!\n");
        pci_release_regions(dev->pci);
        err = -EIO;
        goto fail;
    }
    /* Card configuration - now we're done with the kernel and
     * talk to the card */

    // Until we check the card version, make all commands unmaskable
    dev->hcvr_bits = HCVR_HNMI;

    // Clear any outstanding interrupts
    PRINT_ERR(card, "resetting...");
    dsp_quick_command(dev, HCVR_INT_RST);
    dsp_quick_command(dev, HCVR_INT_DON);
    PRINT_ERR(card, "reset complete.");

    // Set the mode of the data path for reads (24->32 conversion)
    dev->comm_mode = DSP_PCI_MODE;
#ifdef NO_INTERRUPTS
    dev->comm_mode |= DSP_PCI_MODE_NOIRQ;
#endif /* NO_INTERRUPTS */
    dsp_write_hctr(dev->dsp, dev->comm_mode);

    // Reset the card
    dsp_quick_command(dev, HCVR_SYS_RST);

    // Install interrupt handler or polling timer
    dev->int_handler = NULL;
    if (dev->comm_mode & DSP_PCI_MODE_NOIRQ) {
        // Create timer for soft poll interrupt generation
#ifdef NEW_TIMER
        timer_setup(&dev->tim_poll, dsp_timer_function, 0);
#else
        init_timer(&dev->tim_poll);
        dev->tim_poll.function = dsp_timer_function;
        dev->tim_poll.data = (unsigned long)dev;
#endif
        mod_timer(&dev->tim_poll, jiffies + DSP_POLL_JIFFIES);
    } else {
        // Install the interrupt handler (cast necessary for backward compat.)
        err = dsp_pci_set_handler(card, (irq_handler_t)pci_int_handler,
                "mce_dsp");
        if (err)
            goto fail;
    }

    // Assign handlers for REP and HEY interrupts.  These are for
    // DSP communications (rather than the MCE protocol).
    dsp_set_msg_handler(DSP_REP, dsp_reply_handler, (unsigned long)dev, card);
    dsp_set_msg_handler(DSP_HEY, dsp_hey_handler, (unsigned long)dev, card);

    PRINT_INFO(card, "ok\n");
    return card;

fail:
    PRINT_ERR(card, "failed!\n");
    return err;
}


int dsp_unconfigure(int card)
{
    struct dsp_dev_t *dev = dsp_dev + card;

    // Remove int handler or poll timer
    if (dev->comm_mode & DSP_PCI_MODE_NOIRQ) {
        del_timer_sync(&dev->tim_poll);
    } else {
        dsp_pci_remove_handler(dev);
    }

    if (dev->dsp!=NULL) {
        // PCI un-paperwork
        iounmap(dev->dsp);
        dev->dsp = NULL;
    }

    if (dev->pci != NULL) {
        pci_disable_device(dev->pci);
        pci_release_regions(dev->pci);
        dev->pci = NULL;
    }

    return card;
}

void dsp_driver_remove(struct pci_dev *pci)
{
    int card;
    struct dsp_dev_t *dev;

    PRINT_INFO(NOCARD, "entry\n");
    if (pci == NULL) {
        PRINT_ERR(NOCARD, "called with null pointer, ignoring.\n");
        return;
    }

    // Match to existing card
    for (card=0; card < MAX_CARDS; card++) {
        dev = dsp_dev + card;
        if (pci == dev->pci)
            break;
    }
    if (card >= MAX_CARDS) {
        PRINT_ERR(card, "could not match configured device, "
                "ignoring.\n");
        return;
    }

    // Disable higher-level features first
    mce_remove(card);
    del_timer_sync(&dev->tim_dsp);

    // Hopefully these aren't still running...
    tasklet_kill(&dev->priority_tasklet);
    tasklet_kill(&dev->handshake_tasklet);

    /* Revert card to default mode.  For U0107, this has already been done */
    if (dev->version < DSP_U0107)
        dsp_write_hctr(dev->dsp, DSP_PCI_MODE);

    // Do DSP cleanup, free PCI resources
    dsp_unconfigure(card);

    PRINT_INFO(NOCARD, "ok\n");
}


int dsp_ready(int card) {
    struct dsp_dev_t *dev = dsp_dev + card;
    return dev->enabled;
}


/*
   dsp_driver_probe

   Called by kernel's PCI manager with each PCI device.  We first find
   a place to keep the card, then do the PCI level initialization

*/

int dsp_driver_probe(struct pci_dev *pci, const struct pci_device_id *id)
{
    int card;
    struct dsp_dev_t *dev = NULL;
    PRINT_INFO(NOCARD, "entry\n");

    // Setup data structure for the card, configure PCI stuff and
    // the DSP.  After this call, the DSP is ready to go.
    if ((card = dsp_configure(pci)) < 0)
        goto fail;
    dev = dsp_dev + card;

    // Get DSP version, which MCE driver likes to know...
    if (dsp_query_version(card) != 0)
        goto fail;

    /* Use the modern driver in this case */
    if (dev->version >= DSP_U0107) {
        PRINT_ERR(card, "DSP Version not supported by driver.");
        goto fail;
    }

    if (dev->version >= DSP_U0105) {
        // Enable interrupt hand-shaking
        dev->comm_mode |= DSP_PCI_MODE_HANDSHAKE;
        dsp_write_hctr(dev->dsp, dev->comm_mode);
        dev->hcvr_bits &= ~HCVR_HNMI;
    } else {
        // All vector commands must be non-maskable on older firmware
        dev->hcvr_bits |= HCVR_HNMI;
    }

    dev->enabled = 1;

    // Set the PCI latency timer
    if (dsp_set_latency(card, 0))
        goto fail;

    // Enable the character device for this card.
    if (dsp_ops_probe(card) != 0)
        goto fail;

    // DSP is ready, setup a structure for MCE driver
    if (mce_probe(card, dsp_dev[card].version))
        goto fail;

    PRINT_INFO(NOCARD, "ok\n");
    return 0;

fail:
    PRINT_ERR(NOCARD, "failed, calling removal routine.\n");
    dsp_driver_remove(pci);
    return -1;
}


void dsp_driver_cleanup(void)
{
    int i = 0;

    PRINT_INFO(NOCARD, "entry\n");

#ifdef FAKEMCE
    dsp_driver_remove();
    dsp_fake_cleanup();
#else
    pci_unregister_driver(&pci_driver);

    for(i=0; i < MAX_CARDS; i++) {
        struct dsp_dev_t *dev = dsp_dev + i;
        if ( dev->pci != NULL ) {
            PRINT_ERR(NOCARD, "failed to zero dev->pci for "
                    "card %i!\n", i);
        }
    }
#endif
    dsp_ops_cleanup();

    mce_cleanup();

    remove_proc_entry("mce_dsp", NULL);

    PRINT_INFO(NOCARD, "driver removed\n");
}


inline int dsp_driver_init(void)
{
    int i = 0;
    int err = 0;

    PRINT_ERR(NOCARD, "MAS driver version %s\n", VERSION_STRING);
    for(i=0; i<MAX_CARDS; i++) {
        struct dsp_dev_t *dev = dsp_dev + i;
        memset(dev, 0, sizeof(*dev));
    }

    proc_create_data("mce_dsp", 0, NULL, &mcedsp_proc_ops, NULL);

    err = dsp_ops_init();
    if(err != 0)
        goto out;

    err = mce_init();
    if(err != 0)
        goto out;

#ifdef FAKEMCE
    dsp_fake_init( DSPDEV_NAME );
#else
    err = pci_register_driver(&pci_driver);

    if (err) {
        PRINT_ERR(NOCARD, "pci_register_driver failed with code %i.\n",
                err);
        err = -1;
        goto out;
    }
#endif //FAKEMCE

    PRINT_INFO(NOCARD, "ok\n");
    return 0;
out:
    PRINT_ERR(NOCARD, "exiting with error\n");
    return err;
}

module_init(dsp_driver_init);
module_exit(dsp_driver_cleanup);
