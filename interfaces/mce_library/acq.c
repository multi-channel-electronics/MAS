#define _GNU_SOURCE

#include <stdlib.h>
#include <errno.h>
#include <string.h>
#include <ctype.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/ioctl.h>

#include <mce_library.h>
#include <mce/data_ioctl.h>

#include "data_thread.h"
#include "frame_manip.h"

/* #define LOG_LEVEL_CMD     LOGGER_DETAIL */
/* #define LOG_LEVEL_REP_OK  LOGGER_DETAIL */
/* #define LOG_LEVEL_REP_ER  LOGGER_INFO */

#define FRAME_USLEEP 1000


static int copy_frames_mmap(mce_acq_t *acq);

static int copy_frames_read(mce_acq_t *acq);

static int set_n_frames(mce_acq_t *acq, int n_frames);

static int get_n_frames(mce_acq_t *acq);

static int card_count(int cards);

static int load_frame_params(mce_context_t *context, mce_acq_t *acq,
			     int cards);

static int load_ret_dat(mce_acq_t *acq, int cards);

static int cards_to_rcsflags(int c);

static int rcsflags_to_cards(int c);

int mcedata_acq_create(mce_acq_t *acq, mce_context_t* context,
		       int options, int cards, int rows_reported,
		       mcedata_storage_t *storage)
{
	int ret_val = 0;

	// Zero the structure!
	memset(acq, 0, sizeof(*acq));

	// Load frame size parameters from MCE
	ret_val = load_frame_params(context, acq, cards);
	if (ret_val != 0) return ret_val;
	
	// Save frame size and other options
	acq->frame_size = acq->rows * acq->cols * card_count(acq->cards) + 
		MCEDATA_HEADER + MCEDATA_FOOTER;
	acq->options = options;
	acq->context = context;
	acq->storage = storage;

	// Lookup "rc# ret_dat" (go address) location or fail.
	if (load_ret_dat(acq, cards) != 0)
		return -MCE_ERR_FRAME_CARD;
	
	// Lookup "cc ret_dat_s" (frame count) or fail
	if ((ret_val=mcecmd_load_param(
		     acq->context, &acq->ret_dat_s, "cc", "ret_dat_s")) != 0) {
/* 		fprintf(stderr, "Could not load 'cc ret_dat_s'\n"); */
		return ret_val;
	}
	
	// Set frame size in driver.
	ret_val = mcedata_set_datasize(acq->context,
				       acq->frame_size * sizeof(u32));
	if (ret_val != 0) {
 		fprintf(stdout, "Could not set data size [%i]\n", ret_val);
		return -MCE_ERR_FRAME_SIZE;
	}

	if (acq->storage->init != NULL && acq->storage->init(acq) != 0) {
		fprintf(stdout, "Storage init action failed.\n");
		return -MCE_ERR_FRAME_OUTPUT;
	}

	acq->ready = 1;
	return 0;
}

int mcedata_acq_destroy(mce_acq_t *acq)
{
	if (acq->storage->cleanup != NULL && acq->storage->cleanup(acq) != 0) {
		fprintf(stdout, "Storage init action failed.\n");
		return -MCE_ERR_FRAME_OUTPUT;
	}

	acq->storage = mcedata_storage_destroy(acq->storage);

	return 0;
}

int mcedata_acq_go(mce_acq_t *acq, int n_frames)
{
	int ret_val = 0;

	// Assertion
	if (acq==NULL || !acq->ready) {
		fprintf(stderr, "mcedata_acq_go: acq structure null or not ready!\n");
		return -MCE_ERR_FRAME_UNKNOWN;
	}			

	// Does checking / setting ret_dat_s really slow us down?
	if (n_frames < 0) {
		n_frames = get_n_frames(acq);
		if (n_frames <= 0) return -MCE_ERR_FRAME_COUNT;
	}

	// Check if ret_dat_s needs changing...
	if ( n_frames != acq->last_n_frames || acq->last_n_frames <= 0 ) {
		ret_val = set_n_frames(acq, n_frames);
		if (ret_val != 0)
			return -MCE_ERR_FRAME_COUNT;
	}

	// Issue the MCE 'GO' command.
	ret_val = mcecmd_start_application(acq->context, &acq->ret_dat);
	if (ret_val != 0) return ret_val;

	// Launch the data thread, or just block for data.
	acq->n_frames = n_frames;

	if (acq->options & MCEDATA_THREAD) {
		data_thread_t d;
		d.state = MCETHREAD_IDLE;
		d.acq = acq;
		if ((ret_val=data_thread_launcher(&d)) != 0)
			return ret_val;

		while (d.state != MCETHREAD_IDLE) {
/* 			printf("thread state = %i\n", d.state); */
			usleep(FRAME_USLEEP);
		}

	} else {
		/* Block for frames, and return */
		if (acq->context->data.map != NULL) {
			ret_val = copy_frames_mmap(acq);
		} else {
			ret_val = copy_frames_read(acq);
		}
	}

	return ret_val;
}


