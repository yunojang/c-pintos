#include "lib/kernel/list.h"

#ifndef USERPROG_SYSCALL_H
#define USERPROG_SYSCALL_H

void syscall_init(void);

#endif /* userprog/syscall.h */

struct fd_elem
{
    int fd;
    struct file *file;
    struct list_elem elem;
};

void handle_exit(int status);
