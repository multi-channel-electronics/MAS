/* -*- mode: C; tab-width: 4; indent-tabs-mode: nil; c-basic-offset: 4 -*-
 *      vim: sw=4 ts=4 et tw=80
 */
#ifndef MCELIB_CONTEXT_H
#define MCELIB_CONTEXT_H

#include <mce_library.h>
#include <libmaslog.h>

/* Module information structure */

typedef struct mcecmd {

	int connected;
	int fd;

	char dev_name[MCE_LONG];
	char errstr[MCE_LONG];

} mcecmd_t;

/* Module information structure */

typedef struct mcedata {

	int connected;
	int fd;

	char dev_name[MCE_LONG];
	char errstr[MCE_LONG];

	void *map;
	int map_size;
} mcedata_t;

#define MCEDATA_PACKET_MAX 4096 /* Maximum frame size in dwords */

/* Context structure associates connections on the three modules. */

struct mce_context {

  int fibre_card;
  mcecmd_t    cmd;
  mcedata_t   data;
  mceconfig_t config;
  maslog_t maslog;

};

/* Macros for easy dereferencing of mce_context_t context into cmd,
   data, and config members, and to check for connections of each
   type. */

#define  C_cmd          context->cmd
#define  C_data         context->data
#define  C_config       context->config
#define  C_maslog       (context->maslog)

#define  C_cmd_check    if (!C_cmd.connected)    return -MCE_ERR_NEED_CMD
#define  C_data_check   if (!C_data.connected)   return -MCE_ERR_NEED_DATA
#define  C_config_check if (!C_config.connected) return -MCE_ERR_NEED_CONFIG
#define  C_maslog_check if (!C_config.connected) return -MCE_ERR_NEED_CONFIG


#endif