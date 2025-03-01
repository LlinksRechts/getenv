#include <assert.h>
#include <ctype.h>
#include <dlfcn.h>
#include <errno.h>
#include <getopt.h>
#include <limits.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdbool.h>
#include <string.h>
#include <sys/ptrace.h>
#include <sys/mman.h>
#include <sys/types.h>
#include <sys/user.h>
#include <sys/wait.h>
#include <unistd.h>

/* #define DEBUG */

// number of bytes in a JMP/CALL rel32 instruction
#define REL32_SZ 5

// text seen in /proc/<pid>/maps for text areas
static const char *text_area = " r-xp ";

// this should be a string that will uniquely identify libc in /proc/<pid>/maps
static const char *libc_string = "/libc";

// find the location of a shared library in memory
void *find_library(pid_t pid, const char *libname) {
  char filename[32];
  snprintf(filename, sizeof(filename), "/proc/%d/maps", pid);
  FILE *f = fopen(filename, "r");
  char *line = NULL;
  size_t line_size = 0;

  while (getline(&line, &line_size, f) >= 0) {
    char *pos = strstr(line, libname);
    if (pos != NULL && strstr(line, text_area) && (pos[strlen(libc_string)] < 'a' || pos[strlen(libc_string)] > 'z')) {
      long val = strtol(line, NULL, 16);
      free(line);
      fclose(f);
      return (void *)val;
    }
  }
  free(line);
  fclose(f);
  return NULL;
}

// Update the text area of pid at the area starting at where. The data copied
// should be in the new_text buffer whose size is given by len. If old_text is
// not null, the original text data will be copied into it. Therefore old_text
// must have the same size as new_text.
int poke_text(pid_t pid, void *where, void *new_text, void *old_text,
              size_t len) {
  if (len % sizeof(void *) != 0) {
    fprintf(stderr, "invalid len, not a multiple of %zd\n", sizeof(void *));
    return -1;
  }

  long poke_data;
  for (size_t copied = 0; copied < len; copied += sizeof(poke_data)) {
    memmove(&poke_data, new_text + copied, sizeof(poke_data));
    if (old_text != NULL) {
      errno = 0;
      long peek_data = ptrace(PTRACE_PEEKTEXT, pid, where + copied, NULL);
      if (peek_data == -1 && errno) {
        perror("PTRACE_PEEKTEXT");
        return -1;
      }
      memmove(old_text + copied, &peek_data, sizeof(peek_data));
    }
    if (ptrace(PTRACE_POKETEXT, pid, where + copied, (void *)poke_data) < 0) {
      perror("PTRACE_POKETEXT");
      return -1;
    }
  }
  return 0;
}

int do_wait(const char *name) {
  int status;
  if (wait(&status) == -1) {
    perror("wait");
    return -1;
  }
  if (WIFSTOPPED(status)) {
    if (WSTOPSIG(status) == SIGTRAP) {
      return 0;
    }
    fprintf(stderr, "%s unexpectedly got status %s\n", name, strsignal(status));
    return -1;
  }
  fprintf(stderr, "%s got unexpected status %d\n", name, status);
  return -1;

}

int singlestep(pid_t pid) {
  if (ptrace(PTRACE_SINGLESTEP, pid, NULL, NULL)) {
    perror("PTRACE_SINGLESTEP");
    return -1;
  }
  return do_wait("PTRACE_SINGLESTEP");
}

void check_yama(void) {
  FILE *yama_file = fopen("/proc/sys/kernel/yama/ptrace_scope", "r");
  if (yama_file == NULL) {
    return;
  }
  char yama_buf[8];
  memset(yama_buf, 0, sizeof(yama_buf));
  if (fread(yama_buf, 1, sizeof(yama_buf), yama_file) == 0) {
      fprintf(stderr, "Could not read yama file\n");
  }
  if (strcmp(yama_buf, "0\n") != 0) {
    fprintf(stderr, "\nThe likely cause of this failure is that your system has "
           "kernel.yama.ptrace_scope = %s",
           yama_buf);
    fprintf(stderr, "If you would like to disable Yama, you can run: "
           "sudo sysctl kernel.yama.ptrace_scope=0\n");
  }
  fclose(yama_file);
}

