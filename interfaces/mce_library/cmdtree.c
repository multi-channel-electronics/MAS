/* -*- mode: C; tab-width: 4; indent-tabs-mode: nil; c-basic-offset: 4 -*-
 *      vim: sw=4 ts=4 et tw=80
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <errno.h>

#include "mce/cmdtree.h"

typedef mascmdtree_token_t token_t;

int mascmdtree_debug = 0;

static int get_int(char *card, long *card_id);

int mascmdtree_list(char *dest, const mascmdtree_opt_t *opts,
        const char *intro, const char *delim, const char *outro)
{
    int done = 0;
    int i;
    char *d = dest;
    d += sprintf(d, "%s", intro);
    for (i=0; !done; i++) {
        int type = opts[i].flags & MASCMDTREE_TYPE_MASK;

        switch(type) {
            case MASCMDTREE_TERMINATOR:
                done = 1;
                break;

            case MASCMDTREE_SELECT:
                d += sprintf(d, "%s%s", opts[i].name, delim);
                break;

            case MASCMDTREE_INTEGER:
                d += sprintf(d, "(int)%s", delim);
                break;

            case MASCMDTREE_STRING:
                d += sprintf(d, "(string)%s", delim);
                break;

            default:
                d += sprintf(d, "(other)%s", delim);
        }
    }
    d += sprintf(d, "%s", outro);
    return d-dest;
}

static int list_args(char *dest, mascmdtree_opt_t *opts)
{
    int index = 0;
    while (opts != NULL) {
        index += sprintf(dest+index, "%s", opts->name);
        opts = opts->sub_opts;
        if (opts != NULL) {
            index += sprintf(dest+index, ", ");
        }
    }
    return index;
}


static int nocase_cmp(const char *s1, const char *s2)
{
    while (*s1 != 0) {
        if (*s2==0)
            return 1;
        if (toupper(*(s1++)) != toupper(*(s2++)))
            return -1;
    }

    if (*s2 != 0)
        return -1;
    return 0;
}

static int climb(token_t *t, const mascmdtree_opt_t *opt, char *errmsg,
        int suggest)
{
    int i;
    char arg_s[1024];
    long arg_i;
    const mascmdtree_opt_t *my_opt = opt;
    int arg = 0;

    t->type = MASCMDTREE_UNDEF;

    mascmdtree_token_word(arg_s, t);
    if (mascmdtree_debug)
        printf("select %s\n", arg_s);

    for (i = 0; t->type == MASCMDTREE_UNDEF; i++) {

        my_opt = opt+i;
        int type = my_opt->flags & MASCMDTREE_TYPE_MASK;
        arg = my_opt->flags & MASCMDTREE_ARGS;

        if (mascmdtree_debug)
            printf(" compare %i %s\n", type, my_opt->name);

        switch (type) {
            case MASCMDTREE_TERMINATOR:
                t->type = MASCMDTREE_TERMINATOR;
                break;

            case MASCMDTREE_SELECT:
                if ( ((my_opt->flags & MASCMDTREE_NOCASE) &&
                            nocase_cmp(arg_s, my_opt->name)==0) ||
                        strcmp(arg_s, my_opt->name)==0) {
                    t->value = (int)my_opt->user_cargo;
                    t->type = MASCMDTREE_SELECT;
                }
                break;

            case MASCMDTREE_INTEGER:
                if (get_int(arg_s, &arg_i)==0) {
                    t->value = arg_i;
                    t->type = MASCMDTREE_INTEGER;
                } else if (arg) {
                    sprintf(errmsg, "Integer argument '%s' expected.",
                            my_opt->name);
                    t->type = MASCMDTREE_TERMINATOR;
                }
                break;

            case MASCMDTREE_STRING:
                t->value = 0;
                t->type = MASCMDTREE_STRING;
                break;

            default:
                sprintf(errmsg, "Unknown menu option type!");
                t->type = MASCMDTREE_TERMINATOR;
        }
    }

    if (t->type == MASCMDTREE_TERMINATOR)
        return 0;

    // All selections should return the user cargo.
    t->data = my_opt->user_cargo;

    // Get additional args
    int count = 0;
    if (my_opt->sub_opts == NULL) {
        if (t->n > 1) {
            sprintf(errmsg, "orphaned arguments!");
            return -1;
        }
        return 1;
    }

    if (t->n > 1) {
        count = climb( t+1, my_opt->sub_opts, errmsg, suggest);
    }

    if (count < 0)
        return count - 1;

    if (count == 0 && t->n > 1) {
        errmsg += sprintf(errmsg, "%s expects argument from ", arg_s);
        errmsg += mascmdtree_list(errmsg, my_opt->sub_opts, "[ ",
                " ", "]");
        return -2; // Biased so that missing arg position is well-represented.
    }

    // Arg counts checking...
    if (count < my_opt->min_args) {
        if (my_opt->sub_opts == NULL ||
                (my_opt->sub_opts->flags & MASCMDTREE_ARGS)==0 ) {
            sprintf(errmsg, "%s expects at least %i arguments",
                    arg_s, my_opt->min_args);
        } else {
            errmsg += sprintf(errmsg, "%s takes arguments [", arg_s);
            errmsg += list_args(errmsg, my_opt->sub_opts);
            errmsg += sprintf(errmsg, " ]");
        }
        return -1;
    }
    if (my_opt->max_args >=0 && count > my_opt->max_args) {
        sprintf(errmsg, "%s expects at most %i arguments",
                arg_s, my_opt->max_args);
        return -1;
    }

    return count + 1;
}


/* These aren't different. Hm. */

