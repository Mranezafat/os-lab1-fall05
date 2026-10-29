// Shell.

#include "types.h"
#include "user.h"
#include "fcntl.h"

// Parsed command representation
#define EXEC  1
#define REDIR 2
#define PIPE  3
#define LIST  4
#define BACK  5

#define MAXARGS 10

struct cmd {
  int type;
};

struct execcmd {
  int type;
  char *argv[MAXARGS];
  char *eargv[MAXARGS];
};

struct redircmd {
  int type;
  struct cmd *cmd;
  char *file;
  char *efile;
  int mode;
  int fd;
};

struct pipecmd {
  int type;
  struct cmd *left;
  struct cmd *right;
};

struct listcmd {
  int type;
  struct cmd *left;
  struct cmd *right;
};

struct backcmd {
  int type;
  struct cmd *cmd;
};

int fork1(void);  // Fork but panics on failure.
void panic(char*);
struct cmd *parsecmd(char*);

// Execute cmd.  Never returns.
void
runcmd(struct cmd *cmd)
{
  int p[2];
  struct backcmd *bcmd;
  struct execcmd *ecmd;
  struct listcmd *lcmd;
  struct pipecmd *pcmd;
  struct redircmd *rcmd;

  if(cmd == 0)
    exit();

  switch(cmd->type){
  default:
    panic("runcmd");

  case EXEC:
    ecmd = (struct execcmd*)cmd;
    if(ecmd->argv[0] == 0)
      exit();
    exec(ecmd->argv[0], ecmd->argv);
    printf(2, "exec %s failed\n", ecmd->argv[0]);
    break;

  case REDIR:
    rcmd = (struct redircmd*)cmd;
    close(rcmd->fd);
    if(open(rcmd->file, rcmd->mode) < 0){
      printf(2, "open %s failed\n", rcmd->file);
      exit();
    }
    runcmd(rcmd->cmd);
    break;

  case LIST:
    lcmd = (struct listcmd*)cmd;
    if(fork1() == 0)
      runcmd(lcmd->left);
    wait();
    runcmd(lcmd->right);
    break;

  case PIPE:
    pcmd = (struct pipecmd*)cmd;
    if(pipe(p) < 0)
      panic("pipe");
    if(fork1() == 0){
      close(1);
      dup(p[1]);
      close(p[0]);
      close(p[1]);
      runcmd(pcmd->left);
    }
    if(fork1() == 0){
      close(0);
      dup(p[0]);
      close(p[0]);
      close(p[1]);
      runcmd(pcmd->right);
    }
    close(p[0]);
    close(p[1]);
    wait();
    wait();
    break;

  case BACK:
    bcmd = (struct backcmd*)cmd;
    if(fork1() == 0)
      runcmd(bcmd->cmd);
    break;
  }
  exit();
}

int
getcmd(char *buf, int nbuf)
{
  printf(2, "$ ");
  memset(buf, 0, nbuf);
  gets(buf, nbuf);
  if(buf[0] == 0) // EOF
    return -1;
  return 0;
}

#define MAX_CMD_LEN 512
#define MAX_HIST 32
#define MAX_ALIASES 32

struct history_entry {
  int id;
  char cmd[MAX_CMD_LEN];
};

struct alias_entry {
  char name[64];
  char cmd[MAX_CMD_LEN];
};

static struct history_entry history_table[MAX_HIST];
static int history_count = 0;
static int next_history_id = 1;

static struct alias_entry alias_table[MAX_ALIASES];
static int alias_count = 0;

static int
my_strncmp(const char *p, const char *q, uint n)
{
  while(n > 0 && *p && *p == *q){
    n--;
    p++;
    q++;
  }
  if(n == 0)
    return 0;
  return (uchar)*p - (uchar)*q;
}

static char*
my_strncpy(char *dst, const char *src, int n)
{
  char *d = dst;
  while(n > 0 && *src){
    *d++ = *src++;
    n--;
  }
  while(n > 0){
    *d++ = 0;
    n--;
  }
  return dst;
}

