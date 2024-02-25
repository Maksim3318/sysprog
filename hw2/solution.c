#include "parser.h"

#include <assert.h>
#include <stdio.h>
#include <unistd.h>
#include <string.h>
#include <stdlib.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <sys/stat.h>
#include <fcntl.h>

char **build_argv(const struct command *cmd) {
	char **argv = malloc((2 + cmd->arg_count) * sizeof(char *));
	argv[0] = cmd->exe;
	for (uint32_t i = 0; i < cmd->arg_count; ++i) {
		argv[i + 1] = cmd->args[i];
	}
	argv[cmd->arg_count + 1] = NULL;
	return argv;
}

static void execute_command_line(const struct command_line *line) {
	const struct expr *e = line->head;

	if (e->type == EXPR_TYPE_COMMAND 
		&& !strncmp(e->cmd.exe, "exit", 6)
		&& e->next == NULL) {
			int ret = 0;
			if (e->cmd.arg_count) {
				ret = atoi(e->cmd.args[0]);
			}
			exit(ret);
	}
	int dup_stdout = dup(STDOUT_FILENO);
	int dup_stdin = dup(STDIN_FILENO);
	int file = dup(STDOUT_FILENO);
	
	if (line->out_type == OUTPUT_TYPE_FILE_NEW) {
		file = open(line->out_file, O_RDWR | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
		dup2(file, STDOUT_FILENO);
	} else if (line->out_type == OUTPUT_TYPE_FILE_APPEND) {
		file = open(line->out_file, O_RDWR | O_CREAT | O_APPEND | O_CLOEXEC, 0644);
		dup2(file, STDOUT_FILENO);
	}

	// fprintf(stderr, "dup_stdin = %d\n", dup_stdin);
	// fprintf(stderr, "dup_stdout = %d\n", dup_stdout);
	// fprintf(stderr, "file = %d\n", file);

	bool in_pipe_left = false;
	bool in_pipe_right = false;
	int fd_l[2] = {-1, -1};
	int fd_r[2] = {-1, -1};

	while (e != NULL) {
		if (e->type == EXPR_TYPE_COMMAND) {
			// fprintf(stderr, "====================\n");
			if (!strncmp(e->cmd.exe, "cd", 4)) {
				chdir(*e->cmd.args);
				e = e->next;
				continue;
			}
			if (e->next && e->next->type == EXPR_TYPE_PIPE) {
				in_pipe_left = true;
			} else {
				in_pipe_left = false;
			}
			if (in_pipe_right) {
				// fprintf(stderr, "RIGHT FROM PIPE\n");
				dup2(fd_r[0], STDIN_FILENO);
				// fprintf(stderr, "stdin = %d\n", fd_r[0]);
				// fprintf(stderr, "%d closed\n", fd_l[1]);
				close(fd_l[1]);
			} else {
				// fprintf(stderr, "NOT RIGHT FROM PIPE\n");
				dup2(dup_stdin, STDIN_FILENO);
				// fprintf(stderr, "stdin = %d\n", dup_stdin);
				// fprintf(stderr, "%d closed\n", fd_r[0]);
				close(fd_r[0]);
			}
			if (in_pipe_left) {
				// fprintf(stderr, "LEFT FROM PIPE\n");
				pipe(fd_l);
				// fprintf(stderr, "pipe() in = %d, out = %d\n", fd_l[0], fd_l[1]);
				dup2(fd_l[1], STDOUT_FILENO);
				// fprintf(stderr, "stdout = %d\n", fd_l[1]);
			} else {
				// fprintf(stderr, "NOT LEFT FROM PIPE\n");
				dup2(file, fd_r[1]);
				dup2(file, STDOUT_FILENO);
				// fprintf(stderr, "stdout = %d\n", file);
			}
			
			// fprintf(stderr, "FORK CALL\n");
			pid_t pid = fork();
			if (pid == 0) {
				// fprintf(stderr, "EXEC CALL %s\n", e->cmd.exe);
				char **argv = build_argv(&e->cmd);
				execvp(e->cmd.exe, argv);
			} else if (pid > 0) {
				// fprintf(stderr, "WAIT CALL\n");
				wait(NULL);
				// fprintf(stderr, "WAIT END\n");
			}
		} else if (e->type == EXPR_TYPE_PIPE) {
			in_pipe_right = true;
			if (fd_r[0]) {
				close (fd_r[0]);
			}
			if (fd_r[1]) {
				close (fd_r[1]);
			}
			fd_r[0] = dup(fd_l[0]);
			// fprintf(stderr, "pipe in_copy = %d", fd_r[0]);
			// fprintf(stderr, "%d closed\n", fd_l[0]);
			// fprintf(stderr, "%d closed\n", fd_l[1]);
			close(fd_l[0]);
			close(fd_l[1]);
			fd_l[0] = -1;
			fd_l[1] = -1;
		} else if (e->type == EXPR_TYPE_AND) {

		} else if (e->type == EXPR_TYPE_OR) {
		
		}
		e = e->next;
		dup2(dup_stdout, STDOUT_FILENO);
		dup2(dup_stdin, STDIN_FILENO);
	}
	dup2(dup_stdin, STDIN_FILENO);
	dup2(dup_stdout, STDOUT_FILENO);
	close(dup_stdin);
	close(dup_stdout);
}

int main(void) {
	const size_t buf_size = 1024;
	char buf[buf_size];
	int rc;
	struct parser *p = parser_new();
	while ((rc = read(STDIN_FILENO, buf, buf_size)) > 0) {
		parser_feed(p, buf, rc);
		struct command_line *line = NULL;
		// fprintf(stderr, "here\n");
		while (true) {
			enum parser_error err = parser_pop_next(p, &line);
			if (err == PARSER_ERR_NONE && line == NULL)
				break;
			if (err != PARSER_ERR_NONE) {
				printf("Error: %d\n", (int)err);
				continue;
			}
			execute_command_line(line);
			command_line_delete(line);
		}
	}
	parser_delete(p);
	return 0;
}
