/*
 *                         Vortex OpenSplice
 *
 *   This software and documentation are Copyright 2006 to TO_YEAR ADLINK
 *   Technology Limited, its affiliated companies and licensors. All rights
 *   reserved.
 *
 *   Licensed under the Apache License, Version 2.0 (the "License");
 *   you may not use this file except in compliance with the License.
 *   You may obtain a copy of the License at
 *
 *       http://www.apache.org/licenses/LICENSE-2.0
 *
 *   Unless required by applicable law or agreed to in writing, software
 *   distributed under the License is distributed on an "AS IS" BASIS,
 *   WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 *   See the License for the specific language governing permissions and
 *   limitations under the License.
 *
 */
#ifndef OS_WIN32_DEFS_H
#define OS_WIN32_DEFS_H

// Suppress spurious 'child' : inherits 'parent::member' via dominance warnings
#if defined (_MSC_VER)
    #pragma warning(disable:4250)
#endif /* _MSC_VER */

#if defined (__cplusplus)
extern "C" {
#endif

#define OS_SOCKET_USE_FCNTL 0
#define OS_SOCKET_USE_IOCTL 1
#define OS_HAS_UCONTEXT_T

#include "os_os_abstract.h"
#include "os_os_types.h"
#include "os_os_if.h"

#include "os_os_alloca.h"
#include "os_os_mutex.h"
#include "os_os_cond.h"
#include "os_os_thread.h"
#include "os_os_library.h"
#include "os_os_semaphore.h"
#include "os_os_signal.h"
#include "os_os_stdlib.h"
#include "os_os_process.h"
#include "os_os_utsname.h"

#include "os_os_rwlock.h"

#if defined (__cplusplus)
}
#endif

#undef OS_API

#endif /* OS_WIN32_DEFS_H */
