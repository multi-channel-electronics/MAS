/*! \file mce_cmd.c
 *
 *  \brief Program to send commands to the MCE.
 *
 *  This program uses mce_library to send commands and receive
 *  responses from the MCE.  The program accepts instructions from
 *  standard input or from the command line using the -x option.
 */

#define _GNU_SOURCE

#include <stdlib.h>
#include <stdio.h>
#include <fcntl.h>
#include <string.h>
#include <unistd.h>

#include <mcecmd.h>
#include <mcedata.h>
#include <mceconfig.h>

#include <cmdtree.h>

#define LINE_LEN 1024
#define NARGS 64

#define DEFAULT_DEVICE "/dev/mce_cmd0"
#define DEFAULT_DATA "/dev/mce_data0"
#define DEFAULT_XML "/etc/mce.cfg"

#define FRAME_HEADER 43
#define FRAME_FOOTER 1
#define FRAME_COLUMNS 8

enum {
	ENUM_COMMAND_LOW,	
	COMMAND_RB,
	COMMAND_WB,
	COMMAND_R,
	COMMAND_W,
	COMMAND_GO,
	COMMAND_ST,
	COMMAND_RS,
	ENUM_COMMAND_HIGH,
	ENUM_SPECIAL_LOW,
	SPECIAL_HELP,
	SPECIAL_ACQ,
	SPECIAL_ACQ_CONFIG,
	SPECIAL_ACQ_CONFIG_FS,
	SPECIAL_QT_CONFIG,
	SPECIAL_QT_ENABLE,
	SPECIAL_CLEAR,
	SPECIAL_FAKESTOP,
	SPECIAL_EMPTY,
	SPECIAL_SLEEP,
	SPECIAL_COMMENT,
	SPECIAL_FRAME,
	SPECIAL_DEC,
	SPECIAL_HEX,
	SPECIAL_ECHO,
	ENUM_SPECIAL_HIGH,
};   


cmdtree_opt_t anything_opts[] = {
	{ CMDTREE_INTEGER, "", 0, -1, 0, anything_opts },
	{ CMDTREE_STRING , "", 0, -1, 0, anything_opts },
	{ CMDTREE_TERMINATOR, "", 0, 0, 0, NULL},
};

cmdtree_opt_t integer_opts[] = {
	{ CMDTREE_INTEGER   , "", 0, -1, 0, integer_opts },
	{ CMDTREE_TERMINATOR, "", 0, 0, 0, NULL},
};

cmdtree_opt_t string_opts[] = {
	{ CMDTREE_STRING    , "", 0, -1, 0, string_opts },
	{ CMDTREE_TERMINATOR, "", 0, 0, 0, NULL},
};

cmdtree_opt_t command_placeholder_opts[] = {
	{ CMDTREE_INTEGER   , "", 0, -1, 0, integer_opts },
	{ CMDTREE_TERMINATOR, "", 0, 0, 0, NULL},
};


cmdtree_opt_t flat_args[] = {
	{ CMDTREE_STRING | CMDTREE_ARGS, "filename", 0, -1, 0, flat_args+1 },
	{ CMDTREE_STRING | CMDTREE_ARGS, "card"    , 0, -1, 0, NULL },
	{ CMDTREE_TERMINATOR, "", 0, 0, 0, NULL},
};


cmdtree_opt_t fs_args[] = {
	{ CMDTREE_STRING | CMDTREE_ARGS, "filename", 0, -1, 0, fs_args+1 },
	{ CMDTREE_STRING | CMDTREE_ARGS, "card"    , 0, -1, 0, fs_args+2 },
	{ CMDTREE_INTEGER| CMDTREE_ARGS, "interval", 0, -1, 0, NULL},
	{ CMDTREE_TERMINATOR, "", 0, 0, 0, NULL},
};

