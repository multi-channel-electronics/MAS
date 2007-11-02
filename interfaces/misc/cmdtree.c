#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>

#include "cmdtree.h"

typedef cmdtree_token_t token_t;

int cmdtree_debug = 0;

static int get_int(char *card, int *card_id);


int cmdtree_list(char *dest, const cmdtree_opt_t *opts,
		 const char *intro, const char *delim, const char *outro)
{
	int i;
	char *d = dest;
	d += sprintf(d, "%s", intro);
	for (i=0; opts[i].type != CMDTREE_TERMINATOR; i++) {
		switch(opts[i].type) {
		case CMDTREE_SELECT:
			d += sprintf(d, "%s%s", opts[i].name, delim);
			break;
			
		case CMDTREE_INTEGER:
			d += sprintf(d, "(int)%s", delim);
			break;
			
		case CMDTREE_STRING:
			d += sprintf(d, "(string)%s", delim);
			break;
				
		default:
			d += sprintf(d, "(other)%s", delim);
		}
	}
	d += sprintf(d, "%s", outro);
	return d-dest;
}


int climb( token_t *t, const cmdtree_opt_t *opt, char *errmsg, int suggest )
{
	int i;
	char arg_s[1024];
	int  arg_i;
	const cmdtree_opt_t *my_opt = opt;

	t->type = CMDTREE_UNDEF;

	cmdtree_token_word(arg_s, t);
	if (cmdtree_debug) printf("select %s\n", arg_s);
	
	for (i = 0; t->type==CMDTREE_UNDEF; i++) {

		my_opt = opt+i;

		if (cmdtree_debug) printf(" compare %i %s\n", my_opt->type, my_opt->name);
		switch (my_opt->type) {

		case CMDTREE_TERMINATOR:
			t->type = CMDTREE_TERMINATOR;
			break;
			
		case CMDTREE_SELECT:
			if (strcmp(arg_s, my_opt->name)==0) {
				t->value = my_opt->user_cargo;  // We either store this, or i.
				t->type = CMDTREE_SELECT;
			}
			break;
			
		case CMDTREE_INTEGER:
			if (get_int(arg_s, &arg_i)==0) {
				t->value = arg_i;
				t->type = CMDTREE_INTEGER;
			}
			break;

		case CMDTREE_STRING:
			t->value = 0;
			t->type = CMDTREE_STRING;
			break;

		default:
			sprintf(errmsg, "Unknown menu option type!");
			t->type = CMDTREE_TERMINATOR;
		}
	}

	if (t->type == CMDTREE_TERMINATOR)
		return 0;

	// Get additional args
	int count = 0;
	if (my_opt==NULL) {
		if (t->n > 1) {
			sprintf(errmsg, "orphaned arguments!");
			return -1;
		}
		return 0;
	}
	
	if (t->n > 1) {
		count = climb( t+1, my_opt->sub_opts, errmsg, suggest);
	} 

	if (count < 0) return count - 1;

	if (count == 0 && t->n > 1) {
		errmsg += sprintf(errmsg, "%s expects argument from ", arg_s);
		errmsg += cmdtree_list(errmsg, my_opt->sub_opts, "[ ",
				       " ", "]");
		return -2; // Biased so that missing arg position is well-represented.
	}

	// Arg counts checking...
	if (count < my_opt->min_args) {
		sprintf(errmsg, "%s expects at least %i arguments",
			arg_s, my_opt->min_args);
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

int cmdtree_suggest( token_t *t, const cmdtree_opt_t *opt, char *errmsg )
{
	return climb(t, opt, errmsg, 1);
}

int cmdtree_select( token_t *t, const cmdtree_opt_t *opt, char *errmsg )
{
	return climb(t, opt, errmsg, 0);
}


int get_int(char *card, int *card_id) {
	char *last;
	errno = 0;
	if (card==NULL) return -1;
	*card_id = strtol(card, &last, 0);
	if (errno!=0 || *last != 0) return -1;
	return 0;
}


int cmdtree_token_word( char *word, token_t *tset )
{
	strncpy( word, tset->line + tset->start,
		 tset->stop - tset->start );
	word[tset->stop-tset->start] = 0;

	return 0;
}

int cmdtree_tokenize(token_t *t, char *line, int max_tokens) 
{
	if (t==NULL) return -1;
	if (line==NULL) return -2;
	
	int idx = 0;
	int n = 0;

	while (line[idx] != 0 && n < max_tokens) {
		while ( index(WHITESPACE, line[idx]) != NULL &&
			line[idx] != 0) idx++;

		if (line[idx] == 0) break;
		
		t[n].start = idx;
		
		while ( index(WHITESPACE, line[idx]) == NULL && 
			line[idx] != 0 ) idx++;
		
		t[n++].stop = idx;
	}

	for (idx=0; idx<n; idx++ ) {
		t[idx].n = n-idx;
		t[idx].line = line;
	}

	if (cmdtree_debug) {
		printf("%i %s\n", t->n, t->line);
		char word[128];
		int i;
		for (i=0; i<t->n; i++) {
			cmdtree_token_word(word, t+i);
			printf("%i %i %s\n", t->start, t->stop, word);
		}
	}

	if (n >= max_tokens) return -1;

	return 0;		
}