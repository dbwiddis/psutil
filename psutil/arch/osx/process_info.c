/*
 * Copyright (c) 2009, Jay Loden, Giampaolo Rodola'. All rights reserved.
 * Use of this source code is governed by a BSD-style license that can be
 * found in the LICENSE file.
 *
 * Helper functions related to fetching process information.
 * Used by _psutil_osx module methods.
 */


#include <Python.h>
#include <errno.h>
#include <sys/sysctl.h>
#include <libproc.h>

#include "../../_psutil_common.h"
#include "../../_psutil_posix.h"
#include "process_info.h"


/*
 * Returns a list of all BSD processes on the system.  This routine
 * allocates the list and puts it in *procList and a count of the
 * number of entries in *procCount.  You are responsible for freeing
 * this list (use "free" from System framework).
 * On success, the function returns 0.
 * On error, the function returns a BSD errno value.
 */
int
psutil_get_proc_list(kinfo_proc **procList, size_t *procCount) {
    int mib[3];
    size_t size, size2;
    void *ptr;
    int err;
    int lim = 8;  // some limit

    mib[0] = CTL_KERN;
    mib[1] = KERN_PROC;
    mib[2] = KERN_PROC_ALL;
    *procCount = 0;

    /*
     * We start by calling sysctl with ptr == NULL and size == 0.
     * That will succeed, and set size to the appropriate length.
     * We then allocate a buffer of at least that size and call
     * sysctl with that buffer.  If that succeeds, we're done.
     * If that call fails with ENOMEM, we throw the buffer away
     * and try again.
     * Note that the loop calls sysctl with NULL again.  This is
     * is necessary because the ENOMEM failure case sets size to
     * the amount of data returned, not the amount of data that
     * could have been returned.
     */
    while (lim-- > 0) {
        size = 0;
        if (sysctl((int *)mib, 3, NULL, &size, NULL, 0) == -1) {
            PyErr_SetFromOSErrnoWithSyscall("sysctl(KERN_PROC_ALL)");
            return 1;
        }
        size2 = size + (size >> 3);  // add some
        if (size2 > size) {
            ptr = malloc(size2);
            if (ptr == NULL)
                ptr = malloc(size);
            else
                size = size2;
        }
        else {
            ptr = malloc(size);
        }
        if (ptr == NULL) {
            PyErr_NoMemory();
            return 1;
        }

        if (sysctl((int *)mib, 3, ptr, &size, NULL, 0) == -1) {
            err = errno;
            free(ptr);
            if (err != ENOMEM) {
                PyErr_SetFromOSErrnoWithSyscall("sysctl(KERN_PROC_ALL)");
                return 1;
            }
        }
        else {
            *procList = (kinfo_proc *)ptr;
            *procCount = size / sizeof(kinfo_proc);
            if (procCount <= 0) {
                PyErr_Format(PyExc_RuntimeError, "no PIDs found");
                return 1;
            }
            return 0;  // success
        }
    }

    PyErr_Format(PyExc_RuntimeError, "couldn't collect PIDs list");
    return 1;
}


// Read the maximum argument size for processes
static int
psutil_sysctl_argmax() {
    int argmax;
    int mib[2];
    size_t size = sizeof(argmax);

    mib[0] = CTL_KERN;
    mib[1] = KERN_ARGMAX;

    if (sysctl(mib, 2, &argmax, &size, NULL, 0) == 0)
        return argmax;
    PyErr_SetFromOSErrnoWithSyscall("sysctl(KERN_ARGMAX)");
    return 0;
}


