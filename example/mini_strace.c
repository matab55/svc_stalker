#include <errno.h>
#include <limits.h>
#include <mach/mach.h>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/syscall.h>
#include <unistd.h>

#include "array.h"

static pid_t g_pid = 0;

static long SYS_svc_stalker_ctl = 0;

#define PID_MANAGE                  0
#define CALL_LIST_MANAGE            1

#define BEFORE_CALL                 0
#define CALL_COMPLETED              1

static void interrupt(int sig){
    /* unregister this PID upon exit */
    syscall(SYS_svc_stalker_ctl, g_pid, PID_MANAGE, 0, 0);
    write(1, "\nExiting\n", 9);
    exit(0);
}

extern boolean_t mach_exc_server(mach_msg_header_t *, mach_msg_header_t *);

kern_return_t catch_mach_exception_raise_state(mach_port_t exception_port,
        exception_type_t exception, exception_data_t code,
        mach_msg_type_number_t code_count, int *flavor,
        thread_state_t in_state, mach_msg_type_number_t in_state_count,
        thread_state_t out_state, mach_msg_type_number_t *out_state_count){
    /* unused */
    return KERN_FAILURE;
}

kern_return_t catch_mach_exception_raise_state_identity(mach_port_t exception_port,
        mach_port_t thread, mach_port_t task, exception_type_t exception,
        exception_data_t code, mach_msg_type_number_t code_count, int *flavor,
        thread_state_t in_state, mach_msg_type_number_t in_state_count,
        thread_state_t out_state, mach_msg_type_number_t *out_state_count){
    /* unused */
    return KERN_FAILURE;
}

static pthread_mutex_t g_mini_strace_lock = PTHREAD_MUTEX_INITIALIZER;

struct xnu_call {
    pid_t caller;
    /* handle_svc_hook OR's a unique value into the thread's X16 so
     * we can figure out which xnu_call struct corresponds to one that
     * has completed
     */
    int call_id;
    int call_num;
    /* thread state BEFORE this call has happened */
    arm_thread_state64_t *before_state;
};

static int xnu_call_cmp(const void *a, const void *b){
    struct xnu_call *first = *(struct xnu_call **)a;
    struct xnu_call *second = *(struct xnu_call **)b;

    return first->call_id - second->call_id;
}

static struct array *g_pending_calls = NULL;

static void add_pending_call(arm_thread_state64_t *state, pid_t caller){
    pthread_mutex_lock(&g_mini_strace_lock);

    if(!g_pending_calls)
        g_pending_calls = array_new();

    pthread_mutex_unlock(&g_mini_strace_lock);

    struct xnu_call *call = malloc(sizeof(struct xnu_call));
    call->caller = caller;
    call->call_id = state->__x[16] >> 32;
    /* mask for clarity */
    call->call_num = state->__x[16] & 0xffffffff;
    call->before_state = malloc(sizeof(arm_thread_state64_t));
    memcpy(call->before_state, state, sizeof(arm_thread_state64_t));

    pthread_mutex_lock(&g_mini_strace_lock);

    if(array_insert(g_pending_calls, call) != ARRAY_OK){
        printf("mini_strace: could not insert pending call %d, call number %d\n",
                call->call_id, call->call_num);
        free(call->before_state);
        free(call);
    }

    pthread_mutex_unlock(&g_mini_strace_lock);
}
        
/* osfmk/arm/proc_reg.h */
#define PSR_CF 0x20000000

/* void */
#define RETTYPE_NONE        (0)
/* %d */
#define RETTYPE_INT         (1)
/* %#x */
#define RETTYPE_INT_H       (2)
/* %ld */
#define RETTYPE_LONG        (3)
/* %#lx */
#define RETTYPE_LONG_H      (4)

