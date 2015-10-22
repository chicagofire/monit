/*
 * Copyright (C) Tildeslash Ltd. All rights reserved.
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU Affero General Public License version 3.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU Affero General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 * In addition, as a special exception, the copyright holders give
 * permission to link the code of portions of this program with the
 * OpenSSL library under certain conditions as described in each
 * individual source file, and distribute linked combinations
 * including the two.
 *
 * You must obey the GNU Affero General Public License in all respects
 * for all of the code used other than OpenSSL.
 */


#include "config.h"

#ifdef HAVE_ERRNO_H
#include <errno.h>
#endif

#ifdef HAVE_STRING_H
#include <string.h>
#endif

#ifdef HAVE_FCNTL_H
#include <fcntl.h>
#endif

#ifdef HAVE_KVM_H
#include <kvm.h>
#endif

#ifdef HAVE_SYS_SYSCTL_H
#include <sys/sysctl.h>
#endif

#ifdef HAVE_SYS_VMMETER_H
#include <sys/vmmeter.h>
#endif

#ifdef HAVE_MACH_MACH_H
#include <mach/mach.h>
#endif

#ifdef HAVE_MACH_HOST_INFO_H
#include <mach/host_info.h>
#endif

#ifdef HAVE_MACH_MACH_HOST_H
#include <mach/mach_host.h>
#endif

#include "monit.h"
#include "process.h"
#include "process_sysdep.h"


/**
 *  System dependent resource gathering code for MacOS X.
 *
 *  @file
 */


#define ARGSSIZE 8192


/* ----------------------------------------------------------------- Private */


static int  hz;
static int  pagesize_kbyte;
static long total_old    = 0;
static long cpu_user_old = 0;
static long cpu_syst_old = 0;
static boolean_t isSipEnabled = true;


/* ----------------------- OS X >= 10.11 System Integrity Protection Check */

/* Check if OS X 10.11 System Integrity Protection (SIP) is enabled. The idea
 here is to collect all pids via processor_set_tasks() and if we have pid = 1 in
 the list, then SIP is _not_ enabled, otherwise we assume it is. If SIP
 is enabled, we are not allowed to call task_for_pid. The alternative is to 
 call the private API csr_check(CSR_ALLOW_TASK_FOR_PID) but this API is only 
 supported back to 10.10 AFAIK while we need to support systems all the way
 back to 10.6 and test the feature at runtime not at build time (via ifdefs) 
 @return true if enabled otherwise false */
static boolean_t _isSipEnabled() {
        isSipEnabled = true;
        host_t myhost = mach_host_self();
        mach_port_t psDefault;
        kern_return_t status = processor_set_default(myhost, &psDefault);
        mach_port_t psDefaultCtrl;
        status = host_processor_set_priv(myhost, psDefault, &psDefaultCtrl);
        if (status != KERN_SUCCESS) {
                // Will fail if we are not running as root in which case task_for_pid will fail anyway
                DEBUG("host_processor_set_priv failed -- %s\n", mach_error_string(status));
                return isSipEnabled;
        }
        task_array_t tasks;
        mach_msg_type_number_t nTasks;
        status = processor_set_tasks(psDefaultCtrl, &tasks, &nTasks);
        if (status != KERN_SUCCESS) {
                DEBUG("processor_set_tasks failed with error -- %s\n", mach_error_string(status));
                return isSipEnabled;
        }
        for (int i = 0; i < nTasks; i++) {
                int pid;
                pid_for_task(tasks[i], &pid);
                if (pid == 1) {
                        isSipEnabled = false;
                }
                mach_port_deallocate(mach_task_self(), tasks[i]);
        }
        status= vm_deallocate(mach_task_self(), (vm_address_t)tasks, nTasks * sizeof(task_t));
        status = mach_port_deallocate(mach_task_self(), psDefaultCtrl);
#if __MAC_OS_X_VERSION_MAX_ALLOWED >= __MAC_10_11
        if (isSipEnabled) {
                DEBUG("System Integrity Protection is enabled and Monit cannot check process memory or CPU usage\n");
        }
#endif
        return isSipEnabled;
}


/* ------------------------------------------------------------------ Public */