// Read process argument space.
static int
psutil_sysctl_procargs(pid_t pid, char *procargs, size_t *argmax) {
    int mib[3];

    mib[0] = CTL_KERN;
    mib[1] = KERN_PROCARGS2;
    mib[2] = pid;

    if (sysctl(mib, 3, procargs, argmax, NULL, 0) < 0) {
        if (psutil_pid_exists(pid) == 0) {
            NoSuchProcess("psutil_pid_exists -> 0");
            return 1;
        }
        // In case of zombie process we'll get EINVAL. We translate it
        // to NSP and _psosx.py will translate it to ZP.
        if (errno == EINVAL) {
            psutil_debug("sysctl(KERN_PROCARGS2) -> EINVAL translated to NSP");
            NoSuchProcess("sysctl(KERN_PROCARGS2) -> EINVAL");
            return 1;
        }
        // There's nothing we can do other than raising AD.
        if (errno == EIO) {
            psutil_debug("sysctl(KERN_PROCARGS2) -> EIO translated to AD");
            AccessDenied("sysctl(KERN_PROCARGS2) -> EIO");
            return 1;
        }
        PyErr_SetFromOSErrnoWithSyscall("sysctl(KERN_PROCARGS2)");
        return 1;
    }
    return 0;
}


// Return 1 if pid refers to a zombie process else 0.
int
psutil_is_zombie(pid_t pid) {
    struct kinfo_proc kp;

    if (psutil_get_kinfo_proc(pid, &kp) == -1)
        return 0;
    return (kp.kp_proc.p_stat == SZOMB) ? 1 : 0;
}


// return process args as a python list
PyObject *
psutil_proc_cmdline(PyObject *self, PyObject *args) {
    pid_t pid;
    int nargs;
    size_t len;
    char *procargs = NULL;
    char *arg_ptr;
    char *arg_end;
    char *curr_arg;
    size_t argmax;
    PyObject *py_retlist = PyList_New(0);
    PyObject *py_arg = NULL;

    if (py_retlist == NULL)
        return NULL;
    if (! PyArg_ParseTuple(args, _Py_PARSE_PID, &pid))
        goto error;

    // special case for PID 0 (kernel_task) where cmdline cannot be fetched
    if (pid == 0)
        return py_retlist;

    // read argmax and allocate memory for argument space.
    argmax = psutil_sysctl_argmax();
    if (! argmax)
        goto error;

    procargs = (char *)malloc(argmax);
    if (NULL == procargs) {
        PyErr_NoMemory();
        goto error;
    }

    if (psutil_sysctl_procargs(pid, procargs, &argmax) != 0)
        goto error;

    arg_end = &procargs[argmax];
    // copy the number of arguments to nargs
    memcpy(&nargs, procargs, sizeof(nargs));

    arg_ptr = procargs + sizeof(nargs);
    len = strlen(arg_ptr);
    arg_ptr += len + 1;

    if (arg_ptr == arg_end) {
        free(procargs);
        return py_retlist;
    }

    // skip ahead to the first argument
    for (; arg_ptr < arg_end; arg_ptr++) {
        if (*arg_ptr != '\0')
            break;
    }

    // iterate through arguments
    curr_arg = arg_ptr;
    while (arg_ptr < arg_end && nargs > 0) {
        if (*arg_ptr++ == '\0') {
            py_arg = PyUnicode_DecodeFSDefault(curr_arg);
            if (! py_arg)
                goto error;
            if (PyList_Append(py_retlist, py_arg))
                goto error;
            Py_DECREF(py_arg);
            // iterate to next arg and decrement # of args
            curr_arg = arg_ptr;
            nargs--;
        }
    }

    free(procargs);
    return py_retlist;

error:
    Py_XDECREF(py_arg);
    Py_XDECREF(py_retlist);
    if (procargs != NULL)
        free(procargs);
    return NULL;
}


