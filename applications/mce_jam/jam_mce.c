/* -*- mode: C; tab-width: 4; indent-tabs-mode: nil; c-basic-offset: 4 -*-
 *      vim: sw=4 ts=4 et tw=80
 */
#include <unistd.h>
#include <string.h>
#include <stdlib.h>

#include "mce_library.h"
#include "mce/defaults.h"
#include "jam_mce.h"

/* "MAX_PAYLOAD" is the maximum number of 32-bit words that can be
   buffered before writing to MCE.  In principle you can fit 58 data
   words into an MCE command, but one word is needed to communicate
   the total number of bits in the payload (which is not necessary a
   nice multiple of the word size).  So then you are limited to 57.
   We used 56 here for a while, but reading back TDO fails for 56, for
   some reason, so now we set it to 55. */
#define MAX_PAYLOAD 55

/* "MAX_PACKING" is the number of bits actually stored in each 32-bit
   command word. */
#define MAX_PACKING 16
#define MAX_TBITS (MAX_PAYLOAD * MAX_PACKING)
#define TDO_SAMPLE_DELAY 2
#define MIN_TCK_HALF_PERIOD 2
#define MIN_DELAY_PER_BIT MIN_TCK_HALF_PERIOD*2*20 //ns
#define FUDGE_FACTOR 1

typedef enum {
    FW_REV=0,
    JTAG0,
    JTAG1,
    JTAG2,
    TMS_TDI,
    TDO,
    TCK_HALF_PERIOD,
    TDO_DELAY,
    /* Counter! */
    N_PARAM
} param_id;

const char *par_names[N_PARAM] = {
    "fw_rev",
    "jtag0",
    "jtag1",
    "jtag2",
    "tms_tdi",
    "tdo",
    "tck_half_period",
    "tdo_sample_dly",
};


/* Globals! */

static mce_context_t *mce = NULL;
//static int freq = 0;
static int delay_per_bit = 0;
static int tck_half_period = 0;
static int packetization_stats[MAX_PAYLOAD * MAX_PACKING];
//static int reads[MAX_PAYLOAD * MAX_PACKING];
//static int writes[MAX_PAYLOAD * MAX_PACKING];
static int32_t singles = 0;
static int32_t multiples = 0;
static int total_delay = 0;
static int total_bits = 0;
static int total_txs = 0;
//static int reading = 0;
static int reads = 0;
static int writes = 0;
mce_param_t par_addrs[N_PARAM];
int fibre_card = MCE_DEFAULT_MCE;

/* For smart packing of bits */

struct {
    int count; // Number of bit pairs
    uint32_t data[MAX_PAYLOAD];
} write_buffer;

struct {
    int count; // Number of bits read
    uint32_t data[MAX_PAYLOAD];
} read_buffer;

static int verbose = 1;

void initialize_mce(int frequency)
{
    if (verbose) {
        printf("init MCE\n");
        fflush(stdout);
    }

    // Initialize the array for compression statistics
    for (int j=0; j<MAX_PAYLOAD*MAX_PACKING; j++) {
        packetization_stats[j] = 0;
        //reads[j] = 0;
        //writes[j] = 0;
    }

    // Get context and load hardware config
    if (mce == NULL) {
        mce = mcelib_create(fibre_card, NULL, 0);
        if (mceconfig_open(mce, NULL, NULL) != 0) {
            fprintf(stderr, "Failed to load MCE configuration file.\n");
            exit(1);
        }
        if (mcecmd_open(mce) != 0) {
            fprintf(stderr, "Failed to open CMD device\n");
            exit(1);
        }
    }

    // Look up parameters
    for (int i=0; i<N_PARAM; i++) {
        if (verbose) {
            printf("MCE look-up %s\n", par_names[i]);
            fflush(stdout);
        }
        if (mcecmd_load_param(mce, par_addrs + i, "cc", par_names[i]) != 0) {
            fprintf(stderr, "Failed to lookup cc %s\n", par_names[i]);
            exit(1);
        }
    }

    // Clear store write data
    write_buffer.count = 0;

    // Use mce_frequency (Hz) to set:
    // tck_half_period = (50,000,000 [Hz]) / 2*(mce_frequency [Hz])
    tck_half_period = 50000000 / (2 * frequency);
    if (verbose)
        printf(">> initialize_mce(): tck_half_period = %d clock cycles.\n", tck_half_period);
    if (tck_half_period < MIN_TCK_HALF_PERIOD)
    {
        //printf("That MCE frequency (divider %i) probably will not work.\n", datum);
        printf(">> initialize_mce(): tck_half_period is too small.  Setting tck_half_period = %d clock cycles.\n", MIN_TCK_HALF_PERIOD);
        tck_half_period = MIN_TCK_HALF_PERIOD; // Accordingly with MIN_DELAY_PER_BIT, 25 clock cycles is the minimum tck_half_period.
    }

    // Use mce_frequency (Hz) for each tms_tdi transaction to set:
    // delay_per_bit [ns] = tck_half_period [clock cycles] * 20 [ns/clock cycle]
    delay_per_bit = tck_half_period * 20 * 2;
    if (verbose)
        printf(">> initialize_mce(): delay_per_bit = %d ns.\n", delay_per_bit);

#if 0
    // don't check this here, because tck_half_period has already been checked.
    if (delay_per_bit < MIN_DELAY_PER_BIT)
    {
        printf(">> initialize_mce(): delay_per_bit is too small.  Setting delay_per_bit = %d ns.\n", MIN_DELAY_PER_BIT);
        delay_per_bit = MIN_DELAY_PER_BIT; // 1000 ns is the minimum resolution of usleep()
    }
#endif

    // Set timing
    uint32_t datum = tck_half_period;
    mcecmd_write_block(mce, par_addrs + TCK_HALF_PERIOD, 1, &datum);

    if (verbose)
        printf(">> initialize_mce(): tdo_sample_delay = %d clock cycles.\n", TDO_SAMPLE_DELAY);
    datum = TDO_SAMPLE_DELAY;
    mcecmd_write_block(mce, par_addrs + TDO_DELAY, 1, &datum);

    // Enable JTAG programming
    uint32_t data[] = {0, 0, 2}; // Like byteblaster, write 2 to enable.
    if (mcecmd_write_block(mce, par_addrs + JTAG0, 1, data+0) ||
            mcecmd_write_block(mce, par_addrs + JTAG1, 1, data+1) ||
            mcecmd_write_block(mce, par_addrs + JTAG2, 1, data+2)) {
        fprintf(stderr, "MCE does not like JTAG2 commands.\n");
        exit(1);
    }
    if (verbose) {
        printf("ok\n");
        fflush(stdout);
    }
}

