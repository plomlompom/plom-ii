// plom-ii: plomlompom's ii fork
// 
// This file is licensed under the GPLv3; for details, see the file LICENSE or
// <http://www.gnu.org/licenses/gpl-3.0.html>. Changes to the ii original are
// (c)opyright 2012 Christian Heller <c.heller@plomlompom.de>
// 
// All work from the *original* ii as shown in the file ii.c is also covered by
// the MIT/X Consortium License (see file LICENSE.old) and these copyrights:
// (c)opyright 2005-2006 Anselm R. Garbe <garbeam@wmii.de>
// (c)opyright 2005-2008 Nico Golde <nico at ngolde dot de>

#include <errno.h>
#include <netdb.h>
#include <sys/types.h>
#include <netinet/in.h>
#include <stdio.h>
#include <stdlib.h>
#include <limits.h>
#include <fcntl.h>
#include <string.h>
#include <pwd.h>
#include <signal.h>
#include <sys/stat.h>
#include <sys/socket.h>
#include <ctype.h>
#include <time.h>
#include <unistd.h>

#define VERSION "1.6-plom"

#ifndef PIPE_BUF /* FreeBSD don't know PIPE_BUF */
#define PIPE_BUF 4096
#endif
#define PING_TIMEOUT 300
#define SERVER_PORT 6667
enum { TOK_NICKSRV = 0, TOK_USER, TOK_CMD, TOK_CHAN, TOK_ARG, TOK_TEXT, TOK_LAST };

typedef struct Channel Channel;
struct Channel {
  int fd;
  char *name;
  Channel *next; };

static int irc;
static time_t last_response;
static Channel *channels = NULL;
static char *host = "irc.freenode.net";
static char nick[32];			/* might change while running */
static char path[_POSIX_PATH_MAX];
static char message[PIPE_BUF]; /* message buf used for communication */

static void usage() {
// Print help message.
  fprintf(stderr, "%s",
          "ii - irc it - " VERSION "\n"
          "(C)opyright MMV-MMVI Anselm R. Garbe\n"
          "(C)opyright MMV-MMXI Nico Golde\n"
          "usage: ii [-i <irc dir>] [-s <host>] [-p <port>]\n"
          "          [-n <nick>] [-k <password>] [-f <fullname>]\n");
  exit(EXIT_SUCCESS); }

static char *striplower(char *s) {
// Lowercase chars in s[]; replace '/' with '_'.
  char *p = NULL;
  for(p = s; p && *p; p++) {
    if(*p == '/')
      *p = '_';
    *p = tolower(*p); }
  return s; }

static void create_dirtree(const char *dir) {
// Create directories described by "dir" -- top-down, if necessary.
  char tmp[256];
  char *p = NULL;
  size_t len;
  snprintf(tmp, sizeof(tmp),"%s",dir);
  len = strlen(tmp);
  if(tmp[len - 1] == '/')
    tmp[len - 1] = 0;
  for(p = tmp + 1; *p; p++)
    if(*p == '/') {
      *p = 0;
      mkdir(tmp, S_IRWXU);
      *p = '/'; }
  mkdir(tmp, S_IRWXU); }

static int get_filepath(char *filepath, size_t len, char *channel, char *file) {
// Build (only dirs) and return filepath: path + (if provided) channel + file.
  if(channel) {
    if(!snprintf(filepath, len, "%s/%s", path, channel))
      return 0;
    create_dirtree(filepath);
    return snprintf(filepath, len, "%s/%s/%s", path, channel, file); }
  return snprintf(filepath, len, "%s/%s", path, file); }

static void create_filepath(char *filepath, size_t len, char *channel, char *suffix) {
// get_filepath() for file below striplower()'d channel name.
  if(!get_filepath(filepath, len, striplower(channel), suffix)) {
    fprintf(stderr, "%s", "ii: path to irc directory too long\n");
    exit(EXIT_FAILURE); } }