cmdtree_opt_t root_opts[] = {
	{ CMDTREE_SELECT | CMDTREE_NOCASE, "RB"      , 2, 3, COMMAND_RB, command_placeholder_opts},
	{ CMDTREE_SELECT | CMDTREE_NOCASE, "WB"      , 3,-1, COMMAND_WB, command_placeholder_opts},
	{ CMDTREE_SELECT | CMDTREE_NOCASE, "r"       , 2, 3, COMMAND_RB, command_placeholder_opts},
	{ CMDTREE_SELECT | CMDTREE_NOCASE, "w"       , 3,-1, COMMAND_WB, command_placeholder_opts},
	{ CMDTREE_SELECT | CMDTREE_NOCASE, "GO"      , 2,-1, COMMAND_GO, command_placeholder_opts},
	{ CMDTREE_SELECT | CMDTREE_NOCASE, "STOP"    , 2,-1, COMMAND_ST, command_placeholder_opts},
	{ CMDTREE_SELECT | CMDTREE_NOCASE, "RESET"   , 2,-1, COMMAND_RS, command_placeholder_opts},
	{ CMDTREE_SELECT | CMDTREE_NOCASE, "HELP"    , 0, 0, SPECIAL_HELP    , NULL},
	{ CMDTREE_SELECT | CMDTREE_NOCASE, "ACQ_GO"  , 1, 1, SPECIAL_ACQ     , integer_opts},
	{ CMDTREE_SELECT | CMDTREE_NOCASE, "ACQ_CONFIG", 2, 2, SPECIAL_ACQ_CONFIG, flat_args},
 	{ CMDTREE_SELECT | CMDTREE_NOCASE, "ACQ_CONFIG_FS", 3, 3, SPECIAL_ACQ_CONFIG_FS, fs_args},
	{ CMDTREE_SELECT | CMDTREE_NOCASE, "QT_ENABLE", 1, 1, SPECIAL_QT_ENABLE, integer_opts},
	{ CMDTREE_SELECT | CMDTREE_NOCASE, "QT_CONFIG", 1, 1, SPECIAL_QT_CONFIG, integer_opts},
	{ CMDTREE_SELECT | CMDTREE_NOCASE, "FAKESTOP", 0, 0, SPECIAL_FAKESTOP, NULL},
	{ CMDTREE_SELECT | CMDTREE_NOCASE, "EMPTY"   , 0, 0, SPECIAL_EMPTY   , NULL},
	{ CMDTREE_SELECT | CMDTREE_NOCASE, "SLEEP"   , 1, 1, SPECIAL_SLEEP   , integer_opts},
	{ CMDTREE_SELECT | CMDTREE_NOCASE, "FRAME"   , 1, 1, SPECIAL_FRAME   , integer_opts},
	{ CMDTREE_SELECT | CMDTREE_NOCASE, "DEC"     , 0, 0, SPECIAL_DEC     , NULL},
	{ CMDTREE_SELECT | CMDTREE_NOCASE, "HEX"     , 0, 0, SPECIAL_HEX     , NULL},
	{ CMDTREE_SELECT | CMDTREE_NOCASE, "ECHO"    , 1, 1, SPECIAL_ECHO    , integer_opts},
	{ CMDTREE_SELECT | CMDTREE_NOCASE, "#"       , 0,-1, SPECIAL_COMMENT , anything_opts},
	{ CMDTREE_TERMINATOR, "", 0,0,0, NULL},
};
	
struct {
	int interactive;
	int nonzero_only;
	int no_prefix;
	int display;
	int echo;

	int das_compatible; // horror

	char batch_file[LINE_LEN];
	int  batch_now;

	char cmd_command[LINE_LEN];
	int  cmd_now;

	char device_file[LINE_LEN];
	char config_file[LINE_LEN];

} options = {
	0, 0, 0, SPECIAL_HEX, 0, 0, "", 0, "", 0, DEFAULT_DEVICE
};


enum { ERR_MEM=1,
       ERR_OPT=2,
       ERR_MCE=3 };

int handle = -1;
int  command_now = 0;
int  interactive = 0;
char *line;

char errstr[LINE_LEN];

mcedata_t mcedata;
mce_acq_t acq;
mceconfig_t *mce = NULL;

// This structure is used to cache data which eventually constructs acq.

struct {
	char filename[LINE_LEN];
	int  frame_size;
	int  cards;
	int  rows;
	int  n_frames;
	int  interval;
} my_acq;


/* Structure to cache mce parameter id's for items we will often need */

typedef struct preload_struct {
	u32 card_id;
	u32 para_id;
	int count;
	int cards;
} preload_t;

preload_t ret_dat_s;
preload_t num_rows_reported;

