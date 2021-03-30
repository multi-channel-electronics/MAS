/* -*- mode: C; tab-width: 4; indent-tabs-mode: nil; c-basic-offset: 4 -*-
 *      vim: sw=4 ts=4 et tw=80
 */
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include "mce_status.h"

#if !MULTICARD
#  define USAGE_OPTION_N "  -n <card number>       ignored\n"
#else
#  define USAGE_OPTION_N "  -n <card number>       use the specified fibre card\n"
#endif

#define USAGE_MESSAGE \
    "Usage:\n\t%s [options]\n"\
    USAGE_OPTION_N \
    "  -c <hardware config>   choose a particular hardware config file\n"\
    "  -d                     snapshot style, dirfile output\n"\
    "  -f <output filename>   filename for output (stdout by default)\n"\
    "  -g                     dump parameter mapping\n"\
    "  -G                     dump parameter mapping with extra information\n"\
    "  -m <mas config>        choose a particular mce config file\n"\
    "  -o <output directory>  destination folder for output\n"\
    "  -s                     snapshot style, civilized output\n"\
    "\n"\
    "  -h or -?               show this usage information and exit\n"\
    "  -v                     print version string and exit\n"\
    ""

int process_options(options_t* options, int argc, char **argv)
{
#if MULTICARD
    char *s;
#endif
    int option;
    while ( (option = getopt(argc, argv, "?hn:c:m:o:f:Ggvsd")) >=0) {

        switch(option) {
            case '?':
            case 'h':
                printf(USAGE_MESSAGE, argv[0]);
                return -1;

            case 'c':
                if (options->hardware_file)
                    free(options->hardware_file);
                options->hardware_file = strdup(optarg);
                break;

            case 'n':
#if MULTICARD
                options->fibre_card = (int)strtol(optarg, &s, 10);
                if (*optarg == '\0' || *s != '\0' || options->fibre_card < 0 ||
                        options->fibre_card >= MAX_FIBRE_CARD)
                {
                    fprintf(stderr, "%s: invalid fibre card number: %s\n", argv[0],
                            optarg);
                    return -1;
                }
#endif
                break;

            case 'm':
                if (options->config_file)
                    free(options->config_file);
                options->config_file = strdup(optarg);
                break;

            case 'f':
                strcpy(options->output_file, optarg);
                options->output_on = 1;
                break;

            case 'o':
                strcpy(options->output_path, optarg);
                break;

            case 'G':
                options->mode = CRAWLER_CFX;
                break;

            case 'g':
                options->mode = CRAWLER_CFG;
                break;

            case 's':
                options->mode = CRAWLER_MAS;
                break;

            case 'd':
                options->mode = CRAWLER_DRF;
                break;

            case 'v':
                printf("This is %s, version %s, using mce library version %s\n",
                        PROGRAM_NAME, VERSION_STRING, mcelib_version());
                exit(0);

            default:
                printf("Unimplemented option '-%c'!\n", option);
        }
    }

    return 0;
}
