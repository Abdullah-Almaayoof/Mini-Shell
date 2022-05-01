#include <msh.h>
#include <msh_parse.h>
#include <msh_execute.h>
#include <unistd.h>
#include <stdlib.h>
#include <sys/wait.h>
#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <signal.h>
#include <fcntl.h>

struct job_list* jobs = NULL;
static int id_gen = 0;

/* Print sorted jobs listing */
void msh_jobs() 
{
	struct job_list* job;
	int i;

	for ( i = 0; i < id_gen; i++ ) { // Search for jobs by id in order
		for ( job = jobs; job != NULL; job = job->next) { // Search all jobs
			if ( i == job->id ) { // Ignore if wrong id
				printf("[%d] %s\n", job->id, job->cmds);
			}
		}
	}
}

/* Find job and make it the foreground job */
void msh_fg(int j) 
{
	struct job_list* job;
	struct job_list** last = &jobs;

	for ( job = jobs; job != NULL; job = job->next) { // Search all jobs
		if ( j == job->id ) { // Found background job
			job->bg = 0; // Make it a foreground job

			if ( job != jobs ) { // Make it the first job
				*last = job->next;
				job->next = jobs;
				jobs = job;
			}

			return;
		}

		last = &job->next; // Remember where the next job hangs
	}
}

/* Create command string */
static char* make_cmds(struct msh_pipeline *p) 
{ 
	char* ret;
	int i, j;

	if ( NULL == ( ret = calloc(1, 4096) ) ) { // Create oversize buffer
		perror("calloc of job commands");
		exit(1);
	}

	for ( i = 0; i <= p->count; i++ ) { // Walk through all commands
		for ( j = 0; j < p->cmds[i].argc; j++ ) { // Walk through all arguments
			if ( j ) { // Add spaces after first argument
				strcat(ret, " ");
			}

			strcat(ret, p->cmds[i].args[j]); // Add argument to command string
		}

		if ( i < p->count ) { // Add a pipe to the command string
			strcat(ret, " | ");
		}
	}

	if ( NULL == ( ret = realloc(ret, strlen(ret) + 1) ) ) { // Resize the buffer
		perror("realloc of job commands");
		exit(1);
	}

	return ret;
}

