/* -*- Mode: C++; tab-width: 50; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/* ***** BEGIN LICENSE BLOCK *****
 * Version: MPL 1.1/GPL 2.0/LGPL 2.1
 *
 * The contents of this file are subject to the Mozilla Public License Version
 * 1.1 (the "License"); you may not use this file except in compliance with
 * the License. You may obtain a copy of the License at
 * http://www.mozilla.org/MPL/
 *
 * Software distributed under the License is distributed on an "AS IS" basis,
 * WITHOUT WARRANTY OF ANY KIND, either express or implied. See the License
 * for the specific language governing rights and limitations under the
 * License.
 *
 * The Original Code is mozilla.org code.
 *
 * The Initial Developer of the Original Code is
 * mozilla.org
 * Portions created by the Initial Developer are Copyright (C) 2008
 * the Initial Developer. All Rights Reserved.
 *
 * Contributor(s):
 *   Vladimir Vukicevic <vladimir@pobox.com> (original author)
 *
 * Alternatively, the contents of this file may be used under the terms of
 * either of the GNU General Public License Version 2 or later (the "GPL"),
 * or the GNU Lesser General Public License Version 2.1 or later (the "LGPL"),
 * in which case the provisions of the GPL or the LGPL are applicable instead
 * of those above. If you wish to allow use of your version of this file only
 * under the terms of either the GPL or the LGPL, and not to allow others to
 * use your version of this file under the terms of the MPL, indicate your
 * decision by deleting the provisions above and replace them with the notice
 * and other provisions required by the GPL or the LGPL. If you do not delete
 * the provisions above, a recipient may use your version of this file under
 * the terms of any one of the MPL, the GPL or the LGPL.
 *
 * ***** END LICENSE BLOCK ***** */

#include "nsCOMPtr.h"
#include "nsServiceManagerUtils.h"
#include "nsMemoryReporterManager.h"
#include "nsArrayEnumerator.h"

#if defined(XP_LINUX)

#include <unistd.h>
static PRInt64 GetProcSelfStatmField(int n)
{
    // There are more than two fields, but we're only interested in the first
    // two.
    static const int MAX_FIELD = 2;
    size_t fields[MAX_FIELD];
    NS_ASSERTION(n < MAX_FIELD, "bad field number");
    FILE *f = fopen("/proc/self/statm", "r");
    if (f) {
        int nread = fscanf(f, "%lu %lu", &fields[0], &fields[1]);
        fclose(f);
        return (PRInt64) ((nread == MAX_FIELD) ? fields[n]*getpagesize() : -1);
    }
    return (PRInt64) -1;
}

static PRInt64 GetMapped(void *)
{
    return GetProcSelfStatmField(0);
}

static PRInt64 GetResident(void *)
{
    return GetProcSelfStatmField(1);
}

#elif defined(XP_MACOSX)

#include <mach/mach_init.h>
#include <mach/task.h>

static bool GetTaskBasicInfo(struct task_basic_info *ti)
{
    mach_msg_type_number_t count = TASK_BASIC_INFO_COUNT;
    kern_return_t kr = task_info(mach_task_self(), TASK_BASIC_INFO,
                                 (task_info_t)ti, &count);
    return kr == KERN_SUCCESS;
}

// Getting a sensible "mapped" number on Mac is difficult.  The following is
// easy and (I think) corresponds to the VSIZE figure reported by 'top' and
// 'ps', but that includes shared memory and so is always absurdly high.  This
// doesn't really matter as the "mapped" figure is never that useful.
static PRInt64 GetMapped(void *)
{
    task_basic_info ti;
    return (PRInt64) (GetTaskBasicInfo(&ti) ? ti.virtual_size : -1);
}

static PRInt64 GetResident(void *)
{
    task_basic_info ti;
    return (PRInt64) (GetTaskBasicInfo(&ti) ? ti.resident_size : -1);
}

#elif defined(XP_WIN)

#include <windows.h>
#include <psapi.h>

static PRInt64 GetMapped(void *)
{
#if MOZ_WINSDK_TARGETVER >= MOZ_NTDDI_LONGHORN
  PROCESS_MEMORY_COUNTERS_EX pmcex;
  pmcex.cb = sizeof(PROCESS_MEMORY_COUNTERS_EX);

  if (!GetProcessMemoryInfo(GetCurrentProcess(),
                            (PPROCESS_MEMORY_COUNTERS) &pmcex, sizeof(pmcex)))
      return (PRInt64) -1;

  return pmcex.PrivateUsage;
#else
  return (PRInt64) -1;
#endif
}

static PRInt64 GetResident(void *)
{
  PROCESS_MEMORY_COUNTERS pmc;
  pmc.cb = sizeof(PROCESS_MEMORY_COUNTERS);

  if (!GetProcessMemoryInfo(GetCurrentProcess(), &pmc, sizeof(pmc)))
      return (PRInt64) -1;

  return pmc.WorkingSetSize;
}

#else

static PRInt64 GetMapped(void *)
{
    return (PRInt64) -1;
}

