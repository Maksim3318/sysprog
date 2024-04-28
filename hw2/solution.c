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

static char **build_argv(const struct command *cmd) {
	char **argv = malloc((2 + cmd->arg_count) * sizeof(char *));
	argv[0] = cmd->exe;
	for (uint32_t i = 0; i < cmd->arg_count; ++i) {
		argv[i + 1] = cmd->args[i];
	}
	argv[cmd->arg_count + 1] = NULL;
	return argv;
}

struct pq_node {
	pid_t pid;
	struct pq_node *next;
};

struct pqueue {
	struct pq_node *head;
	struct pq_node *tail;
};

static struct pq_node *new_node(pid_t pid) {
	struct pq_node *res = malloc(sizeof(struct pq_node));
	res->pid = pid;
	res->next = NULL;
	return res;
}

static struct pqueue *new_pqueue() {
	struct pqueue *res = malloc(sizeof(struct pqueue));
	res->head = NULL;
	res->tail = NULL;
	return res;
}

static void push_pqueue(struct pqueue *pq, pid_t pid) {
	struct pq_node *node = new_node(pid);
	if (!pq->head) {
		pq->head = node;
		pq->tail = node;
	} else {
		pq->tail->next = node;
		pq->tail = pq->tail->next;
	}
}

static int pop_queue(struct pqueue *pq) {
	int status = -1;
	if (pq->head) {
		struct pq_node *tmp = pq->head;
		pq->head = pq->head->next;
		waitpid(tmp->pid, &status, 0);
		if (WIFEXITED(status)) {
			status = WEXITSTATUS(status);
		}
		free(tmp);
	}
	return status;
}

static void free_pqueue(struct pqueue *pq) {
	while (pq->head) {
		struct pq_node *tmp = pq->head;
		pq->head = pq->head->next;
		free(tmp);
	}
}

static int wait_pqueue(struct pqueue *pq) {
	int exitcode = 0;
	while(pq->head) {
		exitcode = pop_queue(pq);
	}
	free_pqueue(pq);
	return exitcode;
}

static void move_pipe(int fd_l[2], int fd_r[2]) {
	if (fd_r[0] != -1) {
		close(fd_r[0]);
	}	
	fd_r[0] = fd_l[0];
	fd_l[0] = -1;

	if (fd_r[1] != -1) {
		close(fd_r[1]);
	}
	fd_r[1] = fd_l[1];
	fd_l[1] = -1;
}

static struct expr *skip(struct expr *e) {
	struct expr *tmp = e;
	while (tmp->next 
		&& tmp->next->type != EXPR_TYPE_AND 
		&& tmp->next->type != EXPR_TYPE_OR) {
		tmp = tmp->next;
	}
	return tmp;
}

static int execute_command_line(struct command_line *line, struct parser *p) {
	struct expr *e = line->head;
	int exitcode = 0;
	if (e->type == EXPR_TYPE_COMMAND 
		&& !strncmp(e->cmd.exe, "exit", 6)
		&& e->next == NULL) {
			if (e->cmd.arg_count) {
				exitcode = atoi(e->cmd.args[0]);
			}
			parser_delete(p);
			command_line_delete(line);
			exit(exitcode);
	}

	struct pqueue *pq = new_pqueue();
	bool in_pipe_left = false;
	bool in_pipe_right = false;
	int fd_l[2] = {-1, -1};
	int fd_r[2] = {-1, -1};

	while (e != NULL) {
		if (e->type == EXPR_TYPE_COMMAND) {

			if (!strncmp(e->cmd.exe, "cd", 4)) {
				chdir(*e->cmd.args);
				e = e->next;
				continue;
			}
			if (e->next && e->next->type == EXPR_TYPE_PIPE) {
				in_pipe_left = true;
				pipe(fd_l);
			}

			pid_t pid = fork();
			if (pid == 0) {
				if (!strncmp(e->cmd.exe, "exit", 6)) {
					int ret = 0;
					if (e->cmd.arg_count) {
						ret = atoi(e->cmd.args[0]);
					}
					parser_delete(p);
					command_line_delete(line);
					free_pqueue(pq);
					free(pq);
					exit(ret);
				}
				if (in_pipe_left) {
					dup2(fd_l[1], STDOUT_FILENO);
					close(fd_l[0]);
					close(fd_l[1]);
				} else if (line->out_type != OUTPUT_TYPE_STDOUT) {
					int file = -1;
					int appending_flag = line->out_type == OUTPUT_TYPE_FILE_NEW ? O_TRUNC : O_APPEND;
					file = open(line->out_file, O_RDWR | O_CREAT | O_CLOEXEC | appending_flag, 0644);
					dup2(file, STDOUT_FILENO);
					close(file);
				}
				if (in_pipe_right) {
					close(fd_r[1]);
					dup2(fd_r[0], STDIN_FILENO);
					close(fd_r[0]);
				}
				char **argv = build_argv(&e->cmd);
				execvp(e->cmd.exe, argv);
			} else {
				push_pqueue(pq, pid);
			}

		} else if (e->type == EXPR_TYPE_PIPE) {
			in_pipe_right = true;
			in_pipe_left = false;
			move_pipe(fd_l, fd_r);
		} else if (e->type == EXPR_TYPE_AND) {
			move_pipe(fd_l, fd_r);
			exitcode = wait_pqueue(pq);
			if (exitcode) {
				e = skip(e);
			}
			in_pipe_left = false;
			in_pipe_right = false;
		} else if (e->type == EXPR_TYPE_OR) {
			move_pipe(fd_l, fd_r);
			exitcode = wait_pqueue(pq);
			if (!exitcode) {
				e = skip(e);
			}
			in_pipe_left = false;
			in_pipe_right = false;
		}
		e = e->next;
	}
	move_pipe(fd_r, fd_r);
	exitcode = wait_pqueue(pq);
	free(pq);
	return exitcode;
}

int main(void) {
	const size_t buf_size = 1024;
	char buf[buf_size];
	int rc;
	struct parser *p = parser_new();
	int exitcode = 0;
	while ((rc = read(STDIN_FILENO, buf, buf_size)) > 0) {
		parser_feed(p, buf, rc);
		struct command_line *line = NULL;
		while (true) {
			enum parser_error err = parser_pop_next(p, &line);
			if (err == PARSER_ERR_NONE && line == NULL)
				break;
			if (err != PARSER_ERR_NONE) {
				printf("Error: %d\n", (int)err);
				continue;
			}
			exitcode = execute_command_line(line, p);
			// printf("exit code = %d\n", exitcode);
			command_line_delete(line);
		}
	}
	parser_delete(p);
	return exitcode;
}
