#include "userprog/syscall.h"
#include <stdio.h>
#include <syscall-nr.h>
#include "threads/interrupt.h"
#include "threads/thread.h"
#include "threads/loader.h"
#include "userprog/gdt.h"
#include "threads/flags.h"
#include "intrinsic.h"

#define STDOUT_FD 1

void syscall_entry(void);
void syscall_handler(struct intr_frame *);

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

static int handle_exit(int status)
{
	struct thread *cur = thread_current();
	cur->exit_status = status;

	printf("%s: exit(%d)\n", cur->name, cur->exit_status);
	// fd 정리
	// 부모 통지
	sema_up(&cur->cs->dead);
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
	if (f->R.rax == SYS_HALT)
	{
		power_off();
	}

	if (f->R.rax == SYS_EXIT)
	{
		handle_exit(f->R.rdi);
		thread_exit();
	}

	if (f->R.rax == SYS_WRITE)
	{
		int fd = (int)f->R.rdi;
		f->R.rax = handle_write(fd, f->R.rsi, f->R.rdx);
	}
}