boolean_t init_process_info_sysdep(void) {
        int              mib[2];
        size_t           len;
        struct clockinfo clock;
        uint64_t         memsize;

        mib[0] = CTL_KERN;
        mib[1] = KERN_CLOCKRATE;
        len    = sizeof(clock);
        if (sysctl(mib, 2, &clock, &len, NULL, 0) == -1) {
                DEBUG("system statistic error -- cannot get clock rate: %s\n", STRERROR);
                return false;
        }
        hz     = clock.hz;

        mib[0] = CTL_HW;
        mib[1] = HW_NCPU;
        len    = sizeof(systeminfo.cpus);
        if (sysctl(mib, 2, &systeminfo.cpus, &len, NULL, 0) == -1) {
                DEBUG("system statistic error -- cannot get cpu count: %s\n", STRERROR);
                return false;
        }

        mib[1]  = HW_MEMSIZE;
        len     = sizeof(memsize);
        memsize = 0L;
        if (sysctl(mib, 2, &memsize, &len, NULL, 0 ) == -1) {
                DEBUG("system statistic error -- cannot get real memory amount: %s\n", STRERROR);
                return false;
        }
        systeminfo.mem_kbyte_max = (memsize / 1024);

        mib[1] = HW_PAGESIZE;
        len    = sizeof(pagesize_kbyte);
        if (sysctl(mib, 2, &pagesize_kbyte, &len, NULL, 0) == -1) {
                DEBUG("system statistic error -- cannot get memory page size: %s\n", STRERROR);
                return false;
        }
        pagesize_kbyte /= 1024;

        isSipEnabled = _isSipEnabled();

        return true;
}


/**
 * Read all processes to initialize the information tree.
 * @param reference  reference of ProcessTree
 * @return treesize>0 if succeeded otherwise =0.
 */
