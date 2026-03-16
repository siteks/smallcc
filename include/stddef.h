#ifndef STDDEF_H
#define STDDEF_H

typedef unsigned int size_t;
typedef int          ptrdiff_t;
#define NULL         ((void *)0)
#define offsetof(type, member) ((size_t)&((type *)0)->member)

#endif
