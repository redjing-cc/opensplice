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

/****************************************************************
 * Initialization / Deinitialization                            *
 ****************************************************************/

/** \file os/common/code/os_init.c
 *  \brief Initialization / Deinitialization
 */

#include "code/os__sharedmem.h"
#include "code/os__thread.h"
#include "os_abstract.h"
#include "os_report.h"
#include "os_atomics.h"

/** \brief Counter that keeps track of number of times os-layer is initialized */
static pa_uint32_t _ospl_osInitCount = PA_UINT32_INIT(0);

/** \brief OS layer initialization
 *
 * \b os_osInit calls:
 * - \b os_sharedMemoryInit
 * - \b os_threadInit
 */
void
os_osInit (
    void)
{
    os_uint32 initCount;

    initCount = pa_inc32_nv(&_ospl_osInitCount);

    if(initCount == 1){
        os_reportInit(OS_FALSE);
        os_sharedMemoryInit();
        os_threadModuleInit();
    }

    return;
}

/** \brief OS layer deinitialization
 *
 * \b os_osExit calls:
 * - \b os_sharedMemoryExit
 * - \b os_threadExit
 */
void
os_osExit (
    void)
{
    os_uint32 initCount;

    initCount = pa_dec32_nv(&_ospl_osInitCount);

    if(initCount == 0){
        os_sharedMemoryExit();
        os_threadModuleExit();
        os_reportExit();
    } else if ((initCount + 1) < initCount){
        /* The 0 boundary is passed, so os_osExit is called more often than
         * os_osInit. Therefore undo decrement as nothing happened and warn. */
        initCount = pa_inc32_nv(&_ospl_osInitCount);
        OS_REPORT(OS_WARNING, "os_osExit", 1, "OS-layer not initialized");
        /* Fail in case of DEV, as it is incorrect API usage */
        assert(0);
    }
    return;
}