static void print_unix_syscall_retval(arm_thread_state64_t *completed_state,
        int rettype){
    if(rettype == RETTYPE_NONE){
        /* just flush stdout */
        printf("\n");
        return;
    }

    /* see arm_prepare_u64_syscall_return @ bsd/dev/arm/systemcalls.c */
    if(completed_state->__cpsr & PSR_CF)
        printf(" = %s\n", strerror(completed_state->__x[0]));
    else if(rettype == RETTYPE_INT)
        printf(" = %d\n", (int)completed_state->__x[0]);
    else if(rettype == RETTYPE_INT_H)
        printf(" = %#x\n", (int)completed_state->__x[0]);
    else if(rettype == RETTYPE_LONG)
        printf(" = %ld\n", completed_state->__x[0]);
    else if(rettype == RETTYPE_LONG_H)
        printf(" = %#lx\n", completed_state->__x[0]);
}

static void describe_completed_call(mach_port_t task, struct xnu_call *call,
        arm_thread_state64_t *completed_state){
    kern_return_t kret = KERN_SUCCESS;

    int call_num = call->call_num;
    int rettype = RETTYPE_NONE;

    printf("%d: ", call->caller);

    /* if(call_num == 0){ */
    /*     printf("Indirect system call\n"); */
    /* } */
    if(call_num == 1)
        printf("exit(%d)\n", call->before_state->__x[0]);
    else if(call_num == 2)
        printf("fork() = %d\n", completed_state->__x[0]);
    else if(call_num == 3){
        printf("read(%lld, %#llx, %lld)", call->before_state->__x[0],
                call->before_state->__x[1], call->before_state->__x[2]);
        rettype = RETTYPE_LONG;
    }
    else if(call_num == 4){
        /* write(fd, buf, count)
         *
         * W0 == fd
         * X1 == buf
         * X2 == count
         */
        char buf[call->before_state->__x[2]];

        mach_msg_type_number_t sz = call->before_state->__x[2];
        kret = vm_read_overwrite(task, call->before_state->__x[1], sz, buf, &sz);

        if(kret){
            printf("mini_strace: vm_read_overwrite failed: %s\n",
                    mach_error_string(kret));
            return;
        }

        printf("write(%lld, \"%s\", %lld)", call->before_state->__x[0],
                buf, call->before_state->__x[2]);

        rettype = RETTYPE_LONG;
    }
    /* open or access */
    else if(call_num == 5 || call_num == 33){
        /* open(pathname, flags) or access(pathname, mode)
         * 
         * We can handle these both here because their parameters are
         * the exact same type
         *
         * X0 == pathname
         * W1 == flags/mode
         */
        char buf[PATH_MAX] = {0};

        mach_msg_type_number_t sz = PATH_MAX;
        kret = vm_read_overwrite(task, call->before_state->__x[0], sz, buf, &sz);

        if(kret){
            printf("mini_strace: vm_read_overwrite failed: %s\n",
                    mach_error_string(kret));
            return;
        }

        if(call_num == 5)
            printf("open");
        else
            printf("access");

        printf("(\"%s\", %#x)", buf, (uint32_t)call->before_state->__x[1]);

        rettype = RETTYPE_INT;
    }
    /* symlink */
    else if(call_num == 57){
        char path[PATH_MAX];
        char link[PATH_MAX];

        mach_msg_type_number_t sz = PATH_MAX;
        kret = vm_read_overwrite(task, call->before_state->__x[0], sz, path, &sz);

        if(kret){
            printf("mini_strace: vm_read_overwrite failed: %s\n",
                    mach_error_string(kret));
            return;
        }

        sz = PATH_MAX;
        kret = vm_read_overwrite(task, call->before_state->__x[1], sz, link, &sz);

        if(kret){
            printf("mini_strace: vm_read_overwrite failed: %s\n",
                    mach_error_string(kret));
            return;
        }

        printf("symlink(\"%s\", \"%s\")", path, link);
        rettype = RETTYPE_INT;
    }
    /* getpid */
    else if(call_num == 20){
        printf("getpid()");
        rettype = RETTYPE_INT;
    }
    /* a platform syscall, specific number is in X3 */
    else if(call_num == 0x80000000){
        uint64_t ps_call_num = call->before_state->__x[3];

        if(ps_call_num == 3){
            printf("thread_get_cthread_self()");
            rettype = RETTYPE_LONG_H;
        }
    }
    /* mach_absolute_time() */
    else if(call_num == -3){
        printf("mach_absolute_time() = %#llx\n", completed_state->__x[0]);
    }
    /* mach_continuous_time() */
    else if(call_num == -4){
        printf("mach_continuous_time() = %#llx\n", completed_state->__x[0]);
    }
    /* mach_msg_trap */
    else if(call_num == -31){
        printf("mach_msg(%#llx, %#x, %#x, %#x, %#x, %#x, %#x) = %s\n",
                call->before_state->__x[0], (uint32_t)call->before_state->__x[1],
                (uint32_t)call->before_state->__x[2],
                (uint32_t)call->before_state->__x[3],
                (uint32_t)call->before_state->__x[4],
                (uint32_t)call->before_state->__x[5],
                (uint32_t)call->before_state->__x[6],
                mach_error_string(completed_state->__x[0]));
    }
    /* _kernelrpc_mach_port_allocate_trap */
    else if(call_num == -16){
        printf("mach_port_allocate(%#x, %#x, %#llx) = %s\n",
                (uint32_t)call->before_state->__x[0],
                (uint32_t)call->before_state->__x[1],
                call->before_state->__x[2],
                mach_error_string(completed_state->__x[0]));
    }

    if(call_num >= 0)
        /* so we can handle printing of errno */
        print_unix_syscall_retval(completed_state, rettype);
}

