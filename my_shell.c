/* シンプルシェルの実装
 * 複数パイプ + リダイレクション + 連続リダイレクション + >> + &
 * バックグラウンド回収 + cd / pwd / exit
 */

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/wait.h>
#include <signal.h>
#include <fcntl.h>
#include <sys/stat.h>

#define STDIN 0
#define STDOUT 1
#define STDERR 2

#define MAX_ARGS 256
#define MAX_CMDS 32

enum proc_flag {
  FORE,
  BACK
};

char **separate(char *, int, int *);
void sigint_handler(int);
void reap_background(void);
int exec_builtin(char **argv);
void apply_redirection(char **argv);

int main(void)
{
  char prompt[64] = "> ";
  char command[256];
  char *cmd;
  char **argments;
  int amount;
  enum proc_flag flag;

  signal(SIGINT, sigint_handler);

  while (1) {
    char **cmdv[MAX_CMDS];
    int cmd_count = 0;
    int i, j;

    reap_background();

    fprintf(stderr, "%s", prompt);

    if (fgets(command, sizeof(command), stdin) == NULL) {
      break;
    }

    {
      size_t len = strlen(command);
      if (len > 0 && command[len - 1] == '\n') {
        command[len - 1] = '\0';
      }
    }

    cmd = command;

    argments = separate(cmd, MAX_ARGS, &amount);
    if (argments == NULL) {
      continue;
    }

    if (amount == 0) {
      free(argments);
      continue;
    }

    if (strcmp(argments[0], "exit") == 0 || strcmp(argments[0], "quit") == 0) {
      free(argments);
      break;
    }

    flag = FORE;

    if (amount > 0 && strcmp(argments[amount - 1], "&") == 0) {
      flag = BACK;
      argments[amount - 1] = NULL;
      amount--;
    }

    if (amount == 0) {
      free(argments);
      continue;
    }

    cmdv[cmd_count++] = &argments[0];

    for (i = 0; i < amount; i++) {
      if (argments[i] != NULL && strcmp(argments[i], "|") == 0) {
        argments[i] = NULL;

        if (i + 1 >= amount || argments[i + 1] == NULL) {
          fprintf(stderr, "pipe syntax error\n");
          cmd_count = -1;
          break;
        }

        if (cmd_count >= MAX_CMDS) {
          fprintf(stderr, "too many commands\n");
          cmd_count = -1;
          break;
        }

        cmdv[cmd_count++] = &argments[i + 1];
      }
    }

    if (cmd_count == -1) {
      free(argments);
      continue;
    }

    for (i = 0; i < cmd_count; i++) {
      if (cmdv[i][0] == NULL) {
        fprintf(stderr, "empty command error\n");
        cmd_count = -1;
        break;
      }
    }

    if (cmd_count == -1) {
      free(argments);
      continue;
    }

    if (cmd_count == 1 && flag == FORE) {
      if (exec_builtin(cmdv[0]) == 0) {
        free(argments);
        continue;
      }
    }

    {
      int prev_read = -1;
      pid_t pids[MAX_CMDS];

      for (i = 0; i < cmd_count; i++) {
        int fd[2] = { -1, -1 };
        pid_t pid;

        if (i < cmd_count - 1) {
          if (pipe(fd) < 0) {
            perror("pipe");
            break;
          }
        }

        pid = fork();

        if (pid == 0) {
          signal(SIGINT, SIG_DFL);

          if (prev_read >= 0) {
            dup2(prev_read, STDIN);
            close(prev_read);
          }

          if (i < cmd_count - 1) {
            close(fd[0]);
            dup2(fd[1], STDOUT);
            close(fd[1]);
          }

          apply_redirection(cmdv[i]);

          if (exec_builtin(cmdv[i]) == 0) {
            exit(0);
          }

          if (execvp(cmdv[i][0], cmdv[i]) == -1) {
            perror("execvp");
            exit(1);
          }
        }
        else if (pid > 0) {
          pids[i] = pid;

          if (prev_read >= 0) {
            close(prev_read);
          }

          if (i < cmd_count - 1) {
            close(fd[1]);
            prev_read = fd[0];
          }
          else {
            prev_read = -1;
          }
        }
        else {
          perror("fork");
          if (prev_read >= 0) close(prev_read);
          if (fd[0] >= 0) close(fd[0]);
          if (fd[1] >= 0) close(fd[1]);
          break;
        }
      }

      if (flag == BACK) {
        printf("[background]\n");
      }
      else {
        int st;
        for (j = 0; j < cmd_count; j++) {
          waitpid(pids[j], &st, 0);
        }
      }
    }

    free(argments);
  }

  return 0;
}