void preload_pair(preload_t *pre, const card_t *c, const param_t *p);
int  preload_mce_params();
int  load_mce_param(const preload_t *pre, u32 *data);
int  calculate_frame_size();

int  bit_count(int k);

int  menuify_mceconfig( mceconfig_t *mce, cmdtree_opt_t *opts);
int  process_options(int argc, char **argv);

int  process_command(cmdtree_opt_t *opts, cmdtree_token_t *tokens, char *errmsg);
void init_options(void);


int main(int argc, char **argv)
{
	FILE *ferr = stderr;
	FILE *fin  = stdin;

	int err = 0;
	//ignore args for now

	line = (char*) malloc(LINE_LEN);
	if (line==NULL) {
		fprintf(ferr, "memory error!\n");
		err = ERR_MEM;
		goto exit_now;
	}

	init_options();
	if (process_options(argc, argv)) {
		err = ERR_OPT;
		goto exit_now;
	}

	handle = mce_open(options.device_file);
	if (handle<0) {
		fprintf(ferr, "Could not open mce device '%s'\n",
			options.device_file);
		err = ERR_MCE;
		goto exit_now;
	}

	// Ready data thing
	if (mcedata_init(&mcedata, handle, DEFAULT_DATA)!=0) {
		fprintf(ferr, "No data device connection.\n");
		err = ERR_MCE;
		goto exit_now;
	}

	// Zero the acqusition structure
	mcedata_acq_reset(&acq, &mcedata);

	int line_count = 0;

	if (mceconfig_load(options.config_file, &mce)!=0) {
		fprintf(ferr, "Could not load MCE config file '%s'.\n",
			options.config_file);
		err = ERR_MCE;
		goto exit_now;
	}
	menuify_mceconfig(mce, root_opts);

	// Preload useful MCE parameter id's
	if (preload_mce_params()) {
		fprintf(ferr, "Could not pre-load useful MCE parameter id's.\n");
		err = ERR_MCE;
		goto exit_now;
	}
	
	//Open batch file, if given
	if (options.batch_now) {
		fin = fopen(options.batch_file, "r");
		if (fin==NULL) {
			fprintf(ferr, "Could not open batch file '%s'\n",
				options.batch_file);
			err = ERR_MCE;
			goto exit_now;
		}
	}
				

	char errmsg[1024] = "";
	char premsg[1024] = "";

	int done = 0;

	while (!done) {

		unsigned int n = LINE_LEN;

		if ( options.cmd_now ) {
			strcpy(line, options.cmd_command);
			done = 1;
		} else {

			getline(&line, &n, fin);
			if (n==0 || feof(fin)) break;

			n = strlen(line);
			if (line[n-1]=='\n') line[--n]=0;
			line_count++;
		}

		if (options.no_prefix)
			premsg[0] = 0;
		else
			sprintf(premsg, "Line %3i : ", line_count);

		if (options.echo) {
			printf("Cmd  %3i : %s\n", line_count, line);
		}

		errmsg[0] = 0;

		cmdtree_token_t args[NARGS];
		args[0].n = 0;
		int err = 0;

		cmdtree_debug = 0;

		err = cmdtree_tokenize(args, line, NARGS);
		if (err) {
			strcpy(errmsg, "could not tokenize");
		}

		if (!err) {
			int count = cmdtree_select( args, root_opts, errmsg);
			
			if (count < 0) {
				err = -1;
			} else if (count == 0) {
				if (options.interactive || args->n > 0) {
					cmdtree_list(errmsg, root_opts,
						     "mce_cmd expects argument from [ ", " ", "]");
					err = -1;
				}					
			} else {
 				err = process_command(root_opts, args, errmsg);
				if (err==0) err = 1;
			}
		}				

		if (err > 0) {
			if (*errmsg == 0) {
				if (!options.nonzero_only)
					printf("%sok\n", premsg);
			} else 
				printf("%sok : %s\n", premsg, errmsg);
		} else if (err < 0) {
			printf("%serror : %s\n", premsg, errmsg);
			if (options.interactive)
				continue;
			return 1;
		}
	}

	if (!options.nonzero_only)
		printf("Processed %i lines, exiting.\n", line_count);

exit_now:
	if (line!=NULL) free(line);
	if (handle>=0)  mce_close(handle);

	return err;
}

