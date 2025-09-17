#include "userprog/syscall.h"
#include <stdio.h>
#include <syscall-nr.h>
#include "threads/interrupt.h"
#include "threads/thread.h"
#include "threads/loader.h"
#include "userprog/gdt.h"
#include "threads/flags.h"
#include "intrinsic.h"
#include "threads/malloc.h"
#include "include/filesys/directory.h"
#include "filesys/filesys.h"

#define STDOUT_FD 1

void syscall_entry(void);
void syscall_handler(struct intr_frame *);

static bool valid_uaddr(const char *uaddr);
static size_t copy_in_string(char *dst, const char *src, size_t max);

static void handle_exit(int status);
static int handle_read(int fd, void *buffer, unsigned size);

/* System call.
 *
 * Previously system call services was handled by the interrupt handler
 * (e.g. int 0x80 in linux). However, in x86-64, the manufacturer supplies
 * efficient path for requesting the system call, the `syscall` instruction.
 *
 * The syscall instruction works by reading the values from the the Model
 * Specific Register (MSR). For the details, see the manual. */

#define MSR_STAR 0xc0000081			/* Segment selector msr */
#define MSR_LSTAR 0xc0000082		/* Long mode SYSCALL target */
#define MSR_SYSCALL_MASK 0xc0000084 /* Mask for the eflags */

void syscall_init(void)
{
	write_msr(MSR_STAR, ((uint64_t)SEL_UCSEG - 0x10) << 48 |
							((uint64_t)SEL_KCSEG) << 32);
	write_msr(MSR_LSTAR, (uint64_t)syscall_entry);

	/* The interrupt service rountine should not serve any interrupts
	 * until the syscall_entry swaps the userland stack to the kernel
	 * mode stack. Therefore, we masked the FLAG_FL. */
	write_msr(MSR_SYSCALL_MASK,
			  FLAG_IF | FLAG_TF | FLAG_DF | FLAG_IOPL | FLAG_AC | FLAG_NT);
}

// struct --
struct fd_elem
{
	int fd;
	struct file *file;
	struct list_elem elem;
};
// ---

// utils ---

static bool valid_uaddr(const char *uaddr)
{
	return uaddr != NULL && is_user_vaddr(uaddr) && pml4_get_page(thread_current()->pml4, (void *)uaddr) != NULL;
}

static size_t copy_in_string(char *dst, const char *src, size_t max)
{
	size_t n = 0;
	struct thread *t = thread_current();

	while (n < max)
	{
		const char *p = src + n;
		if (!valid_uaddr(p))
		{
			handle_exit(-1);
		}

		char c = *p;
		dst[n++] = c;
		if (c == '\0')
		{
			return n - 1;
		}
	}
	return n;
}

static char *copy_file(char *file)
{
	ASSERT(file != NULL);

	char *name = malloc(NAME_MAX + 1);
	size_t len;
	if ((len = copy_in_string(name, file, NAME_MAX + 1)) > NAME_MAX)
	{
		return NULL;
	}

	return name;
}

// refactor -> min fd
static int next_fd = 2;

static int fd_install()
{
	return next_fd++;
}

// ---

// list utils

typedef bool list_match_func(const struct list_elem *a, void *aux);

static struct list_elem *list_find(struct list *l, list_match_func match, void *aux)
{
	ASSERT(l != NULL);
	struct list_elem *cur;

	for (cur = list_begin(l); cur != list_end(l); cur = list_next(cur))
	{
		if (match(cur, aux))
		{
			return cur;
		}
	}

	return NULL;
}

// ---

static int handle_read(int fd, void *buffer, unsigned size)
{
}

static bool match_fd(const struct list_elem *a, void *aux)
{
	int fd = (int)aux;
	return list_entry(a, struct fd_elem, elem)->fd == fd;
}

static struct fd_elem *find_fd_elem(struct list *l, int find_fd)
{
	struct list_elem *elem;
	if ((elem = list_find(l, match_fd, find_fd)) == NULL)
	{
		return NULL;
	}

	return list_entry(list_find(l, match_fd, find_fd), struct fd_elem, elem);
}

static void handle_close(int fd)
{
	struct thread *t = thread_current();
	struct fd_elem *fe;

	if ((fe = find_fd_elem(&t->fds, fd)) == NULL)
	{
		return;
	}

	list_remove(&fe->elem);
}

static bool lower_fd(const struct list_elem *new, const struct list_elem *item, void *aux)
{
	int new_fd = list_entry(new, struct fd_elem, elem)->fd;
	int item_fd = list_entry(item, struct fd_elem, elem)->fd;

	return new_fd < item_fd;
}

static int handle_open(char *file)
{
	ASSERT(file != NULL);
	struct thread *t = thread_current();

	char *name;
	if ((name = copy_file(file)) == NULL)
	{
		return -1;
	}

	struct file *f = filesys_open(file);
	// file not in dir
	if (f == NULL)
	{
		return -1;
	}

	struct fd_elem *fe = malloc(sizeof *fe);
	fe->fd = fd_install();
	fe->file = f;

	list_insert_ordered(&t->fds, &fe->elem, lower_fd, NULL);
	return fe->fd;
}

static bool handle_create(char *file, unsigned int initial_size)
{
	ASSERT(file != NULL);

	char *name;
	if ((name = copy_file(file)) == NULL)
	{
		return false;
	}

	bool success = filesys_create(name, initial_size);
	free(name);
	return success;
}

static void handle_exit(int status)
{
	struct thread *cur = thread_current();
	cur->exit_status = status;

	// fd 정리

	// exit msg
	printf("%s: exit(%d)\n", cur->name, cur->exit_status);

	// 부모 통지 (msg 출력 후 통지)
	sema_up(&cur->cs->dead);
	thread_exit();
}

static int handle_write(int fd, const void *uaddr, size_t n)
{
	if (fd != STDOUT_FD)
	{
		return -1;
	}
	if (n == 0)
	{
		return 0;
	}

	// uaddr ~ 'uaddr + n' -> 유저영역 & 읽기가능?
	if (!is_user_vaddr(uaddr))
	{
		return -1;
	}

	// 청크 copy-in -> 콘솔 출력
	putbuf(uaddr, n);

	return n;
}

/* The main system call interface */
void syscall_handler(struct intr_frame *f UNUSED)
{
	switch (f->R.rax)
	{
	case SYS_EXIT:
		handle_exit(f->R.rdi);
		break;
	case SYS_HALT:
		power_off();
		break;
	case SYS_CREATE:
		if (f->R.rdi == NULL)
		{
			handle_exit(-1);
		}
		f->R.rax = handle_create(f->R.rdi, f->R.rsi) ? 1 : 0;
		break;
	case SYS_OPEN:
		if (f->R.rdi == NULL)
		{
			handle_exit(-1);
		}
		f->R.rax = handle_open(f->R.rdi);
		break;
	case SYS_READ:
		int fd = (int)f->R.rdi;
		f->R.rax = handle_read(fd, f->R.rsi, f->R.rdx);
		break;
	case SYS_CLOSE:
		handle_close(f->R.rdi);
		break;
	case SYS_WRITE:
		int fd = (int)f->R.rdi;
		f->R.rax = handle_write(fd, f->R.rsi, f->R.rdx);
		break;
	default:
		break;
	}
}
