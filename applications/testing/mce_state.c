/*! \file mce_state.c
 *
 *  \brief Test mce_driver state handling.
 */

#include <stdlib.h>
#include <stdio.h>
#include <fcntl.h>
#include <string.h>
#include <unistd.h>

#include <mce_library.h>

int  main(int argc, char **argv)
{
	mce_context_t *mce1 = mcelib_create();
	mce_context_t *mce2 = mcelib_create();

	// Connect command module.
	char device[] = "/dev/mce_cmd0";
	char hardware[] = "/etc/mce/mce.cfg";
	if (mcecmd_open(mce1, device) || mcecmd_open(mce2, device)) {
		fprintf(stderr, "Could not open mce device '%s'\n",
			device);
		exit(1);
	}
	if (mceconfig_open(mce1, hardware, NULL) || mceconfig_open(mce2, hardware, NULL)) {
		fprintf(stderr, "Could not load MCE config file '%s'.\n",
			hardware);
		exit(1);
	}


	// Write command
	mce_param_t p1, p2;
	mcecmd_load_param(mce1, &p1, "cc", "fw_rev");
	mcecmd_load_param(mce2, &p2, "rc1", "num_rows");

	// Go
	u32 d1[10], d2[10];
	mce_command cmd1, cmd2;
	mce_reply rep1, rep2;
	int err1, err2;
	mcecmd_load_command(&cmd1, MCE_RB, p1.card.id[0], p1.param.id,
			    1, 1, d1);
	mcecmd_load_command(&cmd2, MCE_RB, p2.card.id[0], p2.param.id,
			    1, 1, d2);

	printf("Sequential write/read, write/read:\n");
	err1 = mcecmd_send_command(mce1, &cmd1, &rep1);
	err2 = mcecmd_send_command(mce2, &cmd2, &rep2);
	printf("  Read (%i,%i): %08x %08x\n", err1, err2, rep1.data[0], rep2.data[0]);
		
	// Can reply be read more than once?
	printf("Double reply read?\n");
	mcecmd_send_command_now(mce1, &cmd1);
	err1 = mcecmd_read_reply_now(mce1, &rep1);
	err2 = mcecmd_read_reply_now(mce1, &rep2);
	if (err1 != 0 || err2 == 0) {
		printf("  Weird: (%i,%i): %08x %08x\n", err1, err2, rep1.data[0], rep2.data[0]);
	} else {
		printf("  Single reply successful.\n");
	}

	for (int lock=0; lock<3; lock++) {
		switch(lock) {
		case 0:
			printf("Default mode\n");
			break;
		case 1:
			printf("Unlocked mode.\n");	
			mcecmd_lock_replies(mce1, 0);
			break;
		case 2:
			printf("Locked mode.\n");
			mcecmd_lock_replies(mce1, 1);
			break;
		}
		// Can another connection steal the reply?
		mcecmd_send_command_now(mce1, &cmd1);
		err2 = mcecmd_read_reply_now(mce2, &rep2);
		err1 = mcecmd_read_reply_now(mce1, &rep1);
		if (err1 == 0 && err2 != 0) {
			printf("  Reply successfully locked.\n");
		} else if (err1 != 0 && err2 == 0) {
			printf("  Reply stolen.\n");
		} else {
			printf("  Weird: (%i,%i): %08x %08x\n", err1, err2, rep1.data[0], rep2.data[0]);
		}			
	}
	mcelib_destroy(mce1);
	mcelib_destroy(mce2);
	return 0;
}
