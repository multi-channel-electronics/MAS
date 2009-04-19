#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/fs.h>
#include <linux/sched.h>
#include <asm/uaccess.h>

#include "kversion.h"
#include "mce_options.h"

#include "mce_ops.h"
#include "mce_driver.h"
#include "mce/mce_ioctl.h"
#include "mce/mce_errors.h"

struct file_operations mce_fops = 
{
 	.owner=   THIS_MODULE,
 	.open=    mce_open, 
	.read=    mce_read,
 	.release= mce_release,
	.write=   mce_write,
 	.ioctl=   mce_ioctl,
};

typedef enum {
	OPS_IDLE = 0,
	OPS_CMD,
	OPS_REP,
	OPS_ERR
} mce_ops_state_t;

struct filp_pdata {
	int minor;
};

struct mce_ops_t {

	int major;

	struct semaphore sem;
	wait_queue_head_t queue;

	mce_ops_state_t state;
	int error;

	mce_reply   rep;
	mce_command cmd;

	int properties; /* accessed through MCEDEV_IOCT_GET / SET */

} mce_ops[MAX_CARDS];


/* The operations states are defined as follows:

OPS_IDLE indicates the device is available for a new write operation.
  In this state, writing data to the device will initiate a command,
  and reading from the device will return 0.  Successors to this state
  are CMD and ERR.

OPS_CMD indicates that a command has been initiated.  If a write to
  the device succeeds (i.e. does not return 0 or an error code) then
  the driver will be but into this state.

OPS_REP indicates that a reply to a command has been received
  successfully and is waiting to be read.

OPS_ERR indicates that the command / reply sequence failed at some
  step.  The error state is cleared by reading from the device, which
  will return 0 bytes.

The behaviour of the device files in each of the states is as follows:

Method State          Blocking                 Non-blocking
------------------------------------------------------------------
read   IDLE           return 0                 return 0
read   CMD            wait for state!=CMD      return -EAGAIN
read   REP            [copy reply to the user, returning number
                        of bytes in the reply]
read   ERR            return 0                 return 0

write  IDLE           [copy command from user and initiate it;
                        return size of command or 0 on failure]
write  CMD            return 0                 return 0
write  REP            return 0                 return 0
write  ERR            return 0                 return 0

In non-blocking mode, both methods immediately return -EAGAIN if they
cannot obtain the operations semaphore.


The state update rules are as follows:

IDLE -> CMD   a command was initiated successfully.
CMD  -> REP   the reply to the command was received
CMD  -> ERR   the driver indicated an error in receiving the command
REP  -> IDLE  the reply has been copied to the user
ERR  -> IDLE  the error has been communicated to the user


Note that the state ERR does not represent a problem with driver use;
it represents a failure of the hardware for some reason.  ERR state is
different from the errors when device operations return 0.  The cause
of device operations returning 0 can be obtained by calling ioctl
operation MCEDEV_IOCT_LAST_ERROR.  This error is cleared on the next
read/write operation.

So anyway, we're going to get rid of the ERR state and instead just
return an error packet as the reply.  That way mce_ops doesn't need to
know if the MCE works or not.

*/

