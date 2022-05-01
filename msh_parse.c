#include <msh_parse.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <sys/types.h>
#include <msh_execute.h>

struct msh_sequence {
	struct msh_pipeline* head; // Head of pipeline linked list
};

void
msh_pipeline_free(struct msh_pipeline *p)
{
	if ( p != NULL ) { // Ignore NULL pointer
		int i; 

		for ( i = 0; i <= p->count; i++ ) { // Iterate through commands 
			int j;

			for ( j = 0; j < p->cmds[i].argc; j++ ) { // Iterate through arguments
				if ( p->cmds[i].args[j] != NULL ) {
					free( p->cmds[i].args[j] ); // Free strdup tokens
				}
			}

			free( p->cmds[i].redirfile[0] ); // Free any redirfile name
			free( p->cmds[i].redirfile[1] ); // Free any redirfile name
		}

		free( p ); // Free the pipeline
	}
}

/* Free list of pids */
void msh_pid_free(struct pid_list *p) 
{
	while ( p != NULL ) { // Walk the linked list
		struct pid_list* temp = p;
		p = p->next;
		free(temp); // Free the link
	}
}

void
msh_sequence_free(struct msh_sequence *s)
{
	while ( s->head != NULL ) { // Free unexecuted pipelines
		msh_pipeline_free( msh_sequence_pipeline( s ) );
	}
	
	free(s); // Free the sequence
}

struct msh_sequence *
msh_sequence_alloc(void)
{
	return calloc( 1, sizeof( struct msh_sequence)); // allocate and clear new structure
}

char *
msh_pipeline_input(struct msh_pipeline *p)
{
	(void)p;
	return NULL;
}

