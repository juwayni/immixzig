#include <hxcpp.h>

#include <hx/GC.h>
#include <hx/Memory.h>
#include <hx/Thread.h>
#include <hx/thread/Thread.hpp>
#include "../Hash.h"
#include "GcRegCapture.h"
#include <unordered_set>
#include <unordered_map>
#include <mutex>
#include <thread>
#include <condition_variable>
#include <inttypes.h>

#ifdef __EMSCRIPTEN__
   #include <emscripten/stack.h>
   #ifdef HXCPP_SINGLE_THREADED_APP
      // Use provided tools to measure stack extent
      #define HXCPP_EXPLICIT_STACK_EXTENT
   #endif
#endif

#include <string>
#include <stdlib.h>

namespace
{
    // It took until C++26 for them to add saturating arithmetic!
    // So lets have some very basic saturating maths helpers.
    // Funky branchless saturating from https://web.archive.org/web/20190213215419/https://locklessinc.com/articles/sat_arithmetic/

    template<class T>
    T saturating_add(T x, T delta)
    {
        T res = x + delta;
        res |= -(res < x);

        return res;
    }

    template<class T>
    T saturating_sub(T x, T delta)
    {
        T res = x - delta;
        res &= -(res <= x);

        return res;
    }

    template<class T>
    bool atomic_compare_exchange_strong_discard(std::atomic<T>* obj, typename std::atomic<T>::value_type expected, typename std::atomic<T>::value_type desired)
    {
        typename std::atomic<T>::value_type v{ expected };

        return obj->compare_exchange_strong(v, desired);
    }
}

static bool sgIsCollecting = false;

namespace hx
{
   int gByteMarkID = 0x10;
   int gRememberedByteMarkID = 0x10 | HX_GC_REMEMBERED;


int gFastPath = 0;
int gSlowPath = 0;


}

using hx::gByteMarkID;
using hx::gRememberedByteMarkID;


namespace hx
{
#ifdef HXCPP_GC_DEBUG_ALWAYS_MOVE

enum { gAlwaysMove = true };
typedef std::unordered_set<void *> PointerMovedSet;
PointerMovedSet sgPointerMoved;

#else
enum { gAlwaysMove = false };
#endif
}

#ifdef ANDROID
#include <android/log.h>
#endif

#ifdef HX_WINDOWS
#include <windows.h>
#endif

#include <vector>
#include <stdio.h>

#include <hx/QuickVec.h>

// #define HXCPP_GC_BIG_BLOCKS


#ifndef __has_builtin
#define __has_builtin(x) 0
#endif
namespace {
void DebuggerTrap()
{
   static bool triggeredOnce = false;

   if (!triggeredOnce)
   {
      triggeredOnce = true;

      #if __has_builtin(__builtin_trap)
      __builtin_trap();
      #else
      *(int *)0=0;
      #endif
   }
}
}




static bool sgAllocInit = 0;
static bool sgInternalEnable = true;
// With virtual inheritance, stack pointers can point to the middle of an object
#ifdef _MSC_VER
// MSVC optimizes by taking the address of an initernal data member
static uintptr_t sgCheckInternalOffset = sizeof(void *)*2;
#else
static uintptr_t sgCheckInternalOffset = 0;
#endif

int gInAlloc = false;

// This is recalculated from the other parameters
static size_t sWorkingMemorySize          = 10*1024*1024;

#ifdef HXCPP_GC_MOVING
// Just not sure what this shold be
static size_t sgMaximumFreeSpace  = 1024*1024*1024;
#else
static size_t sgMaximumFreeSpace  = 1024*1024*1024;
#endif


// #define HXCPP_GC_DEBUG_LEVEL 1

#if HXCPP_GC_DEBUG_LEVEL>1
  #define PROFILE_COLLECT
  #if HXCPP_GC_DEBUG_LEVEL>2
     #define SHOW_FRAGMENTATION
     #if HXCPP_GC_DEBUG_LEVEL>3
        #define SHOW_MEM_EVENTS
     #endif
  #endif
#endif

// #define SHOW_FRAGMENTATION_BLOCKS
#if defined SHOW_FRAGMENTATION_BLOCKS
  #define SHOW_FRAGMENTATION
#endif

#define RECYCLE_LARGE

// HXCPP_GC_DYNAMIC_SIZE