/* set_n_frames - must tell both the MCE and the DSP about the number
 * of frames to expect. */

static int set_n_frames(mce_acq_t *acq, int n_frames)
{
	int ret_val;
	u32 args[2];

	args[0] = 0;
	args[1] = n_frames - 1;
	ret_val = mcecmd_write_block(acq->context, &acq->ret_dat_s, 2, args);
	if (ret_val != 0) {
		fprintf(stderr, "Could not set ret_dat_s! [%#x]\n", ret_val);
		acq->last_n_frames = -1;
	} else {
		acq->last_n_frames = n_frames;
	}

	// Inform DSP/driver, also.
	if (mcedata_qt_setup(acq->context, n_frames)) {
		fprintf(stderr, "Failed to set quiet transfer interval!\n");
		return -MCE_ERR_DEVICE;
	}

	return ret_val;
}


static int get_n_frames(mce_acq_t *acq)
{
	int ret_val;
	u32 args[2];
	ret_val = mcecmd_read_block(acq->context, &acq->ret_dat_s, 2, args);
	if (ret_val != 0) {
		fprintf(stderr, "Error reading ret_dat_s: '%s'",
			mcelib_error_string(ret_val));
		return -1;
	}
	return args[1] - args[0] + 1;
}

static int load_frame_params(mce_context_t *context, mce_acq_t *acq,
			     int cards)
{
	/* Determine frame size parameters
	      acq->rows     number of rows returning data
	      acq->cols     number of columns returning data
	      acq->cards    bit-mask (MCEDATA_RC?) of cards returning data
	      acq->row0     per-card values of first row reporting data
	      acq->col0     per-card values of first col reporting data

	   The acq->cards parameter is determined as follows: if the
	   function argument "cards" is positive, it is ANDed with
	   MCEDATA_RCS and stored in acq->cards.  If "cards" is 0 or
	   negative, acq->cards is determined by reading the
	   "rcs_to_report_data" register and recasting the result into
	   the MCEDATA_RC? bit-mask form.
	*/

	mce_param_t para_nrow, para_ncol, para_0, para_rcs;
	u32 data[64];
	int fw_rectangle = 0;         //firmware supports rectangle readout
	int fw_rcsflags = 0;          //firmware supports rcs_to_report_data
	int ret_val = 0;
	int i;
	
	/* Load MCE parameters to determine fw_* supported by this firmware */
	if (mcecmd_load_param(context, &para_ncol, "sys",
			      "num_cols_reported") == 0) {
		fw_rectangle = 1;
		if ((ret_val=mcecmd_load_param(context, &para_nrow, "sys",
					       "num_rows_reported")) != 0)
			return ret_val;
	} else if ((ret_val=mcecmd_load_param(context, &para_nrow, "cc",
					      "num_rows_reported")) != 0) {
		return ret_val;
	}
	if ((ret_val=mcecmd_load_param(context, &para_rcs, "cc",
				       "rcs_to_report_data")) == 0)
		fw_rcsflags = 1;

	/* Determine cards that will be returning data */
	if (cards <= 0) {
		acq->cards = MCEDATA_RCS;
		if (fw_rcsflags) {
			if (mcecmd_read_block(context, &para_rcs, 1, data) != 0)
				return -MCE_ERR_FRAME_COLS;
			acq->cards = rcsflags_to_cards((int)data[0]);
		}
	} else {
		acq->cards = (cards & MCEDATA_RCS);
	}

	/* Determine cols and rows reported */
	acq->cols = MCEDATA_COLUMNS;
	if (fw_rectangle) {
		if (mcecmd_read_block(context, &para_ncol, 1, data) != 0)
			return -MCE_ERR_FRAME_COLS;
		acq->cols = (int)data[0];
	}
	if (mcecmd_read_block(context, &para_nrow, 1, data) != 0)
		return -MCE_ERR_FRAME_ROWS;
	acq->rows = (int)data[0];
	
	// Load the row and column starting indices (for, e.g. dirfile field naming)
	for (i=0; i<MCEDATA_CARDS; i++) {
		char* rc = "rc0";
		if (!(acq->cards & (1<<i))) 
			continue;
		rc[2] = '1'+i;
		acq->row0[i] = 0;
		acq->col0[i] = 0;
		if ((mcecmd_load_param(context, &para_0, rc,
				       "readout_row_index")==0) &&
		    (mcecmd_read_block(context, &para_0, 1, data)==0))
			acq->row0[i] = data[0];
		if (!fw_rectangle)
			continue;
		if ((mcecmd_load_param(context, &para_0, rc,
				       "readout_col_index")==0) &&
		    (mcecmd_read_block(context, &para_0, 1, data)==0))
			acq->col0[i] = data[0];
	}
	return 0;
}


