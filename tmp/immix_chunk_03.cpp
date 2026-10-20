      return allocNone;
   }


   void pin() { mPinned = true; }


   #ifdef HXCPP_VISIT_ALLOCS
   void VisitBlock(hx::VisitContext *inCtx)
   {
      if (isEmpty())
         return;

      unsigned char *rowMarked = mPtr->mRowMarked;
      for(int r=IMMIX_HEADER_LINES;r<IMMIX_LINES;r++)
      {
         if (rowMarked[r])
         {
            unsigned int starts{ allocStart[r] };
            if (!starts)
               continue;
            unsigned char* row{ mPtr->row(static_cast<size_t>(r)) };
            for(int i=0;i<32;i++)
            {
               int pos = i<<2;
               if ( (starts & (1<<i)) &&
                   (row[pos+HX_ENDIAN_MARK_ID_BYTE_HEADER]&FULL_MARK_BYTE_MASK) == gByteMarkID)
                  {
                     if ( (*(unsigned int *)(row+pos)) & IMMIX_ALLOC_IS_CONTAINER )
                     {
                        hx::Object *obj = (hx::Object *)(row+pos+4);
                        // May be partially constructed
                        if (*(void **)obj)
                           obj->__Visit(inCtx);
                     }
                  }
            }
         }
      }
   }
   #endif

   #ifdef HXCPP_GC_VERIFY
   void verify(const char *inWhere)
   {
      for(int i=IMMIX_HEADER_LINES;i<IMMIX_LINES;i++)
      {
         for(int j=0;j<32;j++)
            if (allocStart[i] & (1<<j))
            {
               unsigned int header = *(unsigned int *)( (char *)mPtr + i*IMMIX_LINE_LEN + j*4 );
               if ((header & IMMIX_ALLOC_MARK_ID) == hx::gMarkID )
               {
                  int rows = header & IMMIX_ALLOC_ROW_COUNT;
                  if (rows==0)
                  {
                     printf("BAD ROW0 %s\n", inWhere);
                     DebuggerTrap();
                  }
                  for(int r=0;r<rows;r++)
                     if (!mPtr->mRowMarked[r+i])
                     {
                        printf("Unmarked row %dx %d/%d %s, t=%d!\n", i, r, rows, inWhere,sgTimeToNextTableUpdate);
                        DebuggerTrap();
                     }
               }
            }
         }

   }
   #endif
};

static bool SmallestFreeFirst(BlockDataInfo *inA, BlockDataInfo *inB)
{
   return inA->mMaxHoleSize < inB->mMaxHoleSize;
}

static bool SortMoveOrder(BlockDataInfo *inA, BlockDataInfo *inB)
{
   return inA->mMoveScore > inB->mMoveScore;
}

namespace hx
{



void BadImmixAlloc()
{

   #ifdef HX_WINRT
   WINRT_LOG("Bad local allocator - requesting memory from unregistered thread!");
   #else
   #ifdef ANDROID
   __android_log_print(ANDROID_LOG_ERROR, "hxcpp",
   #else
   fprintf(stderr,
   #endif

   "Bad local allocator - requesting memory from unregistered thread!"

   #ifdef ANDROID
   );
   #else
   );
   #endif
   #endif

   DebuggerTrap();
}



void GCCheckPointer(void *inPtr)
{
   #ifdef HXCPP_GC_DEBUG_ALWAYS_MOVE
   if (hx::sgPointerMoved.find(inPtr)!=hx::sgPointerMoved.end())
   {
      GCLOG("Accessing moved pointer %p\n", inPtr);
      DebuggerTrap();
   }
   #endif

   unsigned char&mark = ((unsigned char *)inPtr)[HX_ENDIAN_MARK_ID_BYTE];
   #ifdef HXCPP_GC_NURSERY
   if (mark)
   #endif
   if ( !(mark & HX_GC_CONST_ALLOC_MARK_BIT) && (mark&FULL_MARK_BYTE_MASK)!=gByteMarkID  )
   {
      GCLOG("Old object access %p\n", inPtr);
      NullReference("Object", false);
   }
}


void GCOnNewPointer(void *inPtr)
{
   #ifdef HXCPP_ALIGN_ALLOC
   if ( reinterpret_cast<uintptr_t>(inPtr) & 0x7 )
   {
      GCLOG("Misaligned pointer %p\n", inPtr);
      NullReference("Object", false);
   }
   #endif

   #ifdef HXCPP_GC_DEBUG_ALWAYS_MOVE
   hx::sgPointerMoved.erase(inPtr);
   sgAllocsSinceLastSpam++;
   #endif
}

// --- Marking ------------------------------------

struct MarkInfo
{
   const char *mClass;
   const char *mMember;
};

struct GlobalChunks
{
   std::atomic<MarkChunk*> processList;
   std::atomic_int         processListPopLock;
   std::atomic<MarkChunk*> freeList;
   std::atomic_int         freeListPopLock;