void init_options()
{
	strcpy(options.config_file, DEFAULT_XML);
	strcpy(options.device_file, DEFAULT_DEVICE);
}

int menuify_mceconfig( mceconfig_t *mce, cmdtree_opt_t *opts)
{
	cmdtree_opt_t *card_opts;
	cmdtree_opt_t *para_opts;
	char *string_table;
	int i,j;
	int n_cards = mce->card_count;
	int n_params = 0;
	
	// Count parameters
	for (i=0; i<n_cards; i++) {
		card_t card;
		cardtype_t ct;
		paramset_t ps;
		if (mceconfig_card(mce, i, &card)) {
			fprintf(stderr, "Problem loading card data at index %i\n", i);
			return -1;
		}
		if (mceconfig_card_cardtype(mce, &card, &ct)) {
			fprintf(stderr, "Problem loading cardtype data for '%s'\n", card.name);
			return -1;
		}
		for (j=0; j<ct.paramset_count; j++) {
			mceconfig_cardtype_paramset(mce, &ct, j, &ps);
			n_params += ps.param_count;
		}
	}
	
	string_table = malloc((n_params+n_cards)*MCE_SHORT);
	card_opts = malloc((n_params + 3 * n_cards + 2)*sizeof(*card_opts));
	para_opts = card_opts + n_cards + 2;
		
	for (i=0; i<n_cards; i++) {
		card_t card;
		cardtype_t ct;
		if (mceconfig_card(mce, i, &card)) {
			fprintf(stderr, "Problem loading card data at index %i\n", i);
			return -1;
		}
		if (mceconfig_card_cardtype(mce, &card, &ct)) {
			fprintf(stderr, "Problem loading cardtype data for '%s'\n", card.name);
			return -1;
		}
		
		// Fill out menu entry for card
		card_opts[i].name = string_table;
 		strcpy(string_table, card.name);
		string_table += strlen(string_table) + 1;
		card_opts[i].min_args = 1;
		card_opts[i].max_args = -1;
		card_opts[i].flags = CMDTREE_SELECT | CMDTREE_NOCASE;
		card_opts[i].sub_opts = para_opts;
		card_opts[i].user_cargo = (unsigned long)card.cfg;


		int k;
		paramset_t ps;
		param_t p;

		for (j=0; j<ct.paramset_count; j++) {
			mceconfig_cardtype_paramset(mce, &ct, j, &ps);
			for (k=0; k<ps.param_count; k++) {
				mceconfig_paramset_param(mce, &ps, k, &p);
				strcpy(string_table, p.name);
				para_opts->name = string_table;
				string_table += strlen(string_table) + 1;
				para_opts->flags = CMDTREE_SELECT | CMDTREE_NOCASE;
				para_opts->min_args = 0;
				para_opts->max_args = p.count;
				para_opts->sub_opts = integer_opts;
				para_opts->user_cargo = (unsigned long)p.cfg;
				para_opts++;
			}
		}

		memcpy(para_opts, integer_opts, sizeof(integer_opts));
		para_opts += 2;
	}

	memcpy(card_opts+n_cards, integer_opts, sizeof(integer_opts));

	for (i=0; (opts[i].flags & CMDTREE_TYPE_MASK) != CMDTREE_TERMINATOR; i++) {
		if (opts[i].sub_opts == command_placeholder_opts)
			opts[i].sub_opts = card_opts;
	}

	return 0;
}

void preload_pair(preload_t *pre, const card_t *c, const param_t *p)
{
	pre->card_id = c->id;
	pre->para_id = p->id;
	pre->count = p->count;
	pre->cards = p->card_count;
}

int preload_mce_params()
{
	int ret_val = 0;
	card_t c;
	param_t p;
	if ((ret_val=mceconfig_lookup(mce, "cc", "num_rows_reported", &c, &p))!=0) {
		fprintf(stderr, "Could not decode 'cc num_rows_reported' [%i]\n",
			ret_val);
		return -1;
	}
	preload_pair(&num_rows_reported, &c, &p);
	
	if ((ret_val=mceconfig_lookup(mce, "cc", "ret_dat_s", &c, &p))!=0) {
		fprintf(stderr, "Could not decode 'cc ret_dat_s' [%i]\n", ret_val);
		return -1;
	}
	preload_pair(&ret_dat_s, &c, &p);
	return 0;
}