int mascmdtree_suggest( token_t *t, const mascmdtree_opt_t *opt, char *errmsg )
{
    return climb(t, opt, errmsg, 1);
}

int mascmdtree_select( token_t *t, const mascmdtree_opt_t *opt, char *errmsg )
{
    return climb(t, opt, errmsg, 0);
}

static int get_int(char *card, long *card_id) {
    char *last;
    errno = 0;

    if (card==NULL)
        return -1;

    *card_id = strtol(card, &last, 0);

    if (errno!=0 || *last != 0)
        return -1;

    return 0;
}


int mascmdtree_token_word( char *word, token_t *tset )
{
    strncpy( word, tset->line + tset->start,
            tset->stop - tset->start );
    word[tset->stop-tset->start] = 0;

    return 0;
}

int mascmdtree_tokenize(token_t *t, char *line, int max_tokens)
{
    if (t == NULL)
        return -1;
    if (line == NULL)
        return -2;

    int idx = 0;
    int n = 0;
    int overflow = 0;

    while (line[idx] != 0 && n < max_tokens) {
        while ( index(MASCMDTREE_WHITESPACE, line[idx]) != NULL &&
                line[idx] != 0)
        {
            idx++;
        }

        if (line[idx] == 0)
            break;

        t[n].start = idx;

        while ( index(MASCMDTREE_WHITESPACE, line[idx]) == NULL &&
                line[idx] != 0 )
        {
            idx++;
        }

        t[n++].stop = idx;
    }
    if (line[idx] != 0)
        overflow = 1;

    for (idx=0; idx<n; idx++ ) {
        t[idx].n = n-idx;
        t[idx].line = line;
    }

    if (mascmdtree_debug) {
        printf("%i %s\n", t->n, t->line);
        char word[128];
        int i;
        for (i=0; i<t->n; i++) {
            mascmdtree_token_word(word, t+i);
            printf("%i %i %s\n", t->start, t->stop, word);
        }
    }

    if (overflow)
        return -2;

    return 0;
}

int match_menu( mascmdtree_token_t *t, const mascmdtree_opt_t *menu )
{
    int index;
    char arg[1024];
    long argi;

    if (t== NULL || t->line == NULL)
        return -1;

    mascmdtree_token_word(arg, t);

    for (index = 0; !(menu[index].flags & MASCMDTREE_TERMINATOR); index++) {

        const mascmdtree_opt_t* opt = menu+index;

        switch (opt->flags & MASCMDTREE_TYPE_MASK) {
            case MASCMDTREE_SELECT:
                if ( ((opt->flags & MASCMDTREE_NOCASE) &&
                            nocase_cmp(arg, opt->name)==0) ||
                        strcmp(arg, opt->name)==0) {
                    t->value = (int)opt->user_cargo;
                    t->data = opt->user_cargo;
                    t->type = MASCMDTREE_SELECT;
                    return index;
                }
                break;

            case MASCMDTREE_INTEGER:
                if (get_int(arg, &argi)==0) {
                    t->value = argi;
                    t->data = opt->user_cargo;
                    t->type = MASCMDTREE_INTEGER;
                    return index;
                }
        }
    }
    return 0;
}