// Return process environment as a python string.
// On Big Sur this function returns an empty string unless:
// * kernel is DEVELOPMENT || DEBUG
// * target process is same as current_proc()
// * target process is not cs_restricted
// * SIP is off
// * caller has an entitlement
// See: https://github.com/apple/darwin-xnu/blob/2ff845c2e033bd0ff64b5b6aa6063a1f8f65aa32/bsd/kern/kern_sysctl.c#L1315-L1321
PyObject *
psutil_proc_environ(PyObject *self, PyObject *args) {
    pid_t pid;
    int nargs;
    char *procargs = NULL;
    char *procenv = NULL;
    char *arg_ptr;
    char *arg_end;
    char *env_start;
    size_t argmax;
    PyObject *py_ret = NULL;

    if (! PyArg_ParseTuple(args, _Py_PARSE_PID, &pid))
        return NULL;

    // special case for PID 0 (kernel_task) where cmdline cannot be fetched
    if (pid == 0)
        goto empty;

    // read argmax and allocate memory for argument space.
    argmax = psutil_sysctl_argmax();
    if (! argmax)
        goto error;

    procargs = (char *)malloc(argmax);
    if (NULL == procargs) {
        PyErr_NoMemory();
        goto error;
    }

    if (psutil_sysctl_procargs(pid, procargs, &argmax) != 0)
        goto error;

    arg_end = &procargs[argmax];
    // copy the number of arguments to nargs
    memcpy(&nargs, procargs, sizeof(nargs));

    // skip executable path
    arg_ptr = procargs + sizeof(nargs);
    arg_ptr = memchr(arg_ptr, '\0', arg_end - arg_ptr);

    if (arg_ptr == NULL || arg_ptr == arg_end) {
        psutil_debug(
            "(arg_ptr == NULL || arg_ptr == arg_end); set environ to empty");
        goto empty;
    }

    // skip ahead to the first argument
    for (; arg_ptr < arg_end; arg_ptr++) {
        if (*arg_ptr != '\0')
            break;
    }

    // iterate through arguments
    while (arg_ptr < arg_end && nargs > 0) {
        if (*arg_ptr++ == '\0')
            nargs--;
    }

    // build an environment variable block
    env_start = arg_ptr;

    procenv = calloc(1, arg_end - arg_ptr);
    if (procenv == NULL) {
        PyErr_NoMemory();
        goto error;
    }

    while (*arg_ptr != '\0' && arg_ptr < arg_end) {
        char *s = memchr(arg_ptr + 1, '\0', arg_end - arg_ptr);
        if (s == NULL)
            break;
        memcpy(procenv + (arg_ptr - env_start), arg_ptr, s - arg_ptr);
        arg_ptr = s + 1;
    }

    py_ret = PyUnicode_DecodeFSDefaultAndSize(
        procenv, arg_ptr - env_start + 1);
    if (!py_ret) {
        // XXX: don't want to free() this as per:
        // https://github.com/giampaolo/psutil/issues/926
        // It sucks but not sure what else to do.
        procargs = NULL;
        goto error;
    }

    free(procargs);
    free(procenv);
    return py_ret;

empty:
    if (procargs != NULL)
        free(procargs);
    return Py_BuildValue("s", "");

error:
    Py_XDECREF(py_ret);
    if (procargs != NULL)
        free(procargs);
    if (procenv != NULL)
        free(procargs);
    return NULL;
}


int
psutil_get_kinfo_proc(pid_t pid, struct kinfo_proc *kp) {
    int mib[4];
    size_t len;
    mib[0] = CTL_KERN;
    mib[1] = KERN_PROC;
    mib[2] = KERN_PROC_PID;
    mib[3] = pid;

    // fetch the info with sysctl()
    len = sizeof(struct kinfo_proc);

    // now read the data from sysctl
    if (sysctl(mib, 4, kp, &len, NULL, 0) == -1) {
        // raise an exception and throw errno as the error
        PyErr_SetFromOSErrnoWithSyscall("sysctl");
        return -1;
    }

    // sysctl succeeds but len is zero, happens when process has gone away
    if (len == 0) {
        NoSuchProcess("sysctl(kinfo_proc), len == 0");
        return -1;
    }
    return 0;
}


/*
 * A wrapper around proc_pidinfo().
 * https://opensource.apple.com/source/xnu/xnu-2050.7.9/bsd/kern/proc_info.c
 * Returns 0 on failure.
 */
int
psutil_proc_pidinfo(pid_t pid, int flavor, uint64_t arg, void *pti, int size) {
    errno = 0;
    int ret;

    ret = proc_pidinfo(pid, flavor, arg, pti, size);
    if (ret <= 0) {
        psutil_raise_for_pid(pid, "proc_pidinfo()");
        return 0;
    }
    if ((unsigned long)ret < sizeof(pti)) {
        psutil_raise_for_pid(
            pid, "proc_pidinfo() return size < sizeof(struct_pointer)");
        return 0;
    }
    return ret;
}
