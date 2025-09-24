#include "lib/kernel/list.h"

#ifndef USERPROG_SYSCALL_H
#define USERPROG_SYSCALL_H

void syscall_init(void);

#endif /* userprog/syscall.h */

#define STDIN_FD 0
#define STDOUT_FD 1

// #define MAX_FD 126

enum fd_type
{
    FD_FILE,
    FD_STD_IN,
    FD_STD_OUT,
};

// struct ofile
// {
//     struct file *file;
// };

struct fd_elem
{
    int fd;
    enum fd_type type;
    struct file *file;
    struct list_elem elem;
};

bool init_fds(struct list *fds);
void handle_exit(int status);