static int open_channel(char *name) {
// Create channel fifo infile, open it / return its file descriptor.
  static char infile[256];
  create_filepath(infile, sizeof(infile), name, "in");
  if(access(infile, F_OK) == -1)
    mkfifo(infile, S_IRWXU);
  return open(infile, O_RDONLY | O_NONBLOCK, 0); }

static void add_channel(char *cname) {
// If not yet in channels list, add channel to it and create its fifo infile.
  Channel *c;
  int fd;
  char *name = striplower(cname);

  // Abort if channel already in channels[].
  for(c = channels; c; c = c->next)
    if(!strcmp(name, c->name))
      return;

  // Try to create channel fifo infile.
  fd = open_channel(name);
  if(fd == -1) {
    printf("ii: exiting, cannot create in channel: %s\n", name);
    exit(EXIT_FAILURE); }

  // Allocate memory for new Channel struct.
  c = calloc(1, sizeof(Channel));
  if(!c) {
    perror("ii: cannot allocate memory");
    exit(EXIT_FAILURE); }

  // Prepend new channel struct to channels chain.
  if(!channels)
    channels = c;
  else {
    c->next = channels;
    channels = c; }

  // Populate new channel struct: channel name, channel fifo descriptor.
  c->fd = fd;
  c->name = strdup(name); }

static void rm_channel(Channel *c) {
// Remove Channel *c from channels chain.
  Channel *p;
  if(channels == c)
    channels = channels->next;
  else {
    for(p = channels; p && p->next != c; p = p->next);
    if(p->next == c)
      p->next = c->next; }
  free(c->name);
  free(c); }

static void login(char *key, char *fullname) {
// Write login info into server socket.
  if(key)
    snprintf(message, PIPE_BUF,
             "PASS %s\r\nNICK %s\r\nUSER %s localhost %s :%s\r\n",
             key, nick, nick, host, fullname ? fullname : nick);
  else
    snprintf(message, PIPE_BUF,
             "NICK %s\r\nUSER %s localhost %s :%s\r\n",
              nick, nick, host, fullname ? fullname : nick);
  write(irc, message, strlen(message));	}

static int tcpopen(unsigned short port) {
// Build socket file connection to host:port, return file descriptor.
  int fd;
  struct sockaddr_in sin;
  memset(&sin, 0, sizeof(struct sockaddr_in));

  // Write host address into sin.
  struct hostent *hp = gethostbyname(host);
  if(!hp) {
    perror("ii: cannot retrieve host information");
    exit(EXIT_FAILURE); }
  memcpy(&sin.sin_addr, hp->h_addr, hp->h_length);

  sin.sin_family = AF_INET;
  sin.sin_port = htons(port);

  // Build/connect socket.
  if((fd = socket(AF_INET, SOCK_STREAM, 0)) < 0) {
    perror("ii: cannot create socket");
    exit(EXIT_FAILURE); }
  if(connect(fd, (const struct sockaddr *) &sin, sizeof(sin)) < 0) {
    perror("ii: cannot connect to host");
    exit(EXIT_FAILURE); }
  return fd; }

static size_t tokenize(char **result, size_t reslen, char *str, char delim) {
// In str[], replace delim with \0, store pointers of first reslen chunks in result[], return chunks number.
  char *p = NULL, *n = NULL;
  size_t i;
  if(!str)
    return 0;

  // Move pointer to first non-whitespace in str[].
  for(n = str; *n == ' '; n++);

  // Replace delim chars, save chunk pointers in result[]. Not sure what the workaround does.
  p = n;
  for(i = 0; *n != 0;) {
    if(i == reslen)
      return 0;
    if(i > TOK_CHAN - TOK_CMD && strtol(result[0], NULL, 10) > 0)
      delim=':'; /* workaround non-RFC compliant messages */
    if(*n == delim) {
      *n = 0;
      result[i++] = p;
      p = ++n; }
    else
      n++; }

  // Add last chunk pointer to result[], if a valid one.
  if(i<reslen && p<n && strlen(p))
    result[i++] = p;
  return i; }

