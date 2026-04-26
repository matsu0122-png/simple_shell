/* =========================================================
 * シンプルシェルの実装
 * 
 * 機能：
 *  - 複数パイプ (|)
 *  - リダイレクション (<, >, >>)
 *  - バックグラウンド実行 (&)
 *  - バックグラウンドプロセス回収
 *  - 組み込みコマンド (cd, pwd, exit)
 * ========================================================= */

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/wait.h>
#include <signal.h>
#include <fcntl.h>
#include <sys/stat.h>

/* 標準入出力のファイルディスクリプタ */
#define STDIN 0
#define STDOUT 1
#define STDERR 2

/* 最大引数数と最大コマンド数 */
#define MAX_ARGS 256
#define MAX_CMDS 32

/* プロセス実行モード */
enum proc_flag {
  FORE,  // フォアグラウンド
  BACK   // バックグラウンド
};

/* 関数プロトタイプ */
char **separate(char *, int, int *);
void sigint_handler(int);
void reap_background(void);
int exec_builtin(char **argv);
void apply_redirection(char **argv);

/* =========================================================
 * main関数
 * ========================================================= */
int main(void)
{
  char prompt[64] = "> ";   // プロンプト表示
  char command[256];        // 入力コマンド
  char **argments;          // 引数配列
  int amount;               // 引数数
  enum proc_flag flag;      // 実行モード

  /* Ctrl+Cハンドラ設定 */
  signal(SIGINT, sigint_handler);

  while (1) {

    char **cmdv[MAX_CMDS];  // 各コマンドの先頭ポインタ
    int cmd_count = 0;      // コマンド数
    int i, j;

    /* 終了したバックグラウンドプロセスを回収 */
    reap_background();

    fprintf(stderr, "%s", prompt);

    /* 入力取得 */
    if (fgets(command, sizeof(command), stdin) == NULL) {
      break;
    }

    /* 改行削除 */
    size_t len = strlen(command);
    if (len > 0 && command[len - 1] == '\n') {
      command[len - 1] = '\0';
    }

    /* 空白で分割 */
    argments = separate(command, MAX_ARGS, &amount);
    if (argments == NULL) continue;

    /* 空入力ならスキップ */
    if (amount == 0) {
      free(argments);
      continue;
    }

    /* exit / quit で終了 */
    if (strcmp(argments[0], "exit") == 0 ||
        strcmp(argments[0], "quit") == 0) {
      free(argments);
      break;
    }

    /* デフォルトはフォアグラウンド */
    flag = FORE;

    /* "&" があればバックグラウンド */
    if (amount > 0 && strcmp(argments[amount - 1], "&") == 0) {
      flag = BACK;
      argments[amount - 1] = NULL;
      amount--;
    }

    if (amount == 0) {
      free(argments);
      continue;
    }

    /* 最初のコマンド登録 */
    cmdv[cmd_count++] = &argments[0];

    /* "|" で分割して複数コマンドに */
    for (i = 0; i < amount; i++) {
      if (argments[i] && strcmp(argments[i], "|") == 0) {

        argments[i] = NULL;

        /* 構文チェック */
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

    /* 空コマンドチェック */
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

    /* =====================
     * 組み込みコマンド処理
     * ===================== */
    if (cmd_count == 1 && flag == FORE) {
      if (exec_builtin(cmdv[0]) == 0) {
        free(argments);
        continue;
      }
    }

    /* =====================
     * パイプ実行
     * ===================== */

    int prev_read = -1;      // 前のパイプの読み取り側
    pid_t pids[MAX_CMDS];

    for (i = 0; i < cmd_count; i++) {

      int fd[2] = { -1, -1 };

      /* 次のパイプ作成 */
      if (i < cmd_count - 1) {
        if (pipe(fd) < 0) {
          perror("pipe");
          break;
        }
      }

      pid_t pid = fork();

      /* ========= 子プロセス ========= */
      if (pid == 0) {

        /* Ctrl+Cは子だけ効くように */
        signal(SIGINT, SIG_DFL);

        /* 前のパイプ入力 */
        if (prev_read >= 0) {
          dup2(prev_read, STDIN);
          close(prev_read);
        }

        /* 次のパイプ出力 */
        if (i < cmd_count - 1) {
          close(fd[0]);
          dup2(fd[1], STDOUT);
          close(fd[1]);
        }

        /* リダイレクション処理 */
        apply_redirection(cmdv[i]);

        /* built-in（子で実行するケース） */
        if (exec_builtin(cmdv[i]) == 0) {
          exit(0);
        }

        /* 外部コマンド実行 */
        if (execvp(cmdv[i][0], cmdv[i]) == -1) {
          perror("execvp");
          exit(1);
        }
      }

      /* ========= 親プロセス ========= */
      else if (pid > 0) {
        pids[i] = pid;

        if (prev_read >= 0) close(prev_read);

        if (i < cmd_count - 1) {
          close(fd[1]);
          prev_read = fd[0];
        } else {
          prev_read = -1;
        }
      }

      else {
        perror("fork");
        break;
      }
    }

    /* フォアグラウンドなら待機 */
    if (flag == BACK) {
      printf("[background]\n");
    } else {
      for (j = 0; j < cmd_count; j++) {
        waitpid(pids[j], NULL, 0);
      }
    }

    free(argments);
  }

  return 0;
}

/* =========================================================
 * 文字列を空白で分割
 * ========================================================= */
char **separate(char *buf, int max, int *count)
{
  char **chops = malloc((max + 1) * sizeof(char *));
  int i;

  if (!chops) return NULL;

  for (i = 0; i < max; i++) {
    char *tok = (i == 0) ? strtok(buf, " \t")
                        : strtok(NULL, " \t");
    if (!tok) break;
    chops[i] = tok;
  }

  chops[i] = NULL;
  *count = i;

  return chops;
}

/* =========================================================
 * リダイレクション処理
 * ========================================================= */
void apply_redirection(char **argv)
{
  for (int k = 0; argv[k]; k++) {

    /* 入力リダイレクト */
    if (!strcmp(argv[k], "<")) {
      int fd = open(argv[k + 1], O_RDONLY);
      dup2(fd, STDIN);
      close(fd);
      argv[k] = NULL;
    }

    /* 出力リダイレクト */
    else if (!strcmp(argv[k], ">")) {
      int fd = open(argv[k + 1],
                    O_WRONLY | O_CREAT | O_TRUNC,
                    0644);
      dup2(fd, STDOUT);
      close(fd);
      argv[k] = NULL;
    }

    /* 追記 */
    else if (!strcmp(argv[k], ">>")) {
      int fd = open(argv[k + 1],
                    O_WRONLY | O_CREAT | O_APPEND,
                    0644);
      dup2(fd, STDOUT);
      close(fd);
      argv[k] = NULL;
    }
  }
}

/* =========================================================
 * バックグラウンド回収
 * ========================================================= */
void reap_background(void)
{
  int st;
  pid_t pid;

  while ((pid = waitpid(-1, &st, WNOHANG)) > 0) {
    printf("\n[background pid=%d done]\n", pid);
    fflush(stdout);
  }
}

/* =========================================================
 * Ctrl+C処理
 * ========================================================= */
void sigint_handler(int x)
{
  (void)x;
  write(STDERR, "\n", 1);
}

/* =========================================================
 * 組み込みコマンド
 * ========================================================= */
int exec_builtin(char **argv)
{
  char cwd[1024];

  if (!argv[0]) return -1;

  /* cd */
  if (!strcmp(argv[0], "cd")) {
    if (!argv[1]) chdir(getenv("HOME"));
    else chdir(argv[1]);
    return 0;
  }

  /* pwd */
  if (!strcmp(argv[0], "pwd")) {
    getcwd(cwd, sizeof(cwd));
    printf("%s\n", cwd);
    return 0;
  }

  return -1;
}