#define SUBNAME "mce_read: "
ssize_t mce_read(struct file *filp, char __user *buf, size_t count,
                 loff_t *f_pos)
{
	struct filp_pdata *fpdata = filp->private_data;
	struct mce_ops_t *mops = mce_ops + fpdata->minor;
	int err = 0;
	int ret_val = 0;

	PRINT_INFO(SUBNAME "state=%#x\n", mops->state);

	// Lock semaphore

	if (filp->f_flags & O_NONBLOCK) {
		PRINT_INFO(SUBNAME "non-blocking call\n");
		if (down_trylock(&mops->sem))
			return -EAGAIN;
	} else {
		PRINT_INFO(SUBNAME "blocking call\n");
		if (down_interruptible(&mops->sem))
			return -ERESTARTSYS;
	}


	// If command in progress, block if we're allowed.

	while ( !(filp->f_flags & O_NONBLOCK) && (mops->state == OPS_CMD) ) {
		if (wait_event_interruptible(mops->queue,
					     mops->state != OPS_CMD)) {
			ret_val = -ERESTARTSYS;
			goto out;
		}
	}

	switch (mops->state) {

	case OPS_IDLE:
		ret_val = 0;
		mops->error = -MCE_ERR_ACTIVE;
		goto out;
		
	case OPS_CMD:
		// Not possible in blocking calls
		ret_val = -EAGAIN;
		mops->error = 0;
		goto out;
			
	case OPS_REP:
		// Fix me: partial packet reads not supported
		ret_val = sizeof(mops->rep);
		if (ret_val > count) ret_val = count;
	
		err = copy_to_user(buf, (void*)&mops->rep, ret_val);
		ret_val -= err;
		if (err) {
			PRINT_ERR(SUBNAME
				  "could not copy %#x bytes to user\n", err );
			mops->error = -MCE_ERR_KERNEL;
		}
		mops->state = OPS_IDLE;
		mops->error = 0;
		break;
		
	case OPS_ERR:
	default:
		ret_val = 0;
		// mops->error is set when state <= OPS_ERR
		mops->state = OPS_IDLE;
	}
		
out:
	PRINT_INFO(SUBNAME "exiting (%i)\n", ret_val);
	
	up(&mops->sem);
	return ret_val;
}
#undef SUBNAME


#define SUBNAME "mce_write_callback: "
int mce_write_callback( int error, mce_reply* rep, int card)
{
	struct mce_ops_t *mops = mce_ops + card;

	// Reject unexpected interrupts
	if (mops->state != OPS_CMD) {
		PRINT_ERR(SUBNAME "state is %#x, expected %#x\n",
			  mops->state, OPS_CMD);
		return -1;
	}			  

	// Change state to REP or ERR, awaken readers, exit with success.
	if (error || rep==NULL) {
		PRINT_ERR(SUBNAME "called with error=-%#x, rep=%lx\n",
			  -error, (unsigned long)rep);
		memset(&mops->rep, 0, sizeof(mops->rep));
		mops->state = OPS_ERR;
		mops->error = error ? error : -MCE_ERR_INT_UNKNOWN;
	} else {
		PRINT_INFO(SUBNAME "type=%#x\n", rep->ok_er);
		memcpy(&mops->rep, rep, sizeof(mops->rep));
		mops->state = OPS_REP;
	}

	wake_up_interruptible(&mops->queue);
	return 0;
}
#undef SUBNAME

#define SUBNAME "mce_write: "
ssize_t mce_write(struct file *filp, const char __user *buf, size_t count,
		  loff_t *f_pos)
{
	struct filp_pdata *fpdata = filp->private_data;
	struct mce_ops_t *mops = mce_ops + fpdata->minor;
	int err = 0;
	int ret_val = 0;

	PRINT_INFO(SUBNAME "state=%#x\n", mops->state);

	// Non-blocking version may not wait on semaphore.

	if (filp->f_flags & O_NONBLOCK) {
		PRINT_INFO(SUBNAME "non-blocking call\n");
		if (down_trylock(&mops->sem))
			return -EAGAIN;
	} else {
		PRINT_INFO(SUBNAME "blocking call\n");
		if (down_interruptible(&mops->sem))
			return -ERESTARTSYS;
	}

	// Reset error flag
	mops->error = 0;

	switch (mops->state) {

	case OPS_IDLE:
		break;

	case OPS_CMD:
	case OPS_REP:
	case OPS_ERR:
	default:
		ret_val = 0;
		mops->error = -MCE_ERR_ACTIVE;
		goto out;
	}

	// Command size check
	if (count != sizeof(mops->cmd)) {
		PRINT_ERR(SUBNAME "count != sizeof(mce_command)\n");
		ret_val = -EPROTO;
		goto out;
	}

	// Copy command to local buffer
	if ( (err=copy_from_user(&mops->cmd, buf,
				 sizeof(mops->cmd)))!=0) {
		PRINT_ERR(SUBNAME "copy_from_user incomplete\n");
		ret_val = count - err;
		goto out;
	}

	// Set CMD, then go back to IDLE on failure.
	//  - leaves us vulnerable to unexpected interrupts
	//  - the alternative risks losing expected interrupts

	mops->state = OPS_CMD;
	if ((err=mce_send_command(&mops->cmd, mce_write_callback, 1, fpdata->minor))!=0) {
		PRINT_ERR(SUBNAME "mce_send_command failed\n");
		mops->state = OPS_IDLE;
		mops->error = err;
		ret_val = 0;
		goto out;
	}
 
	ret_val = count;
 out:
	up(&mops->sem);

	PRINT_INFO(SUBNAME "exiting [%#x]\n", ret_val);
	return ret_val;
}
#undef SUBNAME


