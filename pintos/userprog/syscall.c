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
#include "threads/palloc.h"
#include "include/filesys/directory.h"
#include "filesys/filesys.h"
#include "filesys/file.h"
#include "userprog/process.h"

void syscall_entry(void);
void syscall_handler(struct intr_frame *);

static void *valid_uaddr(const char *uaddr);
static size_t copy_in_string(char *dst, const char *src, size_t max);

static int handle_filesize(int fd);
static int handle_read(int fd, void *buffer, unsigned size);
static int handle_write(int fd, const void *uaddr, size_t n);
static tid_t handle_fork(const char *thread_name, struct intr_frame *parent_if);
static int handle_wait(tid_t tid);
static int handle_exec(const char *cmd_line);
static void handle_seek(int fd, off_t position);
static off_t handle_tell(int fd);
static int handle_dup2(int oldfd, int newfd);

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

// utils ---

static void *valid_uaddr(const char *uaddr)
{
	void *ptr;

	if (uaddr == NULL || !is_user_vaddr(uaddr))
	{
		return NULL;
	}
	if ((ptr = pml4_get_page(thread_current()->pml4, (void *)uaddr)) == NULL)
	{
		return NULL;
	}

	return ptr;
}

// user -> kernel (string)
static size_t copy_in_string(char *kdst, const char *usrc, size_t max)
{
	size_t n;
	for (n = 0; n < max; n++)
	{
		const char *p = usrc + n;
		uint8_t *vaddr;
		if ((vaddr = valid_uaddr(p)) == NULL)
		{
			handle_exit(-1);
		}
		uint8_t c = *vaddr;
		((uint8_t *)kdst)[n] = c;

		if (c == '\0')
		{
			return n;
		}
	}
	return n;
}

static bool copy_in_file(const char *file, char *out)
{
	ASSERT(file != NULL);

	size_t len;
	if ((len = copy_in_string(out, file, NAME_MAX + 1)) > NAME_MAX)
	{
		return false;
	}
	return true;
}

static size_t copy_in(void *kdst, const void *usrc, size_t size)
{
	size_t n = 0;
	while (n < size)
	{
		const uint8_t *p = (uint8_t *)usrc + n;
		uint8_t *vaddr;
		if ((vaddr = valid_uaddr(p)) == NULL)
		{
			handle_exit(-1);
		}
		((uint8_t *)kdst)[n++] = *vaddr;
	}
	return n;
}

// kernel -> user
static size_t copy_out(void *udst, const void *ksrc, size_t size)
{
	struct thread *t = thread_current();
	size_t n = 0;
	while (n < size)
	{
		uint8_t *cur = (uint8_t *)udst + n;
		uint8_t *kaddr;
		if ((kaddr = valid_uaddr(cur)) == NULL
			// || !pml4_is_writable(t->pml4, cur)
		)
		{
			handle_exit(-1);
		}
		*kaddr = ((uint8_t *)ksrc)[n++];
	}
	return n;
}

static int next_fd = 3;
static int fd_install()
{
	return next_fd++;
}

// ---

// fd
static bool match_fd(const struct list_elem *a, void *aux)
{
	int fd = (int)aux;
	return list_entry(a, struct fd_elem, elem)->fd == fd;
}

static struct fd_elem *find_matched_fd(struct list *l, int find_fd)
{
	struct list_elem *le;
	if ((le = list_find(l, match_fd, find_fd)) == NULL)
	{
		return NULL;
	}

	return list_entry(le, struct fd_elem, elem);
}
// ---

static int handle_exec(const char *cmd_line)
{
	char *cmd = palloc_get_page(0);
	if (cmd == NULL)
	{
		handle_exit(-1);
		// return -1;
	}

	memset(cmd, 0, PGSIZE);
	int n = copy_in_string(cmd, cmd_line, PGSIZE);
	if (n >= PGSIZE)
	{
		handle_exit(-1);
		// return -1;
	}

	if (process_exec(cmd) < 0)
	{
		handle_exit(-1);
	}
}