msh_err_t
msh_sequence_parse(char *str0, struct msh_sequence *seq)
{
	char* str; // Hold strdup of 'str0'
	char* token; // Holds next token
	char open_pipe = 0; // Boolean for pipe error checking
	char bg_last = 0; // Boolean for & on last pipeline
	char redir_flag = 0; // Redirection control bits
	struct msh_pipeline** next = &seq->head; // where the next pipeline will hang
	seq->head = NULL; // Remove any past reference to pipelines

	if ( NULL == ( str = strdup( str0 ) ) ) { // Strdup 'str0' so strtok can write it
		return MSH_ERR_NOMEM;
	}

	for ( token = strtok( str, " " ); token != NULL; token = strtok( NULL, " " ) ) { // Find space delimited tokens
		switch ( *token ) { // Assumes '&', ';', and '|' are single character tokens
			case '&':
				if ( *next == NULL ) { // No token before '&'
					free( str );
					return MSH_ERR_MISUSED_BACKGROUND;
				}

				(*next)->bg = bg_last = 1;

				if ( open_pipe ) { // Missing token after pipe
					free( str );
					//fprintf(stderr, "parse return at %d\n", __LINE__ );
					return MSH_ERR_PIPE_MISSING_CMD;
				}

				if ( (redir_flag & 5) == 1 || (redir_flag & 10) == 2 ) { // >> and no file name
					free( str );
					return MSH_ERR_NO_REDIR_FILE;
				}

				redir_flag = 0;

				if ( *next != NULL ) { // Hang the next pipeline after this
					next = &((*next)->next); // Where to hang the next pipeline
				}

				break;
			case ';':
				if ( open_pipe ) { // Missing token after pipe
					free( str );
					//fprintf(stderr, "parse return at %d\n", __LINE__ );
					return MSH_ERR_PIPE_MISSING_CMD;
				}

				if ( (redir_flag & 5) == 1 || (redir_flag & 10) == 2 ) { // >> and no file name
					free( str );
					return MSH_ERR_NO_REDIR_FILE;
				}

				redir_flag = 0;

				if ( *next != NULL ) { // Hang the next pipeline after this
					next = &((*next)->next); // Where to hang the next pipeline
				}

				bg_last = 0;
				break;
			case '|':
				if ( *next == NULL || (*next)->cmds[(*next)->count].argc < 1 ) { // No command before pipe
					free( str );
					//fprintf(stderr, "parse return at %d\n", __LINE__ );
					return bg_last ? MSH_ERR_MISUSED_BACKGROUND : MSH_ERR_PIPE_MISSING_CMD;
				}

				if ( redir_flag & 1 ) { // >> and |
					free( str );
					return MSH_ERR_REDUNDANT_PIPE_REDIRECTION;
				}

				if ( (redir_flag & 5) == 1 || (redir_flag & 10) == 2 ) { // >> and no file name
					free( str );
					return MSH_ERR_NO_REDIR_FILE;
				}

				redir_flag = 0;
				(*next)->count++; // Advance to next command
  
				if ( (*next)->count == MSH_MAXCMNDS ) { // Checking error for too many commands
					free( str );
					//fprintf(stderr, "parse return at %d\n", __LINE__ );
					return MSH_ERR_TOO_MANY_CMDS;
				}

				open_pipe = 1; // Check for following token
				break;
			default:
				open_pipe = 0; // Following token found

				if ( *next == NULL ) { // Starting new pipeline
					if ( NULL == ( *next = calloc( 1, sizeof( struct msh_pipeline ) ) ) ) { // Allocate and clear new pipeline
						free( str );
						return MSH_ERR_NOMEM;
					}
				}

				if ( !strcmp(">>", token) || !strcmp("1>>", token) ) { // Redirect stdout
					if ( !(*next)->cmds[(*next)->count].argc ) { // No command to redirect
						free( str );
						return MSH_ERR_SEQ_REDIR_OR_BACKGROUND_MISSING_CMD;
					}

					if ( redir_flag & 1 ) { // Redirected already
						free( str );
						return MSH_ERR_MULT_REDIRECTIONS;
					}

					redir_flag |= 1; // Remember stdout redirect
					continue;
				}

				if ( redir_flag & 1 ) { // Stdout redirect
					if ( redir_flag & 4 ) { // Redirect file name already parsed
						free( str );
						return MSH_ERR_REDIRECTED_TO_TOO_MANY_FILES;
					}

					if ( NULL == ((*next)->cmds[(*next)->count].redirfile[0] = strdup(token)) ) { // Dup the redirfile name
						free( str );
						return MSH_ERR_NOMEM;
					}

					redir_flag |= 4; // Remember file name already copied
					continue;
				}

				if ( !strcmp("2>>", token) ) { // Stderr redirection
					if ( !(*next)->cmds[(*next)->count].argc ) { // No command to redirect
						free( str );
						return MSH_ERR_SEQ_REDIR_OR_BACKGROUND_MISSING_CMD;
					}
					

					if ( redir_flag & 2 ) { // Already redirected
						free( str );
						return MSH_ERR_MULT_REDIRECTIONS;
					}

					redir_flag |= 2; // Remember stderr redirection
					continue;
				}

				if ( redir_flag & 2 ) { // Stderr redirection in process
					if ( (redir_flag & 8) ) { // Already redirected to a file 
						free( str );
						return MSH_ERR_REDIRECTED_TO_TOO_MANY_FILES;
					}

					if ( NULL == ((*next)->cmds[(*next)->count].redirfile[1] = strdup(token) )) { // Copy redirection file name
						free( str );
						return MSH_ERR_NOMEM;
					}

					redir_flag |= 8; // Remember stderr redirection
					continue;
				}

				if ( redir_flag ) { // Tokens after redirection started
					free(str);
					return MSH_ERR_REDIRECTED_TO_TOO_MANY_FILES;
				}

				if ( NULL == ((*next)->cmds[(*next)->count].args[(*next)->cmds[(*next)->count].argc++] = strdup( token ) ) ) { // Duplicate the token
					free( str );
					return MSH_ERR_NOMEM;
				}
				
				if ( (*next)->cmds[(*next)->count].argc > MSH_MAXARGS ) { // Check argument count
					free( str );
					return MSH_ERR_TOO_MANY_ARGS;
				}

				break;
		} // end switch
	} // end for loop

	if ( open_pipe ) { // Open pipe at end of line
		free( str );
		//fprintf(stderr, "parse return at %d\n", __LINE__ );
		return MSH_ERR_PIPE_MISSING_CMD;
	}

	free( str ); // Free strdup'ed 'str0'
	return 0; // No error
}

struct msh_pipeline *
msh_sequence_pipeline(struct msh_sequence *s)
{
	struct msh_pipeline * ret = s->head; // Save return value

	if ( ret != NULL ) {
		s->head = ret->next; // Snap pipeline out of list
	}

	return ret;
}

struct msh_command *
msh_pipeline_command(struct msh_pipeline *p, size_t nth)
{
	return (size_t)p->count < nth ? NULL : p->cmds + nth; // Return pointer to command in array
}

int
msh_pipeline_background(struct msh_pipeline *p)
{
	return p->bg;
}

int
msh_command_final(struct msh_command *c)
{
	return c[1].argc == 0 ? 1 : 0; // Check next command for arguments
}

void
msh_command_file_outputs(struct msh_command *c, char **stdout, char **stderr)
{
	*stdout = c->redirfile[0]; // Return pointer to stdout redir
	*stderr = c->redirfile[1]; // Return pointer to stderr redir
}

char *
msh_command_program(struct msh_command *c)
{
	return c->args[0]; // Return first token equals executable
}

char **
msh_command_args(struct msh_command *c)
{
	return c->args; // Return array of arguments
}

void
msh_command_putdata(struct msh_command *c, void *data, msh_free_data_fn_t fn)
{
	(void)c;
	(void)data;
	(void)fn;
}

void *
msh_command_getdata(struct msh_command *c)
{
	(void)c;

	return NULL;
}
