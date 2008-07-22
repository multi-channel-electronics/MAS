#ifndef _DSP_PCI_H_
#define _DSP_PCI_H_

#include <linux/interrupt.h>
#include "mce/dsp.h"


/* PCI DMA alignment */

#define DMA_ADDR_ALIGN 1024
#define DMA_ADDR_MASK (0xffffffff ^ (DMA_ADDR_ALIGN-1))


/* PCI register definitions */

#define HSTR_HF3  0x8
#define HSTR_HRRQ 0x4
#define HSTR_HTRQ 0x2
#define HSTR_TRDY 0x1

/* DSP PCI vendor/device id */
#define DSP_VENDORID 0x1057
#define DSP_DEVICEID 0x1801


/* DSP fast interrupt vectors */

#define HCVR_INT_RST     0x8073 /* Clear DSP interrupt */
#define HCVR_INT_DON     0x8075 /* Clear interrupt flag */
#define HCVR_SYS_ERR     0x8077 /* Set fatal error flag */
#define HCVR_SYS_RST     0x808B /* Immediate system reset */
#define HCVR_INT_RPC     0x808D /* Clear RP buffer flag */
#define HCVR_SYS_IRQ0    0x808F /* Disable PCI interrupts */
#define HCVR_SYS_IRQ1    0x8091 /* Enable PCI interrupts */

#define PCI_MAX_FLUSH       256

#define DSP_PCI_MODE      0x900 /* for 32->24 bit conversion */


/* Soft interrupt generation timer frequency */

#define DSP_POLL_FREQ       100
#define DSP_POLL_JIFFIES    (HZ / DSP_POLL_FREQ + 1)


#pragma pack(1)

typedef struct {

	volatile u32 unused1[4];
	volatile u32 hctr;      // Host control register
	volatile u32 hstr;      // Host status register
	volatile u32 hcvr;      // Host command vector register(base+$018)
	volatile u32 htxr_hrxs; // Host transmit / receive data
	volatile u32 unused2[16384-32];

} dsp_reg_t;

#pragma pack()

typedef enum { DSP_PCI, DSP_POLL } dsp_int_mode;

struct dsp_dev_t {

	struct pci_dev *pci;

	dsp_reg_t *dsp;

	dsp_int_mode int_mode;
	irq_handler_t int_handler;
	struct timer_list tim;
};


/* Prototypes */

int   dsp_pci_init( char *dev_name );

int   dsp_pci_cleanup(void);

int   dsp_pci_proc(char *buf, int count);

int   dsp_pci_ioctl(unsigned int iocmd, unsigned long arg);

int   dsp_send_command_now( dsp_command *cmd );

int   dsp_send_command_now_vector( dsp_command *cmd, u32 vector );

int   dsp_pci_set_handler(struct pci_dev *pci,
			  irq_handler_t handler,
			  char *dev_name);

int   dsp_pci_flush(void);

int   dsp_pci_hstr(void);

void* dsp_allocate_dma(ssize_t size, unsigned long* bus_addr_p);

void  dsp_free_dma(void* buffer, int size, unsigned long bus_addr);

#endif