static tid_t handle_fork(const char *thread_name, struct intr_frame *parent_if)
{
	char name[THREAD_NAME_MAX];
	if (copy_in_string(name, thread_name, THREAD_NAME_MAX) >= THREAD_NAME_MAX)
	{
		name[THREAD_NAME_MAX - 1] = '\0';
	}

	tid_t child_tid = process_fork(name, parent_if);
	if (child_tid == TID_ERROR)
	{
		return TID_ERROR;
	}

	return child_tid;
}

static void handle_seek(int fd, off_t position)
{
	struct thread *t = thread_current();
	struct fd_elem *fe;
	if ((fe = find_matched_fd(&t->fds, fd)) == NULL || fe->type != FD_FILE)
	{
		return;
	}

	file_seek(fe->file, position);
}

static off_t handle_tell(int fd)
{
	struct thread *t = thread_current();
	struct fd_elem *fe;
	if ((fe = find_matched_fd(&t->fds, fd)) == NULL || fe->type != FD_FILE)
	{
		return;
	}

	return file_tell(fe->file);
}

static int handle_filesize(int fd)
{
	struct thread *t = thread_current();
	struct fd_elem *fe;

	if ((fe = find_matched_fd(&t->fds, fd)) == NULL || fe->type != FD_FILE)
	{
		return -1;
	}

	return file_length(fe->file);
}

static int handle_read(int fd, void *ubuf, unsigned size)
{
	if (size == 0)
	{
		return 0;
	}

	if (fd == STDOUT_FD)
	{
		return -1;
	}

	struct thread *t = thread_current();
	struct fd_elem *fe;

	// invalid fd (stdin in fds)
	if ((fe = find_matched_fd(&t->fds, fd)) == NULL)
	{
		return -1;
	}

	size_t read_n;
	if (fd == STDIN_FD)
	// if (fe->type == FD_STD_IN)
	{
		for (int i = 0; i < size; i++)
		{
			uint8_t b = input_getc();  // get byte by stdin
			copy_out(ubuf + i, &b, 1); // byte -> ubuf
		}
		read_n = size;
	}
	else
	{
		void *tmp_buf = malloc(size);
		read_n = file_read(fe->file, tmp_buf, size); // file -> tmpbuf
		copy_out(ubuf, tmp_buf, read_n);			 // tmpbuf -> ubuf
		free(tmp_buf);
	}

	return read_n;
}

static int handle_write(int fd, const void *uaddr, size_t n)
{
	if (n == 0 || fd == STDIN_FD)
	{
		return 0;
	}

	struct thread *t = thread_current();
	struct fd_elem *fe;

	if ((fe = find_matched_fd(&t->fds, fd)) == NULL)
	{
		return 0;
	}

	void *tmp_buf = malloc(n);
	size_t write_n;
	if (fe->type == FD_STD_OUT)
	{
		copy_in(tmp_buf, uaddr, n);
		putbuf(tmp_buf, n);
		write_n = n;
	}
	else
	{
		copy_in(tmp_buf, uaddr, n);
		write_n = file_write(fe->file, tmp_buf, n);
	}
	free(tmp_buf);
	return write_n;
}

static void handle_close(int fd)
{
	struct thread *t = thread_current();
	struct fd_elem *fe;

	if ((fe = find_matched_fd(&t->fds, fd)) == NULL)
	{
		return;
	}

	if (fe->type == FD_FILE)
	{
		file_close(fe->file);
	}
	list_remove(&fe->elem);
	free(fe);
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

	char name[NAME_MAX + 1];
	if (!copy_in_file(file, name))
	{
		return -1;
	}

	struct file *f = filesys_open(name);

	// file not in dir
	if (f == NULL)
	{
		return -1;
	}

	struct fd_elem *fe = malloc(sizeof *fe);
	if (fe == NULL)
	{
		file_close(f);
		return -1;
	}

	fe->fd = fd_install();
	fe->file = f;
	fe->type = FD_FILE;

	list_insert_ordered(&t->fds, &fe->elem, lower_fd, NULL);
	return fe->fd;
}

static bool handle_create(char *file, unsigned int initial_size)
{
	ASSERT(file != NULL);

	char name[NAME_MAX + 1];
	if (!copy_in_file(file, name))
	{
		return false;
	}

	bool success = filesys_create(name, initial_size);

	return success;
}