   GlobalChunks() : processList(), processListPopLock(), freeList(), freeListPopLock() {}

   MarkChunk *pushJobNoWake(MarkChunk *inChunk)
   {
      MarkChunk* head{};

      do
      {
         head = processList;
         inChunk->next = head;
      } while (false == processList.compare_exchange_strong(head, inChunk));

      return alloc();
   }

   MarkChunk *pushJob(MarkChunk *inChunk,bool inAndAlloc)
   {
      MarkChunk* head{};

      do
      {
         head = processList;
         inChunk->next = head;
      } while (false == processList.compare_exchange_strong(head, inChunk));

      #ifdef PROFILE_THREAD_USAGE
      sThreadChunkPushCount++;
      #endif

      if (MAX_GC_THREADS>1 && sLazyThreads)
      {
         std::lock_guard<std::mutex> l(sThreadPoolLock);

         #ifdef PROFILE_THREAD_USAGE
           #define CHECK_THREAD_WAKE(tid) \
            if (MAX_GC_THREADS >tid && sgThreadCount>tid && (!(sRunningThreads & (1<<tid)))) { \
            wakeThreadLocked(tid); \
            sThreadChunkWakes++; \
           }
         #else
           #define CHECK_THREAD_WAKE(tid)  \
            if (MAX_GC_THREADS >tid && sgThreadCount>tid && (!(sRunningThreads & (1<<tid)))) { \
            wakeThreadLocked(tid); \
           }
         #endif


         CHECK_THREAD_WAKE(0)
         else CHECK_THREAD_WAKE(1)
         else CHECK_THREAD_WAKE(2)
         else CHECK_THREAD_WAKE(3)
         else CHECK_THREAD_WAKE(4)
         else CHECK_THREAD_WAKE(5)
         else CHECK_THREAD_WAKE(6)
         else CHECK_THREAD_WAKE(7)
      }

      if (inAndAlloc)
         return alloc();
      return 0;
   }

   void addLocked(MarkChunk *inChunk)
   {
      inChunk->next = processList;
      processList = inChunk;
   }

   void copyPointers( QuickVec<hx::Object *> &outPointers,bool andFree=false)
   {
      int size{ 0 };
      for (MarkChunk* c{ processList }; c; c = c->next)
         size += c->count;

      outPointers.setSize(size);
      int idx = 0;
      if (andFree)
      {
         while(processList)
         {
            MarkChunk* c{ processList };
            processList = c->next;

            for(int i=0;i<c->count;i++)
               outPointers[idx++] = c->stack[i];
            c->count = 0;
            c->next = freeList;
            freeList = c;
         }
      }
      else
      {
         for (MarkChunk* c{ processList }; c; c = c->next)
         {
            for(int i=0;i<c->count;i++)
               outPointers[idx++] = c->stack[i];
         }
      }
   }

