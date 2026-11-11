#include <stddef.h>

size_t __wrap_strlen(const char *s) {
    const char *p = s;
    while (*p) {
        ++p;
    }
    return (size_t)(p - s);
}

size_t __wrap___strlen_chk(const char *s, size_t s_len) {
    (void)s_len;
    return __wrap_strlen(s);
}

char *__wrap_strcpy(char *dest, const char *src) {
    char *d = dest;
    while ((*d++ = *src++)) {
    }
    return dest;
}
