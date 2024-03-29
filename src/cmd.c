// SPDX-License-Identifier: BSD-3-Clause

#include <sys/types.h>
#include <sys/stat.h>
#include <sys/wait.h>

#include <fcntl.h>
#include <unistd.h>
#include <string.h>

#include "cmd.h"
#include "utils.h"

#define READ		0
#define WRITE		1

/**
 * Internal change-directory command.
 */
static bool shell_cd(word_t *dir)
{
	// sanity check
	if (dir == NULL || dir->string == NULL)
		return false;

	if (chdir(dir->string) != 0)
		return false;
	return true;
}

/**
 * Internal exit/quit command.
 */
static int shell_exit(void)
{
	/* TODO: Execute exit/quit. */
	exit(0);
	return SHELL_EXIT; /* TODO: Replace with actual exit code. */
}

/**
 * Perform redirections, for cd or for rest of commands
 */

static void doRedirection(simple_command_t *s, int execute_cd, char *cwd)
{
	// < : redirection to stdin
	if (s->in != NULL) {
		int fd = -1;
		char input_path[1026];

		// if cd, path should be the path from where cd was called
		if (execute_cd)
			snprintf(input_path, sizeof(input_path), "%s/%s", cwd, s->in->string);
		else
			strcpy(input_path, s->in->string);

		// if there are environment variables, expand them and add to path
		if (s->in->next_part != NULL)
			strcat(input_path, get_word(s->in->next_part));

		fd = open(input_path, O_RDONLY);

		DIE(fd < 0, "open stdin");
		dup2(fd, STDIN_FILENO);
		close(fd);
	}

	// &> : redirect stdout and stderr
	if (s->out != NULL && s->err != NULL) {
		char output_path[1026];
		char error_path[1026];

		// if cd, path should be the path from where cd was called
		if (execute_cd) {
			snprintf(output_path, sizeof(output_path), "%s/%s", cwd, s->out->string);
			snprintf(error_path, sizeof(error_path), "%s/%s", cwd, s->err->string);
		} else {
			strcpy(output_path, s->out->string);
			strcpy(error_path, s->err->string);
		}

		// if there are any variables, expand them and add to paths
		if (s->out->next_part != NULL)
			strcat(output_path, get_word(s->out->next_part));

		if (s->err->next_part != NULL)
			strcat(error_path, get_word(s->err->next_part));


		int f_out = open(output_path, O_WRONLY | O_CREAT | O_APPEND, 0644);
		int f_err = open(error_path, O_WRONLY | O_CREAT | O_TRUNC, 0644);

		DIE(f_out < 0, "open stdout");
		DIE(f_err < 0, "open stderr");

		dup2(f_out, STDOUT_FILENO);
		dup2(f_err, STDERR_FILENO);

		close(f_out);
		close(f_err);

	} else { // >, 2>, >>, 2>>
		if (s->out != NULL) {
			int fd = -1;
			char output_path[1026];

			if (execute_cd)
				snprintf(output_path, sizeof(output_path), "%s/%s", cwd, s->out->string);
			else
				strcpy(output_path, s->out->string);

			if (s->out->next_part != NULL)
				strcat(output_path, get_word(s->out->next_part));


			if (s->io_flags == IO_REGULAR) // no append
				fd = open(output_path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
			else if (s->io_flags == IO_OUT_APPEND) // append
				fd = open(output_path, O_WRONLY | O_CREAT | O_APPEND, 0644);


			DIE(fd < 0, "open stdout");
			dup2(fd, STDOUT_FILENO);
			close(fd);
		}

		if (s->err != NULL) {
			int fd = -1;
			char error_path[1026];

			if (execute_cd)
				snprintf(error_path, sizeof(error_path), "%s/%s", cwd, s->err->string);
			else
				strcpy(error_path, s->err->string);

			if (s->err->next_part != NULL)
				strcat(error_path, get_word(s->err->next_part));


			if (s->io_flags == IO_REGULAR) // no append
				fd = open(error_path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
			else if (s->io_flags == IO_ERR_APPEND) // append
				fd = open(error_path, O_WRONLY | O_CREAT | O_APPEND, 0644);

			DIE(fd < 0, "open");
			dup2(fd, STDERR_FILENO);
			close(fd);
		}
	}
}

/**
 * Parse a simple command (internal, environment variable assignment,
 * external command).
 */

static int parse_simple(simple_command_t *s, int level, command_t *father)
{
	bool execute_cd = false;
	int result = false;
	char cwd[1024]; // current working directory

	/* TODO: Sanity checks. */
	if (s == NULL)
		return false;


	// get current working directory
	if (getcwd(cwd, sizeof(cwd)) == NULL)
		return false;


	/* TODO: If builtin command, execute the command. */
	if (strcmp(s->verb->string, "cd") == 0) {

		// there are no params or more than one
		if (s->params == NULL || s->params->next_part != NULL)
			return false;

		execute_cd = true;
		// auxiliary file descriptors used for restoring
		int orig_stdout = dup(STDOUT_FILENO);
		int orig_stdin = dup(STDIN_FILENO);
		int orig_stderr = dup(STDERR_FILENO);

		doRedirection(s, execute_cd, cwd); // perform redirections in cwd

		// restore original file descriptors
		dup2(orig_stdout, STDOUT_FILENO);
		dup2(orig_stdin, STDIN_FILENO);
		dup2(orig_stderr, STDERR_FILENO);

		// close auxiliary file descriptors
		close(orig_stdout);
		close(orig_stdin);
		close(orig_stderr);

		return shell_cd(s->params);
	}

	if (strcmp(s->verb->string, "exit") == 0 || strcmp(s->verb->string, "quit") == 0)
		return shell_exit();


	if (strcmp(s->verb->string, "false") == 0)
		return false;


	if (strcmp(s->verb->string, "true") == 0)
		return true;

	// check if is environment variable
	if (s->verb->next_part != NULL) {
		char *varvalue = get_word(s->verb->next_part->next_part); // get value

		setenv(s->verb->string, varvalue, 1); // set variable
		free(varvalue);
		return true;
	}

	pid_t pid = fork();

	if (pid == -1)
		return false;
	else if (pid == 0) { // child

		// auxiliary file descriptors used for restoring
		int orig_stdout = dup(STDOUT_FILENO);
		int orig_stdin = dup(STDIN_FILENO);
		int orig_stderr = dup(STDERR_FILENO);

		int argc;
		char **argv = get_argv(s, &argc); // command arguments

		doRedirection(s, execute_cd, cwd); // perform redirections

		// execute command
		if (execvp(argv[0], argv) == -1)
			exit(0);

		for (int i = 0; i <= argc; ++i)
			free(argv[i]);
		free(argv);

		// restore original file descriptors
		dup2(orig_stdout, STDOUT_FILENO);
		dup2(orig_stdin, STDIN_FILENO);
		dup2(orig_stderr, STDERR_FILENO);

		// close auxiliary file descriptors
		close(orig_stdout);
		close(orig_stdin);
		close(orig_stderr);

	} else {
		int status;

		waitpid(pid, &status, 0);
		if (WEXITSTATUS(status) == 1)
			return false;
		else
			return true;
	}

	return result; /* TODO: Replace with actual exit status. */
}

/**
 * Process two commands in parallel, by creating two children.
 */
static bool run_in_parallel(command_t *cmd1, command_t *cmd2, int level,
		command_t *father)
{
	/* TODO: Execute cmd1 and cmd2 simultaneously. */
	pid_t pidFirst, pidSecond;
	int status1, status2;

	pidFirst = fork(); // create first child

	if (pidFirst < 0)
		return false;
	else if (pidFirst == 0) { // first child
		int status = parse_command(cmd1, level + 1, father);

		exit(status);
	}

	pidSecond = fork(); // create second child
	if (pidSecond < 0)
		return false;
	else if (pidSecond == 0) { // second child
		int status = parse_command(cmd2, level + 1, father);

		exit(status);
	}

	// wait for children to finish
	waitpid(pidFirst, &status1, 0);
	waitpid(pidSecond, &status2, 0);

	// if both exited with succes, return success
	if (WIFEXITED(status1) && WIFEXITED(status2))
		return true;
	return false; /* TODO: Replace with actual exit status. */
}

/**
 * Run commands by creating an anonymous pipe (cmd1 | cmd2).
 */
static bool run_on_pipe(command_t *cmd1, command_t *cmd2, int level,
		command_t *father)
{
	/* TODO: Redirect the output of cmd1 to the input of cmd2. */
	// auxiliary file descriptors used for restoring
	int orig_stdout = dup(STDOUT_FILENO);
	int orig_stdin = dup(STDIN_FILENO);
	int orig_stderr = dup(STDERR_FILENO);
	int fd[2]; // fd[0] - read, fd[1] - write
	pid_t pidFirst, pidSecond;
	int status1, status2;

	int res = pipe(fd); // create file descriptors from pipe

	DIE(res < 0, "error on pipe");

	// do something similar to run_in_parallel
	pidFirst = fork();
	if (pidFirst < 0)
		return false;
	else if (pidFirst == 0) { // first process child
		close(fd[0]); // close read head
		dup2(fd[1], STDOUT_FILENO); // point stdout to write head
		close(fd[1]); // close write head

		int status = parse_command(cmd1, level + 1, father);

		exit(status);
	}

	pidSecond = fork();
	if (pidSecond < 0)
		return false;
	else if (pidSecond == 0) { // second process child
		close(fd[1]); // close write head
		dup2(fd[0], STDIN_FILENO); // point stdin to read head
		close(fd[0]); // close read head

		int status = parse_command(cmd2, level + 1, father);

		exit(status);
	}

	close(fd[0]);
	close(fd[1]);
	waitpid(pidFirst, &status1, 0);
	waitpid(pidSecond, &status2, 0);


	// restore original file descriptors
	dup2(orig_stdout, STDOUT_FILENO);
	dup2(orig_stdin, STDIN_FILENO);
	dup2(orig_stderr, STDERR_FILENO);

	// close auxiliary file descriptors
	close(orig_stdout);
	close(orig_stdin);
	close(orig_stderr);

	return WEXITSTATUS(status2); /* TODO: Replace with actual exit status. */
}

/**
 * Parse and execute a command.
 */
int parse_command(command_t *c, int level, command_t *father)
{
	/* TODO: sanity checks */
	if (c == NULL)
		return false;

	int result = true;

	if (c->op == OP_NONE) {
		/* TODO: Execute a simple command. */
		return parse_simple(c->scmd, level + 1, c); /* TODO: Replace with actual exit code of command. */
	}

	switch (c->op) {
	case OP_SEQUENTIAL:
		/* TODO: Execute the commands one after the other. */
		parse_command(c->cmd1, level, c);
		parse_command(c->cmd2, level, c);
		break;

	case OP_PARALLEL:
		/* TODO: Execute the commands simultaneously. */
		result = run_in_parallel(c->cmd1, c->cmd2, level, c);
		break;

	case OP_CONDITIONAL_NZERO:
		/* TODO: Execute the second command only if the first one
		 * returns non zero.
		 */
		result = parse_command(c->cmd1, level, c);
		if (result == false)
			result = parse_command(c->cmd2, level, c);
		break;

	case OP_CONDITIONAL_ZERO:
		/* TODO: Execute the second command only if the first one
		 * returns zero.
		 */
		result = parse_command(c->cmd1, level, c);
		if (result == true)
			result = parse_command(c->cmd2, level, c);
		break;

	case OP_PIPE:
		/* TODO: Redirect the output of the first command to the
		 * input of the second.
		 */
		result = run_on_pipe(c->cmd1, c->cmd2, level, c);
		break;

	default:
		return SHELL_EXIT;
	}

	return result; /* TODO: Replace with actual exit code of command. */
}