static void
add_history(const char *cmd)
{
  if(cmd == 0 || cmd[0] == 0)
    return;
  if(history_count < MAX_HIST){
    history_table[history_count].id = next_history_id++;
    my_strncpy(history_table[history_count].cmd, cmd, MAX_CMD_LEN - 1);
    history_table[history_count].cmd[MAX_CMD_LEN - 1] = 0;
    history_count++;
  } else {
    for(int i = 0; i < MAX_HIST - 1; i++){
      history_table[i] = history_table[i+1];
    }
    history_table[MAX_HIST - 1].id = next_history_id++;
    my_strncpy(history_table[MAX_HIST - 1].cmd, cmd, MAX_CMD_LEN - 1);
    history_table[MAX_HIST - 1].cmd[MAX_CMD_LEN - 1] = 0;
  }
}

static void
print_history(void)
{
  for(int i = 0; i < history_count; i++){
    printf(1, "%d  %s\n", history_table[i].id, history_table[i].cmd);
  }
}

static int
history_expand(const char *in, char *out, int maxout)
{
  if(in[0] != '!'){
    my_strncpy(out, in, maxout - 1);
    out[maxout - 1] = 0;
    return 0;
  }

  // Case 1: !!
  if(in[1] == '!'){
    if(history_count == 0){
      printf(2, "!!: event not found\n");
      return -1;
    }
    const char *last_cmd = history_table[history_count - 1].cmd;
    const char *rest = in + 2;
    out[0] = 0;
    my_strncpy(out, last_cmd, maxout - 1);
    out[maxout - 1] = 0;
    if(*rest){
      int curlen = strlen(out);
      if(curlen < maxout - 1){
        my_strncpy(out + curlen, rest, maxout - 1 - curlen);
        out[maxout - 1] = 0;
      }
    }
    return 1;
  }

  // Case 2: !n (where n is a number)
  if(in[1] >= '0' && in[1] <= '9'){
    int n = atoi(in + 1);
    const char *rest = in + 1;
    while(*rest >= '0' && *rest <= '9')
      rest++;
    int found = -1;
    for(int i = 0; i < history_count; i++){
      if(history_table[i].id == n){
        found = i;
        break;
      }
    }
    if(found == -1){
      printf(2, "!%d: event not found\n", n);
      return -1;
    }
    out[0] = 0;
    my_strncpy(out, history_table[found].cmd, maxout - 1);
    out[maxout - 1] = 0;
    if(*rest){
      int curlen = strlen(out);
      if(curlen < maxout - 1){
        my_strncpy(out + curlen, rest, maxout - 1 - curlen);
        out[maxout - 1] = 0;
      }
    }
    return 1;
  }

  // Case 3: !prefix
  if(in[1] != 0 && in[1] != ' ' && in[1] != '\t'){
    char prefix[64];
    int plen = 0;
    const char *rest = in + 1;
    while(*rest && *rest != ' ' && *rest != '\t' && plen < sizeof(prefix) - 1){
      prefix[plen++] = *rest++;
    }
    prefix[plen] = 0;

    int found = -1;
    for(int i = history_count - 1; i >= 0; i--){
      if(my_strncmp(history_table[i].cmd, prefix, plen) == 0){
        found = i;
        break;
      }
    }
    if(found == -1){
      printf(2, "!%s: event not found\n", prefix);
      return -1;
    }
    out[0] = 0;
    my_strncpy(out, history_table[found].cmd, maxout - 1);
    out[maxout - 1] = 0;
    if(*rest){
      int curlen = strlen(out);
      if(curlen < maxout - 1){
        my_strncpy(out + curlen, rest, maxout - 1 - curlen);
        out[maxout - 1] = 0;
      }
    }
    return 1;
  }

  printf(2, "!: event not found\n");
  return -1;
}

static int
alias_expand(const char *in, char *out, int maxout)
{
  char first_word[64];
  const char *p = in;
  while(*p == ' ' || *p == '\t')
    p++;

  int wlen = 0;
  while(p[wlen] && p[wlen] != ' ' && p[wlen] != '\t' && wlen < sizeof(first_word) - 1){
    first_word[wlen] = p[wlen];
    wlen++;
  }
  first_word[wlen] = 0;

  if(wlen == 0){
    my_strncpy(out, in, maxout - 1);
    out[maxout - 1] = 0;
    return 0;
  }

  int match = -1;
  for(int i = 0; i < alias_count; i++){
    if(strcmp(alias_table[i].name, first_word) == 0){
      match = i;
      break;
    }
  }

  if(match == -1){
    my_strncpy(out, in, maxout - 1);
    out[maxout - 1] = 0;
    return 0;
  }

  // Replace first word with alias command
  out[0] = 0;
  my_strncpy(out, alias_table[match].cmd, maxout - 1);
  out[maxout - 1] = 0;

  const char *remaining = p + wlen;
  if(*remaining){
    int curlen = strlen(out);
    if(curlen < maxout - 1){
      my_strncpy(out + curlen, remaining, maxout - 1 - curlen);
      out[maxout - 1] = 0;
    }
  }
  return 1;
}