void
msh_execute(struct msh_pipeline *p) 
{
	int i;
	pid_t pid; // Capture child pid
	struct job_list* job;
	struct pid_list* pnode;

	if ( p == NULL || (p->count == 0 && p->cmds[0].argc == 0 ) ) { // Empty pipeline
		return;
	}

	// Wait for background pids that ended
	struct job_list** jlast = &jobs;

	for ( job = jobs; job != NULL;) {
		struct pid_list ** last = &(job->pids);

		if ( ! job->bg ) { // Skip foreground job
			jlast = &(job->next); // Save where next job hangs
			job = job->next; // Move to next job
			continue;
		}

		while ( *last != NULL ) { // Walk through the pid_list of this job
			struct pid_list* pl = *last;

			switch ( waitpid( pl->pid, NULL, WNOHANG ) ) { // Check for pid termination
				case -1:
					perror( "waitpid bg" );
					exit(1);
				case 0: // Child has not exited
					last = &pl->next;
					continue;
				default: // Child has exited
					*last = pl->next; // Snap child out of list
					free( pl );
			}
		} // End while-loop

		if ( job->pids == NULL ) { // All pids have been wait'd
			*jlast = job->next; // Snap job out of list
			free(job->cmds); // Free the commands string
			free(job); // Free the job
			job = *jlast; // Move to next job
			continue;
		}

		jlast = &(job->next); // Remember where the next job hangs
		job = job->next; // Go to the next job
	} // End for-loop

	if ( NULL == ( job = calloc( 1, sizeof( struct job_list ) ) ) ) { // Make a new job for this pipeline
		perror( "calloc of job_list" );
		exit(1);
	}

	job->next = jobs; // Hang list on new job
	jobs = job; // Snap new job into list
	job->bg = p->bg; // Copy background status
	job->id = id_gen++; // Assign job id
	job->cmds = make_cmds(p); 
	
	//printf("%d %s\n", job->id, job->cmds);

	for ( i = 0; i <= p->count; i++ ) { // Create children
		int fd[2]; // Pipe fds
		int ifd; // Prior pipe input fd

		if ( ! strcmp(p->cmds[i].args[0], "jobs") ) { // Built in jobs command
			jobs = job->next; // Delete job from list
			free(job->cmds); // Free the command string
			free(job); // Free the job
			id_gen--; // Reuse id
			msh_jobs(); // Print jobs output
			continue;
		}

		if ( ! strcmp( p->cmds[i].args[0], "fg" ) ) { // Built in fg command
			int j = -1;

			if ( p->cmds[i].argc < 2 || 1 != sscanf(p->cmds[i].args[1], "%d", &j) ) {
				fprintf(stderr, "fg command with no job number\n");
			}
			
			jobs = job->next; // Delete job from list
			free(job->cmds); // Free the command string
			free(job); // Free the job
			id_gen--; // Reuse id
			msh_fg( j );
			continue;
		}

		if ( ! strcmp( p->cmds[i].args[0], "exit" ) ) { // Built in exit command
			exit(0);
		}

		if ( ! strcmp( p->cmds[i].args[0], "cd" ) ) { // Built in cd command
			char newdir[4096]; // Buffer for new working directory path

			jobs = job->next; // Delete job from list
			free(job->cmds); // Free the command string
			free(job); // Free the job
			id_gen--; // Reuse id

			if ( p->cmds[i].argc == 1 ) { // cd with no arguments
				if ( chdir( getenv( "HOME" ) ) ) { // cd to home directory
					perror("cd");
					exit(1);
				}

				continue;
			}

			switch ( p->cmds[i].args[1][0] ) { // cd varies by first character
				case '/': // Absolute path
					if ( chdir( p->cmds[i].args[1] ) ) { // cd to absolute path
						perror("cd /");
						exit(1);
					}

					continue;
				case '~': // cd home dir relative
					sprintf( newdir, "%s%s", getenv( "HOME" ), p->cmds[i].args[1] + 1 ); // Append relative path to home dir

					if ( chdir( newdir ) ) { // cd to relative path
						perror("cd ~");
						exit(1);
					}

					continue;
				case '.': // Working dir relative path
					if ( p->cmds[i].args[1][1] == '.' ) { // cd ..
						strcpy( newdir, getenv( "PWD" ) ); // Get working dir
						*strrchr( newdir, '/' ) = '\0'; // Remove last component
						sprintf( newdir + strlen( newdir ), "/%s", p->cmds[i].args[1] + 2 ); // Append relative path to parent dir
					}
					else {
						sprintf( newdir, "%s%s", getenv( "PWD" ), p->cmds[i].args[1] + 1 ); // Working dir relative path
					}

					if ( chdir( newdir ) ) { // cd to relative path
						perror("cd .");
						exit(1);
					}

					continue;
				default: // Working dir relative path
					sprintf( newdir, "%s/%s", getenv( "PWD" ), p->cmds[i].args[1] ); // Append relative path to working dir
					
					if ( chdir( newdir ) ) { // cd to relative path
						perror("cd *");
						exit(1);
					}

					continue;
			}
		} // End of cd

		if ( i < p->count ) { // Make pipes except for last command
			if ( pipe( fd ) ) { // New pipe failed
				perror( "pipe" );
				exit(1);
			}
			//fprintf(stderr, "Pipe created %d\n", i);
		}

		switch ( pid = fork() ) { // Create child and capture pid
			case -1:
				perror( "fork()" );
				exit(1);
			case 0: // Child
				if ( i < p->count ) { // Redirect fd[1]
					//fprintf( stderr, "Redirect %d for %d %s 0\n", p->cmds[i].fd[0], i, p->cmds[i].args[0] );
					if ( 0 > dup2( fd[1], 1 ) ) { // Replace stdout with pipe
						perror( "dup2(1)" );
						exit(1);
					}
					
					close( fd[0] ); // Unused pipe
					close( fd[1] ); // duplicated pipe
				}

				if ( i ) { // Redirect fd[0]
					//fprintf( stderr, "Redirect %d for %d %s 1\n", p->cmds[i].fd[1], i, p->cmds[i].args[0] );
					if ( 0 > dup2( ifd, 0 ) ) { // Replace stdin with pipe from last loop
						perror( "dup2(0)" );
						exit(1);
					}

					close( ifd ); // Duplicated pipe
				}

				if ( p->cmds[i].redirfile[0] != NULL ) { // Redirect standard out
					int fd;

					if ( 0 > ( fd = open(p->cmds[i].redirfile[0], O_APPEND | O_CREAT | O_WRONLY, 0644) ) ) { // Open redirect file to write and append
						perror( p->cmds[i].redirfile[0] );
						exit(1);
					}

					if ( 0 > dup2( fd, 1 ) ) { // Replace stdout with redirect
						perror( "dup2(1) >>" );
						exit(1);
					}

					close( fd ); // Redirect fd
				}

				if ( p->cmds[i].redirfile[1] != NULL ) { // Redirect standard error
					int fd;

					if ( 0 > ( fd = open(p->cmds[i].redirfile[1], O_APPEND | O_CREAT | O_WRONLY, 0644) ) ) { // Open redirect file to write and append
						perror( p->cmds[i].redirfile[1] );
						exit(1);
					}

					if ( 0 > dup2( fd, 2 ) ) { // Replace stderr with redirect
						perror( "dup2(2) 2>>" );
						exit(1);
					}

					close( fd ); // Redirect fd
				}

				execvp( p->cmds[i].args[0], p->cmds[i].args ); // Run the command
				perror( "execvp" );
				exit(1);
			default: // Parent
				if ( i ) { // Close saved input pipe
					close( ifd );
				}

				ifd = fd[0]; // Save input pipe
				
				if ( i < p->count ) { // Close output pipe unless last command
					close( fd[1] );
				}

				if ( NULL == ( pnode = calloc( 1, sizeof( struct pid_list ) ) ) ) { // Create new pid node
					perror( "calloc pid_list" );
					exit(1);
				}

				pnode->next = job->pids; // Hang list on new pid
				job->pids = pnode; // New pid is head of list
				pnode->pid = pid; // Record new pid
		} // The end of switch
	} // End for-loop

	msh_pipeline_free( p );

	while ( jobs != NULL && jobs->pids != NULL ) { // Wait for the foreground job pids on first job even if interrupted
		struct pid_list** last = &(jobs->pids); // Save where next pid hangs
		struct pid_list* pl = *last; // Next pid

		if ( jobs->bg ) { // This is a background pipeline, no waiting
			return;
		}

		if ( 0 > waitpid( pl->pid, NULL, 0) ) { // Wait for pid to exit
			if ( errno == EINTR ) { // Need to restart entire wait, possible ^z
				continue;
			}

			perror( "waitpid fg" );
			exit(1);
		}
		
		// Pid terminates
		*last = pl->next; // Snap pid out of list
		free( pl );

		if ( *last == NULL ) { // Loop processed all pids
			job = jobs; // Capture the first job
			jobs = job->next; // Snap first job out of list
			free(job->cmds); // Free the commands string
			free(job); // Free first job
			return;
		}
	} // End while-loop
}

void
stop_handler(int s)
{
	(void) s;
	jobs->bg = 1; // Put the foreground pipes into the background
}

void
int_handler(int s)
{
	(void) s;
	if ( jobs->bg ) { // Ignore if no foreground job
		return;
	}

	struct pid_list* pl;

	for ( pl = jobs->pids; pl != NULL; pl = pl->next ) { // Find all foreground pids still active
		if ( kill( pl->pid, SIGTERM ) && errno != ESRCH ) { // Kill the pid if still active
			perror("kill");
			exit(1);
		}
	}
}

void
msh_init(void)
{
	if ( signal( SIGTSTP, stop_handler) == SIG_ERR ) { // Install ctrl-z signal handler
		perror("Signal SIGTSTP");
		exit(1);
	}

	if ( signal( SIGINT, int_handler) == SIG_ERR ) { // Install ctrl-c signal handler
		perror("Signal SIGINT");
		exit(1);
	}

	return;
}
