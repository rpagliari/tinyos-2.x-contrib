/*
 * "Copyright (c) 2008 The Regents of the University  of California.
 * All rights reserved."
 *
 * Permission to use, copy, modify, and distribute this software and its
 * documentation for any purpose, without fee, and without written agreement is
 * hereby granted, provided that the above copyright notice, the following
 * two paragraphs and the author appear in all copies of this software.
 *
 * IN NO EVENT SHALL THE UNIVERSITY OF CALIFORNIA BE LIABLE TO ANY PARTY FOR
 * DIRECT, INDIRECT, SPECIAL, INCIDENTAL, OR CONSEQUENTIAL DAMAGES ARISING OUT
 * OF THE USE OF THIS SOFTWARE AND ITS DOCUMENTATION, EVEN IF THE UNIVERSITY OF
 * CALIFORNIA HAS BEEN ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 *
 * THE UNIVERSITY OF CALIFORNIA SPECIFICALLY DISCLAIMS ANY WARRANTIES,
 * INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY
 * AND FITNESS FOR A PARTICULAR PURPOSE.  THE SOFTWARE PROVIDED HEREUNDER IS
 * ON AN "AS IS" BASIS, AND THE UNIVERSITY OF CALIFORNIA HAS NO OBLIGATION TO
 * PROVIDE MAINTENANCE, SUPPORT, UPDATES, ENHANCEMENTS, OR MODIFICATIONS."
 *
 */
/*
 * @author Stephen Dawson-Haggerty <stevedh@eecs.berkeley.edu>
 */

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <signal.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <arpa/inet.h>
#include <string.h>
#include <stdarg.h>

#include "vty.h"
#include "../logging.h"

#define max(X,Y)  (((X) > (Y)) ?  (X) : (Y))

typedef enum {
  FALSE = 0,
  TRUE  = 1,
} bool;

enum {
  VTY_REMOVE_PENDING = 0x1,
};

struct vty_client {
  int                 flags;
  int                 readfd;
  int                 writefd;
  struct sockaddr_in  ep;
  struct vty_client  *next;
  
  int                 buf_off;
  unsigned char       buf[1024];
};

static int sock = -1;
static struct vty_client *conns = NULL;
static struct vty_cmd_table *cmd_tab;
char prompt_str[40];

int vty_init(struct vty_cmd_table * cmds, short port) {
  struct sockaddr_in si_me;
  struct vty_client *tty_client;
  int len, yes = 1;

  conns = NULL;
  cmd_tab = cmds;

  strncpy(prompt_str, "blip:", 5);
  gethostname(prompt_str+5, sizeof(prompt_str)- 5);
  len=strlen(prompt_str+5);
  prompt_str[len+5]='>';
  prompt_str[len+6]=' ';
  prompt_str[len+7]='\0';

  if (isatty(fileno(stdin))) {
    setbuf(stdin, NULL);
    tty_client = (struct vty_client *)malloc(sizeof(struct vty_client));
    memset(tty_client, 0, sizeof(struct vty_client));
    tty_client->flags = 0;
    tty_client->readfd = fileno(stdin);
    tty_client->writefd = fileno(stdout);
    tty_client->next = NULL;
    conns = tty_client;
  }

  sock = socket(PF_INET, SOCK_STREAM, 0);
  if (sock < 0) {
    perror("vty: socket");
    return -1;
  }

  if (setsockopt(sock, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes)) < 0) {
    perror("vty: setsockopt");
    goto abort;
  }

  si_me.sin_family = AF_INET;
  si_me.sin_port = htons(port);
  si_me.sin_addr.s_addr = htonl(INADDR_ANY);
  if (bind(sock, (struct sockaddr *)&si_me, sizeof(si_me)) < 0) {
    perror("vty: bind");
    goto abort;
  }

  if (listen(sock, 2) < 0) {
    perror("vty: listen");
    goto abort;
  }

  return 0;
 abort:
  close(sock);
  sock = -1;
  return -1;
}

int vty_add_fds(fd_set *fds) {
  int maxfd;
  struct vty_client *cur;
  if (sock >= 0) FD_SET(sock, fds);
  maxfd = sock;

  for (cur = conns; cur != NULL; cur = cur->next) {
    if (cur->flags & VTY_REMOVE_PENDING) continue;
    FD_SET(cur->readfd, fds);
    maxfd = max(maxfd, cur->readfd);
  }
  return maxfd;
}

static void vty_print_string(struct vty_client *c, const char *fmt, ...) {
  char buf[1024];
  int len;
  va_list ap;
  va_start(ap, fmt);
  len = vsnprintf(buf, 1024, fmt, ap);
  write(c->writefd, buf, len);
}

static void prompt(struct vty_client *c) {
  vty_print_string(c, prompt_str);
}