#define SUBNAME "mce_ioctl: "
int mce_ioctl(struct inode *inode, struct file *filp,
	      unsigned int iocmd, unsigned long arg)
{
	struct filp_pdata *fpdata = filp->private_data;
	int card = fpdata->minor;
	struct mce_ops_t *mops = mce_ops + card;
	int x;

	switch(iocmd) {

	case MCEDEV_IOCT_RESET:
	case MCEDEV_IOCT_QUERY:
		PRINT_ERR("ioctl: not yet implemented!\n");
		return -1;

	case MCEDEV_IOCT_HARDWARE_RESET:
		return mce_hardware_reset(card);
	       
	case MCEDEV_IOCT_INTERFACE_RESET:
		return mce_interface_reset(card);

	case MCEDEV_IOCT_LAST_ERROR:
		x = mops->error;
		mops->error = 0;
		return x;

	case MCEDEV_IOCT_GET:
		return mops->properties;
		
	case MCEDEV_IOCT_SET:
		mops->properties = (int)arg;
		return 0;
		
	default:
		PRINT_ERR("ioctl: unknown command (%#x)\n", iocmd );
	}

	return -1;
}
#undef SUBNAME


int mce_open(struct inode *inode, struct file *filp)
{
	struct filp_pdata *fpdata = kmalloc(sizeof(struct filp_pdata), GFP_KERNEL);
	fpdata->minor = iminor(inode);
	filp->private_data = fpdata;

	PRINT_INFO("mce_open\n");
	return 0;
}

#define SUBNAME "mce_release: "
int mce_release(struct inode *inode, struct file *filp)
{
	struct filp_pdata *fpdata = filp->private_data;
	struct mce_ops_t *mops = mce_ops + fpdata->minor;

	PRINT_INFO(SUBNAME "entry\n");

	if(fpdata != NULL) {
		kfree(fpdata);
	} else PRINT_ERR(SUBNAME "called with NULL private_data\n");

	// Re-idle the state so subsequent commands don't (necessarily) fail
	if ((mops->properties & MCEDEV_CLOSE_CLEANLY) && mops->state == OPS_CMD) {
		PRINT_ERR("mce_release: closure forced, setting state to idle.\n");
		mops->state = OPS_IDLE;
	}

	return 0;
}
#undef SUBNAME

#define SUBNAME "mce_ops_init: "
int mce_ops_init(void)
{
	int err = 0;
	int i = 0;
	PRINT_INFO(SUBNAME "entry\n");

	err = register_chrdev(0, MCEDEV_NAME, &mce_fops);
	if (err<0) {
		PRINT_ERR("mce_ops_init: could not register_chrdev,"
			  "err=%#x\n", -err);
	} else {
		for(i=0; i<MAX_CARDS; i++) {
			struct mce_ops_t *mops = mce_ops + i;		
			mops->major = err;
		}
		err = 0;
	}

	PRINT_INFO(SUBNAME "ok\n");
	return err;
}
#undef SUBNAME

#define SUBNAME "mce_ops_probe: "
int mce_ops_probe(int card)
{
	struct mce_ops_t *mops = mce_ops + card;

	PRINT_INFO(SUBNAME "entry\n");

	init_waitqueue_head(&mops->queue);
	init_MUTEX(&mops->sem);

	mops->state = OPS_IDLE;
 
	PRINT_INFO(SUBNAME "ok\n");
	return 0;
}
#undef SUBNAME

#define SUBNAME "mce_ops_remove: "
int mce_ops_cleanup(void)
{
	PRINT_INFO(SUBNAME "entry\n");

	if (mce_ops->major != 0) 
		unregister_chrdev(mce_ops->major, MCEDEV_NAME);

	PRINT_INFO(SUBNAME "ok\n");
	return 0;
}
#undef SUBNAME