int load_mce_param(const preload_t *pre, u32 *data)
{
	return mce_read_block(handle, pre->card_id, pre->para_id,
			      pre->count, data, pre->cards);
}

int learn_acq_params(int get_frame_count, int get_rows)
{
	u32 data[64];

	if (get_frame_count) {
		if (load_mce_param(&ret_dat_s, data)) {
			sprintf(errstr, "Failed to read frame count from MCE");
			return -1;
		}
		my_acq.n_frames = data[1]-data[0]+1;
	}

	if (get_rows) {
		if (load_mce_param(&num_rows_reported, data)) {
			sprintf(errstr, "Failed to read number of reported rows");
			return -1;
		}
		my_acq.rows = data[0];
	}
	return 0;
}

int calculate_frame_size()
{
	my_acq.frame_size = FRAME_HEADER + FRAME_FOOTER +
		my_acq.rows * FRAME_COLUMNS * bit_count(my_acq.cards);
	return my_acq.frame_size;
}

int translate_card_string(char *s)
{	
	if (strcmp(s, "rc1")==0)
		return MCEDATA_RC1;
	else if (strcmp(s, "rc2")==0)
		return MCEDATA_RC2;
	else if (strcmp(s, "rc3")==0)
		return MCEDATA_RC3;
	else if (strcmp(s, "rc4")==0)
		return MCEDATA_RC4;
	else if (strcmp(s, "rcs")==0)
		return MCEDATA_RC1 | MCEDATA_RC2 | MCEDATA_RC3 | MCEDATA_RC4;
	return -1;
}

int bit_count(int k)
{
	int i = 32;
	int count = 0;
	while (i-- > 0) {
		count += (k&1);
		k = k >> 1;
	}
	return count;
}

int prepare_outfile(char *errmsg, int file_sequencing)
{
	// Cleanup last acq
	if (acq.actions.cleanup!=NULL && acq.actions.cleanup(&acq)) {
		sprintf(errmsg, "Failed to clean up previous acq.");
		return -1;
	}

	// Basic init, including framesize -> driver.
	if (mcedata_acq_setup(&acq, 0, my_acq.cards,
			      my_acq.frame_size) != 0) {
		sprintf(errmsg, "Could not configure acq");
		return -1;
	}

	// Output type-specific
	if (file_sequencing) {
		if (mcedata_fileseq_create(&acq, my_acq.filename,
					   my_acq.interval, 3)) {
			sprintf(errmsg, "Could not set up file sequencer.");
			return -1;
		}
	} else {
		if (mcedata_flatfile_create(&acq, my_acq.filename) != 0) {
			sprintf(errmsg, "Could not create flatfile");
			return -1; 
		}
	}

	// Initialize this file type
	if (acq.actions.init!=NULL && acq.actions.init(&acq)) {
		sprintf(errmsg, "Failed to clean up previous acq.");
		return -1;
	}
	return 0;
}

int do_acq_compat(char *errmsg)
{
	if (learn_acq_params(1, 1)!=0)
		return -1;
	
	calculate_frame_size();
	
	mcedata_acq_reset(&acq, &mcedata);

	if (mcedata_acq_setup(&acq, 0, my_acq.cards, my_acq.frame_size) != 0) {
		sprintf(errmsg, "Could not setup acq structure.\n");
		return -1;
	}

	if (mcedata_flatfile_create(&acq, my_acq.filename) != 0) {
		sprintf(errmsg, "Could not create flatfile");
		return -1;
	}

	if (acq.actions.init!=NULL && acq.actions.init(&acq)) {
		sprintf(errmsg, "Failed to initialize acquisition");
		return -1;
	}

	if (mcedata_acq_go(&acq, my_acq.n_frames) != 0) {
		sprintf(errmsg, "Acqusition step failed");
		return -1;
	}

	return 0;
}