static void vty_new_connection() {
  struct vty_client * c = malloc(sizeof(struct vty_client));
  socklen_t len;
  if (c == NULL) return;

  len = sizeof(struct sockaddr_in);
  c->readfd = c->writefd = accept(sock, (struct sockaddr *)&c->ep, &len);
  if (c->readfd < 0) {
    error("Accept failed!\n");
    perror(0);
    free(c);
    return;
  }
  c->buf_off = 0;

  info("VTY: new connection accepted from %s\n", inet_ntoa(c->ep.sin_addr));
  vty_print_string(c, "Welcome to the blip console!\r\n");
  vty_print_string(c, " type 'help' to print the command options\r\n");
  prompt(c);
  c->flags = 0;
  c->next = conns;
  conns = c;
}

void vty_close_connection(struct vty_client *c) {
  close(c->readfd);
  if (c->readfd != c->writefd) close(c->writefd);
  c->flags |= VTY_REMOVE_PENDING;
}


void vty_dispatch_command(struct vty_client *c) {
  int   argc, i;
  char *argv[N_ARGS];
  init_argv((char *)c->buf, c->buf_off, argv, &argc);

  if (argc > 0) {
    for (i = 0; i < cmd_tab->n; i++) {
      if (strncmp(argv[0], cmd_tab->table[i].name, 
                  strlen(cmd_tab->table[i].name)) == 0) {
        cmd_tab->table[i].fn(c->writefd, argc, argv);
        break;
      }
      
      if (strncmp(argv[0], "quit", 4) == 0) {
        vty_close_connection(c);
        return;
      }
    }
  }
  prompt(c);
}

void vty_handle_data(struct vty_client *c) {
  int len, i;
  bool prompt_pending = FALSE;
  len = read(c->readfd, c->buf + c->buf_off, sizeof(c->buf) - c->buf_off);
  if (len <= 0) {
    warn("Invalid read on connection from %s: closing\n", 
         inet_ntoa(c->ep.sin_addr));
    vty_close_connection(c);
  }
  c->buf_off += len;
  // try to scan the whole line we're building up and remove/process
  // any telnet escapes in there
  for (i = 0; i < c->buf_off; i++) {
    int escape_len;

    if (c->buf[i] == 255) {
      escape_len = 2;
      // process and remove a command;
      // the command code is in buf[i+1]
      switch (c->buf[i+1]) {
      case TELNET_INTERRUPT:
        // ignore the command buffer we've accumulated
        memmove(&c->buf[0], &c->buf[i+2], c->buf_off - i - 2);
        c->buf_off -= i + 2;
        i = -1;
        prompt_pending = TRUE;
        continue;
      }
      if (c->buf[i+1] >= 250) {
        unsigned char response[3];
        // we don't do __anything__
        response[0] = 255;
        response[1] = TELNET_WONT;
        response[2] = c->buf[i+2];
        write(c->writefd, response, 3);
        escape_len++;
      }

      // this isn't like the most efficient parser ever, but since we
      // don't ask for anything and reply to everyone with DONT it seems okay.
      memmove(&c->buf[i], &c->buf[i+escape_len], 
              c->buf_off - (i + escape_len));
      c->buf_off -= escape_len;
      // restart processing at the same place
      i--;
    } else if (c->buf[i] == 4) {
      // EOT : make C-d work
      vty_close_connection(c);
    }
    // technically, clients are supposed to send \r\n as a newline.
    // however, the client in busybox (an important one) doesn't
    // escape the terminal input and so only sends \n.
    if (/* i > 0 && c->buf[i-1] == '\r' && */ c->buf[i] == '\n') {
      c->buf[i] = '\0';
      prompt_pending = FALSE;
      vty_dispatch_command(c);

      // start a new command at the head of the buffer
      memmove(&c->buf[0], &c->buf[i+1], c->buf_off - i - 1);
      c->buf_off -= i + 1;
      i = -1;
    }
  }
  
  if (prompt_pending) {
    vty_print_string(c, "\r\n");
    prompt(c);
    prompt_pending = FALSE;
  }
}

int vty_process(fd_set *fds) {
  struct vty_client *prev, *cur, *next;
  if (sock >= 0 && FD_ISSET(sock, fds)) {
    vty_new_connection();
  }
  for (cur = conns; cur != NULL; cur = cur->next) {
    if (FD_ISSET(cur->readfd, fds)) {
      vty_handle_data(cur);
    }
  }

  prev = NULL;
  cur = conns;
  while (cur != NULL) {
    next = cur->next;
    if (cur->flags & VTY_REMOVE_PENDING) {
      info("VTY: removing connection with endpoint %s\n", 
           inet_ntoa(cur->ep.sin_addr));
      free(cur);
      if (cur == conns) conns = next;
      if (prev != NULL) prev->next = next;
    } else {
      prev = cur;
    }
    cur = next;
  }

  return 0;
}

void vty_shutdown() {
  struct vty_client *cur = conns, *next;
  if (sock >= 0) close(sock);

  while (cur != NULL) {
    next = cur->next;
    if (!(cur->flags & VTY_REMOVE_PENDING)) {
      close(cur->readfd);
      if (cur->readfd != cur->writefd) close(cur->writefd);
    }
    free(cur);
    cur = next;
  }
}