static int
handle_alias_command(char *cmd)
{
  char *p = cmd;
  while(*p == ' ' || *p == '\t')
    p++;

  if(strcmp(p, "alias") == 0){
    for(int i = 0; i < alias_count; i++){
      printf(1, "%s=%s\n", alias_table[i].name, alias_table[i].cmd);
    }
    return 1;
  }

  if(my_strncmp(p, "alias ", 6) == 0){
    p += 6;
    while(*p == ' ' || *p == '\t')
      p++;

    char *eq = strchr(p, '=');
    if(eq == 0){
      printf(2, "alias: usage: alias <name>=<command>\n");
      return 1;
    }

    char name[64];
    int nlen = eq - p;
    // Trim trailing spaces in name
    while(nlen > 0 && (p[nlen-1] == ' ' || p[nlen-1] == '\t'))
      nlen--;
    if(nlen == 0 || nlen >= sizeof(name)){
      printf(2, "alias: invalid name\n");
      return 1;
    }
    my_strncpy(name, p, nlen);
    name[nlen] = 0;

    char *val = eq + 1;
    // Trim leading spaces in command
    while(*val == ' ' || *val == '\t')
      val++;

    // Check if alias already exists
    int existing = -1;
    for(int i = 0; i < alias_count; i++){
      if(strcmp(alias_table[i].name, name) == 0){
        existing = i;
        break;
      }
    }

    if(existing >= 0){
      my_strncpy(alias_table[existing].cmd, val, MAX_CMD_LEN - 1);
      alias_table[existing].cmd[MAX_CMD_LEN - 1] = 0;
    } else {
      if(alias_count < MAX_ALIASES){
        my_strncpy(alias_table[alias_count].name, name, sizeof(alias_table[alias_count].name) - 1);
        alias_table[alias_count].name[sizeof(alias_table[alias_count].name) - 1] = 0;
        my_strncpy(alias_table[alias_count].cmd, val, MAX_CMD_LEN - 1);
        alias_table[alias_count].cmd[MAX_CMD_LEN - 1] = 0;
        alias_count++;
      } else {
        printf(2, "alias: table full (max %d)\n", MAX_ALIASES);
      }
    }
    return 1;
  }

  return 0;
}

int
main(void)
{
  static char buf[MAX_CMD_LEN];
  int fd;

  // Ensure that three file descriptors are open.
  while((fd = open("console", O_RDWR)) >= 0){
    if(fd >= 3){
      close(fd);
      break;
    }
  }

  // Read and run input commands.
  while(getcmd(buf, sizeof(buf)) >= 0){
    // Strip trailing \n and \r
    int len = strlen(buf);
    while(len > 0 && (buf[len-1] == '\n' || buf[len-1] == '\r')){
      buf[--len] = 0;
    }

    // Trim leading whitespace
    char *trimmed = buf;
    while(*trimmed == ' ' || *trimmed == '\t')
      trimmed++;
    if(*trimmed == 0)
      continue;

    // 1. History Expansion
    char hist_expanded[MAX_CMD_LEN];
    int hexp_ret = history_expand(trimmed, hist_expanded, sizeof(hist_expanded));
    if(hexp_ret < 0){
      // Expansion failed (event not found error already printed)
      continue;
    }

    char *cmd_after_hist = trimmed;
    if(hexp_ret > 0){
      // Print expanded command before execution
      printf(1, "%s\n", hist_expanded);
      cmd_after_hist = hist_expanded;
    }

    // Save final command to history (before alias expansion, exactly as typed/expanded)
    add_history(cmd_after_hist);

    // 2. Alias Expansion
    char final_cmd[MAX_CMD_LEN];
    alias_expand(cmd_after_hist, final_cmd, sizeof(final_cmd));

    char *cmd = final_cmd;
    while(*cmd == ' ' || *cmd == '\t')
      cmd++;
    if(*cmd == 0)
      continue;

    // 3. Built-in: cd
    if(cmd[0] == 'c' && cmd[1] == 'd' && (cmd[2] == ' ' || cmd[2] == 0)){
      char *path = cmd + 2;
      while(*path == ' ' || *path == '\t')
        path++;
      if(*path == 0)
        path = "/";
      if(chdir(path) < 0)
        printf(2, "cannot cd %s\n", path);
      continue;
    }

    // 4. Built-in: history
    if(strcmp(cmd, "history") == 0){
      print_history();
      continue;
    }

    // 5. Built-in: alias
    if(handle_alias_command(cmd)){
      continue;
    }

    // 6. External command execution
    if(fork1() == 0)
      runcmd(parsecmd(cmd));
    wait();
  }
  exit();
}