int process_command(cmdtree_opt_t *opts, cmdtree_token_t *tokens, char *errmsg)
{
	int ret_val = 0;
	int err = 0;
	int i;
	param_t p;
	card_t c;
	int to_read, to_write, card_mul;
	u32 buf[NARGS];
	char s[LINE_LEN];

	errmsg[0] = 0;

	int is_command = (tokens[0].value >= ENUM_COMMAND_LOW && 
			  tokens[0].value < ENUM_COMMAND_HIGH);
	
	if (is_command) {

		// Token[0] is the command (RB, WB, etc.)
		// Token[1] is the card
		// Token[2] is the param
		// Token[3+] are the data

		// Allow integer values for card and para.

		int raw_mode = 1;

		card_mul = 1;
		to_read = 0;
		to_write = tokens[3].n;
		int card_id = tokens[1].value;
		int para_id = tokens[2].value;

		if ( tokens[1].type == CMDTREE_SELECT ) {
			mceconfig_cfg_card ((config_setting_t*)tokens[1].value, &c);
			card_id = c.id;
			
			if (tokens[2].type == CMDTREE_SELECT ) {
				raw_mode = 0;
				mceconfig_cfg_param((config_setting_t*)tokens[2].value, &p);
				para_id = p.id;
				to_read = p.count;
				card_mul = c.card_count*p.card_count;
			}
		}

		if (to_read == 0 && tokens[3].type == CMDTREE_INTEGER) {
			to_read = tokens[3].value;
			if (tokens[4].type == CMDTREE_INTEGER) {
				card_mul = tokens[4].value;
			}
		}

		switch( tokens[0].value ) {
		
		case COMMAND_RS:
			err = mce_reset(handle, card_id, para_id);
			break;

		case COMMAND_GO:
			if (options.das_compatible) {
				cmdtree_token_word( s, tokens+1 );
				my_acq.cards = translate_card_string(s);
				if (my_acq.cards<0) {
					sprintf(errmsg, "Bad card name.\n");
					ret_val = -1;
					break;
				}

				// Get num_rows and n_frames
				if (learn_acq_params(1, 1)) {
					ret_val = -1;
					break;
				}

				// Calculate frame size
				calculate_frame_size();
				
				ret_val = prepare_outfile(errmsg, 0);
				if (ret_val)
					break;

				ret_val = mcedata_acq_go(&acq, my_acq.n_frames);
				if (ret_val != 0) {
					sprintf(errmsg, "Acquisition failed.\n");
				}
				
			} else {
				// If you get here, your data just accumulates
				//  in the driver's buffer, /dev/mce_data0, assuming that
				//  the frame size has been set correctly.
				err = mce_start_application(handle,
							    card_id, para_id);
			}
			break;

		case COMMAND_ST:
			err = mce_stop_application(handle,
						   card_id, para_id);
			break;

		case COMMAND_RB:
			//Check count in bounds; scale if "card" returns multiple data
			if (to_read*card_mul<0 || to_read*card_mul>MCE_REP_DATA_MAX) {
				sprintf(errstr, "read length out of bounds!");
				return -1;
			}

			err = mce_read_block(handle, card_id, para_id,
					     to_read, buf, card_mul);
			if (err)
				break;

			for (i=0; i<to_read*card_mul; i++) {
				if (options.display == SPECIAL_HEX )
					errmsg += sprintf(errmsg, "%#x ", buf[i]);
				else 
					errmsg += sprintf(errmsg, "%i ", buf[i]);
			}
			break;

		case COMMAND_WB:
			
			for (i=0; i<tokens[3].n && i<NARGS; i++) {
				buf[i] = tokens[3+i].value;
			}

			if (!raw_mode &&
			    mceconfig_check_data(&c, &p, to_write, buf,
						 MCE_PARAM_RONLY, errmsg)) {
				return -1;
			}

			err = mce_write_block(handle,
					      card_id, para_id, to_write, buf);
			break;
			
		default:
			sprintf(errmsg, "command not implemented");
			return -1;
		}
		
		if (err!=0 && errmsg[0] == 0) {
			sprintf(errmsg, "mce library error %#08x", err);
			ret_val = -1;
		} 
	} else {

		switch(tokens[0].value) {

		case SPECIAL_HELP:
/* 			cmdtree_list(errmsg, root_opts, */
/* 				     "MCE commands: [ ", " ", "]"); */
			break;

		case SPECIAL_ACQ:
			my_acq.n_frames = tokens[1].value;
			if (mcedata_acq_go(&acq, my_acq.n_frames) != 0) {
				sprintf(errmsg, "Acquisition failed.\n");
				ret_val = -1;
			}
			break;

		case SPECIAL_ACQ_CONFIG:
			/* Args: filename, card */

			cmdtree_token_word( my_acq.filename, tokens+1 );

			cmdtree_token_word( s, tokens+2 );
			my_acq.cards = translate_card_string(s);
			if (my_acq.cards < 0) {
				sprintf(errmsg, "Bad card option '%s'", s);
				ret_val = -1;
			}

			// Get num_rows from MCE
			if (learn_acq_params(0, 1)) {
				ret_val = -1;
				break;
			}

			// Calculate frame size
			calculate_frame_size();

			ret_val = prepare_outfile(errmsg, 0);
			break;

		case SPECIAL_ACQ_CONFIG_FS:
			/* Args: filename, card, interval */

			cmdtree_token_word( my_acq.filename, tokens+1 );

			cmdtree_token_word( s, tokens+2 );
			my_acq.cards = translate_card_string(s);
			if (my_acq.cards < 0) {
				sprintf(errmsg, "Bad card option '%s'", s);
				ret_val = -1;
			}

			// Get num_rows from MCE
			if (learn_acq_params(0, 1)) {
				ret_val = -1;
				break;
			}

			calculate_frame_size();
			
			ret_val = prepare_outfile(errmsg, 1);
			break;

		case SPECIAL_QT_ENABLE:
			ret_val = mcedata_qt_enable(&mcedata, tokens[1].value);
			break;

		case SPECIAL_QT_CONFIG:
			ret_val = mcedata_qt_setup(&mcedata, tokens[1].value);
			break;

		case SPECIAL_CLEAR:
			ret_val = mce_reset(handle, tokens[1].value, tokens[2].value);
			break;

		case SPECIAL_FAKESTOP:
			ret_val = mcedata_fake_stopframe(&mcedata);
			break;

		case SPECIAL_EMPTY:
			ret_val = mcedata_empty_data(&mcedata);
			break;

		case SPECIAL_SLEEP:
			usleep(tokens[1].value);
			break;

		case SPECIAL_COMMENT:
			break;

		case SPECIAL_FRAME:
			ret_val = mcedata_set_datasize(&mcedata, tokens[1].value);
			if (ret_val != 0) {
				sprintf(errmsg, "mce_library error %i", ret_val);
			}
			break;

		case SPECIAL_DEC:
			options.display = SPECIAL_DEC;
			break;

		case SPECIAL_HEX:
			options.display = SPECIAL_HEX;
			break;

		case SPECIAL_ECHO:
			options.echo = tokens[1].value;
			break;

		default:
			sprintf(errmsg, "command not implemented");
			ret_val = -1;
		}
	}

	return ret_val;
}

