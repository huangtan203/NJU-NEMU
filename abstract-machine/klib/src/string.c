#include <klib.h>
#include <klib-macros.h>
#include <stdint.h>

#if !defined(__ISA_NATIVE__) || defined(__NATIVE_USE_KLIB__)

size_t strlen(const char *s) {
  size_t n = 0;
  while (s[n] != '\0') n++;
  return n;
}

char *strcpy(char *dst, const char *src) {
  char *ret = dst;
  while ((*dst++ = *src++) != '\0');
  return ret;
}

char *strncpy(char *dst, const char *src, size_t n) {
  char *ret = dst;
  size_t i = 0;
  for (i = 0; i < n && src[i] != '\0'; i++) dst[i] = src[i];
  for (; i < n; i++) dst[i] = '\0';
  return ret;
}

char *strcat(char *dst, const char *src) {
  char *ret = dst;
  while (*dst) dst++;
  while ((*dst++ = *src++) != '\0');
  return ret;
}

int strcmp(const char *s1, const char *s2) {
  while (*s1 && *s2 && *s1 == *s2) { s1++; s2++; }
  return (unsigned char)*s1 - (unsigned char)*s2;
}

int strncmp(const char *s1, const char *s2, size_t n) {
  for (size_t i = 0; i < n; i++) {
    if (s1[i] != s2[i]) return (unsigned char)s1[i] - (unsigned char)s2[i];
    if (s1[i] == '\0') return 0;
  }
  return 0;
}

void *memset(void *s, int c, size_t n) {
  unsigned char *p = s;
  while (n-- > 0) *p++ = (unsigned char)c;
  return s;
}

void *memmove(void *dst, const void *src, size_t n) {
  unsigned char *d = dst;
  const unsigned char *s = src;
  if (d < s) {
    while (n--) *d++ = *s++;
  } else {
    d += n; s += n;
    while (n--) *--d = *--s;
  }
  return dst;
}

void *memcpy(void *out, const void *in, size_t n) {
  unsigned char *d = out;
  const unsigned char *s = in;
  while (n--) *d++ = *s++;
  return out;
}

int memcmp(const void *s1, const void *s2, size_t n) {
  const unsigned char *p1 = s1, *p2 = s2;
  while (n--) {
    if (*p1 != *p2) return *p1 - *p2;
    p1++; p2++;
  }
  return 0;
}

#endif