//#define HXCPP_GC_SUMMARY
//#define PROFILE_COLLECT
//#define PROFILE_THREAD_USAGE
//#define HXCPP_GC_VERIFY
//#define HX_GC_VERIFY_ALLOC_START
//#define SHOW_MEM_EVENTS
//#define SHOW_MEM_EVENTS_VERBOSE
//#define SHOW_FRAGMENTATION
//
// Setting this can make it easier to reproduce this problem since it takes
//  the timing of the zeroing out of the equation
//#define HX_GC_ZERO_EARLY

// Setting this on windows64 will mean this objects are allocated in the same place,
//  which can make native debugging easier
//#define HX_GC_FIXED_BLOCKS
//
// If the problem happens on the same object, you can print info about the object
//  every collect so you can see where it goes wrong (usually requires HX_GC_FIXED_BLOCKS)
//#define HX_WATCH

#if defined(HXCPP_GC_VERIFY) && defined(HXCPP_GC_GENERATIONAL)
#define HX_GC_VERIFY_GENERATIONAL
#endif


#ifdef HX_WATCH
uintptr_t hxWatchList[] = {
  uintptr_t{ 0x0000000100000200 },
  uintptr_t{ 0 }
};
bool hxInWatchList(uintptr_t watch)
{
   for(uintptr_t* t = hxWatchList; *t; t++)
      if (*t==watch)
         return true;
   return false;
}
#endif

#ifdef HX_GC_FIXED_BLOCKS
#ifdef HX_WINDOWS
#include <Memoryapi.h>
#endif
#endif

#ifdef PROFILE_COLLECT
   #define HXCPP_GC_SUMMARY
#endif

#ifdef HX_GC_VERIFY_GENERATIONAL
static bool sGcVerifyGenerational = false;
#endif


#if HX_HAS_ATOMIC && (HXCPP_GC_DEBUG_LEVEL==0) && !defined(HXCPP_GC_VERIFY) && !defined(__EMSCRIPTEN__)
  #if defined(HX_MACOS) || defined(HX_WINDOWS) || defined(HX_LINUX)
  enum { MAX_GC_THREADS = 4 };
  #else
  enum { MAX_GC_THREADS = 2 };
  #endif

  // You can uncomment this for better call stacks if it crashes while collecting
  #define HX_MULTI_THREAD_MARKING
#else
  enum { MAX_GC_THREADS = 1 };
#endif

#ifdef PROFILE_THREAD_USAGE
static int sThreadMarkCountData[MAX_GC_THREADS+1];
static int sThreadArrayMarkCountData[MAX_GC_THREADS+1];
static int *sThreadMarkCount = sThreadMarkCountData + 1;
static int *sThreadArrayMarkCount = sThreadArrayMarkCountData + 1;
static std::atomic_int sThreadChunkPushCount;
static int sThreadChunkWakes;
static std::atomic_int sSpinCount = 0;
static int sThreadZeroWaits = 0;
static int sThreadZeroPokes = 0;
static int sThreadBlockZeroCount = 0;
static std::atomic_int sThreadZeroMisses{};
#endif

enum { MARK_BYTE_MASK = 0x0f };
enum { FULL_MARK_BYTE_MASK = 0x3f };


enum
{
   MEM_INFO_USAGE = 0,
   MEM_INFO_RESERVED = 1,
   MEM_INFO_CURRENT = 2,
   MEM_INFO_LARGE = 3,
};

enum GcMode
{
   gcmFull,
   gcmGenerational,
};

// Start with full gc - since all objects will be new
static GcMode sGcMode = gcmFull;



#ifndef HXCPP_GC_MOVING
  #ifdef HXCPP_GC_DEBUG_ALWAYS_MOVE
  #define HXCPP_GC_MOVING
  #endif
// Enable moving collector...
//#define HXCPP_GC_MOVING
#endif

// Allocate this many blocks at a time - this will increase memory usage % when rounding to block size must be done.
// However, a bigger number makes it harder to release blocks due to pinning
constexpr uint8_t IMMIX_BLOCK_GROUP_BITS{ 5 };


#ifdef HXCPP_DEBUG
static hx::Object *gCollectTrace = 0;
static bool gCollectTraceDoPrint = false;
static int gCollectTraceCount = 0;
static int sgSpamCollects = 0;
#endif

#if defined(HXCPP_DEBUG) || defined(HXCPP_GC_DEBUG_ALWAYS_MOVE)
static std::atomic_int sgAllocsSinceLastSpam{};
#endif

#ifdef ANDROID
#define GCLOG(...) __android_log_print(ANDROID_LOG_INFO, "gclog", __VA_ARGS__)
#else
#define GCLOG printf
#endif

