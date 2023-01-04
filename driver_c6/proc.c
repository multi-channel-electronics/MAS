/* -*- mode: C; tab-width: 4; indent-tabs-mode: nil; c-basic-offset: 4 -*-
 *      vim: sw=4 ts=4 et tw=80
 */
#include "proc.h"

#include "mce_options.h"
#include "data.h"
#include "mce_driver.h"
#include "dsp_driver.h"
#include "autoversion.h"

#ifdef FAKEMCE
#  include <dsp_fake.h>
#else
#  include "dsp_driver.h"
#endif

static int proc_show(struct seq_file *sfile, void *data)
{
    int i;

    seq_printf(sfile,"\nmce_dsp driver version %s\n",
            VERSION_STRING);
    seq_printf(sfile,"    fakemce:  "
#ifdef FAKEMCE
            "yes\n"
#else
            "no\n"
#endif
            );
    seq_printf(sfile,"    realtime: "
#ifdef REALTIME
            "yes\n"
#else
            "no\n"
#endif
            );
    seq_printf(sfile,"    bigphys:  "
#ifdef BIGPHYS
            "yes\n"
#else
            "no\n"
#endif
            );

    for (i=0; i<MAX_CARDS; i++) {
        void *data = (void*)(unsigned long)i;
        PRINT_INFO(NOCARD, "i=%d\n", i);
        seq_printf(sfile,"\nCARD: %d\n\n", i);
        seq_printf(sfile,"  data buffer:\n");
        data_proc(sfile, data);
        seq_printf(sfile,"  mce commander:\n");
        mce_proc(sfile, data);
        seq_printf(sfile,"  dsp commander:\n");
        dsp_proc(sfile, data);
    }

    seq_printf(sfile,"\n");

    return 0;
}

static int mcedsp_proc_open(struct inode *inode, struct file *file)
{
    return single_open(file, &proc_show, NULL);
}

#if LINUX_VERSION_CODE >= KERNEL_VERSION(5,6,0)
const struct proc_ops mcedsp_proc_ops = {
    .proc_open = mcedsp_proc_open,
    .proc_read = seq_read,
    .proc_lseek = seq_lseek,
    .proc_release = single_release
};
#else
const struct file_operations mcedsp_proc_ops = {
    .owner = THIS_MODULE,
    .open = mcedsp_proc_open,
    .read = seq_read,
    .llseek = seq_lseek,
    .release = single_release
};
#endif