int initprocesstree_sysdep(ProcessTree_T **reference) {
        size_t             treesize;
        mach_port_t        mytask = mach_task_self();
        ProcessTree_T     *pt;
        struct kinfo_proc *pinfo;
        size_t             pinfo_size = 0;
        char              *args;
        size_t             args_size = 0;
        size_t             size;
        int                mib[4];

        mib[0] = CTL_KERN;
        mib[1] = KERN_PROC;
        mib[2] = KERN_PROC_ALL;
        mib[3] = 0;
        if (sysctl(mib, 4, NULL, &pinfo_size, NULL, 0) < 0) {
                LogError("system statistic error -- sysctl failed: %s\n", STRERROR);
                return 0;
        }
        pinfo = CALLOC(1, pinfo_size);
        if (sysctl(mib, 4, pinfo, &pinfo_size, NULL, 0)) {
                FREE(pinfo);
                LogError("system statistic error -- sysctl failed: %s\n", STRERROR);
                return 0;
        }
        treesize = pinfo_size / sizeof(struct kinfo_proc);
        pt = CALLOC(sizeof(ProcessTree_T), treesize);

        mib[0] = CTL_KERN;
        mib[1] = KERN_ARGMAX;
        size = sizeof(args_size);
        if (sysctl(mib, 2, &args_size, &size, NULL, 0) == -1) {
                FREE(pinfo);
                FREE(pt);
                LogError("system statistic error -- sysctl failed: %s\n", STRERROR);
                return 0;
        }
        args = CALLOC(1, args_size + 1);
        size = args_size; // save for per-process sysctl loop

        for (int i = 0; i < treesize; i++) {
                mach_port_t task;

                pt[i].pid       = pinfo[i].kp_proc.p_pid;
                pt[i].ppid      = pinfo[i].kp_eproc.e_ppid;
                pt[i].uid       = pinfo[i].kp_eproc.e_pcred.p_ruid;
                pt[i].euid      = pinfo[i].kp_eproc.e_ucred.cr_uid;
                pt[i].gid       = pinfo[i].kp_eproc.e_pcred.p_rgid;
                pt[i].starttime = pinfo[i].kp_proc.p_starttime.tv_sec;

                args_size = size;
                mib[0] = CTL_KERN;
                mib[1] = KERN_PROCARGS2;
                mib[2] = pt[i].pid;
                if (sysctl(mib, 3, args, &args_size, NULL, 0) != -1) {
                        /* KERN_PROCARGS2 sysctl() returns following pseudo structure:
                         *        struct {
                         *                int argc
                         *                char execname[];
                         *                char argv[argc][];
                         *                char env[][];
                         *        }
                         * The strings are terminated with '\0' and may have variable '\0' padding
                         */
                        int  argc = *args;
                        char *p = args + sizeof(int); // arguments beginning
                        StringBuffer_T cmdline = StringBuffer_create(64);
                        p += strlen(p); // skip exename
                        while (argc && p < args + args_size) {
                                if (*p == 0) { // skip terminating 0 and variable length 0 padding
                                        p++;
                                        continue;
                                }
                                StringBuffer_append(cmdline, argc-- ? "%s " : "%s", p);
                                p += strlen(p);
                        }
                        if (StringBuffer_length(cmdline))
                                pt[i].cmdline = Str_dup(StringBuffer_toString(StringBuffer_trim(cmdline)));
                        StringBuffer_free(&cmdline);
                }
                if (! pt[i].cmdline || ! *pt[i].cmdline) {
                        FREE(pt[i].cmdline);
                        pt[i].cmdline = Str_dup(pinfo[i].kp_proc.p_comm);
                }

                if (pinfo[i].kp_proc.p_stat == SZOMB)
                        pt[i].zombie = true;
                pt[i].time = get_float_time();

                /* Issue #266: As of OS X 10.11 a new System Integrity Protection policy
                 (SIP) is in use which deny usage of task_for_pid, i.e. we cannot get
                 process info and to continue here would be useless. SIP is enabled by
                 default on 10.11. If we are running as non-root 'isSipEnabled' is also
                 set to true, as we are not allowed to call task_for_pid then neither */
                if (! isSipEnabled) {
                        if (task_for_pid(mytask, pt[i].pid, &task) == KERN_SUCCESS) {
                                mach_msg_type_number_t   count;
                                task_basic_info_data_t   taskinfo;
                                thread_array_t           threadtable;
                                unsigned int             threadtable_size;
                                thread_basic_info_t      threadinfo;
                                thread_basic_info_data_t threadinfo_data;

                                count = TASK_BASIC_INFO_COUNT;
                                if (task_info(task, TASK_BASIC_INFO, (task_info_t)&taskinfo, &count) == KERN_SUCCESS) {
                                        pt[i].mem_kbyte   = (unsigned long)(taskinfo.resident_size / 1024);
                                        pt[i].cputime     = (long)((taskinfo.user_time.seconds + taskinfo.system_time.seconds) * 10 + (taskinfo.user_time.microseconds + taskinfo.system_time.microseconds) / 100000);
                                        pt[i].cpu_percent = 0;
                                }
                                if (task_threads(task, &threadtable, &threadtable_size) == KERN_SUCCESS) {
                                        threadinfo = &threadinfo_data;
                                        for (int j = 0; j < threadtable_size; j++) {
                                                count = THREAD_BASIC_INFO_COUNT;
                                                if (thread_info(threadtable[j], THREAD_BASIC_INFO, (thread_info_t)threadinfo, &count) == KERN_SUCCESS) {
                                                        if ((threadinfo->flags & TH_FLAGS_IDLE) == 0) {
                                                                pt[i].cputime += (long)((threadinfo->user_time.seconds + threadinfo->system_time.seconds) * 10 + (threadinfo->user_time.microseconds + threadinfo->system_time.microseconds) / 100000);
                                                                pt[i].cpu_percent = 0;
                                                        }
                                                }
                                                mach_port_deallocate(mytask, threadtable[j]);
                                        }
                                        vm_deallocate(mytask, (vm_address_t)threadtable,threadtable_size * sizeof(thread_act_t));
                                }
                                mach_port_deallocate(mytask, task);
                        }
                }
                if (task_for_pid(mytask, pt[i].pid, &task) == KERN_SUCCESS) {
                        mach_msg_type_number_t   count;
                        task_basic_info_data_t   taskinfo;
                        thread_array_t           threadtable;
                        unsigned int             threadtable_size;
                        thread_basic_info_t      threadinfo;
                        thread_basic_info_data_t threadinfo_data;

                        count = TASK_BASIC_INFO_COUNT;
                        if (task_info(task, TASK_BASIC_INFO, (task_info_t)&taskinfo, &count) == KERN_SUCCESS) {
                                pt[i].mem_kbyte   = (unsigned long)(taskinfo.resident_size / 1024);
                                pt[i].cputime     = (long)((taskinfo.user_time.seconds + taskinfo.system_time.seconds) * 10 + (taskinfo.user_time.microseconds + taskinfo.system_time.microseconds) / 100000);
                                pt[i].cpu_percent = 0;
                        }
                        if (task_threads(task, &threadtable, &threadtable_size) == KERN_SUCCESS) {
                                threadinfo = &threadinfo_data;
                                for (int j = 0; j < threadtable_size; j++) {
                                        count = THREAD_BASIC_INFO_COUNT;
                                        if (thread_info(threadtable[j], THREAD_BASIC_INFO, (thread_info_t)threadinfo, &count) == KERN_SUCCESS) {
                                                if ((threadinfo->flags & TH_FLAGS_IDLE) == 0) {
                                                        pt[i].cputime += (long)((threadinfo->user_time.seconds + threadinfo->system_time.seconds) * 10 + (threadinfo->user_time.microseconds + threadinfo->system_time.microseconds) / 100000);
                                                        pt[i].cpu_percent = 0;
                                                }
                                        }
                                        mach_port_deallocate(mytask, threadtable[j]);
                                }
                                vm_deallocate(mytask, (vm_address_t)threadtable,threadtable_size * sizeof(thread_act_t));
                        }
                        mach_port_deallocate(mytask, task);
                }
        }
        FREE(args);
        FREE(pinfo);
        
        *reference = pt;
        
        return (int)treesize;
}