static void print_out(char *channel, char *buf) {
// Append buf[] to appropriate out file, prefixed with localtime string.
  static char outfile[256], server[256], buft[18];
  FILE *out = NULL;
  time_t t = time(0);

  // Unset channel[] if buf[] contains "-!-" followed by it.
  if(channel)
    snprintf(server, sizeof(server), "-!- %s", channel);
  if(strstr(buf, server))
    channel="";

  // Create (if non-existant), open outfile.
  create_filepath(outfile, sizeof(outfile), channel, "out");
  if(!(out = fopen(outfile, "a")))
    return;

  // Only add channel if channel[] is set /appropriately/.
  if(channel && channel[0])
    add_channel(channel);

  // Finish by printing out buf[], prefixed with localtime string.
  strftime(buft, sizeof(buft), "%F %T", localtime(&t));
  fprintf(out, "%s %s\n", buft, buf);
  fclose(out); }

static void proc_channels_privmsg(char *channel, char *buf) {
// Translate buf[] into message to channel/user, write to outfile and socket.
  snprintf(message, PIPE_BUF, "<%s> %s", nick, buf);
  print_out(channel, message);
  snprintf(message, PIPE_BUF, "PRIVMSG %s :%s\r\n", channel, buf);
  write(irc, message, strlen(message)); }

static void proc_channels_input(Channel *c, char *buf) {
// Translate buf[] to message to write to socket; interpret commands and act accordingly.
  char *p = NULL;

  // Treat legal lines not starting with '/' as messages directly to a channel / user.
  if(buf[0] != '/' && buf[0] != 0) {
    proc_channels_privmsg(c->name, buf);
    return; }

  // Legal messages will start with anything but a null byte.
  message[0] = '\0';

  // Handle ii-specific single char commands.
  if(buf[2] == ' ' || buf[2] == '\0')
    switch (buf[1]) {

      // Either "JOIN" a channel or send a message to another user.
      case 'j':
        p = strchr(&buf[3], ' ');
        if(p)
          *p = 0;
        if((buf[3]=='#')||(buf[3]=='&')||(buf[3]=='+')||(buf[3]=='!')){
          if(p)
            snprintf(message, PIPE_BUF, "JOIN %s %s\r\n", &buf[3], p + 1); /* password protected channel */
          else
            snprintf(message, PIPE_BUF, "JOIN %s\r\n", &buf[3]);
          add_channel(&buf[3]); }
        else {
          if(p){
            add_channel(&buf[3]);
            proc_channels_privmsg(&buf[3], p + 1);
            return; } }
        break;

      // Set channel topic via "TOPIC" command to server.
      case 't':
        if(strlen(buf)>=3)
          snprintf(message, PIPE_BUF, "TOPIC %s :%s\r\n", c->name, &buf[3]);
        break;

      // Deliver "AWAY" to socket; only write to outfile if an away message is provided.
      case 'a':
        if(strlen(buf)>=3){
          snprintf(message, PIPE_BUF, "-!- %s is away \"%s\"", nick, &buf[3]);
          print_out(c->name, message); }
        if(buf[2] == 0 || strlen(buf)<3) /* or used to make else part safe */
          snprintf(message, PIPE_BUF, "AWAY\r\n");
        else
          snprintf(message, PIPE_BUF, "AWAY :%s\r\n", &buf[3]);
        break;

      // Change nick, towards ii process and via "NICK" command towards server.
      case 'n':
        if(strlen(buf)>=3){
          snprintf(nick, sizeof(nick),"%s", &buf[3]);
          snprintf(message, PIPE_BUF, "NICK %s\r\n", &buf[3]); }
        break;

      // Leave channel via "PART"; provide ii default leave message; remove channel.
      case 'l':
        if(c->name[0] == 0)
          return;
        if(buf[2] == ' ' && strlen(buf)>=3)
          snprintf(message, PIPE_BUF, "PART %s :%s\r\n", c->name, &buf[3]);
        else
          snprintf(message, PIPE_BUF,"PART %s :ii - 500 SLOC are too much\r\n", c->name);
        write(irc, message, strlen(message));
        close(c->fd);
        rm_channel(c);
        return;

  // Anything not defined by ii as a known command gets written to message[] directly.
      default:
        snprintf(message, PIPE_BUF, "%s\r\n", &buf[1]);
        break; }
  else
    snprintf(message, PIPE_BUF, "%s\r\n", &buf[1]);

  // Write legal message to socket.
  if (message[0] != '\0')
    write(irc, message, strlen(message)); }