static void process_completed_call(mach_port_t task,
        arm_thread_state64_t *state, pid_t caller){
    pthread_mutex_lock(&g_mini_strace_lock);

    if(!g_pending_calls){
        /* strange... */
        pthread_mutex_unlock(&g_mini_strace_lock);
        return;
    }

    /* sort for bsearch */
    array_qsort(g_pending_calls, xnu_call_cmp);

    int call_id = state->__x[16] >> 32;
    /* mask for clarity */
    int call_num = state->__x[16] & 0xffffffff;

    struct xnu_call key = {
        .caller = 0,
        .call_id = call_id,
        .call_num = 0,
        .before_state = NULL
    };

    struct xnu_call *keyp = &key;
    struct xnu_call *foundp = NULL;

    if(array_bsearch(g_pending_calls, &keyp, xnu_call_cmp, &foundp) == ARRAY_OK){
        struct xnu_call *found = *(struct xnu_call **)foundp;

        describe_completed_call(task, found, state);

        if(array_remove_elem(g_pending_calls, found) == ARRAY_OK){
            free(found->before_state);
            free(found);
        }
    }

    pthread_mutex_unlock(&g_mini_strace_lock);
}

kern_return_t catch_mach_exception_raise(mach_port_t exception_port,
        mach_port_t thread, mach_port_t task, exception_type_t exception,
        mach_exception_data_t code, mach_msg_type_number_t code_count){
    pid_t pid = code[0];
    long call_status = code[1];

    mach_msg_type_number_t count = ARM_THREAD_STATE64_COUNT;
    arm_thread_state64_t state = {0};
    kern_return_t kret = thread_get_state(thread, ARM_THREAD_STATE64,
                (thread_state_t)&state, &count);

    if(kret){
        printf("mini_strace: thread_get_state failed: %s\n",
                mach_error_string(kret));
        return KERN_SUCCESS;
    }

    /* for both of these, you're free to inspect/modify registers before
     * giving contol back to the kernel
     */
    if(call_status == BEFORE_CALL)
        add_pending_call(&state, pid);
    else
        process_completed_call(task, &state, pid);

    /* always return KERN_SUCCESS to let the kernel know you've handled
     * this exception
     */
    return KERN_SUCCESS;
}

static void *e_thread_func(void *arg){
    mach_port_t eport = (mach_port_t)arg;

    for(;;)
        mach_msg_server_once(mach_exc_server, 4096, eport, 0);

    return NULL;
}