static void fds_flush(struct list *fds)
{
	while (!list_empty(fds))
	{
		struct fd_elem *fe = list_entry(list_pop_front(fds), struct fd_elem, elem);
		if (fe->type == FD_FILE)
		{
			file_close(fe->file);
		}
		free(fe);
	}
}

void handle_exit(int status)
{
	struct thread *cur = thread_current();
	cur->exit_status = status;
	cur->cs->exit_status = status;

	// fd 정리 -> fd 전체 close 및 정리하기
	fds_flush(&cur->fds);
	// exit msg
	printf("%s: exit(%d)\n", cur->name, cur->exit_status);

	// 부모 통지 (msg 출력 후 통지)
	sema_up(&cur->cs->dead);
	thread_exit();
}

static int handle_wait(tid_t tid)
{
	struct child_status *cs;
	if ((cs = find_matched_tid(tid)) == NULL)
	{
		return -1; // invalid tid fault
	}
	sema_down(&cs->dead);
	int exit_status = cs->exit_status;
	list_remove(&cs->elem);
	free(cs);
	return exit_status;
}

static int handle_dup2(int oldfd, int newfd)
{
	struct thread *t = thread_current();
	struct fd_elem *old_fe;
	if ((old_fe = find_matched_fd(&t->fds, oldfd)) == NULL)
	{
		return -1;
	}

	if (oldfd == newfd)
	{
		return newfd;
	}

	struct fd_elem *new_fe;
	if (new_fe = find_matched_fd(&t->fds, newfd))
	{
		handle_close(newfd);
	}

	new_fe = malloc(sizeof(struct fd_elem));
	new_fe->fd = newfd;
	new_fe->file = old_fe->file;
	file_ref(new_fe->file);
	new_fe->type = old_fe->type;

	list_insert_ordered(&t->fds, &new_fe->elem, lower_fd, NULL);
	return newfd;
}

bool init_fds(struct list *fds)
{
	struct fd_elem *in_fd = malloc(sizeof(struct fd_elem));
	if (in_fd == NULL)
	{
		return false;
	}
	in_fd->fd = STDIN_FD;
	in_fd->type = FD_STD_IN;
	in_fd->file = NULL;
	list_push_back(fds, &in_fd->elem);

	struct fd_elem *out_fd = malloc(sizeof(struct fd_elem));
	if (out_fd == NULL)
	{
		return false;
	}
	out_fd->fd = STDOUT_FD;
	out_fd->type = FD_STD_OUT;
	out_fd->file = NULL;
	list_push_back(fds, &out_fd->elem);
	return true;
}

/* The main system call interface */
void syscall_handler(struct intr_frame *f UNUSED)
{
	// printf("[syscall] thr=%s tid=%d no=%lld rip=%p rsp=%p\n",
	// 	   thread_current()->name, thread_current()->tid,
	// 	   f->R.rax, (void *)f->rip, (void *)f->rsp);

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
	case SYS_FILESIZE:
		f->R.rax = handle_filesize(f->R.rdi);
		break;
	case SYS_READ:
		f->R.rax = handle_read(f->R.rdi, f->R.rsi, f->R.rdx);
		break;
	case SYS_WRITE:
		f->R.rax = handle_write(f->R.rdi, f->R.rsi, f->R.rdx);
		break;
	case SYS_CLOSE:
		handle_close(f->R.rdi);
		break;
	case SYS_FORK:
		f->R.rax = handle_fork(f->R.rdi, f);
		break;
	case SYS_EXEC:
		f->R.rax = handle_exec(f->R.rdi);
		break;
	case SYS_WAIT:
		f->R.rax = handle_wait(f->R.rdi);
		break;
	case SYS_SEEK:
		handle_seek(f->R.rdi, f->R.rsi);
		break;
	case SYS_TELL:
		f->R.rax = handle_tell(f->R.rdi);
		break;
	case SYS_DUP2:
		f->R.rax = handle_dup2(f->R.rdi, f->R.rsi);
	default:
		break;
	}
}
