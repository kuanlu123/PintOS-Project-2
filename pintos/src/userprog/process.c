#include "userprog/process.h"
#include <debug.h>
#include <inttypes.h>
#include <round.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "userprog/gdt.h"
#include "userprog/pagedir.h"
#include "userprog/tss.h"
#include "filesys/directory.h"
#include "filesys/file.h"
#include "filesys/filesys.h"
#include "threads/flags.h"
#include "threads/init.h"
#include "threads/interrupt.h"
#include "threads/palloc.h"
#include "threads/thread.h"
#include "threads/vaddr.h"
#include "threads/synch.h"
#include "threads/malloc.h"

#include <list.h>

static thread_func start_process NO_RETURN;
static bool load (const char *cmdline, void (**eip) (void), void **esp);

/* Starts a new thread running a user program loaded from
   FILENAME.  The new thread may be scheduled (and may even exit)
   before process_execute() returns.  Returns the new process's
   thread id, or TID_ERROR if the thread cannot be created. */
tid_t
process_execute (const char *cmd_line) 
{
  struct exec_helper exec;
  struct thread *t = thread_current ();
  char thread_name[16];
  char *save_ptr;
  
  exec.cmd_line = cmd_line;
  
  char cmd_line_[strlen (cmd_line) + 1];
  strlcpy (cmd_line_, cmd_line, strlen (cmd_line) + 1);

  char *thread_name_ = strtok_r (cmd_line_, " ", &save_ptr);
  strlcpy (exec.file_name_, thread_name_, strlen (thread_name_) + 1);

  exec.fn_length = strlen (thread_name_);

  if (exec.fn_length > 15 || thread_name_ == NULL )
    return TID_ERROR;

  strlcpy (thread_name, thread_name_, strlen (thread_name_) + 1);

  tid_t tid;

  /* Create a new thread to execute FILE_NAME. */
  tid = thread_create (thread_name, PRI_DEFAULT, start_process,(void *)&exec);
  if (tid != TID_ERROR)
    {
      exec.thread->parent = t;
      list_push_back (&t->child_list, &(exec.thread->child_of));
      
      sema_down (&exec.thread->exit_sema);
    }
  return tid;
}

/* A thread function that loads a user process and starts it
   running. */
static void
start_process (void *exec)
{
  struct exec_helper *exec_ = ((struct exec_helper *)exec);
  const char *cmd_line = exec_->cmd_line;
  struct intr_frame if_;

  struct thread *t = thread_current ();
  t->exec = (void *)exec_;
  exec_->thread = t;

  /* Initialize interrupt frame and load executable. */
  memset (&if_, 0, sizeof if_);
  if_.gs = if_.fs = if_.es = if_.ds = if_.ss = SEL_UDSEG;
  if_.cs = SEL_UCSEG;
  if_.eflags = FLAG_IF | FLAG_MBS;
  exec_->load_success = load (cmd_line, &if_.eip, &if_.esp);

  /* If load failed, quit. */
  if (!(exec_->load_success) )
    {
      t->load_success = false;
      thread_exit ();
    }
  else
    {
      /* load successful */
      t->load_success = true;
    }

  sema_up (&t->exit_sema);
  /* Start the user process by simulating a return from an
     interrupt, implemented by intr_exit (in
     threads/intr-stubs.S).  Because intr_exit takes all of its
     arguments on the stack in the form of a `struct intr_frame',
     we just point the stack pointer (%esp) to our stack frame
     and jump to it. */
  asm volatile ("movl %0, %%esp; jmp intr_exit" : : "g" (&if_) : "memory");

  NOT_REACHED ();
}

static bool
find_child (const struct list_elem *a, int tid, void *aux UNUSED)
{
  struct thread *x = list_entry (a, struct thread, child_of);
  if (x->tid == tid)
    return true;
    
  return false;
}

/* Waits for thread TID to die and returns its exit status.  If
   it was terminated by the kernel (i.e. killed due to an
   exception), returns -1.  If TID is invalid or if it was not a
   child of the calling process, or if process_wait() has already
   been successfully called for the given TID, returns -1
   immediately, without waiting.

   This function will be implemented in problem 2-2.  For now, it
   does nothing. */
