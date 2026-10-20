         for(int i=0;i<mAllBlocks.size();i++)
         {
            if (full)
               mAllBlocks[i]->reclaim<true>(&outStats);
            else
               mAllBlocks[i]->reclaim<false>(&outStats);
         }
      }
   }


   void countRows(BlockDataStats &outStats)
   {
      if (MAX_GC_THREADS>1)
      {
         for(int i=0;i<MAX_GC_THREADS;i++)
            sThreadBlockDataStats[i].clear();
         StartThreadJobs(tpjCountRows, mAllBlocks.size(), true);
         outStats = sThreadBlockDataStats[0];
         for(int i=1;i<MAX_GC_THREADS;i++)
            outStats.add(sThreadBlockDataStats[i]);
      }
      else
      {
         outStats.clear();
         for(int i=0;i<mAllBlocks.size();i++)
            mAllBlocks[i]->countRows(outStats);
      }
   }


   // buils mFreeBlocks and maybe starts the async-zero process on mZeroList
   void createFreeList()
   {
      mFreeBlocks.clear();

      for(int i=0;i<mAllBlocks.size();i++)
      {
         BlockDataInfo *info = mAllBlocks[i];
         if (info->GetFreeRows() > 0 && info->mMaxHoleSize>256)
         {
            info->mOwned = false;
            mFreeBlocks.push(info);
         }
      }

      size_t extra{ std::max(mAllBlocks.size(), static_cast<size_t>(8 << IMMIX_BLOCK_GROUP_BITS)) };
      mFreeBlocks.safeReserveExtra(extra);

      std::sort(&mFreeBlocks[0], &mFreeBlocks[0] + mFreeBlocks.size(), SmallestFreeFirst );

      for(int i=0;i<BLOCK_OFSIZE_COUNT;i++)
         mNextFreeBlockOfSize[i] = mFreeBlocks.size();

      for(int i=mFreeBlocks.size()-1;i>=0;i--)
      {
         int slot = mFreeBlocks[i]->mMaxHoleSize >> IMMIX_LINE_BITS;
         if (slot>=BLOCK_OFSIZE_COUNT)
            slot = BLOCK_OFSIZE_COUNT-1;
         mNextFreeBlockOfSize[slot] = i;
      }

      for(int i=BLOCK_OFSIZE_COUNT-2;i>=0;i--)
         if (mNextFreeBlockOfSize[i]>mNextFreeBlockOfSize[i+1])
            mNextFreeBlockOfSize[i] = mNextFreeBlockOfSize[i+1];

      mZeroList.clear();
   }

   void backgroundProcessFreeList(bool inJit)
   {
      mZeroListQueue = 0;
      #ifdef HX_GC_ZERO_EARLY
      mZeroList.setSize(mFreeBlocks.size());
      memcpy( &mZeroList[0], &mFreeBlocks[0], mFreeBlocks.size()*sizeof(void *));

      StartThreadJobs(tpjAsyncZero, mZeroList.size(),true);
      #else
      if ( MAX_GC_THREADS>1 && mFreeBlocks.size()>4)
      {
         mZeroList.setSize(mFreeBlocks.size());
         memcpy( &mZeroList[0], &mFreeBlocks[0], mFreeBlocks.size()*sizeof(void *));

         // Only use one thread for parallel zeroing.  Try to get though the work wihout
         //  slowing down the main thread
         StartThreadJobs(inJit ? tpjAsyncZeroJit : tpjAsyncZero, mZeroList.size(), false, 1);
      }
      #endif
   }




   size_t MemLarge()
   {
      return mLargeAllocated;
   }

   size_t MemReserved()
   {
      return mLargeAllocated + (mAllBlocksCount*IMMIX_USEFUL_LINES<<IMMIX_LINE_BITS);
   }

   size_t MemCurrent()
   {
      return mLargeAllocated + (mCurrentRowsInUse<<IMMIX_LINE_BITS);
   }

   size_t MemUsage()
   {
      return mLargeAllocated + (mRowsInUse<<IMMIX_LINE_BITS);
   }

   bool IsAllBlock(BlockData *block)
   {
      if (mAllBlocks.size())
      {
         int min = 0;
         int max = mAllBlocks.size()-1;
         if (block==mAllBlocks[0]->mPtr)
            return true;
         if (block==mAllBlocks[max]->mPtr)
            return true;
         if (block>mAllBlocks[0]->mPtr && block<mAllBlocks[max]->mPtr)
         {
            while(min<max-1)
            {
               int mid = (max+min)>>1;
               if (mAllBlocks[mid]->mPtr==block)
                  return true;

               if (mAllBlocks[mid]->mPtr<block)
                  min = mid;
               else
                  max = mid;
            }
         }
      }
      return false;
   }

   MemType GetMemType(uintptr_t inPtr)
   {
      BlockData* block{ reinterpret_cast<BlockData*>(inPtr & IMMIX_BLOCK_BASE_MASK) };

      bool isBlock{ IsAllBlock(block) };

      if (isBlock)
         return memBlock;

      for(int i=0;i<mLargeList.size();i++)
      {
         uintptr_t blob{ reinterpret_cast<uintptr_t>(mLargeList[i] + 2) };
         if (blob==inPtr)
            return memLarge;
      }

      return memUnmanaged;
   }


   size_t mRowsInUse;
   size_t mCurrentRowsInUse;
   size_t mLargeAllocSpace;
   size_t mLargeAllocForceRefresh;
   size_t mLargeAllocated;
   size_t mTotalAfterLastCollect;
   size_t mAllBlocksCount;
   double mGenerationalRetainEstimate;

   hx::MarkContext mMarker;

   volatile int mNextFreeBlockOfSize[BLOCK_OFSIZE_COUNT];
   std::atomic_size_t mThreadJobId;

   BlockList mAllBlocks;
   BlockList mFreeBlocks;
   BlockList mZeroList;
   std::atomic_int mZeroListQueue;

   LargeList mLargeList;
   std::mutex mLargeListLock;
   hx::QuickVec<LocalAllocator *> mLocalAllocs;
   LocalAllocator *mLocalPool[LOCAL_POOL_SIZE];
   hx::QuickVec<unsigned int *> largeObjectRecycle;
};