int32_t compute_jmp(void *from, void *to) {
  int64_t delta = (int64_t)to - (int64_t)from - REL32_SZ;
  if (delta < INT_MIN || delta > INT_MAX) {
    fprintf(stderr, "cannot do relative jump of size %li; did you compile with -fPIC?\n",
           delta);
    exit(1);
  }
  return (int32_t)delta;
}

int getenv_process(pid_t pid, char* env) {
  // attach to the process
  if (ptrace(PTRACE_ATTACH, pid, NULL, NULL)) {
    perror("PTRACE_ATTACH");
    check_yama();
    return -1;
  }

  // wait for the process to actually stop
  if (waitpid(pid, 0, WSTOPPED) == -1) {
    perror("wait");
    return -1;
  }

  // save the register state of the remote process
  struct user_regs_struct oldregs;
  if (ptrace(PTRACE_GETREGS, pid, NULL, &oldregs)) {
    perror("PTRACE_GETREGS");
    ptrace(PTRACE_DETACH, pid, NULL, NULL);
    return -1;
  }
  void *rip = (void *)oldregs.rip;
  #ifdef DEBUG
  fprintf(stderr, "their %%rip           %p\n", rip);
  #endif

  // First, we are going to allocate some memory for ourselves so we don't
  // need
  // to stop on the remote process' memory. We will do this by directly
  // invoking
  // the mmap(2) system call and asking for a single page.
  struct user_regs_struct newregs;
  memmove(&newregs, &oldregs, sizeof(newregs));
  newregs.rax = 9;                           // mmap
  newregs.rdi = 0;                           // addr
  newregs.rsi = PAGE_SIZE;                   // length
  newregs.rdx = PROT_READ | PROT_EXEC;       // prot
  newregs.r10 = MAP_PRIVATE | MAP_ANONYMOUS; // flags
  newregs.r8 = -1;                           // fd
  newregs.r9 = 0;                            //  offset

  uint8_t old_word[8];
  uint8_t new_word[8];
  new_word[0] = 0x0f; // SYSCALL
  new_word[1] = 0x05; // SYSCALL
  new_word[2] = 0xff; // JMP %rax
  new_word[3] = 0xe0; // JMP %rax

  // insert the SYSCALL instruction into the process, and save the old word
  if (poke_text(pid, rip, new_word, old_word, sizeof(new_word))) {
    goto fail;
  }

  // set the new registers with our syscall arguments
  if (ptrace(PTRACE_SETREGS, pid, NULL, &newregs)) {
    perror("PTRACE_SETREGS");
    goto fail;
  }

  // invoke mmap(2)
  if (singlestep(pid)) {
    goto fail;
  }

  // read the new register state, so we can see where the mmap went
  if (ptrace(PTRACE_GETREGS, pid, NULL, &newregs)) {
    perror("PTRACE_GETREGS");
    return -1;
  }

  // this is the address of the memory we allocated
  void *mmap_memory = (void *)newregs.rax;
  if (mmap_memory == (void *)-1) {
    fprintf(stderr, "failed to mmap\n");
    goto fail;
  }
  #ifdef DEBUG
  fprintf(stderr, "allocated memory at  %p\n", mmap_memory);

  fprintf(stderr, "executing jump to mmap region\n");
  #endif
  if (singlestep(pid)) {
    goto fail;
  }

  if (ptrace(PTRACE_GETREGS, pid, NULL, &newregs)) {
    perror("PTRACE_GETREGS");
    goto fail;
  }
  if (newregs.rip == (long)mmap_memory) {
    #ifdef DEBUG
    fprintf(stderr, "successfully jumped to mmap area\n");
    #endif
  } else {
    fprintf(stderr, "unexpectedly jumped to %p\n", (void *)newregs.rip);
    goto fail;
  }

  // Calculate the position of the getenv routine in the other process'
  // address
  // space. This is a little bit tricky because of ASLR on Linux. What we do
  // is
  // we find the offset in memory that libc has been loaded in their process,
  // and then we find the offset in memory that libc has been loaded in our
  // process. Then we take the delta betwen our getenv and our libc start,
  // and
  // assume that the same delta will apply to the other process.
  //
  // For this mechanism to work, this program must be compiled with -fPIC to
  // ensure that our getenv has an address relative to the one in libc.
  //
  // Additionally, this could fail if libc has been updated since the remote
  // process has been restarted. This is a pretty unlikely situation, but if
  // the
  // remote process has been running for a long time and you update libc, the
  // offset of the symbols could have changed slightly.
  void *their_libc = find_library(pid, libc_string);
  void *our_libc = find_library(getpid(), libc_string);
  void *their_getenv = their_libc + ((void *)getenv - our_libc);
  /* FILE *their_stderr = their_libc + ((void *)stderr - our_libc); */
  #ifdef DEBUG
  fprintf(stderr, "their libc           %p\n", their_libc);
  fprintf(stderr, "their getenv        %p\n", their_getenv);
  /* fprintf(stderr, "their stderr         %p\n", their_stderr); */
  #endif

  // We want to make a call like:
  //
  //   getenv(stderr, "instruction pointer = %p\n", rip);
  //
  // To do this we're going to do the following:
  //
  //   * put a CALL instruction into the mmap area that calls getenv
  //   * put a TRAP instruction right after the CALL
  //   * put the format string right after the TRAP
  //   * use the TRAP to restore the original text/program state

  size_t memsize = strlen(env) + 6;
  size_t blocksize = 32;
  while (blocksize < memsize) blocksize <<= 1;

  // memory we are going to copy into our mmap area
  uint8_t *new_text = calloc(blocksize, sizeof(uint8_t));

  // insert a CALL instruction
  size_t offset = 0;
  new_text[offset++] = 0xe8; // CALL rel32
  int32_t getenv_delta = compute_jmp(mmap_memory, their_getenv);
  memmove(new_text + offset, &getenv_delta, sizeof(getenv_delta));
  offset += sizeof(getenv_delta);

  // insert a TRAP instruction
  new_text[offset++] = 0xcc;

  // copy our getenv format string right after the TRAP instruction
  memmove(new_text + offset, env, strlen(env));

  // update the mmap area
  #ifdef DEBUG
  fprintf(stderr, "inserting code/data into the mmap area at %p\n", mmap_memory);
  #endif
  if (poke_text(pid, mmap_memory, new_text, NULL, blocksize)) {
    goto fail;
  }
  free(new_text);

  if (poke_text(pid, rip, new_word, NULL, sizeof(new_word))) {
    goto fail;
  }

  // set up our registers with the args to getenv
  // memmove(&newregs, &oldregs, sizeof(newregs));
  newregs.rax = 0;                          // no vector registers are used
  /* newregs.rdi = (long)their_stderr;         // pointer to stderr in the caller */
  newregs.rdi = (long)mmap_memory + offset; // pointer to the format string
  /* newregs.rdx = oldregs.rip;                // the integer we want to print */

  #ifdef DEBUG
  fprintf(stderr, "setting the registers of the remote process\n");
  #endif
  if (ptrace(PTRACE_SETREGS, pid, NULL, &newregs)) {
    perror("PTRACE_SETREGS");
    goto fail;
  }

  // continue the program, and wait for the trap
  #ifdef DEBUG
  fprintf(stderr, "continuing execution\n");
  #endif
  ptrace(PTRACE_CONT, pid, NULL, NULL);
  if (do_wait("PTRACE_CONT")) {
    goto fail;
  }

  if (ptrace(PTRACE_GETREGS, pid, NULL, &newregs)) {
    perror("PTRACE_GETREGS");
    goto fail;
  }
  char* offs = (char*) newregs.rax;
  if (offs != NULL) {
    while (true) {
      uint32_t data = ptrace(PTRACE_PEEKDATA, pid, offs, NULL);
      offs += 4;
      if (data << 24 < 0x1000000 || data << 16 < 0x1000000 ||
          data << 8 < 0x1000000 || data < 0x1000000) {
        break;
      }
    }
    size_t slen = (size_t)(offs - newregs.rax);
    char *buf = malloc(sizeof(char) * (slen + 1));
    offs = (char *)newregs.rax;
    for (size_t i = 0; i < slen / 4; i++) {
      uint32_t data = ptrace(PTRACE_PEEKDATA, pid, offs, NULL);
      offs += 4;
      ((uint32_t *)buf)[i] = data;
    }
    printf("%s\n", buf);
    free(buf);
  }
  newregs.rax = (long)rip;
  if (ptrace(PTRACE_SETREGS, pid, NULL, &newregs)) {
    perror("PTRACE_SETREGS");
    goto fail;
  }

  new_word[0] = 0xff; // JMP %rax
  new_word[1] = 0xe0; // JMP %rax
  poke_text(pid, (void *)newregs.rip, new_word, NULL, sizeof(new_word));

  #ifdef DEBUG
  fprintf(stderr, "jumping back to original rip\n");
  #endif
  if (singlestep(pid)) {
    goto fail;
  }
  if (ptrace(PTRACE_GETREGS, pid, NULL, &newregs)) {
    perror("PTRACE_GETREGS");
    goto fail;
  }

  if (newregs.rip == (long)rip) {
    #ifdef DEBUG
    fprintf(stderr, "successfully jumped back to original %%rip at %p\n", rip);
    #endif
  } else {
    fprintf(stderr, "unexpectedly jumped to %p (expected to be at %p)\n",
           (void *)newregs.rip, rip);
    goto fail;
  }

  // unmap the memory we allocated
  newregs.rax = 11;                // munmap
  newregs.rdi = (long)mmap_memory; // addr
  newregs.rsi = PAGE_SIZE;         // size
  if (ptrace(PTRACE_SETREGS, pid, NULL, &newregs)) {
    perror("PTRACE_SETREGS");
    goto fail;
  }

  // make the system call
  #ifdef DEBUG
  fprintf(stderr, "making call to mmap\n");
  #endif
  if (singlestep(pid)) {
    goto fail;
  }
  if (ptrace(PTRACE_GETREGS, pid, NULL, &newregs)) {
    perror("PTRACE_GETREGS");
    goto fail;
  }
  #ifdef DEBUG
  fprintf(stderr, "munmap returned with status %llu\n", newregs.rax);

  fprintf(stderr, "restoring old text at %p\n", rip);
  #endif
  poke_text(pid, rip, old_word, NULL, sizeof(old_word));

  #ifdef DEBUG
  fprintf(stderr, "restoring old registers\n");
  #endif
  if (ptrace(PTRACE_SETREGS, pid, NULL, &oldregs)) {
    perror("PTRACE_SETREGS");
    goto fail;
  }

  // detach the process
  #ifdef DEBUG
  fprintf(stderr, "detaching\n");
  #endif
  if (ptrace(PTRACE_DETACH, pid, NULL, NULL)) {
    perror("PTRACE_DETACH");
    goto fail;
  }
  return 0;

fail:
  poke_text(pid, rip, old_word, NULL, sizeof(old_word));
  if (ptrace(PTRACE_DETACH, pid, NULL, NULL)) {
    perror("PTRACE_DETACH");
  }
  return 1;
}

