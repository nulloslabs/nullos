#pragma once

#include <freestanding/stdint.h>

#define __SI_MAX_SIZE  128
#define __SI_PAD_SIZE  ((__SI_MAX_SIZE / sizeof(int)) - 4)

typedef int __pid_t_;
typedef unsigned int __uid_t_;
typedef long __clock_t_;

typedef struct {
    int si_signo;
    int si_errno;
    int si_code;
    int __pad0;

    union {
        int _pad[__SI_PAD_SIZE];

        struct {
            __pid_t_ si_pid;
            __uid_t_ si_uid;
        } _kill;

        struct {
            int      si_tid;
            int      si_overrun;
            uint64_t si_sigval;
        } _timer;

        struct {
            __pid_t_ si_pid;
            __uid_t_ si_uid;
            uint64_t si_sigval;
        } _rt;

        struct {
            __pid_t_   si_pid;
            __uid_t_   si_uid;
            int        si_status;
            __clock_t_ si_utime;
            __clock_t_ si_stime;
        } _sigchld;

        struct {
            void     *si_addr;
            short int si_addr_lsb;
        } _sigfault;

        struct {
            long int si_band;
            int      si_fd;
        } _sigpoll;

        struct {
            void        *_call_addr;
            int          _syscall;
            unsigned int _arch;
        } _sigsys;
    } _sifields;
} siginfo_t;

#define si_pid       _sifields._kill.si_pid
#define si_uid       _sifields._kill.si_uid
#define si_timerid   _sifields._timer.si_tid
#define si_overrun   _sifields._timer.si_overrun
#define si_status    _sifields._sigchld.si_status
#define si_utime     _sifields._sigchld.si_utime
#define si_stime     _sifields._sigchld.si_stime
#define si_addr      _sifields._sigfault.si_addr
#define si_addr_lsb  _sifields._sigfault.si_addr_lsb
#define si_band      _sifields._sigpoll.si_band
#define si_fd        _sifields._sigpoll.si_fd
#define si_call_addr _sifields._sigsys._call_addr
#define si_syscall   _sifields._sigsys._syscall
#define si_arch      _sifields._sigsys._arch