void close_mce(void)
{
    if (verbose)
        printf("MCE close\n");
    if (mce == NULL)
        return;

    // Initialize the array for compression statistics
    if (verbose) {
        printf("------------------------------------------\n");
        printf(">> close_mce(): flush statistics:\n");

        for (int j=0; j<MAX_PAYLOAD*MAX_PACKING; j++) {
            if (packetization_stats[j] != 0)
                printf("   %d bits: %d MCE packets.\n", j+1, packetization_stats[j]);
            total_bits += packetization_stats[j]*(j+1);
            total_txs += packetization_stats[j];
        }
        printf("------------------------------------------\n");
        printf(">> close_mce(): %d bits transmitted in %d MCE packets.\n", total_bits, total_txs);
        int32_t percentage = (singles / total_bits) * 100;
        printf(">> close_mce(): %ld single-bit stack pushes = %ld percent of data.\n",
                (long)singles, (long)percentage);
        printf(">> close_mce(): %ld multiple-bit stack pushes = %ld percent of data.\n",
                (long)multiples, 100L - percentage);
        printf(">> close_mce(): %d us spent delaying.\n", total_delay);
        printf(">> close_mce(): %d stack pushes for reads (=flush), %d stack pushes for writes.\n", reads, writes);
    }

    uint32_t data[] = {0}; // Like bitblaster, write 0 to disable
    write_buffer.count = 0;
    write_mce_single(0,0,0);

    if (mcecmd_write_block(mce, par_addrs + JTAG2, 1, data)!=0) {
        fprintf(stderr, "MCE does not like JTAG2 commands.\n");
        exit(1);
    }

    mcelib_destroy(mce);
    mce = NULL;
}


int flush_stack(int do_read)
{
    int mce_err = 0;

    if (write_buffer.count <= 0) {
        fprintf(stderr, "MCE: flush_stack called with no data.\n");
        read_buffer.count = 0;
        return 0;
    }

    packetization_stats[write_buffer.count - 1]++;

    int n_data = (write_buffer.count + MAX_PACKING - 1) / MAX_PACKING;
    uint32_t data[MCE_CMD_DATA_MAX];
    data[0] = write_buffer.count*2;
    for (int i=0; i < n_data; i++)
        data[i+1] = write_buffer.data[i];
    if ((mce_err=mcecmd_write_block(mce, par_addrs + TMS_TDI, n_data+1, data))!=0) {
        fprintf(stderr, "MCE does not like TMS_TDI commands.\n");
        fprintf(stderr, "mcelib error: %s\n", mcelib_error_string(mce_err));
        exit(1);
    }

    // delay [us] = number_of_bits [bits] * delay_per_bit [ns/bit] / 1000 [ns/us]
    int delay = ((int32_t)write_buffer.count * delay_per_bit) /
        (1000 * FUDGE_FACTOR);
    // Add one to delay, so that any fractions of a us are topped up.
    delay++;
    total_delay += delay;
    //if (verbose && delay > 1) printf(">> flush_stack(): %d us delay.\n", delay);
    usleep(delay);

    //if (verbose) printf(".");

    if (do_read) {
        if ((mce_err=mcecmd_read_block(mce, par_addrs + TDO, n_data, read_buffer.data))!=0) {
            fprintf(stderr, "MCE failed to read from TDO (%i words; "
                    "write count is %i).\n", n_data, write_buffer.count);
            fprintf(stderr, "mcelib error: %s\n", mcelib_error_string(mce_err));
            exit(1);
        }
        read_buffer.count = write_buffer.count;
    }

    // Flag the write buffer as 'empty.'
    write_buffer.count = 0;
    memset(write_buffer.data, 0, MAX_PAYLOAD *sizeof(*write_buffer.data));
    return 0;
}

