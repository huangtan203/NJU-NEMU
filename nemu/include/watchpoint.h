#ifndef __WATCHPOINT_H__
#define __WATCHPOINT_H__

#include <common.h>

typedef struct watchpoint {
  int NO;
  struct watchpoint *next;
  char expr[32];
  word_t last_val;
} WP;

int set_watchpoint(char *e);
int delete_watchpoint(int no);
void list_watchpoint();
int scan_watchpoint();

#endif
