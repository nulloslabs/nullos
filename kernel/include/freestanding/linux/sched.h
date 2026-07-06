#pragma once

// I know, the scheduler file is called scheduler.c but like that isn't that bad...xd

#define CSIGNAL                   0x000000FF      // signal mask to be sent at exit
#define CLONE_VM                  0x00000100      // share address space
#define CLONE_FS                  0x00000200      // share cwd, root, umask
#define CLONE_FILES               0x00000400      // share open file table
#define CLONE_SIGHAND             0x00000800      // share signal handlers
#define CLONE_PIDFD               0x00001000      // return pidfd (not supported)
#define CLONE_PTRACE              0x00002000      // ignored
#define CLONE_VFORK               0x00004000      // block parent until child exec/exit
#define CLONE_PARENT              0x00008000      // share parent
#define CLONE_THREAD              0x00010000      // same thread group (same pid, no SIGCHLD)
#define CLONE_NEWNS               0x00020000      // ignored
#define CLONE_SYSVSEM             0x00040000      // ignored
#define CLONE_SETTLS              0x00080000      // set TLS (r8)
#define CLONE_PARENT_SETTID       0x00100000      // store child tid at parent_tidptr
#define CLONE_CHILD_CLEARTID      0x00200000      // clear child tid at exit + futex wake
#define CLONE_DETACHED            0x00400000      // ignored
#define CLONE_UNTRACED            0x00800000      // ignored
#define CLONE_CHILD_SETTID        0x01000000      // store child tid at child_tidptr
#define CLONE_NEWCGROUP           0x02000000      // ignored
#define CLONE_NEWUTS              0x04000000      // ignored
#define CLONE_NEWIPC              0x08000000      // ignored
#define CLONE_NEWUSER             0x10000000      // ignored
#define CLONE_NEWPID              0x20000000      // ignored
#define CLONE_NEWNET              0x40000000      // ignored
#define CLONE_IO                  0x80000000      // ignored