/**
 * This routine returns 'nelem' double precision floats containing
 * the load averages in 'loadv'; at most 3 values will be returned.
 * @param loadv destination of the load averages
 * @param nelem number of averages
 * @return: 0 if successful, -1 if failed (and all load averages are 0).
 */
int getloadavg_sysdep (double *loadv, int nelem) {
        return getloadavg(loadv, nelem);
}


/**
 * This routine returns kbyte of real memory in use.
 * @return: true if successful, false if failed (or not available)
 */
boolean_t used_system_memory_sysdep(SystemInfo_T *si) {
        /* Memory */
        vm_statistics_data_t page_info;
        mach_msg_type_number_t count = HOST_VM_INFO_COUNT;
        kern_return_t kret = host_statistics(mach_host_self(), HOST_VM_INFO, (host_info_t)&page_info, &count);
        if (kret != KERN_SUCCESS) {
                DEBUG("system statistic error -- cannot get memory usage\n");
                return false;
        }
        si->total_mem_kbyte = (page_info.wire_count + page_info.active_count) * pagesize_kbyte;

        /* Swap */
        int mib[2] = {CTL_VM, VM_SWAPUSAGE};
        size_t len = sizeof(struct xsw_usage);
        struct xsw_usage swap;
        if (sysctl(mib, 2, &swap, &len, NULL, 0) == -1) {
                DEBUG("system statistic error -- cannot get swap usage: %s\n", STRERROR);
                si->swap_kbyte_max = 0;
                return false;
        }
        si->swap_kbyte_max   = (unsigned long)(double)(swap.xsu_total) / 1024.;
        si->total_swap_kbyte = (unsigned long)(double)(swap.xsu_used) / 1024.;

        return true;
}


/**
 * This routine returns system/user CPU time in use.
 * @return: true if successful, false if failed
 */
boolean_t used_system_cpu_sysdep(SystemInfo_T *si) {
        long                      total;
        long                      total_new = 0;
        kern_return_t             kret;
        host_cpu_load_info_data_t cpu_info;
        mach_msg_type_number_t    count;

        count = HOST_CPU_LOAD_INFO_COUNT;
        kret  = host_statistics(mach_host_self(), HOST_CPU_LOAD_INFO, (host_info_t)&cpu_info, &count);
        if (kret == KERN_SUCCESS) {
                for (int i = 0; i < CPU_STATE_MAX; i++)
                        total_new += cpu_info.cpu_ticks[i];
                total     = total_new - total_old;
                total_old = total_new;

                si->total_cpu_user_percent = (total > 0) ? (int)(1000 * (double)(cpu_info.cpu_ticks[CPU_STATE_USER] - cpu_user_old) / total) : -10;
                si->total_cpu_syst_percent = (total > 0) ? (int)(1000 * (double)(cpu_info.cpu_ticks[CPU_STATE_SYSTEM] - cpu_syst_old) / total) : -10;
                si->total_cpu_wait_percent = 0; /* there is no wait statistic available */

                cpu_user_old = cpu_info.cpu_ticks[CPU_STATE_USER];
                cpu_syst_old = cpu_info.cpu_ticks[CPU_STATE_SYSTEM];

                return true;
        }
        return false;
}