static void proc_server_cmd(char *buf) {
// Interpret line from server; if appropriate, send PONG to server or write message to appropriate outfile.
  char *argv[TOK_LAST], *cmd = NULL, *p = NULL;
  int i;
  if(!buf || *buf=='\0')
    return;
  for(i = 0; i < TOK_LAST; i++)
    argv[i] = NULL;

  // Use buf[] as cmd[], unless it opens with ':': Then use next chars as "NICKSRV" token and the chars after
  // a first '!' as "USR" token; use as cmd[] what remains in buf[] after first whitespace sequence.
  if(buf[0] == ':') {		/* check prefix */
    if (!(p = strchr(buf, ' ')))
      return;
    *p = 0;
    for(++p; *p == ' '; p++);
    cmd = p;
    argv[TOK_NICKSRV] = &buf[1];
    if((p = strchr(buf, '!'))) {
      *p = 0;
      argv[TOK_USER] = ++p; } }
  else
    cmd = buf;

  // Replace '\r', '\n' chars in cmd[] with '\0'.
  for(p = cmd; p && *p != 0; p++)
    if(*p == '\r' || *p == '\n')
      *p = 0;

  // If cmd[] contains ':', save next address as start of "TEXT" token.
  if((p = strchr(cmd, ':'))) {
    *p = 0;
    argv[TOK_TEXT] = ++p; }

  // In TOK_CMD, save chunks separated by ' ' as tokens TOK_CMD, [TOK_CHAN], [TOK_ARG], [TOK_TXT]. 
  tokenize(&argv[TOK_CMD], TOK_LAST - TOK_CMD, cmd, ' ');

  // Do nothing if no TOK_CMD, or if "PONG" received.
  if(!argv[TOK_CMD] || !strncmp("PONG", argv[TOK_CMD], 5)) {
    return; }

  // Answer "PING" with "PONG" followed by TOK_TEXT and leave.
  else if(!strncmp("PING", argv[TOK_CMD], 5)) {
    snprintf(message, PIPE_BUF, "PONG %s\r\n", argv[TOK_TEXT]);
    write(irc, message, strlen(message));
    return; }

  // If no TOK_NICKSRV / TOK_USER, append TOK_ARG + TOK_TEXT directly to meta out file and leave.
  else if(!argv[TOK_NICKSRV] || !argv[TOK_USER]) {
    snprintf(message, PIPE_BUF, "%s%s", argv[TOK_ARG] ? argv[TOK_ARG] : "", argv[TOK_TEXT] ? argv[TOK_TEXT] : "");
    print_out(0, message);
    return; }

  // If "ERROR" received, write error description into message[].
  else if(!strncmp("ERROR", argv[TOK_CMD], 6))
    snprintf(message, PIPE_BUF, "-!- error %s", argv[TOK_TEXT] ? argv[TOK_TEXT] : "unknown");

  // Translate "JOIN" command to message, but only up to the first whitespace of the TOK_TEXT string.
  else if(!strncmp("JOIN", argv[TOK_CMD], 5)) {
    if(argv[TOK_TEXT] != NULL) {
      p = strchr(argv[TOK_TEXT], ' ');
      if(p)
        *p = 0; }
    argv[TOK_CHAN] = argv[TOK_TEXT];
    snprintf(message, PIPE_BUF, "-!- %s(%s) has joined %s", argv[TOK_NICKSRV], argv[TOK_USER], argv[TOK_TEXT]); }

  // Translate other commands to respective messages.
  else if(!strncmp("PART", argv[TOK_CMD], 5)) {
    snprintf(message, PIPE_BUF, "-!- %s(%s) has left %s", argv[TOK_NICKSRV], argv[TOK_USER], argv[TOK_CHAN]); }
  else if(!strncmp("MODE", argv[TOK_CMD], 5))
    snprintf(message, PIPE_BUF, "-!- %s changed mode/%s -> %s %s", argv[TOK_NICKSRV],
             argv[TOK_CMD + 1] ? argv[TOK_CMD + 1] : "" , argv[TOK_CMD + 2]? argv[TOK_CMD + 2] : "", argv[TOK_CMD + 3] ? argv[TOK_CMD + 3] : "");
  else if(!strncmp("QUIT", argv[TOK_CMD], 5))
    snprintf(message, PIPE_BUF, "-!- %s(%s) has quit \"%s\"", argv[TOK_NICKSRV], argv[TOK_USER], argv[TOK_TEXT] ? argv[TOK_TEXT] : "");
  else if(!strncmp("NICK", argv[TOK_CMD], 5))
    snprintf(message, PIPE_BUF, "-!- %s changed nick to %s", argv[TOK_NICKSRV], argv[TOK_TEXT]);
  else if(!strncmp("TOPIC", argv[TOK_CMD], 6))
    snprintf(message, PIPE_BUF, "-!- %s changed topic to \"%s\"", argv[TOK_NICKSRV], argv[TOK_TEXT] ? argv[TOK_TEXT] : "");
  else if(!strncmp("KICK", argv[TOK_CMD], 5))
    snprintf(message, PIPE_BUF, "-!- %s kicked %s (\"%s\")", argv[TOK_NICKSRV], argv[TOK_ARG], argv[TOK_TEXT] ? argv[TOK_TEXT] : "");
  else if(!strncmp("NOTICE", argv[TOK_CMD], 7))
    snprintf(message, PIPE_BUF, "-!- \"%s\")", argv[TOK_TEXT] ? argv[TOK_TEXT] : "");
  else if(!strncmp("PRIVMSG", argv[TOK_CMD], 8))
    snprintf(message, PIPE_BUF, "<%s> %s", argv[TOK_NICKSRV], argv[TOK_TEXT] ? argv[TOK_TEXT] : "");

  // Write message to (if token provided) channel/user!=nick or to server outfile.
  if(!argv[TOK_CHAN] || !strncmp(argv[TOK_CHAN], nick, strlen(nick)))
    print_out(argv[TOK_NICKSRV], message);
  else
    print_out(argv[TOK_CHAN], message); }

