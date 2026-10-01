#include <am.h>
#include <klib.h>
#include <klib-macros.h>
#include <stdarg.h>

#if !defined(__ISA_NATIVE__) || defined(__NATIVE_USE_KLIB__)

static int print_num(char *buf, int pos, unsigned int num, int base, int width, int zero_pad, int left_align, int sign, int upper) {
  char tmp[32];
  int i = 0;
  const char *digits = upper ? "0123456789ABCDEF" : "0123456789abcdef";
  
  if (num == 0) {
    tmp[i++] = '0';
  } else {
    while (num > 0) {
      tmp[i++] = digits[num % base];
      num /= base;
    }
  }
  
  if (sign && width > 0 && !zero_pad && !left_align) {
    while (i < width) { buf[pos++] = ' '; width--; }
  }
  if (sign) {
    buf[pos++] = sign;
    width--;
  }
  if (width > 0 && zero_pad && !left_align) {
    while (i < width) { buf[pos++] = '0'; width--; }
  }
  
  for (int j = i - 1; j >= 0; j--) {
    buf[pos++] = tmp[j];
  }
  if (width > i && left_align) {
    while (width-- > i) buf[pos++] = ' ';
  }
  return pos;
}

int vsnprintf(char *out, size_t n, const char *fmt, va_list ap) {
  int pos = 0;
  for (int i = 0; fmt[i] && pos < (int)n - 1; i++) {
    if (fmt[i] != '%') {
      out[pos++] = fmt[i];
      continue;
    }
    i++;
    
    int left_align = 0, zero_pad = 0, width = 0;
    if (fmt[i] == '-') { left_align = 1; i++; }
    if (fmt[i] == '0') { zero_pad = 1; i++; }
    while (fmt[i] >= '0' && fmt[i] <= '9') {
      width = width * 10 + (fmt[i] - '0');
      i++;
    }
    
    char sign = 0;
    switch (fmt[i]) {
      case 'd': case 'i': {
        int val = va_arg(ap, int);
        unsigned int uval;
        if (val < 0) { sign = '-'; uval = (unsigned int)(-val); }
        else uval = (unsigned int)val;
        pos = print_num(out, pos, uval, 10, width, zero_pad, left_align, sign, 0);
        break;
      }
      case 'u': {
        unsigned int val = va_arg(ap, unsigned int);
        pos = print_num(out, pos, val, 10, width, zero_pad, left_align, 0, 0);
        break;
      }
      case 'x': case 'X': {
        unsigned int val = va_arg(ap, unsigned int);
        pos = print_num(out, pos, val, 16, width, zero_pad, left_align, 0, fmt[i] == 'X');
        break;
      }
      case 'p': {
        void *ptr = va_arg(ap, void *);
        out[pos++] = '0'; out[pos++] = 'x';
        pos = print_num(out, pos, (unsigned int)(uintptr_t)ptr, 16, 0, 0, 0, 0, 0);
        break;
      }
      case 'c': {
        char c = (char)va_arg(ap, int);
        out[pos++] = c;
        break;
      }
      case 's': {
        const char *s = va_arg(ap, const char *);
        if (!s) s = "(null)";
        while (*s && pos < (int)n - 1) out[pos++] = *s++;
        break;
      }
      case '%': {
        out[pos++] = '%';
        break;
      }
      default:
        out[pos++] = fmt[i];
        break;
    }
    if (pos >= (int)n - 1) break;
  }
  out[pos] = '\0';
  return pos;
}

int snprintf(char *out, size_t n, const char *fmt, ...) {
  va_list ap;
  va_start(ap, fmt);
  int ret = vsnprintf(out, n, fmt, ap);
  va_end(ap);
  return ret;
}

int vsprintf(char *out, const char *fmt, va_list ap) {
  return vsnprintf(out, (size_t)-1, fmt, ap);
}

int sprintf(char *out, const char *fmt, ...) {
  va_list ap;
  va_start(ap, fmt);
  int ret = vsprintf(out, fmt, ap);
  va_end(ap);
  return ret;
}

int printf(const char *fmt, ...) {
  char buf[1024];
  va_list ap;
  va_start(ap, fmt);
  int n = vsnprintf(buf, sizeof(buf), fmt, ap);
  va_end(ap);
  putstr(buf);
  return n;
}

#endif
