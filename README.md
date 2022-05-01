# `msh` - the `M`ini-`SH`ell

This is the largest assignment in the class: to implement a mini-shell!
Though we'll be simplifying typical shell design, you'll implement a very real, functional shell!
It is broken into four milestones that should enable iterative development:

- M0: Parsing input (string functions, mainly `strtok` and `strtok_r`) -- the most code is in this milestone.
- M1: Executing pipelines (`fork`, `exec`, `dup`, `close`, `wait`) and builtin commands (`getenv`, `exit`) -- relatively straightforward, but will challenge your understanding of how to use these functions.
- M2: Job control (`sigaction`, `waitpid`, `kill`) -- conceptually the most difficult.
- M3: File redirection (`open`, `lseek`, `close`) -- straightforward and not too much work.

We are not implementing a [full, traditional shell](https://www.gnu.org/software/libc/manual/html_node/Implementing-a-Shell.html).
For example, we won't use process groups, and support full terminal support to run the likes of `nano`, `emacs`, `vim`, or `top`, nor are we implementing wildcards (`*`) and strings (`""` nor `''`).
Instead, we'll support the core of a shell: terminal input and output, pipelines, output redirection, and shell control over individual processes.

## Shell Design

There are many ways to implement a shell.
They are motivated by the requirements of

- Parsing inputs,
- Enabling program composition into pipelines,
- The need for the shell to `wait` (or `waitpid`) on children,
- The interactions between the shell and children via signals (i.e. terminating them),
- Enabling background computation,
- The interactions between the terminal and the shell (keyboard input and `cntl-c`/`cntl-z`), and
- The redirection of child output to files.

We suggest the following structure for the outer input loop (codified in `msh_main.c`).

```python
def msh_main_loop():
	while True:
		input = get_user_input()
		sequence = parse_input(input)
		while pipeline = sequence.get_first():
			pipeline.execute()
```

### Executing Pipelines

When the `execute` logic returns, there are no foreground tasks.
Either they have excited, or the foreground task was put into the background.
This means that either the shell decided to not block `wait`ing for the pipeline, or that the `wait` calls have returned, indicating that the foreground commands have exited.

```python
# Milestone 1 (M1), unless otherwise noted:
def execute(self): # self = pipeline
	# execute all commands in the pipeline, thus uses
	# fork, exec, pipe, and dup (or dup2) to execute
	# each command in the pipeline
	self.fork_and_exec()
	pipelines.add(self)  # M2: track all pipelines
	if (not self.is_background()):
		foreground = self

	# Milestone 2: We want to check to see if pipelines potentially
	# in the background have completed, thus we don't want to block
	# on the `wait` call. See the discussion on foreground and
	# background pipelines below. To do this:
	#
	# Wait for all pipelines using `waitpid(pid, &status, WNOHANG)`
	# to avoid blocking. This requires you to track the pid
	# of commands in a pipeline (recall the
	# `msh_command_{get|set}data` functions), and track which
	# commands have exited. Once all commands in a pipeline have
	# exited, you can free the pipeline structure (not shown here).
	pipelines.wait_but_dont_block()
    # wait for the foreground with blocking `wait`
	foreground.wait()
	# have to check if the reason we returned was due to
	# a signal (below), which happens if wait returns `-1` and
	# `errno == EINTR`.

# Milestone 2: handling the signals
def signal_handler():
	# The signal should cause the wait to return for
	# any foreground tasks
	if cntl_c:
		# send SIGTERM to all foreground processes
	if cntl_z:
		# lets put the foreground task into the background
		foreground = None
```

The global `pipelines` data-structure tracks all active pipelines and is used to formulate output of the `jobs` command, and is accessed using the `fg` command.

Instead of using non-blocking wait (with `waitpid(pid, &status, WNOHANG`), you can instead of `SIGCHLD` to receive notifications of children exiting, and only wait on them and deallocate pipelines at that point.
Signals are hard to think about, thus why I suggest the structure above.
Either way, you need to reap the children, even those running in the background.

```python
def fork_and_exec(self): # self = pipeline
	# The following assumes that we only have two
	# commands, for example `ps aux | grep gparmer`.
	# You need to generalize this to up to
	# `MSH_MAXCMNDS` commands, so will need to change
	# the structure of this quite a bit (i.e. add a
	# loop, etc...)
	p = pipe()
	if fork() == 0:   # child
		# 1. Close the STDOUT
		# 2. Dup the write end of the pipe into STDOUT
		# 3. Close both of the pipe's descriptors
		execvp(self.commands[0], ...)
	if fork() == 0:   # child
		# 1. Close the STDIN
		# 2. Dup the read end of the pipe into STDIN
		# 3. Close both of the pipe's descriptors
		execvp(self.commands[1], ...)
	# parent returns
```

Note that you must ensure that the newly executing programs only have descriptors for `STDIN_FILENO`, `STDOUT_FILENO`, and `STDERR_FILENO`.
The rest should be `close`d.

### Foreground and Background Pipelines

This outer structure in the `msh_main_loop` above essentially sends a succession of pipelines to be executed.
Pipelines can be executed in the *foreground* or the *background*^[We will *not* support suspending commands (using `SIGSTOP`, and later waking using `SIGCONT`), and will instead only support tasks running in the foreground and background.].
Foreground pipelines are the default.
A foreground pipeline has three key properties:

1. it can be interacted with on the keyboard,
2. it prevents the shell prompt from being presented, and
3. it can be interacted with using keyboard shortcuts, for example, `cntl-c` to terminate the pipeline or `cntl-z` to put the pipeline into the background.

The core of how these are enabled is that the shell `wait`s (or `waitpid`s) for the foreground processes, thus enabling them to interact with the keyboard.

Background tasks are still executing (unlike in traditional shells that might suspend tasks with `cntl-z`), but can only be interacted with using a few builtin commands.

1. `jobs` which lists all of the background pipelines, and
2. `fg N` in which pipeline `N` is brought to the foreground.

As background tasks don't require terminal input, there can be multiple/many executing at any point in time.
A pipeline that terminates with a `&` is run in the background immediately.

### Exiting the Shell

- `exit` - builtin command to exit.
- an empty input must exit the shell (this is necessary for the automatic tests)

## Parsing and a Specification of Inputs

The string inputs on the command line include the following operators.
See `msh.h` for the description of the referenced errors.

- `;` - the sequence operator separates pipelines.
    The pipeline preceding the `;` must finish execution before the pipeline after is executed.
    Empty pipelines before or after the `;` are allowed (indicating "there is no command").
	The last pipeline is the one after the last `;`.
	When the last pipeline in a sequence finishes (all its commands have `exit`ed), the user has the ability to type more inputs.
	Errors include `MSH_ERR_SEQ_REDIR_OR_BACKGROUND_MISSING_CMD`.
- `|` -  the pipe operator to redirect output from the previous command to the input of the next.
    Errors include `MSH_ERR_PIPE_MISSING_CMD`, `MSH_ERR_TOO_MANY_CMDS`.
- `&` - the background operator is the last non-space character in a pipeline, and indicates that this pipeline should be run in the background.
    Pipelines run in the background enable the next pipeline in the sequence to execute before the previous one that executes in the background is finished, or if the background pipeline is the last, the shell returns to take additional user input.
	Errors include `MSH_ERR_MISUSED_BACKGROUND`.
- `1>`, `1>>`, `2>`, and `2>>` - are redirection operators requesting that the standard output, file descriptor `1`,  should be output to a file, or that the standard error, file descriptor `2`, should be output a file.
    The file name must follow these operators, and redirection operators must be last in a command aside from the optional `&` (they must follow the program and the arguments).
    The `>` variants will delete the specified file before outputting into it, while the `>>` variants will append to the file.
	In either case, if the file does not already exist, it will be created.
	Redirections of the standard output stream are *only allowed*
	Errors include `MSH_ERR_MULT_REDIRECTIONS`, `MSH_ERR_REDIRECTED_TO_TOO_MANY_FILES`, and `MSH_ERR_NO_REDIR_FILE`.

Each operator must be surrounded by spaces unless at the end of the input.
Each pipeline is a set of commands that includes a *program* to execute, and optionally:

1. a set of arguments to that program, and then, optionally,
2. redirection operators with their associated files.

Potential errors include `MSH_ERR_TOO_MANY_ARGS`, `MSH_ERR_NO_EXEC_PROG`.

### Assumptions

To simplify your allocations and tracking, you can assume:

```c
/* Maximum number of background pipelines */
#define MSH_MAXBACKGROUND 16
/* each command can have MSH_MAXARGS or fewer arguments */
#define MSH_MAXARGS  16
/* each pipeline has MSH_MAXCMNDS or fewer commands */
#define MSH_MAXCMNDS 16
```

These are defined in `msh.h`.
Make sure to use the macros, not the constant values (i.e. use `MSH_MAXARGS`, not `16` in your code).
Given these assumptions, you might make your `struct msh_pipeline` hold a fixed array of `MSH_MAXCMNDS` `struct msh_command`s.
As such, these assumptions are meant to simply your allocations, should you choose to use internal array allocations.

## Code Organization

As always, the tests are in `tests/`.
They are explained further in the specific milestone.
The `msh_main.c` is provided for you, and you should not modify it unless you do the extra credit (see the end of this file).
The `mshparse/` directory holds the files necessary and specific to milestones 0, all of which will be built into a library.
You can add additional `.c` files in the same directory this `README.md` for your implementation.
We've started you off with `msh_execute.c`.
You should not modify the `mshparse/msh_parse.h` nor `msh.h` files.

## Milestone 0: Parsing Sequences, Pipelines, and Commands

This milestone will focus on parsing the shell's input.
The strings entered as input into the shell require pulling them apart into

1. different pipelines,
2. the commands in the pipelines, and
3. the commands and arguments.

You'll see in `mshparse/msh_parse.h` that there are corresponding structures:

1. `struct sequence` which is a set of pipelines (one to be executed after the other),
2. `struct pipeline` which is a set of commands whereby the output of the previous is piped to the input of the next, and
3. `struct command` which is a program or builtin command followed by the arguments to pass to that program/command.

For this milestone, no commands will be *executed* in the shell.
Instead, you'll take the input string, and generate the data-structures that correspond to the input's contents.

### Examples

- `ls` - a single command run immediately in the foreground.
- `ls -l` - a command with an argument.
- `ls -1 | grep msh` - a pipeline with two commands.
- `ls | grep msh ; cd .. ; ls | grep msh` -
    A sequence of *three* pipelines, the first and the third having two commands.
	The `grep` commands have a single argument.

### Tests

Tests for milestone 0 can be found in `tests/m0_*`.
We will expand on these tests in our grading.

The valgrind tests for *all milestones in this homework* will give you 15% credit while the functional tests will give 85%.
This is not to downplay the importance of valgrind tests, rather to allow you to focus on getting everything functionally sound.

### Assumptions

You can make the following assumptions about the pipelines passed in:

- Whitespace will include only spaces (not tabs or newlines).
- You do *not* need to consider `&` nor redirection operators for this milestone.
    If you complete them now, you'll save yourself future time, but they can be delayed.

### Parsing Hints

When parsing the command line, I urge you to consider the following algorithm (likely with `strtok_r`):

- Break up the string into different pipelines (separated by `;`).
    Challenges to consider here are figuring out how many pipelines there are, and detecting the `&` per pipeline.
- Break up the string into various, pipeline-separated (`|`) commands.
- Break up the command into the program, the arguments, and the file redirections.

You'll have to read `man` pages to understand string parsing functions.
Don't forget to look at the "See Also" section at the bottom of each `man` page.

Some helpful functions include:

- `strlen`
- `strtok` & `strtok_r`
- `strstr`
- `isspace`

## Milestone 1: Executing Commands and Pipelines

For this milestone, you'll implement a first version of the shell that supports pipes.
This means that you'll need to actually execute the commands, passing to them their arguments, and set up the pipes and descriptors to support pipelines.
Pipes must be set up as follows: the standard output the command on the left of the `|` goes to a `pipe`, and the standard input of the command on the right.

For example, the command `a b c d` would attempt to execute a program called `a`, passing it arguments `b`, `c`, and `d`.
The commands `a b | c d` execute programs `a` and `c` passing them `b` and `d`, respectively, and the output from `a` is a pipe that provides the input for `c`.
This is, of course, meant to strongly mimic how normal shells work.

Additionally, you'll support a number of *builtin commands*.
These are commands that do not result in program execution.
Instead they are simply defined by logic in the shell.
You must support the following builtin commands:

- `cd` - change the current working directory.
	This takes a *single* argument, which is the directory to switch to.
    This must support the ability to change to a relative directory paths (e.g. `cd ..`, or `cd blah/`), absolute paths (e.g. `cd /home/gparmer`, or `cd /proc/`), and paths relative to our home directory (e.g. `cd ~` to switch to the home directory, and `cd ~/blahblah/` to switch into the `blahblah` directory in our home directory.)
	To support `~`, you'll need to access environment variables to understand the location of the home directory.
	An example:

	```
	> pwd
	/home/gparmer/tmp
	> ls
	hw.c
	> cd ~
	> pwd
	/home/gparmer
	> ls
	tmp
	> cd ~/tmp
	> pwd
	/home/gparmer/tmp
	```

	Experiment with your shell to understand this.
	We will *not* support the general use of `~` in arbitrary commands, instead only supporting it in `cd`.
- `exit` - exit from the shell.
	As this milestone does not support background computation, no pipelines should be executing when we exit from the shell.

To exit from the shell *before you implement `exit`*, simply type an empty command, or `cntl-d` (hold the control button, and press "d").
You *cannot* change this behavior.
The testing harness requires that an empty command or a `cntl-d` exits the shell.
This is the default behavior of `msh_main.c`.

### Tests

Tests for milestone 1 can be found in `tests/m1_*`.
These files include the first line that is the input, and the rest of the file that is the expected output.
To test your implementation, you can look at these files and use the given input in your implementation:

```
$ head -n 1 tests/m1_01*
echo hello
$ echo "echo hello" | ./msh
hello
```

The following milestones (for each later milestone `N`) has its tests in `tests/mN_*`.
We will expand on these tests in our grading.

## Milestone 2: Job Control

This milestone will build on your previous support by adding "job control".
This requires the use of signals to coordinate between the shell, user, and children, and a set of builtin commands to control pipelines (which are synonymous with "job").

When a pipeline is executing in the *foreground*, the shell is `wait`ing for it complete, thus will not receive additional inputs.
A pipeline is executed in the foreground by default.
If the `&` operator is provided at the end of a pipeline, it is executed in the background.
A foreground pipeline can be

1. *suspended* with `cntl-z` (that is, holding "control" and pressing "z") at which point the shell can receive input again, or
2. *terminated* with `cntl-c`.

A suspended pipeline can be placed into the background with the `bg` command or into the foreground with `fg`.

Signals are used to suspend, continue, and terminate pipelines.
Signals of interest include the following^[See `man 7 signal` for more information.]

- `SIGINT` - sent with `cntl-c` to the shell
- `SIGTSTP` - sent with `cntl-z` to the shell
- `SIGTERM` - sent to the pipeline processes, they will terminate

There are decent examples for [similar commands and signals in bash](https://superuser.com/questions/262942/whats-different-between-ctrlz-and-ctrlc-in-unix-command-line).

You should support three additional builtin commands:

1. `jobs` - list all of the current background and suspended pipelines, along with an id, indexed starting at `0`, with pipelines created earlier having lower indices.
2. `fg N` - execute the background or suspended pipeline with id `N` in the foreground.

```
> ./sleep 10 &
> jobs
[0] ./sleep 10
> ./sleep 20
// no interaction here as sleep 20 is waiting for a signal
cntl-z
> jobs
[0] ./sleep 10
[1] ./sleep 20
> fg 0
// no interaction here as we're `wait`ing on a `sleep 10`ed program
cntl-c
// this terminates the first ./sleep 10 command
> jobs
[0] ./sleep 20
> fg 0
// back in ./sleep 20, `wait`ing for it to finish
cntl-c
// terminate this ./sleep 20 command as well
> jobs
>
```

Job control here is a simplified version of what `bash` provides, so feel free to [read more context](https://www.gnu.org/software/bash/manual/html_node/Job-Control-Basics.html).
Your format for the output of `jobs` must match what is above.

Note that most tests only require tracking a *single* background pipeline.
Please start out with the simple case of one tracked background pipeline!

### Examples

- `sleep 10 &` - a single command, an argument, and to be run in the background.
- `sleep 10 & ; ls ; sleep 9` -
    Three commands, the first `sleep` runs in the background, thus the shell immediately executes the `ls`, then the last `sleep` which is run in the foreground, thus maintains the command line for `9` seconds.
- `sleep 10 ; ls ; sleep 9` -
    Since the first `sleep` is run in the foreground, the shell waits for `10` seconds before proceeding to the latter commands.
	If the user types `cntl-z` while the first sleep is executing, it will be moved to the background, thus the `ls` is immediately executed.

### Hints

You likely want your `cntl-z` and `cntl-c` to be able to wake your shell up from a `wait`.
If this is what you want, then you do *not* want to pass `SA_RESTART` as part of the `struct sigaction` to `sigaction`.

When you start defining your own signal handlers, you might end up in a situation where you cannot "control-c" out of your shell even though you want to!
But if you want to "kill" your program, how do you do it?

```
$ ps aux | grep msh
```

This should let you find the process id of your shell, and you can send it the "kill" signal, which non-optionally terminates the shell.
If the pid is `1234`,

```
$ kill -9 1234
```

The `-9` here is the `9th` signal, which you can see from `man 7 signal` is `SIGKILL`.

## Milestone 3: File Redirection

In this (final) milestone, you'll add the ability to redirect standard input and standard error to files.
This requires implementing the redirection operators introduced earlier.
After a program and its arguments, a command can include a redirection of standard error to a file, then either

- a redirection of standard output via a pipe (as implemented in Milestone 1),
- a redirection of standard output to a file, or
- no redirection for standard output, thus sending its output to the command line.

### Examples

- `echo hi 2> err.output | cat 2> caterrs.output` - This will run `echo`, redirecting its standard error to the `err.output` file, and redirect its standard output to the pipeline, and also redirect the errors for `cat` to a file.
    The standard output of the `cat` goes, as normal, to the terminal.
- `ls | grep msh 1>> output` - The last command in a pipeline can redirect its output to a file, in this case `grep`'s output is *appended* to output.

## Extra Credit

This extra credit is due by the *last* milestone's deadline (M3).
This is worth 30% extra credit (i.e. an additional 30% credit on top of a single milestone).

Use the `ptrie` data-structure to perform autocompletion in two key ways.
Each of these that you implement will get you a portion of extra credit.

1. Autocomplete based on past inputs. If you entered a sequence previously, it becomes an autocompletion.
2. Use the `PATH` environmental variable to find all of the directories that are used to look for programs to execute (when you use `execvp`, it uses this variable), and autocomplete any of these programs.

To do this, you must use `linenoise`'s autocompletion/hint functionality, which you'll need to read about in `ln/README.markdown`.
Specifically, look into the following functions, both of which must be used:
```
linenoiseSetCompletionCallback(...);
linenoiseSetHintsCallback(...);
```
The hints are used to "suggest" a completion, while the completions are used to autocomplete upon pressing `<TAB>` in the shell.

We will *not* test for this extra credit automatically.
Instead, you must add an `EXTRA_CREDIT_STATUS.md` file with contents `attempted`.
Only if you do so will we grade the extra credit.

We'll provide a `ptrie.a` library for your use if your `ptrie` library doesn't work.

## Submission

Remember that you must use the `util/final_commit.sh` script to submit each milestone.
You can use it multiple times, and we'll use the closest usage before the deadline.
