/*
  dsp_pci.c

  Contains all PCI related code, including register definitions and
  lowest level i/o routines.

  Spoofing can be accomplished at this level by setting up alternate
  handlers for reads and writes to the PCI card.
*/

#include <linux/kernel.h>
#include <linux/pci.h>
#include <linux/interrupt.h>

#include "mce_options.h"
#include "kversion.h"

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

#include "dsp_driver.h"
#include "mce/dsp_ioctl.h"

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

//#ifdef FAKEMCE
//#  include <dsp_fake.h>
//#else
//#  include "dsp_pci.h"
//#endif

#include "mce_driver.h"
#include "dsp_ops.h"
#include "proc.h"

MODULE_LICENSE("GPL");
MODULE_AUTHOR ("Matthew Hasselfield"); 

/*
 *  dsp_pci.c variables
 */


struct dsp_dev_t dsp_dev[MAX_CARDS];

/* Internal prototypes */

void  dsp_clear_interrupt(dsp_reg_t *dsp);

void  dsp_pci_remove(struct pci_dev *pci);

int   dsp_pci_probe(struct pci_dev *pci, const struct pci_device_id *id);


/* Data for PCI enumeration */

static const struct pci_device_id pci_ids[] = {
	{ PCI_DEVICE(DSP_VENDORID, DSP_DEVICEID) },
	{ 0 },
};

static struct pci_driver pci_driver = {
	.name = "mce_dsp",
	.id_table = pci_ids,
	.probe = dsp_pci_probe,
	.remove = dsp_pci_remove,
};

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
	{DSP_WRM, 0x8079, VECTOR_STANDARD},
	{DSP_RDM, 0x807B, VECTOR_STANDARD},
	{DSP_VER, 0x807B, VECTOR_STANDARD},
	{DSP_GOA, 0x807D, VECTOR_STANDARD},
	{DSP_STP, 0x807F, VECTOR_STANDARD},
	{DSP_RST, 0x8081, VECTOR_STANDARD},
	{DSP_CON, 0x8083, VECTOR_STANDARD},
	{DSP_HST, 0x8085, VECTOR_STANDARD},
	{DSP_RCO, 0x8087, VECTOR_STANDARD},
	{DSP_QTS, 0x8089, VECTOR_STANDARD},
	{DSP_INT_RST, HCVR_INT_RST, VECTOR_QUICK},
	{DSP_INT_DON, HCVR_INT_DON, VECTOR_QUICK},
	{DSP_INT_RPC, HCVR_INT_RPC, VECTOR_QUICK},
	{DSP_SYS_ERR, HCVR_SYS_ERR, VECTOR_QUICK},
	{DSP_SYS_RST, HCVR_SYS_RST, VECTOR_QUICK},
	{DSP_SYS_IRQ0, HCVR_SYS_IRQ0, VECTOR_QUICK},
	{DSP_SYS_IRQ1, HCVR_SYS_IRQ1, VECTOR_QUICK},
};

/* DSP register wrappers */

static inline int dsp_read_hrxs(dsp_reg_t *dsp) {
	return ioread32((void*)&(dsp->htxr_hrxs));
}

