/* -*- mode: C; tab-width: 4; indent-tabs-mode: nil; c-basic-offset: 4 -*-
 *      vim: sw=4 ts=4 et tw=80
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>

#include "libmaslog.h"


#define LINE 1024

int main(int argc, char **argv) {
    mce_context_t *context;
    maslog_t *logger;

    if (argc > 1) {
        context = mcelib_create(MCE_DEFAULT_MCE, argv[1], 0);
    } else {
        context = mcelib_create(MCE_DEFAULT_MCE, NULL, 0);
    }

    if (context == NULL)
        exit(1);

    logger = maslog_connect(context, "notes");

    if (logger == NULL)
        exit(1);

    char *line = (char*)malloc(LINE);

    while (!feof(stdin) ) {
        size_t nin = LINE-1;
        int nout = getline(&line, &nin, stdin);
        if (nout>0) {
            if (line[nout]!=0)
                line[nout]=0;
            if (line[nout-1]=='\n')
                line[--nout]=0;
            maslog_print(logger, line);
        }
    }

    maslog_close(logger);

    return 0;
}