namespace hx
{

MarkChunk *MarkChunk::swapForNew()
{
   return sGlobalChunks.pushJobNoWake(this);
}



void MarkConservative(int *inBottom, int *inTop,hx::MarkContext *__inCtx)
{
   #ifdef VERIFY_STACK_READ
   VerifyStackRead(inBottom, inTop);
   #endif

   #ifdef SHOW_MEM_EVENTS
   GCLOG("Mark conservative %p...%p (%d) [...", inBottom, inTop, (int)(inTop-inBottom) );
     #ifdef HX_WATCH
     GCLOG("\n");
     #endif
   #endif

   #ifdef HXCPP_STACK_UP
   int *start = inTop-1;
   inTop = inBottom+1;
   inBottom = start;
   #endif

   if (sizeof(int)==4 && sizeof(void *)==8)
   {
      // Can't start pointer on last integer boundary...
      inTop--;
   }

   uintptr_t prev{};
   uintptr_t lastPin{};
   #ifdef HX_WATCH
   uintptr_t lastWatch{};
   bool isWatch{};
   #endif

   #ifdef HXCPP_GC_GENERATIONAL
   // If this is a generational mark, then the byte marker has not been increased.
   // Previous mark Ids are therfore from more than 1 collection ago
   bool allowPrevious{ !__inCtx->isGenerational };
   #else
   const bool allowPrevious{ true };
   #endif


   for(int *ptr = inBottom ; ptr<inTop; ptr++)
   {
      uintptr_t potentialObject{ reinterpret_cast<uintptr_t>(*reinterpret_cast<void**>(ptr)) };

      #ifdef HXCPP_ALIGN_ALLOC
      const size_t validObjectMask{ 0x07 };
      #else
      const size_t validObjectMask{ 0x03 };
      #endif

      if (potentialObject && !(potentialObject & validObjectMask) && potentialObject != prev && potentialObject != lastPin)
      {

         #ifdef PROFILE_COLLECT
         hx::localCount++;
         #endif
         MemType mem{ sGlobalAlloc->GetMemType(potentialObject) };

         #ifdef HX_WATCH
         isWatch = false;
         if (hxInWatchList(potentialObject) && potentialObject !=lastWatch)
         {
            isWatch = true;
            lastWatch = potentialObject;
            GCLOG("********* Watch location conservative mark %p:%d\n", reinterpret_cast<void*>(potentialObject), mem);
         }
         #endif

         if (mem!=memUnmanaged)
         {
            if (mem==memLarge)
            {
               unsigned char& mark{ reinterpret_cast<unsigned char*>(potentialObject)[HX_ENDIAN_MARK_ID_BYTE] };
               if (mark != gByteMarkID)
                  mark = static_cast<unsigned char>(gByteMarkID);
            }
            else
            {
               BlockData* block{ reinterpret_cast<BlockData*>(potentialObject & IMMIX_BLOCK_BASE_MASK) };
               BlockDataInfo* info{ (*gBlockInfo)[block->mId] };

               size_t pos{ potentialObject & IMMIX_BLOCK_OFFSET_MASK };
               AllocType t{
                  sgCheckInternalOffset ?
                     info->GetEnclosingAllocType(pos - sizeof(int), &potentialObject, allowPrevious) :
                     info->GetAllocType(pos - sizeof(int), allowPrevious) };

               #ifdef HX_WATCH
               if (!isWatch && hxInWatchList(potentialObject))
               {
                  isWatch = true;
                  GCLOG("********* Watch location conservative mark offset %p:%d\n", reinterpret_cast<void*>(potentialObject), mem);
               }
               #endif


               if ( t==allocObject )
               {
                  #ifdef HX_WATCH
                  if (isWatch)
                  {
                     GCLOG(" Mark object %p (%p)\n", reinterpret_cast<void*>(potentialObject), ptr);
                  }
                  #endif
                  hx::MarkObjectAlloc( reinterpret_cast<hx::Object*>(potentialObject), __inCtx );
                  lastPin = potentialObject;
                  info->pin();
               }
               else if (t==allocString)
               {
                  #ifdef HX_WATCH
                  if (isWatch)
                     GCLOG(" Mark string %p (%p)\n", reinterpret_cast<void*>(potentialObject), ptr);
                  #endif
                  HX_MARK_STRING(reinterpret_cast<void*>(potentialObject));
                  lastPin = potentialObject;
                  info->pin();
               }
               else if (t==allocMarked)
               {
                  #ifdef HX_WATCH
                  if (isWatch)
                     GCLOG(" pin alloced %p (%p)\n", reinterpret_cast<void*>(potentialObject), ptr);
                  #endif
                  lastPin = potentialObject;
                  info->pin();
               }
               #ifdef HX_WATCH
               else // memBlock
               {
                  if (isWatch)
                  {
                     GCLOG(" missed watch %p:%d\n", reinterpret_cast<void*>(potentialObject), t);
                     int x = info->GetAllocType(pos-sizeof(int),allowPrevious);
                     int y = info->GetEnclosingAllocType(pos-sizeof(int),&potentialObject,allowPrevious);
                     #ifdef HXCPP_GC_NURSERY
                     int z = info->GetEnclosingNurseryType(pos-sizeof(int),nullptr);
                     #else
                     int z = 0;
                     #endif
                     printf("but got alloc type=%d, enclosing=%d nurs=%d o=%" PRIdPTR "\n", x, y, z, sgCheckInternalOffset);
                  }
               }
               #endif
            }
         }
         // GCLOG(" rejected %p %p %d %p %d=%d\n", ptr, vptr, !((size_t)vptr & 0x03), prev,
         // sGlobalAlloc->GetMemType(vptr) , memUnmanaged );
      }
   }
   #ifdef SHOW_MEM_EVENTS
   GCLOG("...]\n");
   #endif
}

} // namespace hx