void
panic(char *s)
{
  printf(2, "%s\n", s);
  exit();
}

int
fork1(void)
{
  int pid;

  pid = fork();
  if(pid == -1)
    panic("fork");
  return pid;
}

//PAGEBREAK!
// Constructors

struct cmd*
execcmd(void)
{
  struct execcmd *cmd;

  cmd = malloc(sizeof(*cmd));
  memset(cmd, 0, sizeof(*cmd));
  cmd->type = EXEC;
  return (struct cmd*)cmd;
}

struct cmd*
redircmd(struct cmd *subcmd, char *file, char *efile, int mode, int fd)
{
  struct redircmd *cmd;

  cmd = malloc(sizeof(*cmd));
  memset(cmd, 0, sizeof(*cmd));
  cmd->type = REDIR;
  cmd->cmd = subcmd;
  cmd->file = file;
  cmd->efile = efile;
  cmd->mode = mode;
  cmd->fd = fd;
  return (struct cmd*)cmd;
}

struct cmd*
pipecmd(struct cmd *left, struct cmd *right)
{
  struct pipecmd *cmd;

  cmd = malloc(sizeof(*cmd));
  memset(cmd, 0, sizeof(*cmd));
  cmd->type = PIPE;
  cmd->left = left;
  cmd->right = right;
  return (struct cmd*)cmd;
}

struct cmd*
listcmd(struct cmd *left, struct cmd *right)
{
  struct listcmd *cmd;

  cmd = malloc(sizeof(*cmd));
  memset(cmd, 0, sizeof(*cmd));
  cmd->type = LIST;
  cmd->left = left;
  cmd->right = right;
  return (struct cmd*)cmd;
}

struct cmd*
backcmd(struct cmd *subcmd)
{
  struct backcmd *cmd;

  cmd = malloc(sizeof(*cmd));
  memset(cmd, 0, sizeof(*cmd));
  cmd->type = BACK;
  cmd->cmd = subcmd;
  return (struct cmd*)cmd;
}
//PAGEBREAK!
// Parsing

char whitespace[] = " \t\r\n\v";
char symbols[] = "<|>&;()";

int
gettoken(char **ps, char *es, char **q, char **eq)
{
  char *s;
  int ret;

  s = *ps;
  while(s < es && strchr(whitespace, *s))
    s++;
  if(q)
    *q = s;
  ret = *s;
  switch(*s){
  case 0:
    break;
  case '|':
  case '(':
  case ')':
  case ';':
  case '&':
  case '<':
    s++;
    break;
  case '>':
    s++;
    if(*s == '>'){
      ret = '+';
      s++;
    }
    break;
  default:
    ret = 'a';
    while(s < es && !strchr(whitespace, *s) && !strchr(symbols, *s))
      s++;
    break;
  }
  if(eq)
    *eq = s;

  while(s < es && strchr(whitespace, *s))
    s++;
  *ps = s;
  return ret;
}

int
peek(char **ps, char *es, char *toks)
{
  char *s;

  s = *ps;
  while(s < es && strchr(whitespace, *s))
    s++;
  *ps = s;
  return *s && strchr(toks, *s);
}

struct cmd *parseline(char**, char*);
struct cmd *parsepipe(char**, char*);
struct cmd *parseexec(char**, char*);
struct cmd *nulterminate(struct cmd*);

