# getenv

When processes update their own environment variables while running,
these changes will usually not be reflected in `/proc/<pid>/env`. This
program will attach to a running process, inject code that runs
`getenv('VAR')`, and print the result; this way, variables can be read
live.

WARNING: Due to environment variables being handled by the program
itself and not the kernel, this will only work for programs that use
libc, and only ones using the same version of libc as `getenv` is
linked to.

## Usage

You can compile the code with `make`. You should see that it builds an
executable called `getenv`. Invoke it like this:

    getenv -p <pid> -e <envvar>

An easy way to test this is to open two terminals, run `echo $$` in the first
terminal to get the pid of the shell, and then in the other terminal run
`getenv` with the first shell's pid.

The output of this call will be the content of the corresponding
environment variable, or empty if it's not set.

## Issues With Yama ptrace_scope

If you get a failure like this:
```bash
$ ./getenv -p 1 -e var
PTRACE_ATTACH: Operation not permitted
```

then you are trying to trace a process that you don't have permissions to trace,
i.e. a process with a different user id than you. You can only ptrace a process
whose effective user id is the same as yours (or if you are root).

If you instead get a failure like this:
```bash
$ ./getenv -p 5603 -e var
PTRACE_ATTACH: Operation not permitted

The likely cause of this failure is that your system has kernel.yama.ptrace_scope = 1
If you would like to disable Yama, you can run: sudo sysctl kernel.yama.ptrace_scope=0
```

Then the issue is that you have
[Yama ptrace_scope](https://www.kernel.org/doc/Documentation/security/Yama.txt)
configured to disallow ptrace. In particular, the default behavior of Ubuntu
since Ubuntu 10.10 has been to set `kernel.yama.ptrace_scope = 1`. If this
affects you, you can either run `getenv` as root, or you can run the
command listed in the error message to disable the Yama setting.

## Original work

This project is based on
https://github.com/eklitzke/ptrace-call-userspace, a POC for calling
userspace functions using ptrace. I used most of the code from there
to go from a POC to a program solving an actual use case.
