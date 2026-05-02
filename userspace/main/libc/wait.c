#include <sys/wait.h>
#include <unistd.h>

pid_t wait(int *wstatus) {
    return waitpid(-1, wstatus, 0); 
}