struct cmd*
parsecmd(char *s)
{
  char *es;
  struct cmd *cmd;

  es = s + strlen(s);
  cmd = parseline(&s, es);
  peek(&s, es, "");
  if(s != es){
    printf(2, "leftovers: %s\n", s);
    panic("syntax");
  }
  nulterminate(cmd);
  return cmd;
}

struct cmd*
parseline(char **ps, char *es)
{
  struct cmd *cmd;

  cmd = parsepipe(ps, es);
  while(peek(ps, es, "&")){
    gettoken(ps, es, 0, 0);
    cmd = backcmd(cmd);
  }
  if(peek(ps, es, ";")){
    gettoken(ps, es, 0, 0);
    cmd = listcmd(cmd, parseline(ps, es));
  }
  return cmd;
}

struct cmd*
parsepipe(char **ps, char *es)
{
  struct cmd *cmd;

  cmd = parseexec(ps, es);
  if(peek(ps, es, "|")){
    gettoken(ps, es, 0, 0);
    cmd = pipecmd(cmd, parsepipe(ps, es));
  }
  return cmd;
}

struct cmd*
parseredirs(struct cmd *cmd, char **ps, char *es)
{
  int tok;
  char *q, *eq;

  while(peek(ps, es, "<>")){
    tok = gettoken(ps, es, 0, 0);
    if(gettoken(ps, es, &q, &eq) != 'a')
      panic("missing file for redirection");
    switch(tok){
    case '<':
      cmd = redircmd(cmd, q, eq, O_RDONLY, 0);
      break;
    case '>':
      cmd = redircmd(cmd, q, eq, O_WRONLY|O_CREATE, 1);
      break;
    case '+':  // >>
      cmd = redircmd(cmd, q, eq, O_WRONLY|O_CREATE, 1);
      break;
    }
  }
  return cmd;
}

struct cmd*
parseblock(char **ps, char *es)
{
  struct cmd *cmd;

  if(!peek(ps, es, "("))
    panic("parseblock");
  gettoken(ps, es, 0, 0);
  cmd = parseline(ps, es);
  if(!peek(ps, es, ")"))
    panic("syntax - missing )");
  gettoken(ps, es, 0, 0);
  cmd = parseredirs(cmd, ps, es);
  return cmd;
}

struct cmd*
parseexec(char **ps, char *es)
{
  char *q, *eq;
  int tok, argc;
  struct execcmd *cmd;
  struct cmd *ret;

  if(peek(ps, es, "("))
    return parseblock(ps, es);

  ret = execcmd();
  cmd = (struct execcmd*)ret;

  argc = 0;
  ret = parseredirs(ret, ps, es);
  while(!peek(ps, es, "|)&;")){
    if((tok=gettoken(ps, es, &q, &eq)) == 0)
      break;
    if(tok != 'a')
      panic("syntax");
    cmd->argv[argc] = q;
    cmd->eargv[argc] = eq;
    argc++;
    if(argc >= MAXARGS)
      panic("too many args");
    ret = parseredirs(ret, ps, es);
  }
  cmd->argv[argc] = 0;
  cmd->eargv[argc] = 0;
  return ret;
}

// NUL-terminate all the counted strings.
struct cmd*
nulterminate(struct cmd *cmd)
{
  int i;
  struct backcmd *bcmd;
  struct execcmd *ecmd;
  struct listcmd *lcmd;
  struct pipecmd *pcmd;
  struct redircmd *rcmd;

  if(cmd == 0)
    return 0;

  switch(cmd->type){
  case EXEC:
    ecmd = (struct execcmd*)cmd;
    for(i=0; ecmd->argv[i]; i++)
      *ecmd->eargv[i] = 0;
    break;

  case REDIR:
    rcmd = (struct redircmd*)cmd;
    nulterminate(rcmd->cmd);
    *rcmd->efile = 0;
    break;

  case PIPE:
    pcmd = (struct pipecmd*)cmd;
    nulterminate(pcmd->left);
    nulterminate(pcmd->right);
    break;

  case LIST:
    lcmd = (struct listcmd*)cmd;
    nulterminate(lcmd->left);
    nulterminate(lcmd->right);
    break;

  case BACK:
    bcmd = (struct backcmd*)cmd;
    nulterminate(bcmd->cmd);
    break;
  }
  return cmd;
}