   int takeArrayJob(hx::Object **inPtr, int inLen)
   {
      if (sLazyThreads)
      {
         int n = (inLen/2) & ~15;

         if (n)
         {
            MarkChunk *chunk = alloc();
            chunk->count = MarkChunk::OBJ_ARRAY_JOB;
            chunk->arrayBase = inPtr;
            chunk->arrayElements = n;

            pushJob(chunk,false);
         }

         // Return how many to skip
         return n;
      }

      return 0;
   }

   inline void release(MarkChunk *inChunk)
   {
      MarkChunk* head{};

      do
      {
         head = freeList;
         inChunk->next = head;
      } while (false == freeList.compare_exchange_strong(head, inChunk));
   }


   MarkChunk *popJobLocked(MarkChunk *inChunk)
   {
      if (inChunk)
         release(inChunk);

      while(false == atomic_compare_exchange_strong_discard(&processListPopLock, 0, 1))
      {
         // Spin
         #ifdef PROFILE_THREAD_USAGE
         sSpinCount++;
         #endif
      }

      while(true)
      {
         MarkChunk* head{ processList };
         if (!head)
         {
            processListPopLock = 0;
            return 0;
         }
         MarkChunk *next = head->next;
         if (processList.compare_exchange_strong(head, next))
         {
            processListPopLock = 0;

            head->next = 0;
            return head;
         }
      }
      return 0;
   }


   void completeThreadLocked(int inThreadId)
   {
      if (!(sRunningThreads & (1<<inThreadId)))
      {
         printf("Complete non-running thread?\n");
         DebuggerTrap();
      }
      sRunningThreads &= ~(1<<inThreadId);
      sLazyThreads = sRunningThreads != sAllThreads;

      if (!sRunningThreads)
         SignalThreadPool(sThreadJobDone,sThreadJobDoneSleeping);
   }


   // Optionally returns inChunk to empty pool (while we have the lock),
   //  and returns a new job if there is one
   MarkChunk *popJobOrFinish(MarkChunk *inChunk,int inThreadId)
   {
      #ifdef HX_MULTI_THREAD_MARKING
      if (sAllThreads)
      {
         MarkChunk *result =  popJobLocked(inChunk);
         if (!result)
         {
            for(int spinCount = 0; spinCount<10000; spinCount++)
            {
               if ( sgThreadPoolAbort || sAllThreads == (1<<inThreadId) )
                  break;
               if (processList)
               {
                  result =  popJobLocked(0);
                  if (result)
                     return result;
               }
            }
            std::lock_guard<std::mutex> l(sThreadPoolLock);
            completeThreadLocked(inThreadId);
         }
         return result;
      }
      #endif

      return popJobLocked(inChunk);
   }


   void free(MarkChunk *inChunk)
   {
      release(inChunk);
   }

   inline MarkChunk *alloc()
   {
      while(false == atomic_compare_exchange_strong_discard(&freeListPopLock, 0, 1))
      {
         // Spin
         #ifdef PROFILE_THREAD_USAGE
         sSpinCount++;
         #endif
      }

      while(true)
      {
         MarkChunk* head{ freeList };
         if (!head)
         {
            freeListPopLock = 0;
            return new MarkChunk;
         }
         MarkChunk* next{ head->next };
         if (freeList.compare_exchange_strong(head, next))
         {
            freeListPopLock = 0;

            head->next = 0;
            return head;
         }
      }
   }




   MarkChunk *getInitJob()
   {
      MarkChunk *result = popJobLocked(0);
      if (result)
      {
         if (result->count==MarkChunk::OBJ_ARRAY_JOB)
         {
            GCLOG("Popped array job?\n");
            pushJob(result,false);
         }
         else
            return result;
      }
      return alloc();
   }

};

GlobalChunks sGlobalChunks;

class MarkContext
{
    #ifdef HXCPP_DEBUG
    MarkInfo  *mInfo;
    int       mPos;
    #endif

    int       mThreadId;
    MarkChunk *marking;