static inline int dsp_read_hstr(dsp_reg_t *dsp) {
	return ioread32((void*)&(dsp->hstr));
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

/*
 *  dsp_driver.c variables
 */ 

typedef struct {

	u32 code;
	dsp_handler handler;
	unsigned long data;

} dsp_handler_entry;

#define MAX_HANDLERS 16


typedef enum {

	DDAT_IDLE = 0,
	DDAT_CMD,

} dsp_state_t;

struct dsp_local {

	struct semaphore sem;
	wait_queue_head_t queue;
	dsp_message *msg;
	int flags;
#define   LOCAL_CMD 0x01
#define   LOCAL_REP 0x02
#define   LOCAL_ERR 0x08

};

struct dsp_control {

	struct dsp_local local;
	
	struct semaphore sem;
	struct timer_list tim;

	dsp_state_t state;
	int version;

	int n_handlers;
	dsp_handler_entry handlers[MAX_HANDLERS];

	dsp_callback callback;

} dsp_dat[MAX_CARDS];

/*
 *  dsp_driver.c function definitions
 */

#define SUBNAME "dsp_set_handler: "
int dsp_set_handler(u32 code, dsp_handler handler, unsigned long data, int card)
{
	struct dsp_control *ddat = dsp_dat + card;
	int i;

	PRINT_INFO(SUBNAME "entry\n");

	// Replace handler if it exists
	for (i=0; i<ddat->n_handlers; i++) {
		if (ddat->handlers[i].code == code) {
			ddat->handlers[i].handler = handler;
			ddat->handlers[i].data = data;
			return 0;
		}
	}

	// Add to end of list
	if (i < MAX_HANDLERS) {
		ddat->handlers[i].code = code;
		ddat->handlers[i].handler = handler;
		ddat->handlers[i].data = data;
	        ddat->n_handlers++;
		PRINT_INFO(SUBNAME "ok\n");
		return 0;
	}

	PRINT_ERR(SUBNAME "no available handler slots\n");
	return -1;
}
#undef SUBNAME

//Can be called in the future to clear a handler, not in use atm
#define SUBNAME "dsp_clear_handler: "
int dsp_clear_handler(u32 code, int card)
{
  	struct dsp_control *ddat = dsp_dat + card;
	int i = 0;

	PRINT_INFO(SUBNAME "entry\n");

	for (i=0; i<ddat->n_handlers; i++) {
		if (ddat->handlers[i].code == code)
			break;
	}
	
	if (i>=ddat->n_handlers)
		return -1;
	
	ddat->n_handlers--;

	// Move entries i+1 to i.
	for ( ; i<ddat->n_handlers; i++) {
		memcpy(ddat->handlers + i, ddat->handlers + i + 1,
		       sizeof(*ddat->handlers));
	}

	// Clear the removed entry
	memset(ddat->handlers + ddat->n_handlers, 0, sizeof(*ddat->handlers));

	PRINT_INFO(SUBNAME "ok\n");
	return 0;
}
#undef SUBNAME


/*
  dsp_int_handler

  This is not to be confused with pci_int_handler, which is the entry
  point for the DSP's hardware interrupts over the PCI bus.  Rather,
  this is the function that is called once pci_int_handler has
  identified a message as an REP.

  Although we'd like to respond to NFY immediately, we may be in the
  middle of some other command.  Not!
*/


#define SUBNAME "dsp_int_handler: "
int dsp_int_handler(dsp_message *msg, int card)
{
  	struct dsp_control *ddat = dsp_dat + card;
	int i;

	PRINT_INFO(SUBNAME "entry\n");

	if (msg==NULL) {
		PRINT_ERR(SUBNAME "called with NULL message pointer!\n");
		return -1;
	}

	// Discover handler for this message type
	
	for (i=0; i < ddat->n_handlers; i++) {
		if ( (ddat->handlers[i].code == msg->type) &&
		     (ddat->handlers[i].handler != NULL) ) {
			ddat->handlers[i].handler(msg, ddat->handlers[i].data);
			PRINT_INFO(SUBNAME "ok\n");
			return 0;
		}
	}

	PRINT_ERR(SUBNAME "unknown message type: %#06x\n", msg->type);
	return -1;
}
#undef SUBNAME

/* 
 * This will handle REP interrupts from the DSP, which correspond to
 * replies to DSP commands.
 */
#define SUBNAME "dsp_reply_handler: "
int dsp_reply_handler(dsp_message *msg, unsigned long data)
{
  	struct dsp_control *ddat = (struct dsp_control *)data;

	if (ddat->state == DDAT_CMD) {
		PRINT_INFO(SUBNAME
			   "REP received, calling back.\n");

		// Call the registered callbacks
		if (ddat->callback != NULL) {
			int card = ddat - dsp_dat;
			ddat->callback(0, msg, card);
		} else {
			PRINT_ERR(SUBNAME "no handler defined\n");
		}
		ddat->state = DDAT_IDLE;
	} else {
		PRINT_ERR(SUBNAME
			  "unexpected REP received [state=%i].\n",
			  ddat->state);
	}
	return 0;
}
#undef SUBNAME

/* This will handle HEY interrupts from the DSP, which are generic
 * communications from the DSP typically used for debugging.
 */
#define SUBNAME "dsp_hey_handler: "
int dsp_hey_handler(dsp_message *msg, unsigned long data)
{
	PRINT_ERR(SUBNAME "dsp HEY received: %06x %06x %06x\n",
		  msg->command, msg->reply, msg->data);
	return 0;
}
#undef SUBNAME


#define SUBNAME "dsp_timeout: "
void dsp_timeout(unsigned long data)
{
	struct dsp_control *ddat = (struct dsp_control*)data;

	if (ddat->state == DDAT_IDLE) {
		PRINT_INFO(SUBNAME "timer ignored\n");
		return;
	}

	PRINT_ERR(SUBNAME "dsp reply timed out!\n");
	if (ddat->callback != NULL) {
		int card = ddat - dsp_dat;
		ddat->callback(-DSP_ERR_TIMEOUT, NULL, card);
	}
	ddat->state = DDAT_IDLE;
}
#undef SUBNAME


/*
  DSP command sending framework

  - The most straight-forward approach to sending DSP commands is to
    call dsp_command with pointers to the full dsp_command structure
    and an empty dsp_message structure.

  - A non-blocking version of the above that allows the caller to
    specify a callback routine that will be run upon receipt of the
    DSP ack/err interrupt message is exposed as dsp_command_nonblock.
    The calling module must notify the dsp module that the message has
    been processed by calling dsp_clear_commflags; this cannot be done
    from within the callback routine!

  - The raw, no-semaphore-obtaining, no-flag-checking-or-setting,
    non-invalid-command-rejecting command issuer is dsp_command_now.
    Don't use it.  Only I'm allowed to use it.

*/


/* For better or worse, this routine returns linux error codes.
   
   The callback error codes are 0 (success, msg will be non-null) or
   DSP_ERR_TIMEOUT (failure, msg will be null).                    */
#define SUBNAME "dsp_send_command: "
int dsp_send_command(dsp_command *cmd, dsp_callback callback, int card)
{
  	struct dsp_control *ddat = dsp_dat + card;
	int err = 0;

	// This will often be called in atomic context
	if (down_trylock(&ddat->sem)) {
		PRINT_ERR(SUBNAME "could not get sem\n");
		return -EAGAIN;
	}
	
	PRINT_INFO(SUBNAME "entry\n");
		
	ddat->callback = callback;
	ddat->state = DDAT_CMD;

	if ( (err = dsp_send_command_now(cmd, card)) ) {
		ddat->callback = NULL;
		ddat->state = DDAT_IDLE;
	} else {
		mod_timer(&ddat->tim, jiffies + DSP_DEFAULT_TIMEOUT);
	}

	PRINT_INFO(SUBNAME "returning [%i]\n", err);
	up(&ddat->sem);
	return err;
}
#undef SUBNAME

#define SUBNAME "dsp_send_command_wait_callback"
int dsp_send_command_wait_callback(int error, dsp_message *msg, int card)
{
	struct dsp_control *ddat = dsp_dat + card;

	wake_up_interruptible(&ddat->local.queue);

	if (ddat->local.flags != LOCAL_CMD) {
		PRINT_ERR(SUBNAME "unexpected flags, cmd=%x rep=%x err=%x\n",
			  ddat->local.flags & LOCAL_CMD,
			  ddat->local.flags & LOCAL_REP,
			  ddat->local.flags & LOCAL_ERR);
		return -1;
	}
	memcpy(ddat->local.msg, msg, sizeof(*ddat->local.msg));
	ddat->local.flags |= LOCAL_REP;

	return 0;
}
#undef SUBNAME


#define SUBNAME "dsp_send_command_wait: "
int dsp_send_command_wait(dsp_command *cmd,
			  dsp_message *msg, int card)
{
	struct dsp_control *ddat = dsp_dat + card;
	int err = 0;

	PRINT_INFO(SUBNAME "entry\n");

	// Try to get the default sem (spinlock!)
	if (down_trylock(&ddat->local.sem))
		return -ERESTARTSYS;

	//Register message for our callback to fill
	ddat->local.msg = msg;
	ddat->local.flags = LOCAL_CMD;
	
	if ((err=dsp_send_command(cmd, dsp_send_command_wait_callback, card)) != 0)
		goto up_and_out;

	PRINT_INFO(SUBNAME "commanded, waiting\n");
	if (wait_event_interruptible(ddat->local.queue,
				     ddat->local.flags
				     & (LOCAL_REP | LOCAL_ERR))) {
		ddat->local.flags = 0;
		err = -ERESTARTSYS;
		goto up_and_out;
	}
	
	err = (ddat->local.flags & LOCAL_ERR) ? -EIO : 0;
	
 up_and_out:

	PRINT_INFO(SUBNAME "returning %x\n", err);
	up(&ddat->local.sem);
	return err;
}
#undef SUBNAME

/*
 *  Initialization and clean-up
 */


#define SUBNAME "dsp_query_version: "
int dsp_query_version(int card)
{
  	struct dsp_control *ddat = dsp_dat + card;
	int err = 0;
	dsp_command cmd = { DSP_VER, {0,0,0} };
	dsp_message msg;
	char version[8] = "<=U0103";
	
	ddat->version = 0;
	if ( (err=dsp_send_command_wait(&cmd, &msg, card)) != 0 )
		return err;

	ddat->version = DSP_U0103;

	if (msg.reply == DSP_ACK) {
	
		version[0] = msg.data >> 16;
		sprintf(version+1, "%02i%02i",
			(msg.data >> 8) & 0xff, msg.data & 0xff);

		ddat->version = msg.data;
	}
		
	PRINT_ERR(SUBNAME " discovered PCI card DSP code version %s\n", version);
	return 0;
}
#undef SUBNAME

void dsp_clear_RP(int card)
{
	dsp_command cmd = { DSP_INT_RPC, {0, 0, 0} };
	
	// This is interrupt safe because it is a vector command
	dsp_send_command_now(&cmd, card);
}

/*
 *  dsp_pci.c function definitions
 */

#define SUBNAME "pci_int_handler: "
irqreturn_t pci_int_handler(int irq, void *dev_id, struct pt_regs *regs)
{
	/* Note that the regs argument is deprecated in newer kernels,
	   do not use it.  It is left here for compatibility with
	   2.6.18-                                                    */

	struct dsp_dev_t *dev = NULL;
	dsp_reg_t *dsp = NULL;
	dsp_message msg;
	int i = 0;
	int k = 0; 
	int n = sizeof(msg) / sizeof(u32);

	for(k=0; k<MAX_CARDS; k++) {
		dev = dsp_dev + k;
		if (dev_id == dev) {
			dsp = dev->dsp;
			break;
		}
	}

	if (dev_id != dev) return IRQ_NONE;

	//Verify handshake bit
	if ( !(dsp_read_hstr(dsp) & HSTR_HC3) ) {
		//FIX ME:: Continuous stream of general interrupts
		PRINT_ERR(SUBNAME "irq entry without HF3 bit!\n");
		return IRQ_NONE;
	}

	// Immediately clear interrupt bit
	dsp_write_hcvr(dsp, HCVR_INT_RST);

	// Read data into dsp_message structure
	while ( i<n && (dsp_read_hstr(dsp) & HSTR_HRRQ) ) {
		((u32*)&msg)[i++] = dsp_read_hrxs(dsp) & DSP_DATAMASK;
	}

	//Completed reads?
	if (i<n)
		PRINT_ERR(SUBNAME "could not obtain entire message.\n");
	
	PRINT_INFO(SUBNAME "%6x %6x %6x %6x\n",
		  msg.type, msg.command, msg.reply, msg.data);

	// Call the generic message handler
	dsp_int_handler(&msg, k);

	// At end, clear DSP handshake bit
	dsp_write_hcvr(dsp, HCVR_INT_DON);

/* 	// Clear DSP interrupt flags */
/* 	dsp_clear_interrupt(dsp); */

	PRINT_INFO(SUBNAME "ok\n");
	return IRQ_HANDLED;
}
#undef SUBNAME

#define SUBNAME "dsp_timer_function: "
void dsp_timer_function(unsigned long data)
{
	struct dsp_dev_t *dev = (struct dsp_dev_t *)data;
	PRINT_INFO(SUBNAME "entry\n");
	pci_int_handler(0, dev, NULL);
	mod_timer(&dev->tim, jiffies + DSP_POLL_JIFFIES);
}
#undef SUBNAME

void dsp_clear_interrupt(dsp_reg_t *dsp)
{
	// Clear interrupt flags
	dsp_write_hcvr(dsp, HCVR_INT_RST);
	dsp_write_hcvr(dsp, HCVR_INT_DON);
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


#define SUBNAME "dsp_send_command_now_vector: "
int dsp_send_command_now_vector(dsp_command *cmd, u32 vector, int card) 
{
	struct dsp_dev_t *dev = dsp_dev + card;
	int i = 0;
	int n = sizeof(dsp_command) / sizeof(u32);

	// HSTR must be ready to receive
	if ( !(dsp_read_hstr(dev->dsp) & HSTR_TRDY) ) {
		PRINT_ERR(SUBNAME "HSTR not ready to transmit!\n");
		return -EIO;
	}

	//Write bytes and interrupt
	while ( i<n && (dsp_read_hstr(dev->dsp) & HSTR_HTRQ)!=0 )
		dsp_write_htxr(dev->dsp, ((u32*)cmd)[i++]);

	if (i<n) {
		PRINT_ERR(SUBNAME "HTXR filled up during write! HSTR=%#x\n",
			  dsp_read_hstr(dev->dsp));
		return -EIO;
	}
	
	dsp_write_hcvr(dev->dsp, vector);

	return 0;
}
#undef SUBNAME


#define SUBNAME "dsp_quick_command: "
int dsp_quick_command(u32 vector, int card) 
{
	struct dsp_dev_t *dev = dsp_dev + card;
	PRINT_INFO(SUBNAME "sending vector %#x\n", vector);
	dsp_write_hcvr(dev->dsp, vector);
	return 0;
}
#undef SUBNAME


#define SUBNAME "dsp_lookup_vector: "
struct dsp_vector *dsp_lookup_vector(dsp_command *cmd)
{
	int i;
	for (i = 0; i < NUM_DSP_CMD; i++)
		if (dsp_vector_set[i].key == cmd->command)
			return dsp_vector_set+i;
	
	PRINT_ERR(SUBNAME "could not identify command %#x\n",
		  cmd->command);
	return NULL;
}
#undef SUBNAME


#define SUBNAME "dsp_send_command_now: "
int dsp_send_command_now(dsp_command *cmd, int card) 
{
	struct dsp_vector *vect = dsp_lookup_vector(cmd);

	PRINT_INFO(SUBNAME "cmd=%06x\n", cmd->command);

	if (vect==NULL) return -ERESTARTSYS;
	
	switch (vect->type) {

	case VECTOR_STANDARD:
		return dsp_send_command_now_vector(cmd, vect->vector, card);

	case VECTOR_QUICK:
		// FIXME: these don't reply so they'll always time out.
		return dsp_quick_command(vect->vector, card);

	}

	PRINT_ERR(SUBNAME
		  "unimplemented vector command type %06x for command %06x\n",
		  vect->type, cmd->command);
	return -ERESTARTSYS;
}
#undef SUBNAME


	

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

#define SUBNAME "dsp_pci_flush: "
int dsp_pci_flush()
{
	//FIX ME: no current called, needs card info
	struct dsp_dev_t *dev = dsp_dev;
	dsp_reg_t *dsp = dev->dsp;

	int count = 0, tmp;

	PRINT_INFO("dsp_pci_flush:");
	while ((dsp_read_hstr(dsp) & HSTR_HRRQ) && (count++ < PCI_MAX_FLUSH))
	{
		tmp = dsp_read_hrxs(dsp);
		if (count<4) PRINT_INFO(" %x", tmp);
		else if (count==4) PRINT_INFO(" ...");
	}

	PRINT_INFO("\n");

	if (dsp_read_hstr(dsp) & HSTR_HRRQ) {
		PRINT_ERR("dsp_pci_flush: could not empty HRXS!\n");
		return -1;
	}

	return 0;
}
#undef SUBNAME


#define SUBNAME "dsp_pci_remove_handler: "
int dsp_pci_remove_handler(struct dsp_dev_t *dev)
{
	struct pci_dev *pci = dev->pci;

	if (dev->int_handler==NULL) {
		PRINT_INFO(SUBNAME "no handler installed\n");
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
#undef SUBNAME


#define SUBNAME "dsp_pci_set_handler: "
int dsp_pci_set_handler(int card, irq_handler_t handler,
			char *dev_name)
{
	struct dsp_dev_t *dev = dsp_dev + card;
	struct pci_dev *pci = dev->pci;	
	int err = 0;
	int cfg_irq = 0;

	if (pci==NULL || dev==NULL) {
		PRINT_ERR(SUBNAME "Null pointers! pci=%lx dev=%lx\n",
			  (long unsigned)pci, (long unsigned)dev);
		return -ERESTARTSYS;
	}
	
	pci_read_config_byte(pci, PCI_INTERRUPT_LINE, (char*)&cfg_irq);
	PRINT_INFO(SUBNAME "pci has irq %i and config space has irq %i\n",
		   pci->irq, cfg_irq);

	// Free existing handler
	if (dev->int_handler!=NULL)
		dsp_pci_remove_handler(dev);
	
#ifdef REALTIME
	PRINT_ERR(SUBNAME "request REALTIME irq %#x\n", pci->irq);
	rt_disable_irq(pci->irq);
	err = rt_request_global_irq(pci->irq, (void*)handler);
#else
	PRINT_INFO(SUBNAME "requesting irq %#x\n", pci->irq);
	err = request_irq(pci->irq, handler, IRQ_FLAGS, dev_name, dev);
#endif

	if (err!=0) {
		PRINT_ERR(SUBNAME "irq request failed with error code %#x\n",
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
#undef SUBNAME


#define SUBNAME "dsp_pci_configure: "
int dsp_pci_configure(int card)
{
	struct dsp_dev_t *dev = dsp_dev + card;
	int err = 0;
	PRINT_INFO(SUBNAME "entry\n");

	err = pci_enable_device(dev->pci);
	if (err) goto failed;

        // Request regions and map i/o registers.
	if (pci_request_regions(dev->pci, DEVICE_NAME)!=0) {
		PRINT_ERR(SUBNAME "pci_request_regions failed.\n");
		err = -1;
		goto failed;
	}

	dev->dsp = (dsp_reg_t *)ioremap_nocache(pci_resource_start(dev->pci, 0) & 
						PCI_BASE_ADDRESS_MEM_MASK,
						sizeof(*dev->dsp));
	if (dev->dsp==NULL) {
		PRINT_ERR(SUBNAME "Could not map PCI registers!\n");
		pci_release_regions(dev->pci);			
		err = -EIO;
		goto failed;
	}

	// Mark PCI card as bus master
	pci_set_master(dev->pci);

	dsp_clear_interrupt(dev->dsp);
	dsp_write_hctr(dev->dsp, DSP_PCI_MODE);
	
	// Enable / disable PCI interrupts.
	// These two vector addresses are NOP in PCI firmware before U0105.
	switch (dev->int_mode) {
	case DSP_POLL:
		dsp_write_hcvr(dev->dsp, HCVR_SYS_IRQ0);
		break;
	case DSP_PCI:
		dsp_write_hcvr(dev->dsp, HCVR_SYS_IRQ1);
		break;
	}

	PRINT_INFO(SUBNAME "ok\n");
	return 0;

 failed:
	PRINT_ERR(SUBNAME "failed!\n");
	return err;
}
#undef SUBNAME


#define SUBNAME "dsp_driver_configure: "
int dsp_driver_configure(int card)
{	
  	struct dsp_control *ddat = dsp_dat + card;
	int err = 0;

	PRINT_INFO(SUBNAME "entry\n");
		
	init_MUTEX(&ddat->sem);
	init_MUTEX(&ddat->local.sem);
	init_waitqueue_head(&ddat->local.queue);
	init_timer(&ddat->tim);

	ddat->tim.function = dsp_timeout;
	ddat->tim.data = (unsigned long)ddat;
	ddat->state = DDAT_IDLE;

	// Set up handlers for the DSP interrupts - additional
	//  handlers will be set up by sub-modules.
	dsp_set_handler(DSP_REP, dsp_reply_handler, (unsigned long)ddat, card);
	dsp_set_handler(DSP_HEY, dsp_hey_handler, (unsigned long)ddat, card);

	// Version can only be obtained after REP handler has been set
	if (dsp_query_version(card)) {
		err = -1;
		goto out;
	}

	if(dsp_ops_probe(card) != 0) {
		err = -1;
		goto out;
	}
	
	if (mce_probe(ddat->version, card)) {
		err = -1;
		goto out;
	}

	PRINT_INFO(SUBNAME "driver ok\n");
	return 0;

 out:
	PRINT_ERR(SUBNAME "exiting with errors!\n");
	return err;
}
#undef SUBNAME


#define SUBNAME "dsp_pci_remove: "
void dsp_pci_remove(struct pci_dev *pci)
{
	int i = 0;

	PRINT_INFO(SUBNAME "entry\n");

	if (pci == NULL) {
		PRINT_ERR(SUBNAME "called with null pointer!\n");
		return;
	}
	
	for (i=0; i < MAX_CARDS; i++) {
		struct dsp_dev_t *dev = dsp_dev + i;
		struct dsp_control *ddat = dsp_dat + i;		

		if (pci != dev->pci)
			continue;
			
		// Disable higher-level features first
		mce_remove(i);	
		del_timer_sync(&ddat->tim);
			
		// Remove int handler or poll timer
		switch (dev->int_mode) {
		case DSP_PCI:
			dsp_pci_remove_handler(dev);
			break;
		case DSP_POLL:
			del_timer_sync(&dev->tim);
			break;
		}
		
		if (dev->dsp!=NULL) {
			dsp_clear_interrupt(dev->dsp);
			iounmap(dev->dsp);
			pci_release_regions(pci);			
			dev->dsp = NULL;
		}

		dev->pci = NULL;
		PRINT_INFO(SUBNAME "ok\n");
		return;
	}

	PRINT_ERR(SUBNAME "called with unknown device!\n");
	return;
}
#undef SUBNAME


#define SUBNAME "dsp_probe: "
int dsp_pci_probe(struct pci_dev *pci, const struct pci_device_id *id)
{
	struct dsp_dev_t *dev = dsp_dev;
	int err = 0;
	int i = 0;

	PRINT_INFO(SUBNAME "entry\n");

	for(i=0; i<MAX_CARDS; i++) {
		dev = dsp_dev + i;
		if(dev->pci == NULL) break;
	} 	

	if (pci==NULL) {
		PRINT_ERR(SUBNAME "Called with NULL pci_dev!\n");
		err = -EPERM;
		goto fail;
	}

	if (dev->pci != NULL) {
		PRINT_ERR(SUBNAME "called after device configured or dsp_dev[] is full.\n");
		err = -EPERM;
		goto fail;
	}

	// Intialize dev and set dev->pci
	memset(dev, 0, sizeof(dev));
	dev->pci = pci;

#ifdef NO_INTERRUPTS
	dev->int_mode = DSP_POLL;
#else
	dev->int_mode = DSP_PCI;
#endif
	
	// Configure the card for our purposes
	if (dsp_pci_configure(i)) {
		goto fail;
	}

	dev->int_handler = NULL;

	switch (dev->int_mode) {
	case DSP_PCI:
		// Install the interrupt handler (cast necessary for backward compat.)
		err = dsp_pci_set_handler(i, (irq_handler_t)pci_int_handler,
					  "mce_dsp");		
		if (err) goto fail;
		break;
	case DSP_POLL:
		// Create timer for soft poll interrupt generation
		init_timer(&dev->tim);
		dev->tim.function = dsp_timer_function;
		dev->tim.data = (unsigned long)dev;
		mod_timer(&dev->tim, jiffies + DSP_POLL_JIFFIES);
		break;
	}

	// Call init function for higher levels.
	err = dsp_driver_configure(i);
	if (err) goto fail;

	PRINT_INFO(SUBNAME "ok\n");
	return 0;

fail:
	dsp_pci_remove(pci);

	PRINT_ERR(SUBNAME "failed with code %i\n", err);
	return -1;
}
#undef SUBNAME


#define SUBNAME "cleanup_module: "
void dsp_driver_cleanup(void)
{
	int i = 0;

	PRINT_INFO(SUBNAME "entry\n");

#ifdef FAKEMCE
	dsp_driver_remove();
	dsp_fake_cleanup();
#else
	pci_unregister_driver(&pci_driver);

	for(i=0; i < MAX_CARDS; i++) {
		struct dsp_dev_t *dev = dsp_dev + i;
		if ( dev->pci != NULL ) {
			PRINT_ERR(SUBNAME "failed to zero dev->pci for card; %i!\n", i);
		}
	}
#endif
	dsp_ops_cleanup();

	mce_cleanup();

	remove_proc_entry("mce_dsp", NULL);

	PRINT_INFO(SUBNAME "driver removed\n");
}    
#undef SUBNAME


#define SUBNAME "init_module: "
inline int dsp_driver_init(void)
{
	int i = 0;
	int err = 0;

	PRINT_INFO(SUBNAME "driver init...\n");

	for(i=0; i<MAX_CARDS; i++) {
		struct dsp_control *ddat = dsp_dat + i;
		memset(ddat, 0, sizeof(*ddat));
	}
  
	create_proc_read_entry("mce_dsp", 0, NULL, read_proc, NULL);

#ifdef FAKEMCE
	dsp_fake_init( DSPDEV_NAME );
#else
	err = pci_register_driver(&pci_driver);

	if (err) {
		PRINT_ERR(SUBNAME "pci_register_driver failed with code %i.\n", err);
		err = -1;
		goto out;
	}			  
#endif //FAKEMCE

	err = dsp_ops_init();
	if(err != 0) goto out;

	err = mce_init();
	if(err != 0) goto out;

	PRINT_INFO(SUBNAME "ok\n");
	return 0;
 out:
	PRINT_ERR(SUBNAME "exiting with error\n");
	return err;
}

module_init(dsp_driver_init);
module_exit(dsp_driver_cleanup);

#undef SUBNAME

/*
 *  IOCTL, for what it's worth...
 */

#define SUBNAME "dsp_driver_ioctl: "
int dsp_driver_ioctl(unsigned int iocmd, unsigned long arg, int card)
{
  	struct dsp_control *ddat = dsp_dat + card;
	struct dsp_dev_t *dev = dsp_dev + card;

	switch(iocmd) {

	case DSPDEV_IOCT_SPEAK:
		PRINT_IOCT(SUBNAME "state=%#x\n", ddat->state);
		break;

	case DSPDEV_IOCT_CORE:
		
		if (dev->pci == NULL) {
			PRINT_IOCT(SUBNAME "pci structure is null\n");
			return 0;
		}
		if (dev->dsp == NULL) {
			PRINT_IOCT(SUBNAME
				   "pci-dsp memory structure is null\n");
			return 0;
		}
		PRINT_IOCT(SUBNAME "hstr=%#06x hctr=%#06x\n",
			   dsp_read_hstr(dev->dsp), dsp_read_hctr(dev->dsp));		
		break;

	case DSPDEV_IOCT_CORE_IRQ:
		if (arg) {
			PRINT_IOCT(SUBNAME "Enabling interrupt\n");
			/* Cast on handler is necessary for backward compat. */
			if (dsp_pci_set_handler(card, (irq_handler_t)pci_int_handler,
						"mce_hacker") < 0) {
				PRINT_ERR(SUBNAME "Could not install interrupt handler!\n");
				return -1;
			}
		} else {
			PRINT_IOCT(SUBNAME "Disabling interrupt\n");
			dsp_pci_remove_handler(dev);
		}
		break;

	default:
		PRINT_ERR(SUBNAME "I don't handle iocmd=%ui\n", iocmd);
		return -1;
	}

	return 0;

}
#undef SUBNAME

int dsp_proc(char *buf, int count, int card)
{
	struct dsp_dev_t *dev = dsp_dev + card;
  	struct dsp_control *ddat = dsp_dat + card;
	int len = 0;


	PRINT_INFO("dsp_proc: card = %d\n", card);

	if (len < count) {
		len += sprintf(buf+len, "    state:    ");
		switch (ddat->state) {
		case DDAT_IDLE:
			len += sprintf(buf+len, "idle\n");
			break;
		case DDAT_CMD:
			len += sprintf(buf+len, "commanded\n");
			break;
		default:
			len += sprintf(buf+len, "unknown! [%i]\n", ddat->state);
			break;
		}
	}

	if (len < count && dev->dsp != NULL) {
		len += sprintf(buf+len, "    hstr:     %#06x\n"
			                "    hctr:     %#06x\n",
			       dsp_read_hstr(dev->dsp),
			       dsp_read_hctr(dev->dsp));
	} else if (len < count) {
		len += sprintf(buf+len, "    Card not present/intialized\n");
	}
	
/* 	len += sprintf(buf+len, "    virtual: %#010x\n", */
/* 		       (unsigned)frames.base); */
	return len;
}