// --- LocalAllocator -------------------------------------------------------
//
// One per thread ...

static int sFragIgnore=0;

class LocalAllocator : public hx::StackContext
{
   uint8_t        mCurrentHole;
   uint8_t        mCurrentHoles;
   HoleRange     *mCurrentRange;
   uint8_t       *mFraggedRows;

   bool           mMoreHoles;

   #ifndef HXCPP_EXPLICIT_STACK_EXTENT
   int *mTopOfStack;
   int *mBottomOfStack;
   #endif

   hx::RegisterCaptureBuffer mRegisterBuf;
   int                   mRegisterBufSize;

   #ifndef HXCPP_SINGLE_THREADED_APP
   bool            mGCFreeZone;
   HxSemaphore     mReadyForCollect;
   HxSemaphore     mCollectDone;
   #endif

   int             mID;


   // Must be called locked
   ~LocalAllocator()
   {
   }

public:
   bool            mGlobalStackLock;
   int             mStackLocks;

public:
   LocalAllocator(int *inTopOfStack=0)
   {
      Reset();

      #ifdef HXCPP_GC_GENERATIONAL
      mOldReferrers = 0;
      #endif

      AttachThread(inTopOfStack);
   }


   void AttachThread(int *inTopOfStack)
   {
      #ifndef HXCPP_EXPLICIT_STACK_EXTENT
      mTopOfStack = mBottomOfStack = inTopOfStack;
      #endif

      mRegisterBufSize = 0;
      mStackLocks = 0;
      mGlobalStackLock = false;
      #ifdef HX_WINDOWS
      mID = GetCurrentThreadId();
      #endif

      #ifdef HXCPP_GC_GENERATIONAL
      if (mOldReferrers)
      {
         GCLOG("Uncleaned referrers\n");
      }

      if (sGcMode==gcmGenerational)
         mOldReferrers = hx::sGlobalChunks.alloc();
      else
         mOldReferrers = 0;
      #endif


      // It is in the free zone - wait for 'SetTopOfStack' to activate
      #ifndef HXCPP_SINGLE_THREADED_APP
      mGCFreeZone = true;
      mReadyForCollect.Set();
      #endif
      sGlobalAlloc->AddLocal(this);
   }

   void Release()
   {
      mStackLocks = 0;