int process_options(int argc, char **argv)
{
	char *s;
	int option;
	while ( (option = getopt(argc, argv, "?hiqpf:c:d:C:x")) >=0) {

		switch(option) {
		case '?':
		case 'h':
			printf("Usage:\n\t%s [-i] [-q] [-p] [-d devfile] "
			       "[-c <config file> ] "
			       "[-f <batch file> |\n"
			       "\t\t-x <command>] [-C <data file> ]\n",
			       argv[0]);
			return -1;

		case 'i':
			options.interactive = 1;
			break;

		case 'q':
			options.nonzero_only = 1;
			break;

		case 'p':
			options.no_prefix = 1;
			break;

		case 'f':
			strcpy(options.batch_file, optarg);
			options.batch_now = 1;
			break;

		case 'c':
			strcpy(options.config_file, optarg);
			break;

		case 'd':
			strcpy(options.device_file, optarg);
			break;

		case 'C':
			options.das_compatible = 1;
			strcpy(my_acq.filename, optarg);
			break;

		case 'x':
			s = options.cmd_command;
			while (optind < argc) {
				s += sprintf(s, "%s ", argv[optind++]);
			}
			options.cmd_now = 1;
			break;

		default:
			printf("Unimplemented option '-%c'!\n", option);
		}
	}

	return 0;
}


int get_int(char *card, int *card_id)
{
	char *end = card;
	if (end==NULL || *end==0) return -1;
	*card_id = strtol(card, &end, 0);
	if (*end!=0) return -1;
	return 0;
}