int
process_wait (tid_t child_tid) 
{

  struct thread *t = thread_current();
  struct list_elem *e = list_find (&t->child_list, &find_child, child_tid,
                                   NULL);
  if (e == NULL)
    return -1;
  struct thread *child = list_entry (e, struct thread, child_of);

  //if the child does not exist
  if (child == NULL)
		return -1;
		
  sema_down (&child->exit_sema);
		
  return child_tid;
}

/* Free the current process's resources. */
void
process_exit (void)
{
  struct thread *cur = thread_current ();
  if (&cur->exit_sema != NULL)
    sema_up (&cur->exit_sema);
  uint32_t *pd;

  /* Destroy the current process's page directory and switch back
     to the kernel-only page directory. */
  pd = cur->pagedir;
  
  if (pd != NULL) 
    {
      /* Correct ordering here is crucial.  We must set
         cur->pagedir to NULL before switching page directories,
         so that a timer interrupt can't switch back to the
         process page directory.  We must activate the base page
         directory before destroying the process's page
         directory, or our active page directory will be one
         that's been freed (and cleared). */
      cur->pagedir = NULL;
      pagedir_activate (NULL);
      pagedir_destroy (pd);
    }
}

/* Sets up the CPU for running user code in the current
   thread.
   This function is called on every context switch. */
void
process_activate (void)
{
  struct thread *t = thread_current ();

  /* Activate thread's page tables. */
  pagedir_activate (t->pagedir);

  /* Set thread's kernel stack for use in processing
     interrupts. */
  tss_update ();
}

/* We load ELF binaries.  The following definitions are taken
   from the ELF specification, [ELF1], more-or-less verbatim.  */

/* ELF types.  See [ELF1] 1-2. */
typedef uint32_t Elf32_Word, Elf32_Addr, Elf32_Off;
typedef uint16_t Elf32_Half;

/* For use with ELF types in printf(). */
#define PE32Wx PRIx32   /* Print Elf32_Word in hexadecimal. */
#define PE32Ax PRIx32   /* Print Elf32_Addr in hexadecimal. */
#define PE32Ox PRIx32   /* Print Elf32_Off in hexadecimal. */
#define PE32Hx PRIx16   /* Print Elf32_Half in hexadecimal. */

/* Executable header.  See [ELF1] 1-4 to 1-8.
   This appears at the very beginning of an ELF binary. */
struct Elf32_Ehdr
  {
    unsigned char e_ident[16];
    Elf32_Half    e_type;
    Elf32_Half    e_machine;
    Elf32_Word    e_version;
    Elf32_Addr    e_entry;
    Elf32_Off     e_phoff;
    Elf32_Off     e_shoff;
    Elf32_Word    e_flags;
    Elf32_Half    e_ehsize;
    Elf32_Half    e_phentsize;
    Elf32_Half    e_phnum;
    Elf32_Half    e_shentsize;
    Elf32_Half    e_shnum;
    Elf32_Half    e_shstrndx;
  };

/* Program header.  See [ELF1] 2-2 to 2-4.
   There are e_phnum of these, starting at file offset e_phoff
   (see [ELF1] 1-6). */
struct Elf32_Phdr
  {
    Elf32_Word p_type;
    Elf32_Off  p_offset;
    Elf32_Addr p_vaddr;
    Elf32_Addr p_paddr;
    Elf32_Word p_filesz;
    Elf32_Word p_memsz;
    Elf32_Word p_flags;
    Elf32_Word p_align;
  };

/* Values for p_type.  See [ELF1] 2-3. */
#define PT_NULL    0            /* Ignore. */
#define PT_LOAD    1            /* Loadable segment. */
#define PT_DYNAMIC 2            /* Dynamic linking info. */
#define PT_INTERP  3            /* Name of dynamic loader. */
#define PT_NOTE    4            /* Auxiliary info. */
#define PT_SHLIB   5            /* Reserved. */
#define PT_PHDR    6            /* Program header table. */
#define PT_STACK   0x6474e551   /* Stack segment. */

/* Flags for p_flags.  See [ELF3] 2-3 and 2-4. */
#define PF_X 1          /* Executable. */
#define PF_W 2          /* Writable. */
#define PF_R 4          /* Readable. */

static bool setup_stack (void **esp, const char* cmd_line);
static bool validate_segment (const struct Elf32_Phdr *, struct file *);
static bool load_segment (struct file *file, off_t ofs, uint8_t *upage,
                          uint32_t read_bytes, uint32_t zero_bytes,
                          bool writable);