int main(int argc, char **argv){
    if(argc < 2){
        printf("No PID\n");
        return 1;
    }

    /* before we begin, figure out what system call was patched */
    size_t oldlen = sizeof(long);
    int ret = sysctlbyname("kern.svc_stalker_ctl_callnum", &SYS_svc_stalker_ctl,
            &oldlen, NULL, 0);

    if(ret == -1){
        printf("sysctlbyname with kern.svc_stalker_ctl_callnum failed: %s\n",
                strerror(errno));
        return 1;
    }

    /* first, was svc_stalker_ctl patched correctly? For all my phones, the patched
     * system call is always number 8. It could be different for you.
     */
    ret = syscall(SYS_svc_stalker_ctl, -1, PID_MANAGE, 0, 0);

    if(ret != 999){
        printf("svc_stalker_ctl wasn't patched correctly\n");
        return 1;
    }

    /* install signal handler for Ctrl-C so when user wants to exit this
     * program, we also unregister the PID we're intercepting calls for
     */
    signal(SIGINT, interrupt);

    g_pid = atoi(argv[1]);

    mach_port_t tfp = MACH_PORT_NULL;
    kern_return_t kret = task_for_pid(mach_task_self(), g_pid, &tfp);

    if(kret){
        printf("task_for_pid for pid %d failed: %s\n", g_pid, mach_error_string(kret));
        return 1;
    }

    mach_port_t eport = MACH_PORT_NULL;

    kret = mach_port_allocate(mach_task_self(), MACH_PORT_RIGHT_RECEIVE, &eport);
    
    if(kret){
        printf("mach_port_allocate failed: %s\n", mach_error_string(kret));
        return 1;
    }

    kret = mach_port_insert_right(mach_task_self(), eport, eport,
            MACH_MSG_TYPE_MAKE_SEND);

    if(kret){
        printf("mach_port_insert_right failed: %s\n", mach_error_string(kret));
        return 1;
    }

    /* You *always* need to register exception ports for a process before registering
     * its PID with svc_stalker_ctl.
     *
     * System call/Mach trap interception exception messages will be sent to
     * you as EXC_SYSCALL or EXC_MACH_SYSCALL exceptions, so only filter for
     * those. After the kernel patches svc_stalker does, we are the only
     * thing which uses these types of exceptions.
     */
    kret = task_set_exception_ports(tfp, EXC_MASK_SYSCALL | EXC_MASK_MACH_SYSCALL,
            eport, EXCEPTION_DEFAULT | MACH_EXCEPTION_CODES, THREAD_STATE_NONE);

    if(kret){
        printf("task_set_exception_ports failed: %s\n", mach_error_string(kret));
        return 1;
    }

    /* That's all set up, so register the pid argument for interception */
    ret = syscall(SYS_svc_stalker_ctl, g_pid, PID_MANAGE, 1, 0);

    if(ret){
        printf("Failed registering %d for call interception: %s\n", g_pid,
                strerror(errno));
        return 1;
    }

    /* register some call numbers to intercept */
#define REGISTER_CALL(callnum) \
    do { \
        ret = syscall(SYS_svc_stalker_ctl, g_pid, CALL_LIST_MANAGE, callnum, 1); \
        if(ret){ \
            printf("Couldn't register %d: %s\n", callnum, strerror(errno)); \
            /* you always need to free the stalker_ctl entry upon process exit */ \
            syscall(SYS_svc_stalker_ctl, g_pid, PID_MANAGE, 0, 0); \
            return 1; \
        } \
    } while (0) \

    REGISTER_CALL(0);
    /* write */
    REGISTER_CALL(4);
    /* read */
    REGISTER_CALL(3);
    /* exit */
    REGISTER_CALL(1);
    /* open */
    REGISTER_CALL(5);
    /* access */
    REGISTER_CALL(33);
    /* fork */
    REGISTER_CALL(2);
    /* getpid */
    REGISTER_CALL(20);
    /* symlink */
    REGISTER_CALL(57);
    /* platform syscalls */
    REGISTER_CALL(0x80000000);
    /* mach_msg */
    REGISTER_CALL(-31);
    /* mach_absolute_time */
    REGISTER_CALL(-3);
    /* mach_continuous_time */
    REGISTER_CALL(-4);
    /* mach_port_allocate */
    REGISTER_CALL(-16);

    pthread_t e_thread;
    pthread_create(&e_thread, NULL, e_thread_func, (void *)(uintptr_t)eport);
    pthread_join(e_thread, NULL);

    /* not reached */

    return 0;
}
