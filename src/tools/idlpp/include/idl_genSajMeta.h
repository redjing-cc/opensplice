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
#ifndef IDL_GENSAJMETA_H
#define IDL_GENSAJMETA_H

#if defined (__cplusplus)
extern "C" {
#endif
#include "os_if.h"

#ifdef OSPL_BUILD_IDL
#define OS_API OS_API_EXPORT
#else
#define OS_API OS_API_IMPORT
#endif
/* !!!!!!!!NOTE From here no more includes are allowed!!!!!!! */

#define IDL_MAX_JSTRING_META_SIZE (32*1024) /* 32KB */

#if defined (__cplusplus)
}
#endif


#undef OS_API

#endif /* IDL_GENSAJMETA_H */