static PRInt64 GetResident(void *)
{
    return (PRInt64) -1;
}

#endif

// aboutMemory.js requires that this reporter always be registered, even if the
// byte count returned is always -1.
NS_MEMORY_REPORTER_IMPLEMENT(Mapped,
    "mapped",
    "Memory mapped by the process. Note that 'resident' is a better measure "
    "of memory resources used by the process. "
    "On Windows (XP SP2 or later only) this is the private usage and does not "
    "include memory shared with other processes. "
    "On Mac and Linux this is the vsize figure as reported by 'top' or 'ps' "
    "and includes memory shared with other processes;  on Mac the amount of "
    "shared memory can be very high and so this figure is of limited use.",
    GetMapped,
    NULL)

NS_MEMORY_REPORTER_IMPLEMENT(Resident,
    "resident",
    "Memory mapped by the process that is present in physical memory, "
    "also known as the resident set size (RSS).  This is the best single "
    "figure to use when considering the memory resources used by the process, "
    "but it depends both on other processes being run and details of the OS "
    "kernel and so is best used for comparing the memory usage of a single "
    "process at different points in time.",
    GetResident,
    NULL)

/**
 ** memory reporter implementation for jemalloc and OSX malloc,
 ** to obtain info on total memory in use (that we know about,
 ** at least -- on OSX, there are sometimes other zones in use).
 **/

#if defined(MOZ_MEMORY)
#  if defined(XP_WIN) || defined(SOLARIS) || defined(ANDROID)
#    define HAVE_JEMALLOC_STATS 1
#    include "jemalloc.h"
#  elif defined(XP_LINUX)
#    define HAVE_JEMALLOC_STATS 1
#    include "jemalloc_types.h"
// jemalloc is directly linked into firefox-bin; libxul doesn't link
// with it.  So if we tried to use jemalloc_stats directly here, it
// wouldn't be defined.  Instead, we don't include the jemalloc header
// and weakly link against jemalloc_stats.
extern "C" {
extern void jemalloc_stats(jemalloc_stats_t* stats)
  NS_VISIBILITY_DEFAULT __attribute__((weak));
}
#  endif  // XP_LINUX
#endif  // MOZ_MEMORY

#if HAVE_JEMALLOC_STATS

static PRInt64 GetMappedHeapUsed(void *)
{
    jemalloc_stats_t stats;
    jemalloc_stats(&stats);
    return (PRInt64) stats.allocated;
}

static PRInt64 GetMappedHeapUnused(void *)
{
    jemalloc_stats_t stats;
    jemalloc_stats(&stats);
    return (PRInt64) (stats.mapped - stats.allocated);
}

static PRInt64 GetHeapCommitted(void *)
{
    jemalloc_stats_t stats;
    jemalloc_stats(&stats);
    return (PRInt64) stats.committed;
}

static PRInt64 GetHeapDirty(void *)
{
    jemalloc_stats_t stats;
    jemalloc_stats(&stats);
    return (PRInt64) stats.dirty;
}

NS_MEMORY_REPORTER_IMPLEMENT(HeapCommitted,
                             "heap-committed",
                             "Memory mapped by the heap allocator that is "
                             "committed, i.e. in physical memory or paged to "
                             "disk.",
                             GetHeapCommitted,
                             NULL)

NS_MEMORY_REPORTER_IMPLEMENT(HeapDirty,
                             "heap-dirty",
                             "Memory mapped by the heap allocator that is "
                             "committed but unused.",
                             GetHeapDirty,
                             NULL)

#elif defined(XP_MACOSX) && !defined(MOZ_MEMORY)
#include <malloc/malloc.h>

static PRInt64 GetMappedHeapUsed(void *)
{
    struct mstats stats = mstats();
    return (PRInt64) stats.bytes_used;
}

static PRInt64 GetMappedHeapUnused(void *)
{
    struct mstats stats = mstats();
    return (PRInt64) (stats.bytes_total - stats.bytes_used);
}

static PRInt64 GetHeapZone0Committed(void *)
{
    malloc_statistics_t stats;
    malloc_zone_statistics(malloc_default_zone(), &stats);
    return stats.size_in_use;
}

static PRInt64 GetHeapZone0Used(void *)
{
    malloc_statistics_t stats;
    malloc_zone_statistics(malloc_default_zone(), &stats);
    return stats.size_allocated;
}

NS_MEMORY_REPORTER_IMPLEMENT(HeapZone0Committed,
                             "heap-zone0-committed",
                             "Memory mapped by the heap allocator that is "
                             "committed in the default zone.",
                             GetHeapZone0Committed,
                             NULL)

NS_MEMORY_REPORTER_IMPLEMENT(HeapZone0Used,
                             "heap-zone0-used",
                             "Memory mapped by the heap allocator in the "
                             "default zone that is available for use by the "
                             "application.",
                             GetHeapZone0Used,
                             NULL)
#else

static PRInt64 GetMappedHeapUsed(void *)
{
    return (PRInt64) -1;
}

static PRInt64 GetMappedHeapUnused(void *)
{
    return (PRInt64) -1;
}