int main(int argc, char **argv) {
  long pid = -1;
  char* env = NULL;
  int c;
  opterr = 0;
  while ((c = getopt(argc, argv, "hp:e:")) != -1) {
    switch (c) {
    case 'h':
      fprintf(stderr, "Usage: %s -p <pid>\n", argv[0]);
      return 0;
      break;
    case 'p':
      pid = strtol(optarg, NULL, 10);
      if ((errno == ERANGE && (pid == LONG_MAX || pid == LONG_MIN)) ||
          (errno != 0 && pid == 0)) {
        perror("strtol");
        return 1;
      }
      if (pid < 0) {
        fprintf(stderr, "cannot accept negative pids\n");
        return 1;
      }
      break;
    case 'e':
      env = optarg;
      break;
    case '?':
      if (optopt == 'p') {
        fprintf(stderr, "Option -p requires an argument.\n");
      } else if (optopt == 'e') {
        fprintf(stderr, "Option -e requires an argument.\n");
      } else if (isprint(optopt)) {
        fprintf(stderr, "Unknown option `-%c`.\n", optopt);
      } else {
        fprintf(stderr, "Unknown option character `\\x%x'.\n", optopt);
      }
      return 1;
      break;
    default:
      abort();
    }
  }
  if (pid == -1) {
    fprintf(stderr, "must specify a remote process with -p\n");
    return 1;
  }
  if (env == NULL) {
    fprintf(stderr, "must specify an env var with -e\n");
    return 1;
  }
  return getenv_process((pid_t)pid, env);
}