int load_ret_dat(mce_acq_t *acq, int cards)
{
	/* Return value is non-zero on error, but is not an mcelib error code! */
	if (cards <=0 || acq->cards==MCEDATA_RCS) {
		return mcecmd_load_param(acq->context, &acq->ret_dat, "rcs", "ret_dat");
	}
	
	// Lookup the go command location for the specified card set.
	switch (acq->cards) {
	case MCEDATA_RC1:
		return mcecmd_load_param(acq->context, &acq->ret_dat, "rc1", "ret_dat");
	case MCEDATA_RC2:
		return mcecmd_load_param(acq->context, &acq->ret_dat, "rc2", "ret_dat");
	case MCEDATA_RC3:
		return mcecmd_load_param(acq->context, &acq->ret_dat, "rc3", "ret_dat");
	case MCEDATA_RC4:
		return mcecmd_load_param(acq->context, &acq->ret_dat, "rc4", "ret_dat");
	}

	fprintf(stderr, "Invalid card set selection [%#x]\n",
		acq->cards);
	return -1;
}


int copy_frames_mmap(mce_acq_t *acq)
{
	int ret_val = 0;
	int done = 0;
	int count = 0;
	int index = 0;
	u32 *data;

	int waits = 0;
	int max_waits = 1000;
	
	acq->n_frames_complete = 0;

	/* memmap loop */
	while (!done) {

		if (acq->storage->pre_frame != NULL &&
		    acq->storage->pre_frame(acq) != 0) {
				fprintf(stderr, "pre_frame action failed\n");
		}

		while (mcedata_poll_offset(acq->context, &ret_val) == 0) {
			usleep(1000);
			waits++;
			if (waits >= max_waits)
				done = EXIT_TIMEOUT;
			continue;
		}
		waits = 0;

		// New frame at offset ret_val
		data = acq->context->data.map + ret_val;

		// Logical formatting
		sort_columns( acq, data );

		if ( (acq->storage->post_frame != NULL) &&
		     acq->storage->post_frame( acq, count, data ) ) {
			fprintf(stderr, "post_frame action failed\n");
		}

		index = 0;
		if (++count >= acq->n_frames)
			done = EXIT_COUNT;

		if (frame_property(data, &frame_header_v6, status_v6)
		    & FRAME_STATUS_V6_STOP)
			done = EXIT_STOP;

		if (frame_property(data, &frame_header_v6, status_v6)
		    & FRAME_STATUS_V6_LAST)
			done = EXIT_LAST;

		// Inform driver of consumption
		mcedata_consume_frame(acq->context);
	}

	switch (done) {
	case EXIT_COUNT:
	case EXIT_LAST:
		acq->status = MCEDATA_IDLE;
		break;

	case EXIT_TIMEOUT:
		acq->status = MCEDATA_TIMEOUT;
		break;

	case EXIT_STOP:
		acq->status = MCEDATA_STOP;
		break;
		
	case EXIT_READ:
	case EXIT_WRITE:
	case EXIT_EOF:
	default:
		acq->status = MCEDATA_ERROR;
		break;
	}

	acq->n_frames_complete = count;

	return 0;
}