char **separate(char *buf, int max, int *count)
{
  char **chops = malloc((max + 1) * sizeof(char *));
  int i;

  if (chops == NULL) {
    perror("malloc");
    return NULL;
  }

  for (i = 0; i < max; i++) {
    char *tok;

    if (i == 0) {
      tok = strtok(buf, " \t");
    }
    else {
      tok = strtok(NULL, " \t");
    }

    if (tok == NULL) {
      break;
    }

    chops[i] = tok;
  }

  if (i >= max) {
    printf("Too many args\n");
    free(chops);
    return NULL;
  }

  chops[i] = NULL;
  *count = i;

  return chops;
}

void apply_redirection(char **argv)
{
  int k;

  for (k = 0; argv[k] != NULL; k++) {
    if (strcmp(argv[k], "<") == 0) {
      int in_fd;

      if (argv[k + 1] == NULL) {
        fprintf(stderr, "input redirection error\n");
        exit(1);
      }

      in_fd = open(argv[k + 1], O_RDONLY);
      if (in_fd < 0) {
        perror("open");
        exit(1);
      }

      dup2(in_fd, STDIN);
      close(in_fd);

      argv[k] = NULL;
    }
    else if (strcmp(argv[k], ">") == 0) {
      int out_fd;

      if (argv[k + 1] == NULL) {
        fprintf(stderr, "output redirection error\n");
        exit(1);
      }

      out_fd = open(argv[k + 1],
                    O_WRONLY | O_CREAT | O_TRUNC,
                    S_IRUSR | S_IWUSR);

      if (out_fd < 0) {
        perror("open");
        exit(1);
      }

      dup2(out_fd, STDOUT);
      close(out_fd);

      argv[k] = NULL;
    }
    else if (strcmp(argv[k], ">>") == 0) {
      int out_fd;

      if (argv[k + 1] == NULL) {
        fprintf(stderr, "append redirection error\n");
        exit(1);
      }

      out_fd = open(argv[k + 1],
                    O_WRONLY | O_CREAT | O_APPEND,
                    S_IRUSR | S_IWUSR);

      if (out_fd < 0) {
        perror("open");
        exit(1);
      }

      dup2(out_fd, STDOUT);
      close(out_fd);

      argv[k] = NULL;
    }
  }
}

void reap_background(void)
{
  int st;
  pid_t pid;

  while ((pid = waitpid(-1, &st, WNOHANG)) > 0) {
    printf("\n[background pid=%d done]\n", pid);
    fflush(stdout);
  }
}

void sigint_handler(int x)
{
  (void)x;
  write(STDERR, "\n", 1);
}

int exec_builtin(char **argv)
{
  char cwd[1024];

  if (argv[0] == NULL) {
    return -1;
  }

  if (strcmp(argv[0], "cd") == 0) {
    if (argv[1] == NULL) {
      char *home = getenv("HOME");
      if (home == NULL) {
        fprintf(stderr, "cd: HOME not set\n");
        return 0;
      }

      if (chdir(home) < 0) {
        perror("cd");
      }
    }
    else {
      if (chdir(argv[1]) < 0) {
        perror("cd");
      }
    }

    return 0;
  }

  if (strcmp(argv[0], "pwd") == 0) {
    if (getcwd(cwd, sizeof(cwd)) == NULL) {
      perror("pwd");
    }
    else {
      printf("%s\n", cwd);
    }

    return 0;
  }

  return -1;
}