static int read_line(int fd, size_t res_len, char *buf) {
// Read into buf[] one line, up to size res_len. End line with '\0'.
  size_t i = 0;
  char c = 0;
  do {
    if(read(fd, &c, sizeof(char)) != sizeof(char))
      return -1;
    buf[i++] = c;
  } while(c != '\n' && i < res_len);
  buf[i - 1] = 0;
  return 0; }

static void handle_channels_input(Channel *c) {
// Try to read line from fifo for processing; if failure, try to re-open channel fifo before removing channel.
  static char buf[PIPE_BUF];
  if(read_line(c->fd, PIPE_BUF, buf) == -1) {
    close(c->fd);
    int fd = open_channel(c->name);
    if(fd != -1)
      c->fd = fd;
    else
      rm_channel(c);
    return; }
  proc_channels_input(c, buf); }

static void handle_server_output() {
// Try to read line from socket, process it with proc_server_cmd().
  static char buf[PIPE_BUF];
  if(read_line(irc, PIPE_BUF, buf) == -1) {
    perror("ii: remote host closed connection");
    exit(EXIT_FAILURE); }
  proc_server_cmd(buf); }

static void run() {
// Repeatedly check socket and fifo descriptors, handle input / output.
  Channel *c;
  int r, maxfd;
  fd_set rd;
  struct timeval tv;
  char ping_msg[512];
  snprintf(ping_msg, sizeof(ping_msg), "PING %s\r\n", host);
  for(;;) {

    // Put socket and channel fifo descriptors into fd_set "rd".
    FD_ZERO(&rd);
    maxfd = irc;
    FD_SET(irc, &rd);
    for(c = channels; c; c = c->next) {
      if(maxfd < c->fd)
        maxfd = c->fd;
      FD_SET(c->fd, &rd); }

    // Use select() to check file descriptors' read-access readiness. Exit on failure.
    tv.tv_sec = 120;
    tv.tv_usec = 0;
    r = select(maxfd + 1, &rd, 0, 0, &tv);
    if(r < 0) {
      if(errno == EINTR)
        continue;
      perror("ii: error on select()");
      exit(EXIT_FAILURE); }

    // If select() time-outs, check for ping timeout, ping to socket. 
    else if(r == 0) {
      if(time(NULL) - last_response >= PING_TIMEOUT) {
        print_out(NULL, "-!- ii shutting down: ping timeout");
        exit(EXIT_FAILURE); }
      write(irc, ping_msg, strlen(ping_msg));
      continue; }

    // Else, handle server output / channel inputs, reset last_response.
    if(FD_ISSET(irc, &rd)) {
      handle_server_output();
      last_response = time(NULL); }
    for(c = channels; c; c = c->next)
      if(FD_ISSET(c->fd, &rd))
        handle_channels_input(c); } }