/* Loads an ELF executable from FILE_NAME into the current thread.
   Stores the executable's entry point into *EIP
   and its initial stack pointer into *ESP.
   Returns true if successful, false otherwise. */
bool
load (const char *cmd_line, void (**eip) (void), void **esp) 
{ 
  struct thread *t = thread_current ();
  char file_name[16];
  char cmd_line_[strlen (cmd_line) + 1];
  strlcpy (cmd_line_, cmd_line, strlen (cmd_line) + 1);

  struct Elf32_Ehdr ehdr;
  struct file *file = NULL;
  off_t file_ofs;
  bool success = false;
  int i;


  /* Allocate and activate page directory. */
  t->pagedir = pagedir_create ();
  if (t->pagedir == NULL) 
    goto done;
  process_activate ();
  
  char *save_ptr;
  char *file_name_ = strtok_r (cmd_line_, " ", &save_ptr);

  if (strlen (file_name_) > 14)
    goto done;

  strlcpy (file_name, file_name_, strlen (file_name_) + 1);
 
  /* Open executable file. */
  file = filesys_open (file_name);
  if (file == NULL) 
    {
      printf ("load: %s: open failed\n", file_name);
      goto done; 
    }

  file_deny_write (file);

  struct pair *p = malloc (4);
  p->file = file;
  p->fd = t->new_fd;
  ++t->new_fd;
  list_push_back (&t->file_list, &p->elem);
  
  /* Read and verify executable header. */
  if (file_read (file, &ehdr, sizeof ehdr) != sizeof ehdr
      || memcmp (ehdr.e_ident, "\177ELF\1\1\1", 7)
      || ehdr.e_type != 2
      || ehdr.e_machine != 3
      || ehdr.e_version != 1
      || ehdr.e_phentsize != sizeof (struct Elf32_Phdr)
      || ehdr.e_phnum > 1024) 
    {
      printf ("load: %s: error loading executable\n", file_name_);
      goto done; 
    }

  /* Read program headers. */
  file_ofs = ehdr.e_phoff;
  for (i = 0; i < ehdr.e_phnum; i++) 
    {
      struct Elf32_Phdr phdr;

      if (file_ofs < 0 || file_ofs > file_length (file))
        goto done;
      file_seek (file, file_ofs);

      if (file_read (file, &phdr, sizeof phdr) != sizeof phdr)
        goto done;
      file_ofs += sizeof phdr;
      switch (phdr.p_type) 
        {
        case PT_NULL:
        case PT_NOTE:
        case PT_PHDR:
        case PT_STACK:
        default:
          /* Ignore this segment. */
          break;
        case PT_DYNAMIC:
        case PT_INTERP:
        case PT_SHLIB:
          goto done;
        case PT_LOAD:
          if (validate_segment (&phdr, file)) 
            {
              bool writable = (phdr.p_flags & PF_W) != 0;
              uint32_t file_page = phdr.p_offset & ~PGMASK;
              uint32_t mem_page = phdr.p_vaddr & ~PGMASK;
              uint32_t page_offset = phdr.p_vaddr & PGMASK;
              uint32_t read_bytes, zero_bytes;
              if (phdr.p_filesz > 0)
                {
                  /* Normal segment.
                     Read initial part from disk and zero the rest. */
                  read_bytes = page_offset + phdr.p_filesz;
                  zero_bytes = (ROUND_UP (page_offset + phdr.p_memsz, PGSIZE)
                                - read_bytes);
                }
              else 
                {
                  /* Entirely zero.
                     Don't read anything from disk. */
                  read_bytes = 0;
                  zero_bytes = ROUND_UP (page_offset + phdr.p_memsz, PGSIZE);
                }
              if (!load_segment (file, file_page, (void *) mem_page,
                                 read_bytes, zero_bytes, writable))
                goto done;
            }
          else
            goto done;
          break;
        }
    }

  /* Set up stack. */
  if (!setup_stack (esp, cmd_line))
    goto done;

  /* Start address. */
  *eip = (void (*) (void)) ehdr.e_entry;

  success = true;

 done:
  /* We arrive here whether the load is successful or not. */
  return success;
}

/* load() helpers. */

static bool install_page (void *upage, void *kpage, bool writable);

/* Checks whether PHDR describes a valid, loadable segment in
   FILE and returns true if so, false otherwise. */