void set_bits(int idx, int tms, int tdi)
{
    uint32_t *dest = write_buffer.data + idx / MAX_PACKING;
    int bit_idx = (idx % MAX_PACKING)*2;
    *dest &= ~(0x3 << bit_idx);
    *dest |= (((tms&1)<<1)|(tdi&1)) << bit_idx;
}

int get_bit(int idx)
{
    uint32_t *src = read_buffer.data + idx / MAX_PACKING;
    int bit_idx = idx % MAX_PACKING;
    if (read_buffer.count / MAX_PACKING == idx / MAX_PACKING) {
        bit_idx = read_buffer.count - idx - 1;
    } else {
        bit_idx = MAX_PACKING - 1 - bit_idx;
    }
    return (((*src) >> bit_idx) & 1) ^ 1;
}

// Push a single bit onto the stack to write, and flush the whole stack if we need to read a bit
// This limits the size of the stack to the max payload (MAX_TBITS) and triggers a stack-flush at the first instance of a read.
// read_tdo = 0, reading = 0: push it.
// read_tdo = 0, reading = 1: flush. push it.
// read_tdo = 1, reading = 0: flush. push it.
// read_tdo = 1, reading = 1: push it.
int write_mce_single(int tms, int tdi, int read_tdo)
{
    singles++;

    if (read_tdo)
        reads++;
    else
        writes++;

    set_bits(write_buffer.count++, tms, tdi);
    if (write_buffer.count >= MAX_TBITS || read_tdo)
        flush_stack(read_tdo);
    if (read_tdo)
        return get_bit(read_buffer.count-1);
    return 0;
}

/* Transmit the bits in tdi, setting tms only on the last bit.  If tdo
   is non-null, record the returned bits. */

// This function is for reading/ writing blocks of data.
// This limits the size of the stack to the size of count (the transaction), and flushes immediately.
// The disadvantage of this is that the flushes tend to be smaller in size, and more are necessary.
// This means that it takes longer to use this function
// We could fix this if we didn't have to flush for every transaction.
// The problem here is that when we are stacking reads, it is impossible to know when the program is going to want to examine that data..
// Also, is it safe to say that every program finishes with a read?  Perhaps.  Is this how Matt knows when to do the last flush?
// I need to determine this one level up.
int scan_mce(int count, char *tdi, char *tdo)
{
    /* printf("count=%i\n", count); */
    int dest_idx = 0;
    //int new_size = 0;

    multiples++;

    if (tdo != NULL)
        reads++;
    else
        writes++;

    //printf(">> scan_mce()\n");
    // If there is anything on the stack, flush it first and read back
    if (write_buffer.count > 0)
        flush_stack(1);

    // Now, push the new bits to write onto the stack.
    for (int i=0; i<count; i++) {
        set_bits(write_buffer.count++, (int)(i==count-1), (tdi[i/8] >> (i%8)) & 1);

        // If we haven't maxed out the buffer size, and we haven't finished pushing on bits, skip the rest of this for-loop
        if (write_buffer.count < MAX_TBITS && i<count-1)
            continue;

        // Otherwise, flush the stack and read back if necessary.
        flush_stack(tdo != NULL);
        // If we are not expecting output from the flush, skip the rest of this for-loop
        if (tdo == NULL)
            continue;
        // Otherwise, read out the buffer until the end.
        for (int j=0; j<read_buffer.count; j++) {
            if (get_bit(j))
                tdo[dest_idx / 8] |=  (1 << (dest_idx%8));
            else
                tdo[dest_idx / 8] &= ~(1 << (dest_idx%8));
            dest_idx++;
        }
#if 0
        if (i % 8 == 7) {
            printf("M %x\n", (int)(unsigned char)tdo[i/8]);
        }
#endif
    }
    return 0;
}

void dump_stack()
{
    for (int i=0; i<read_buffer.count; i++) {
        printf("%3i %i\n", i, get_bit(i));
    }
}


void reset_test()
{
    // Run a reset.
    const char my_seq[] = {
        // 0, 0, 0, 0, 0, 0, 0, 0,
        1, 1, 1, 1, 1, 0,
        1, 1, 1, 0,
        1, 1, 0,
        0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
        0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
        -1
    };
    initialize_mce(1000000);

    for (int i=0; my_seq[i]>=0; i++) {
        if (my_seq[i] == 9) {
            flush_stack(1);
            dump_stack();
            sleep(1);
            continue;
        }
        printf("%i %i %i\n", my_seq[i], 0, write_mce_single(my_seq[i], 0, 0));
    }
    flush_stack(1);
    dump_stack();
    close_mce();
}