#ifdef PROFILE_COLLECT
   #define STAMP(t) double t = __hxcpp_time_stamp();
   #define MEM_STAMP(t) t = __hxcpp_time_stamp();
   static double sLastCollect = __hxcpp_time_stamp();
   static int sObjectMarks =0;
   static int sAllocMarks =0;

#else
   #define STAMP(t)
   #define MEM_STAMP(t)
#endif

#if defined(HXCPP_GC_SUMMARY) || defined(HXCPP_GC_DYNAMIC_SIZE)
struct ProfileCollectSummary
{
   enum { COUNT = 10 };
   double timeWindow[COUNT];
   int    windowIdx;
   double startTime;
   double lastTime;
   double totalCollecting;
   double maxStall;
   double spaceFactor;

   ProfileCollectSummary()
   {
      startTime = __hxcpp_time_stamp();
      lastTime = startTime;
      totalCollecting = 0;
      maxStall = 0;
      windowIdx = 0;
      spaceFactor = 1.0;
      for(int i=0;i<COUNT;i++)
         timeWindow[i] = 0.1;
   }
   ~ProfileCollectSummary()
   {
      #ifdef HXCPP_GC_SUMMARY
      double time = __hxcpp_time_stamp() - startTime;
      GCLOG("Total time     : %.2fms\n", time*1000.0);
      GCLOG("Collecting time: %.2fms\n", totalCollecting*1000.0);
      GCLOG("Max Stall time : %.2fms\n", maxStall*1000.0);
      if (time==0) time = 1;
      GCLOG(" Fraction      : %.2f%%\n",totalCollecting*100.0/time);

      #ifdef HXCPP_GC_DYNAMIC_SIZE
      GCLOG("Space factor   : %.2fx\n",spaceFactor);
      #endif
      #endif
   }

   void addTime(double inCollectStart)
   {
      double now = __hxcpp_time_stamp();
      double dt =  now-inCollectStart;
      if (dt>maxStall)
         maxStall = dt;
      totalCollecting += dt;
      #ifdef HXCPP_GC_DYNAMIC_SIZE
      double wholeTime = now - lastTime;
      if (wholeTime)
      {
         lastTime = now;
         double ratio = dt/wholeTime;
         windowIdx = (windowIdx +1)%COUNT;
         timeWindow[windowIdx] = ratio;
         double sum = 0;
         for(int i=0;i<COUNT;i++)
            sum += timeWindow[i];
         ratio = sum/COUNT;

         if (ratio<0.05)
         {
            if (spaceFactor>1.0)
            {
               spaceFactor -= 0.2;
               if (spaceFactor<1.0)
                  spaceFactor = 1.0;
            }
         }
         else if (ratio>0.12)
         {
            if (ratio>0.2)
               spaceFactor += 0.5;
            else
               spaceFactor += 0.2;

            if (spaceFactor>5.0)
               spaceFactor = 5.0;
         }
      }
      #endif
   }

};

static ProfileCollectSummary profileCollectSummary;
#define PROFILE_COLLECT_SUMMARY_START double collectT0 = __hxcpp_time_stamp();
#define PROFILE_COLLECT_SUMMARY_END profileCollectSummary.addTime(collectT0);

#else
#define PROFILE_COLLECT_SUMMARY_START
#define PROFILE_COLLECT_SUMMARY_END
#endif



// TODO: Telemetry.h ?
#ifdef HXCPP_TELEMETRY
extern void __hxt_gc_realloc(void* old_obj, void* new_obj, int new_size);
extern void __hxt_gc_start();
extern void __hxt_gc_end();
extern void __hxt_gc_after_mark(int gByteMarkID, int markIdByte);
#endif

static int sgTimeToNextTableUpdate = 1;




std::recursive_mutex *gThreadStateChangeLock=nullptr;
std::mutex *gSpecialObjectLock=nullptr;

class LocalAllocator;
enum LocalAllocState { lasNew, lasRunning, lasStopped, lasWaiting, lasTerminal };


static void MarkLocalAlloc(LocalAllocator *inAlloc,hx::MarkContext *__inCtx);
#ifdef HXCPP_VISIT_ALLOCS
static void VisitLocalAlloc(LocalAllocator *inAlloc,hx::VisitContext *__inCtx);
#endif
static void WaitForSafe(LocalAllocator *inAlloc);
static void ReleaseFromSafe(LocalAllocator *inAlloc);
static void ClearPooledAlloc(LocalAllocator *inAlloc);
static void CollectFromThisThread(bool inMajor,bool inForceCompact);

namespace hx
{
std::atomic_uint gPauseForCollect{ 0x00000000 };

StackContext *gMainThreadContext = 0;