static bool
validate_segment (const struct Elf32_Phdr *phdr, struct file *file) 
{
  /* p_offset and p_vaddr must have the same page offset. */
  if ((phdr->p_offset & PGMASK) != (phdr->p_vaddr & PGMASK)) 
    return false; 

  /* p_offset must point within FILE. */
  if (phdr->p_offset > (Elf32_Off) file_length (file)) 
    return false;

  /* p_memsz must be at least as big as p_filesz. */
  if (phdr->p_memsz < phdr->p_filesz) 
    return false; 

  /* The segment must not be empty. */
  if (phdr->p_memsz == 0)
    return false;
  
  /* The virtual memory region must both start and end within the
     user address space range. */
  if (!is_user_vaddr ((void *) phdr->p_vaddr))
    return false;
  if (!is_user_vaddr ((void *) (phdr->p_vaddr + phdr->p_memsz)))
    return false;

  /* The region cannot "wrap around" across the kernel virtual
     address space. */
  if (phdr->p_vaddr + phdr->p_memsz < phdr->p_vaddr)
    return false;

  /* Disallow mapping page 0.
     Not only is it a bad idea to map page 0, but if we allowed
     it then user code that passed a null pointer to system calls
     could quite likely panic the kernel by way of null pointer
     assertions in memcpy(), etc. */
  if (phdr->p_vaddr < PGSIZE)
    return false;

  /* It's okay. */
  return true;
}

/* Loads a segment starting at offset OFS in FILE at address
   UPAGE.  In total, READ_BYTES + ZERO_BYTES bytes of virtual
   memory are initialized, as follows:

        - READ_BYTES bytes at UPAGE must be read from FILE
          starting at offset OFS.

        - ZERO_BYTES bytes at UPAGE + READ_BYTES must be zeroed.

   The pages initialized by this function must be writable by the
   user process if WRITABLE is true, read-only otherwise.

   Return true if successful, false if a memory allocation error
   or disk read error occurs. */
static bool
load_segment (struct file *file, off_t ofs, uint8_t *upage,
              uint32_t read_bytes, uint32_t zero_bytes, bool writable) 
{
  ASSERT ((read_bytes + zero_bytes) % PGSIZE == 0);
  ASSERT (pg_ofs (upage) == 0);
  ASSERT (ofs % PGSIZE == 0);

  file_seek (file, ofs);
  while (read_bytes > 0 || zero_bytes > 0) 
    {
      /* Calculate how to fill this page.
         We will read PAGE_READ_BYTES bytes from FILE
         and zero the final PAGE_ZERO_BYTES bytes. */
      size_t page_read_bytes = read_bytes < PGSIZE ? read_bytes : PGSIZE;
      size_t page_zero_bytes = PGSIZE - page_read_bytes;

      /* Get a page of memory. */
      uint8_t *kpage = palloc_get_page (PAL_USER);
      if (kpage == NULL)
        return false;

      /* Load this page. */
      if (file_read (file, kpage, page_read_bytes) != (int) page_read_bytes)
        {
          palloc_free_page (kpage);
          return false; 
        }
      memset (kpage + page_read_bytes, 0, page_zero_bytes);

      /* Add the page to the process's address space. */
      if (!install_page (upage, kpage, writable)) 
        {
          palloc_free_page (kpage);
          return false; 
        }

      /* Advance. */
      read_bytes -= page_read_bytes;
      zero_bytes -= page_zero_bytes;
      upage += PGSIZE;
    }
  return true;
}

static void *
push (uint8_t *kpage, size_t *offset, const void *buf, size_t size)
{
  size_t padsize = ROUND_UP (size, sizeof (uint32_t));
  if (*offset < padsize)
    return NULL;
  
  *offset -= padsize;
  memcpy (kpage + *offset + (padsize - size), buf, size);
  return kpage + *offset + (padsize - size);
}

static size_t
arg_count (const char *cmd_line)
{
  char cmd_line_[strlen (cmd_line) + 1];
  strlcpy (cmd_line_, cmd_line, strlen (cmd_line) + 1);
  int count = 0;
  char *token, *save_ptr;
  for (token = strtok_r (cmd_line_, " ", &save_ptr); token != NULL;
       token = strtok_r (NULL, " ", &save_ptr))
    ++count;
  return count;
}