int copy_frames_read(mce_acq_t *acq)
{
	int ret_val = 0;
	int done = 0;
	int count = 0;
	int index = 0;
	u32 *data = malloc(acq->frame_size * sizeof(*data));

	int waits = 0;
	int max_waits = 1000;
	
	acq->n_frames_complete = 0;

	if (data==NULL) {
		fprintf(stderr, "Could not allocate frame buffer of size %i\n",
			acq->frame_size);
		return -MCE_ERR_FRAME_SIZE;
	}

	/* read method loop */
	while (!done) {

		if (acq->storage->pre_frame != NULL &&
		    acq->storage->pre_frame(acq) != 0) {
				fprintf(stderr, "pre_frame action failed\n");
		}

		ret_val = read(acq->context->data.fd, (void*)data + index,
			       acq->frame_size*sizeof(*data) - index);

		if (ret_val<0) {
			if (errno==EAGAIN) {
				usleep(1000);
				waits++;
				if (waits >= max_waits)
					done = EXIT_TIMEOUT;
			} else {
				// Error: clear rest of frame and quit
				fprintf(stderr,
					"read failed with code %i\n", ret_val);
				memset((void*)data + index, 0,
				       acq->frame_size*sizeof(*data) - index);
				done = EXIT_READ;
				break;
			}
		} else if (ret_val==0) {
			done = EXIT_EOF;
		} else {
			index += ret_val;
			waits = 0;
		}

		// Only dump complete frames to disk
		if (index < acq->frame_size*sizeof(*data))
			continue;

		// Logical formatting
		sort_columns( acq, data );

		if ( (acq->storage->post_frame != NULL) &&
		     acq->storage->post_frame( acq, count, data ) ) {
			fprintf(stderr, "post_frame action failed\n");
		}

		index = 0;
		if (++count >= acq->n_frames)
			done = EXIT_COUNT;

		if (frame_property(data, &frame_header_v6, status_v6)
		    & FRAME_STATUS_V6_STOP)
			done = EXIT_STOP;

		if (frame_property(data, &frame_header_v6, status_v6)
		    & FRAME_STATUS_V6_LAST)
			done = EXIT_LAST;
	}

	switch (done) {
	case EXIT_COUNT:
	case EXIT_LAST:
		acq->status = MCEDATA_IDLE;
		break;

	case EXIT_TIMEOUT:
		acq->status = MCEDATA_TIMEOUT;
		break;

	case EXIT_STOP:
		acq->status = MCEDATA_STOP;
		break;
		
	case EXIT_READ:
	case EXIT_WRITE:
	case EXIT_EOF:
	default:
		acq->status = MCEDATA_ERROR;
		break;
	}

	acq->n_frames_complete = count;

	free(data);
	return 0;
}


int card_count(int cards)
{
	int n = 0;
	if (cards & MCEDATA_RC1) n++;
	if (cards & MCEDATA_RC2) n++;
	if (cards & MCEDATA_RC3) n++;
	if (cards & MCEDATA_RC4) n++;
	return n;
}

static int cards_to_rcsflags(int c)
{
	//Sure, there are cuter ways.
	int out = 0;
	if (c & MCEDATA_RC1) out |= MCEDATA_RCSFLAG_RC1;
	if (c & MCEDATA_RC2) out |= MCEDATA_RCSFLAG_RC2;
	if (c & MCEDATA_RC3) out |= MCEDATA_RCSFLAG_RC3;
	if (c & MCEDATA_RC4) out |= MCEDATA_RCSFLAG_RC4;
	return out;
}

static int rcsflags_to_cards(int c)
{
	//Sure, there are cuter ways.
	int out = 0;
	if (c & MCEDATA_RCSFLAG_RC1) out |= MCEDATA_RC1;
	if (c & MCEDATA_RCSFLAG_RC2) out |= MCEDATA_RC2;
	if (c & MCEDATA_RCSFLAG_RC3) out |= MCEDATA_RC3;
	if (c & MCEDATA_RCSFLAG_RC4) out |= MCEDATA_RC4;
	return out;
}
