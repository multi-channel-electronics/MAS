/* -*- mode: C; tab-width: 4; indent-tabs-mode: nil; c-basic-offset: 4 -*-
 *      vim: sw=4 ts=4 et tw=80
 */
/*! \file mce_status.c
 *
 *  \brief Program to read and record the status of the MCE.
 *
 *  How about we make this a generic tree traverser that you can plug
 *  your own operations into.
 */

#include <stdlib.h>
#include <stdio.h>
#include <fcntl.h>
#include <string.h>
#include <unistd.h>

#include "mce_status.h"
#include "das.h"
#include "mas.h"
#include "cfg_dump.h"

options_t options = {
    .fibre_card = -1,
    .config_file = NULL,
    .hardware_file = NULL,
    .mode = CRAWLER_DAS,
};


void error_log_exit(maslog_t* logger, const char *msg, int error);

int crawl_festival(crawler_t *crawler);

int main(int argc, char **argv)
{
    maslog_t *logger = NULL;
    char msg[MCE_LONG];

    if (process_options(&options, argc, argv)) {
        fprintf(stderr, "invalid arguments");
        exit(2);
    }

    // Connect to MCE, if necessary
    if ((options.context = mcelib_create(options.fibre_card,
                    options.config_file, 0)) == NULL)
    {
        fprintf(stderr, "failed to initialise MAS.\n");
        return 3;
    }

    if (options.mode != CRAWLER_CFG && options.mode != CRAWLER_CFX) {
        logger = maslog_connect(options.context, "mce_status");
        sprintf(msg, "initiated with hardware config '%s'",
                options.config_file);
        maslog_print(logger, msg);

        if (mcecmd_open(options.context) != 0) {
            sprintf(msg, "Could not open CMD device\n");
            error_log_exit(logger, msg, 3);
        }
    }

    // Load configuration
    if (mceconfig_open(options.context,
                options.hardware_file, "hardware")!=0) {
        sprintf(msg, "Could not load MCE config file '%s'.\n",
                options.hardware_file);
        error_log_exit(logger, msg, 3);
    }


    // Set up the action handler
    crawler_t crawler;

    switch (options.mode) {

        case CRAWLER_DAS:
            das_crawler(&options, &crawler);
            break;

        case CRAWLER_MAS:
            mas_crawler(&options, &crawler);
            break;

        case CRAWLER_CFG:
        case CRAWLER_CFX:
            cfg_crawler(&options, &crawler);
            break;

        case CRAWLER_DRF:
            dirfile_crawler(&options, &crawler);
            break;

        default:
            fprintf(stderr, "Craler not implemented.\n");
            return 1;
    }

    // Loop through cards and parameters
    if (crawl_festival(&crawler) != 0)
        error_log_exit(logger, "failed commands", 3);

    maslog_print(logger, "successful");
    return 0;
}

int crawl_festival(crawler_t *crawler)
{
    int i,j;
    int n_cards = mceconfig_card_count(options.context);

    if (options.output_path[0] != 0 &&
            chdir(options.output_path)!=0) {
        fprintf(stderr, "Failed to move to working directory '%s'\n",
                options.output_path);
        return -1;
    }

    if (crawler->init != NULL &&
            crawler->init(crawler->user_data, &options)!=0 ) {
        fprintf(stderr, "Crawler failed to initialize.\n");
    }

    for (i=0; i<n_cards; i++) {
        mce_param_t m;
        card_t *c = &m.card;
        param_t *p = &m.param;

        if (mceconfig_card(options.context, i, c)) {
            fprintf(stderr, "Problem loading card data at index %i\n", i);
            return -1;
        }

        if (crawler->card)
            crawler->card(crawler->user_data, c);

        for (j=0; mceconfig_card_param(options.context, c, j, p)==0; j++) {
            if (crawler->item != NULL)
                crawler->item(crawler->user_data, &m);
        }
    }

    if (crawler->cleanup != NULL &&
            crawler->cleanup(crawler->user_data)!= 0) {
        return 1;
    }

    return 0;
}

void error_log_exit(maslog_t* logger, const char *msg, int error)
{
    if (logger)
        maslog_print(logger, msg);
    fprintf(stderr, "%s", msg);
    exit(error);
}
