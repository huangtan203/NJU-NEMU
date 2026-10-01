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

#include <isa.h>
#include <memory/vaddr.h>
#include <regex.h>
#include <stdlib.h>

enum {
  TK_NOTYPE = 256, TK_EQ, TK_NEQ, TK_AND, TK_OR,
  TK_NUM, TK_REG, TK_HEXNUM,
};

static struct rule {
  const char *regex;
  int token_type;
} rules[] = {
  {" +", TK_NOTYPE},
  {"0[xX][0-9a-fA-F]+", TK_HEXNUM},
  {"[0-9]+", TK_NUM},
  {"\\$[$]?[a-zA-Z0-9]+", TK_REG},
  {"\\+", '+'},
  {"-", '-'},
  {"\\*", '*'},
  {"/", '/'},
  {"\\(", '('},
  {"\\)", ')'},
  {"==", TK_EQ},
  {"!=", TK_NEQ},
  {"&&", TK_AND},
  {"\\|\\|", TK_OR},
};

#define NR_REGEX ARRLEN(rules)
static regex_t re[NR_REGEX] = {};

void init_regex() {
  char error_msg[128];
  for (int i = 0; i < NR_REGEX; i ++) {
    int ret = regcomp(&re[i], rules[i].regex, REG_EXTENDED);
    if (ret != 0) {
      regerror(ret, &re[i], error_msg, 128);
      panic("regex compilation failed: %s\n%s", error_msg, rules[i].regex);
    }
  }
}

typedef struct token {
  int type;
  char str[32];
} Token;

static Token tokens[256] = {};
static int nr_token = 0;

static bool make_token(char *e) {
  int position = 0;
  nr_token = 0;
  while (e[position] != '\0') {
    regmatch_t pmatch;
    int i;
    for (i = 0; i < NR_REGEX; i ++) {
      if (regexec(&re[i], e + position, 1, &pmatch, 0) == 0 && pmatch.rm_so == 0) {
        char *substr_start = e + position;
        int substr_len = pmatch.rm_eo;
        position += substr_len;
        if (rules[i].token_type != TK_NOTYPE) {
          Assert(nr_token < 256, "too many tokens");
          strncpy(tokens[nr_token].str, substr_start, substr_len);
          tokens[nr_token].str[substr_len] = '\0';
          tokens[nr_token].type = rules[i].token_type;
          nr_token++;
        }
        break;
      }
    }
    if (i == NR_REGEX) {
      printf("no match at position %d\n%s\n%*.s^\n", position, e, position, "");
      return false;
    }
  }
  return true;
}

static word_t eval(int p, int q);

word_t expr(char *e, bool *success) {
  if (!make_token(e)) { *success = false; return 0; }
  if (nr_token == 0) { *success = false; return 0; }
  *success = true;
  return eval(0, nr_token - 1);
}

static bool check_parentheses(int p, int q) {
  if (tokens[p].type != '(' || tokens[q].type != ')') return false;
  int cnt = 0;
  for (int i = p; i <= q; i++) {
    if (tokens[i].type == '(') cnt++;
    else if (tokens[i].type == ')') {
      cnt--;
      if (cnt == 0 && i != q) return false;
    }
  }
  return cnt == 0;
}

static int find_main_op(int p, int q) {
  int cnt = 0, op = -1, op_prio = 100;
  for (int i = p; i <= q; i++) {
    if (tokens[i].type == '(') { cnt++; continue; }
    if (tokens[i].type == ')') { cnt--; continue; }
    if (cnt > 0) continue;
    int prio = 0;
    switch (tokens[i].type) {
      case TK_OR:  prio = 1; break;
      case TK_AND: prio = 2; break;
      case TK_EQ: case TK_NEQ: prio = 3; break;
      case '+': case '-': prio = 4; break;
      case '*': case '/': prio = 5; break;
      default: continue;
    }
    if (prio <= op_prio) { op_prio = prio; op = i; }
  }
  return op;
}

static bool is_unary(int p) {
  if (p == 0) return true;
  int t = tokens[p-1].type;
  return t == '(' || t == '+' || t == '-' || t == '*' || t == '/' ||
         t == TK_EQ || t == TK_NEQ || t == TK_AND || t == TK_OR;
}

static word_t eval(int p, int q) {
  if (p > q) return 0;
  if (p == q) {
    switch (tokens[p].type) {
      case TK_NUM:    return strtol(tokens[p].str, NULL, 10);
      case TK_HEXNUM: return strtol(tokens[p].str, NULL, 16);
      case TK_REG: {
        bool s; word_t v = isa_reg_str2val(tokens[p].str, &s);
        if (!s) printf("Invalid register: %s\n", tokens[p].str);
        return v;
      }
      default: printf("Invalid token: %s\n", tokens[p].str); return 0;
    }
  }
  if (check_parentheses(p, q)) return eval(p + 1, q - 1);

  // unary minus/plus/deref
  if (tokens[p].type == '-' && is_unary(p)) return -eval(p + 1, q);
  if (tokens[p].type == '+' && is_unary(p)) return eval(p + 1, q);
  if (tokens[p].type == '*' && is_unary(p)) return vaddr_read(eval(p + 1, q), 4);

  int op = find_main_op(p, q);
  if (op == -1) return 0;
  word_t v1 = eval(p, op - 1);
  word_t v2 = eval(op + 1, q);
  switch (tokens[op].type) {
    case '+': return v1 + v2;
    case '-': return v1 - v2;
    case '*': return v1 * v2;
    case '/': return v2 != 0 ? v1 / v2 : 0;
    case TK_EQ:  return v1 == v2;
    case TK_NEQ: return v1 != v2;
    case TK_AND: return v1 && v2;
    case TK_OR:  return v1 || v2;
    default: return 0;
  }
}
