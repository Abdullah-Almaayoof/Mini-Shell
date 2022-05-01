#pragma once
#include <unistd.h>

struct job_list {
	struct job_list* next;
	struct pid_list* pids;
	char bg; // Boolean for background pipeline
	char* cmds; // Commands running in this job
	int id; // ID of the job
};

// Holds list of pids we must wait for
struct pid_list {
	struct pid_list* next; // Next link in pid_list
	pid_t pid;
};

struct msh_command {
	char * args[MSH_MAXARGS + 1]; // Arguments of the command
	int argc; // Count of arguments
	char * redirfile[2]; // File names of any redirect
};

struct msh_pipeline {
	struct msh_pipeline* next; // Next link in pipeline list
	struct msh_command cmds[MSH_MAXCMNDS + 1]; // Extra command as marker for final
	int pipes[MSH_MAXCMNDS * 2 - 2]; // Pipe fd's for pipeline
	int count; // Index of next command
	char bg; // Boolean for background pipeline
};