int main(int argc, char *argv[]) {
  int i;
  unsigned short port = SERVER_PORT;
  char *key = NULL, *fullname = NULL;
  char prefix[_POSIX_PATH_MAX];

  // Derive nickname and prefix from getpwuid(getuid()).
  struct passwd *spw = getpwuid(getuid());
  if(!spw) {
    fprintf(stderr,"ii: getpwuid() failed\n");
    exit(EXIT_FAILURE); }
  snprintf(nick, sizeof(nick), "%s", spw->pw_name);
  snprintf(prefix, sizeof(prefix),"%s/irc", spw->pw_dir);

  // Print help screen if no command line argument, or "-h*".
  if (argc <= 1 || (argc == 2 && argv[1][0] == '-' && argv[1][1] == 'h'))
    usage();

  // Fill variables according to command line arguments.
  for(i = 1; (i + 1 < argc) && (argv[i][0] == '-'); i++) {
    switch (argv[i][1]) {
      case 'i': snprintf(prefix,sizeof(prefix),"%s", argv[++i]); break;
      case 's': host = argv[++i]; break;
      case 'p': port = strtol(argv[++i], NULL, 10); break;
      case 'n': snprintf(nick,sizeof(nick),"%s", argv[++i]); break;
      case 'k': key = argv[++i]; break;
      case 'f': fullname = argv[++i]; break;
      default: usage(); break; } }

  // Open socket to IRC server.
  irc = tcpopen(port);

  // Set and, if necessary, create path: homedir prefix + "/" + host.
  if(!snprintf(path, sizeof(path), "%s/%s", prefix, host)) {
    fprintf(stderr, "%s", "ii: path to irc directory too long\n");
    exit(EXIT_FAILURE); }
  create_dirtree(path);

  // Open server master channel; write login data to socket; start loop handling input/output.
  add_channel("");
  login(key, fullname);
  run();
  return 0; }