static bool
setup_stack_helper (const char *cmd_line, uint8_t *kpage, uint8_t *upage,
                    void **esp)
{
  size_t ofs = PGSIZE;
  size_t argc = arg_count (cmd_line);
  int i = 0;
  uint8_t word_align = 0;
  char *const null = NULL;
  char **argv = malloc (argc * 4 + 1);
  char **uarg = malloc (argc * 4 + 1);
  char *karg = NULL;
  bool success = true;

  char *token, *save_ptr;
  char cmd_line_[strlen (cmd_line) + 1];
  strlcpy (cmd_line_, cmd_line, strlen (cmd_line) + 1);

  for (token = strtok_r (cmd_line_, " ", &save_ptr); token != NULL;
       token = strtok_r (NULL, " ", &save_ptr))
    {
      argv[i] = malloc (sizeof token);
      strlcpy (argv[i], token, strlen (token) + 1);
      ++i;
    }

  /* push null argument */
  argv[argc] = &null;
  karg = push (kpage, &ofs, argv[argc], sizeof argv[argc]);
  ASSERT (karg != null);
  if (!karg)
    return false;
  uarg[argc] = upage + (karg - (char *)kpage);
  
  /* push words onto stack */
  int j;
  unsigned k;
  unsigned c_count = 0;
  for (j = argc - 1; j >= 0; --j)
    {
      k = strlen (argv[j]) + 1;
      karg = push (kpage, &ofs, argv[j], k);
      if (karg == NULL)
	return false;
      uarg[j] = upage + (karg - (char *)kpage);
      ASSERT (is_user_vaddr (uarg[j]));
      c_count += k;
    }

  /* push multiple 0 values for alignment */

  unsigned divisible = c_count % 4;
  for (; divisible != 0; --divisible)
    {
      success = push (kpage, &ofs, &word_align,
                      sizeof(word_align));
      if (!success)
        return false;
    }

  ASSERT (argv == &argv[0]);
  ASSERT (argv[0] == &argv[0][0]);
  ASSERT (*argv[0] == argv[0][0]);

  /* push null */
  ASSERT (is_user_vaddr (uarg[argc]));
  uarg[argc] = 0;
  success = push (kpage, &ofs, &uarg[argc], sizeof uarg[argc]);
  if (!success)
    return false;

  /* push argument addresses */
  for (j = argc; j >= 0; --j)
    {
      karg = push (kpage, &ofs, &uarg[j], sizeof (uarg[j]));
      if (!karg)
        return false;
    }
  void *uarg_;

  uarg_ = upage + (karg - (char *)kpage);
  /* push argv address */
  ASSERT (is_user_vaddr (uarg_));
  success = push (kpage, &ofs, &uarg_, sizeof (uarg));
  if (!success)
    return false;

  /* push argc address */
  success = push (kpage, &ofs, &argc, sizeof argc);
  if (!success)
    return false;

  /* push return address */
  int return_address = 0;
  success = push (kpage, &ofs, &return_address, sizeof (return_address));
  if (!success)
    return false;

  *esp = upage + ofs;
  //hex_dump ((uintptr_t) &esp, kpage, PGSIZE, true);
  return true;
}

/* Create a minimal stack by mapping a zeroed page at the top of
   user virtual memory. */
static bool
setup_stack (void **esp, const char *cmd_line) 
{
  uint8_t *kpage;
  bool success = false;

  kpage = palloc_get_page (PAL_USER | PAL_ZERO);
  if (kpage != NULL) 
    {
      uint8_t *upage = ((uint8_t *)PHYS_BASE) - PGSIZE;
      success = install_page (upage, kpage, true);
      if (success)
        success = setup_stack_helper (cmd_line, kpage, upage, esp);
      else
        palloc_free_page (kpage);
    }
  return success;
}

/* Adds a mapping from user virtual address UPAGE to kernel
   virtual address KPAGE to the page table.
   If WRITABLE is true, the user process may modify the page;
   otherwise, it is read-only.
   UPAGE must not already be mapped.
   KPAGE should probably be a page obtained from the user pool
   with palloc_get_page().
   Returns true on success, false if UPAGE is already mapped or
   if memory allocation fails. */
static bool
install_page (void *upage, void *kpage, bool writable)
{
  struct thread *t = thread_current ();

  /* Verify that there's not already a page at that virtual
     address, then map our page there. */
  return (pagedir_get_page (t->pagedir, upage) == NULL
          && pagedir_set_page (t->pagedir, upage, kpage, writable));
}