#endif

// aboutMemory.js requires that this reporter always be registered, even if the
// byte count returned is always -1.
NS_MEMORY_REPORTER_IMPLEMENT(MappedHeapUsed,
    "mapped/heap/used",
    "Memory mapped by the heap allocator that is available for use by the "
    "application.  This may exceed the amount of memory requested by the "
    "application due to the allocator rounding up request sizes.  "
    "(The exact amount requested is not measured.) "
    "This is usually the best figure for developers to focus on when trying "
    "to reduce memory consumption.",
    GetMappedHeapUsed,
    NULL)

NS_MEMORY_REPORTER_IMPLEMENT(MappedHeapUnused,
    "mapped/heap/unused",
    "Memory mapped by the heap allocator and not available for use by the "
    "application.  This can grow large if the heap allocator is holding onto "
    "memory that the application has freed.",
    GetMappedHeapUnused,
    NULL)

/**
 ** nsMemoryReporterManager implementation
 **/

NS_IMPL_THREADSAFE_ISUPPORTS1(nsMemoryReporterManager, nsIMemoryReporterManager)

NS_IMETHODIMP
nsMemoryReporterManager::Init()
{
#if HAVE_JEMALLOC_STATS && defined(XP_LINUX)
    if (!jemalloc_stats)
        return NS_ERROR_FAILURE;
#endif

#define REGISTER(_x)  RegisterReporter(new NS_MEMORY_REPORTER_NAME(_x))

    REGISTER(Mapped);
    REGISTER(MappedHeapUsed);
    REGISTER(MappedHeapUnused);
    REGISTER(Resident);

#if defined(HAVE_JEMALLOC_STATS)
    REGISTER(HeapCommitted);
    REGISTER(HeapDirty);
#elif defined(XP_MACOSX) && !defined(MOZ_MEMORY)
    REGISTER(HeapZone0Committed);
    REGISTER(HeapZone0Used);
#endif

    return NS_OK;
}

nsMemoryReporterManager::nsMemoryReporterManager()
  : mMutex("nsMemoryReporterManager::mMutex")
{
}

nsMemoryReporterManager::~nsMemoryReporterManager()
{
}

NS_IMETHODIMP
nsMemoryReporterManager::EnumerateReporters(nsISimpleEnumerator **result)
{
    nsresult rv;
    mozilla::MutexAutoLock autoLock(mMutex);
    rv = NS_NewArrayEnumerator(result, mReporters);
    return rv;
}

NS_IMETHODIMP
nsMemoryReporterManager::RegisterReporter(nsIMemoryReporter *reporter)
{
    mozilla::MutexAutoLock autoLock(mMutex);
    if (mReporters.IndexOf(reporter) != -1)
        return NS_ERROR_FAILURE;

    mReporters.AppendObject(reporter);
    return NS_OK;
}

NS_IMETHODIMP
nsMemoryReporterManager::UnregisterReporter(nsIMemoryReporter *reporter)
{
    mozilla::MutexAutoLock autoLock(mMutex);
    if (!mReporters.RemoveObject(reporter))
        return NS_ERROR_FAILURE;

    return NS_OK;
}

NS_IMPL_ISUPPORTS1(nsMemoryReporter, nsIMemoryReporter)

nsMemoryReporter::nsMemoryReporter(nsCString& prefix,
                                   nsCString& path,
                                   nsCString& desc,
                                   PRInt64 memoryUsed)
: mDesc(desc)
, mMemoryUsed(memoryUsed)
{
  if (!prefix.IsEmpty()) {
      mPath.Append(prefix);
      mPath.Append(NS_LITERAL_CSTRING(":"));
  }
  mPath.Append(path);
}

nsMemoryReporter::~nsMemoryReporter()
{
}

NS_IMETHODIMP nsMemoryReporter::GetPath(char **aPath)
{
    *aPath = strdup(mPath.get());
    return NS_OK;
}

NS_IMETHODIMP nsMemoryReporter::GetDescription(char **aDescription)
{
    *aDescription = strdup(mDesc.get());
    return NS_OK;
}

NS_IMETHODIMP nsMemoryReporter::GetMemoryUsed(PRInt64 *aMemoryUsed)
{
    *aMemoryUsed = mMemoryUsed;
    return NS_OK;
}


NS_COM nsresult
NS_RegisterMemoryReporter (nsIMemoryReporter *reporter)
{
    nsCOMPtr<nsIMemoryReporterManager> mgr = do_GetService("@mozilla.org/memory-reporter-manager;1");
    if (mgr == nsnull)
        return NS_ERROR_FAILURE;
    return mgr->RegisterReporter(reporter);
}

NS_COM nsresult
NS_UnregisterMemoryReporter (nsIMemoryReporter *reporter)
{
    nsCOMPtr<nsIMemoryReporterManager> mgr = do_GetService("@mozilla.org/memory-reporter-manager;1");
    if (mgr == nsnull)
        return NS_ERROR_FAILURE;
    return mgr->UnregisterReporter(reporter);
}

