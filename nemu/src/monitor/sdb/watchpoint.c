/***************************************************************************************
* Copyright (c) 2014-2024 Zihao Yu, Nanjing University
*
* NEMU is licensed under Mulan PSL v2.
* You can use this software according to the terms and conditions of the Mulan PSL v2.
* You may obtain a copy of Mulan PSL v2 at:
*          http://license.coscl.org.cn/MulanPSL2
*
* THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND,
* EITHER EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT,
* MERCHANTABILITY OR FIT FOR A PARTICULAR PURPOSE.
*
* See the Mulan PSL v2 for more details.
***************************************************************************************/

#include "sdb.h"
#include <watchpoint.h>

#define NR_WP 32

static WP wp_pool[NR_WP] = {};
static WP *head = NULL, *free_ = NULL;

void init_wp_pool() {
  int i;
  for (i = 0; i < NR_WP; i ++) {
    wp_pool[i].NO = i;
    wp_pool[i].next = (i == NR_WP - 1 ? NULL : &wp_pool[i + 1]);
  }

  head = NULL;
  free_ = wp_pool;
}

int set_watchpoint(char *e) {
  if (free_ == NULL) return -1;
  bool success;
  word_t val = expr(e, &success);
  if (!success) return -1;
  WP *wp = free_;
  free_ = free_->next;
  wp->next = head;
  head = wp;
  strncpy(wp->expr, e, sizeof(wp->expr) - 1);
  wp->expr[sizeof(wp->expr) - 1] = '\0';
  wp->last_val = val;
  return wp->NO;
}

int delete_watchpoint(int no) {
  WP **p = &head;
  while (*p != NULL) {
    if ((*p)->NO == no) {
      WP *wp = *p;
      *p = wp->next;
      wp->next = free_;
      free_ = wp;
      return 0;
    }
    p = &(*p)->next;
  }
  return -1;
}

void list_watchpoint() {
  if (head == NULL) {
    printf("No watchpoints.\n");
    return;
  }
  WP *p = head;
  printf("%-8s %-16s %s\n", "Num", "What", "Value");
  while (p != NULL) {
    printf("%-8d %-16s " FMT_WORD "\n", p->NO, p->expr, p->last_val);
    p = p->next;
  }
}

int scan_watchpoint() {
  WP *p = head;
  int changed = 0;
  while (p != NULL) {
    bool success;
    word_t val = expr(p->expr, &success);
    if (success && val != p->last_val) {
      printf("Watchpoint %d: %s\n", p->NO, p->expr);
      printf("Old value = " FMT_WORD "\n", p->last_val);
      printf("New value = " FMT_WORD "\n", val);
      p->last_val = val;
      changed = 1;
    }
    p = p->next;
  }
  return changed;
}
