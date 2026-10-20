      mLocalAllocs.push(inAlloc);
      // TODO Attach debugger
   }

   bool ReturnToPoolLocked(LocalAllocator *inAlloc)
   {
      // Until we add ourselves, the colled will not wait
      //  on us - ie, we are assumed ot be in a GC free zone.
      for(int p=0;p<LOCAL_POOL_SIZE;p++)
      {
         if (!mLocalPool[p])
         {
            mLocalPool[p] = inAlloc;
            return true;
         }
      }
      // I told him we already got one
      return false;
   }

   LocalAllocator *GetPooledAllocator()
   {
      std::lock_guard<std::recursive_mutex> lock(*gThreadStateChangeLock);
      for(int p=0;p<LOCAL_POOL_SIZE;p++)
      {
         if (mLocalPool[p])
         {
            LocalAllocator *result = mLocalPool[p];
            mLocalPool[p] = 0;
            return result;
         }
      }
      return 0;
   }

   void RemoveLocalLocked(LocalAllocator *inAlloc)
   {
      // You should be in the GC free zone before you call this...
      if (!mLocalAllocs.qerase_val(inAlloc))
      {
         CriticalGCError("LocalAllocator removed without being added");
      }
   }

   void FreeLarge(void *inLarge)
   {
#ifdef HXCPP_TELEMETRY
       __hxt_gc_free_large(inLarge);
#endif

      ((unsigned char *)inLarge)[HX_ENDIAN_MARK_ID_BYTE] = 0;
      // AllocLarge will not lock this list unless it decides there is a suitable
      //  value, so we can't doa realloc without potentially crashing it.
      if (largeObjectRecycle.hasExtraCapacity(1))
      {
         unsigned int *blob = ((unsigned int *)inLarge) - 2;
         unsigned int size = *blob;
         mLargeListLock.lock();
         mLargeAllocated -= size;
         // Could somehow keep it in the list, but mark as recycled?
         mLargeList.qerase_val(blob);
         // We could maybe free anyhow?
         if (!largeObjectRecycle.hasExtraCapacity(1))
         {
            mLargeListLock.unlock();
            HxFree(blob);
            return;
         }
         largeObjectRecycle.push(blob);
         mLargeListLock.unlock();
      }
   }

   void *AllocLarge(size_t inSize, bool inClear)
   {
      if (hx::gPauseForCollect)
         __hxcpp_gc_safe_point();

      //Should we force a collect ? - the 'large' data are not considered when allocating objects
      // from the blocks, and can 'pile up' between smalll object allocations
      if ((inSize+mLargeAllocated > mLargeAllocForceRefresh) && sgInternalEnable)
      {
         #ifdef SHOW_MEM_EVENTS
         //GCLOG("Large alloc causing collection");
         #endif
         CollectFromThisThread(false,false);
      }

      inSize = (inSize + 3) & ~size_t{ 3 };

      if (inSize<<1 > mLargeAllocSpace)
         mLargeAllocSpace = inSize<<1;

      unsigned int* result{};
      #ifndef HXCPP_SINGLE_THREADED_APP
      bool do_lock{ true };
      #else
      bool do_lock{ false };
      #endif
      bool isLocked{};


      if (largeObjectRecycle.size())
      {
         for(int i=0;i<largeObjectRecycle.size();i++)
         {
            if ( largeObjectRecycle[i][0] == inSize )
            {
               if (do_lock && !isLocked)
               {
                  mLargeListLock.lock();
                  isLocked = true;
                  if (  i>=largeObjectRecycle.size() || largeObjectRecycle[i][0] != inSize )
                     continue;
               }

               result = largeObjectRecycle[i];
               largeObjectRecycle.qerase(i);
               // You can use this to test race condition
               //Sleep(1);
               break;
            }
         }
      }

      if (!result)
         result = static_cast<unsigned int*>(HxAlloc(inSize + sizeof(int)*2));

      if (!result)
      {
         #ifdef SHOW_MEM_EVENTS
         GCLOG("Large alloc failed - forcing collect\n");
         #endif

         if (isLocked)
         {
            mLargeListLock.unlock();
            isLocked = false;
         }

         CollectFromThisThread(true,true);
         result = static_cast<unsigned int*>(HxAlloc(inSize + sizeof(int) * 2));
      }

      if (!result)
      {
         GCLOG("Memory Exhausted!\n");
         DebuggerTrap();
      }

      if (inClear)
         ZERO_MEM(result, inSize + sizeof(int)*2);

      result[0] = static_cast<unsigned int>(inSize);
      #ifdef HXCPP_GC_NURSERY
      result[1] = 0;
      #else
      result[1] = hx::gMarkID;
      #endif

      if (do_lock && !isLocked)
         mLargeListLock.lock();

      mLargeList.push(result);
      mLargeAllocated += inSize;

      if (do_lock)
         mLargeListLock.unlock();

#ifdef HXCPP_TELEMETRY
      __hxt_gc_alloc(result + 2, inSize);
#endif

      return result+2;
   }

   void onMemoryChange(int inDelta, const char *inWhy)
   {
      if (hx::gPauseForCollect)
         __hxcpp_gc_safe_point();

      if (inDelta>0)
      {
         //Should we force a collect ? - the 'large' data are not considered when allocating objects
         // from the blocks, and can 'pile up' between smalll object allocations
         if (inDelta>0 && (inDelta+mLargeAllocated > mLargeAllocForceRefresh) && sgInternalEnable)
         {
            //GCLOG("onMemoryChange alloc causing collection");
            CollectFromThisThread(false,false);
         }

         int rounded = (inDelta +3) & ~3;

         if (rounded<<1 > mLargeAllocSpace)
            mLargeAllocSpace = rounded<<1;
      }

      #ifndef HXCPP_SINGLE_THREADED_APP
      bool do_lock = true;
      #else
      bool do_lock = false;
      #endif

      if (do_lock)
         mLargeListLock.lock();

      mLargeAllocated += inDelta;

      if (do_lock)
         mLargeListLock.unlock();
   }

   // Gets a block with the 'zeroLock' acquired, which means the zeroing thread
   //  will leave it alone from now on
   //
   // This contains some "lock free" code that triggers the thread sanitizer.
   //  * mOwned can only be changed with the mZeroLock
   //  * mFreeBlocks may increase size, but it is not critical whether the mNextFreeBlockOfSize
   //     is increased since skipping only leads to a bit of extra searhing for the
   //     next allocation
   #if defined(__has_feature)
     #if __has_feature(thread_sanitizer)
      __attribute__((no_sanitize("thread")))
     #endif
   #endif
   BlockDataInfo *GetNextFree(size_t inRequiredBytes)
   {
      bool failedLock = true;
      size_t sizeSlot{ inRequiredBytes >> IMMIX_LINE_BITS };
      if (sizeSlot>=BLOCK_OFSIZE_COUNT)
         sizeSlot = BLOCK_OFSIZE_COUNT-1;
      //volatile int &nextFreeBlock = mNextFreeBlockOfSize[sizeSlot];
      int nextFreeBlock{ mNextFreeBlockOfSize[sizeSlot] };
      while(failedLock && nextFreeBlock<mFreeBlocks.size())
      {
         failedLock = false;

         for (int i{ nextFreeBlock }; i < mFreeBlocks.size(); i++)
         {
             BlockDataInfo *info = mFreeBlocks[i];
             if (!info->mOwned && info->mMaxHoleSize>=inRequiredBytes)
             {
                // Acquire the zero-lock
                if (atomic_compare_exchange_strong_discard(&info->mZeroLock, 0, 1))
                {
                   // Acquire ownership...
                   if (info->mOwned)
                   {
                      // Someone else got it...
                      info->mZeroLock = 0;
                   }
                   else
                   {
                      info->mOwned = true;

                      // Increase the mNextFreeBlockOfSize
                      int idx = nextFreeBlock;
                      while(idx<mFreeBlocks.size() && mFreeBlocks[idx]->mOwned)
                      {
                         _hx_atomic_compare_exchange(mNextFreeBlockOfSize+sizeSlot, idx, idx+1);
                         idx++;
                      }

                      if (sgThreadPoolJob==tpjAsyncZeroJit)
                      {
                         if (info->mZeroed==ZEROED_THREAD)
                            onZeroedBlockDequeued();
                         #ifdef PROFILE_THREAD_USAGE
                         else
                         {
                            if (!info->mZeroed)
                                sThreadZeroMisses++;
                         }
                         #endif
                       }

                      return info;
                   }
                 }
                 else if (!info->mOwned)
                 {
                    // Zeroing thread is currently working on this block
                    // Go to next one or spin around again
                    failedLock = true;
                 }
             }
         }
      }

      return 0;
   }

   inline size_t GetWorkingMemory()
   {
       return ((size_t)mAllBlocks.size()) << IMMIX_BLOCK_BITS;
   }

   // Making this function "virtual" is actually a (big) performance enhancement!
   // On the iphone, sjlj (set-jump-long-jump) exceptions are used, which incur a
   //  performance overhead.  It seems that the overhead in only in routines that call
   //  malloc/new.  This is not called very often, so the overhead should be minimal.
   //  However, gcc inlines this function!  requiring every alloc the have sjlj overhead.
   //  Making it virtual prevents the overhead.
   virtual bool AllocMoreBlocks(bool &outForceCompact, bool inJustBorrowing)
   {
      enum { newBlockCount = 1<<(IMMIX_BLOCK_GROUP_BITS) };

      #ifdef HXCPP_GC_MOVING
      if (!inJustBorrowing)
      {
         // Do compact next collect sooner
         if (sgTimeToNextTableUpdate>1)
         {
            #ifdef SHOW_FRAGMENTATION
              #ifdef SHOW_MEM_EVENTS_VERBOSE
                GCLOG("  alloc -> enable full collect\n");
              #endif
            #endif
            //sgTimeToNextTableUpdate = 1;
         }
      }
      #endif

      #ifndef HXCPP_GC_BIG_BLOCKS
      // Currently, we only have 2 bytes for a block header
      if (mAllBlocks.size()+newBlockCount >= 0xfffe )
      {
         #if defined(SHOW_MEM_EVENTS) || defined(SHOW_FRAGMENTATION)
         GCLOG("Block id count used - collect");
         #endif
         // The problem is we are out of blocks, not out of memory
         outForceCompact = false;
         return false;
      }
      #endif

      // Find spare group...
      int gid = -1;
      for(int i=0;i<gAllocGroups.size();i++)
         if (!gAllocGroups[i].alloc)
         {
            gid = i;
            break;
         }
      if (gid<0)
      {
         if (!gAllocGroups.safeReserveExtra(1))
         {
            outForceCompact = true;
            return false;
         }
         gid = gAllocGroups.next();
         gAllocGroups[gid].alloc = 0;
      }

      int n = 1<<IMMIX_BLOCK_GROUP_BITS;


      if (!mAllBlocks.safeReserveExtra(n) || !mFreeBlocks.hasExtraCapacity(n))
      {
         outForceCompact = true;
         return false;
      }

      char *chunk = (char *)HxAllocGCBlock( 1<<(IMMIX_BLOCK_GROUP_BITS + IMMIX_BLOCK_BITS) );
      if (!chunk)
      {
         //DebuggerTrap();
         #ifdef SHOW_MEM_EVENTS
         GCLOG("Alloc failed - try collect\n");
         #endif
         outForceCompact = true;
         return false;
      }

      char *aligned = (char *)( (((size_t)chunk) + IMMIX_BLOCK_SIZE-1) & IMMIX_BLOCK_BASE_MASK);
      if (aligned!=chunk)
         n--;
      gAllocGroups[gid].alloc = chunk;
      gAllocGroups[gid].blocks = n;
      gAllocGroups[gid].clear();

      int newSize = mFreeBlocks.size();
      for(int i=0;i<n;i++)
      {
         BlockData *block = (BlockData *)(aligned + i*IMMIX_BLOCK_SIZE);
         BlockDataInfo *info = new BlockDataInfo(gid,block);

         mAllBlocks.push(info);
         mFreeBlocks.push(info);
      }
      std::stable_sort(&mAllBlocks[0], &mAllBlocks[0] + mAllBlocks.size(), SortByBlockPtr );
      mAllBlocksCount = mAllBlocks.size();
      for(int i=0;i<BLOCK_OFSIZE_COUNT;i++)
         mNextFreeBlockOfSize[i] = newSize;

      #ifdef HXCPP_GC_VERIFY
      VerifyBlockOrder();
      #endif

      #if defined(SHOW_MEM_EVENTS_VERBOSE) || defined(SHOW_FRAGMENTATION_BLOCKS)
      if (inJustBorrowing)
      {
         GCLOG("Borrow Blocks %d = %d k\n", mAllBlocks.size(), (mAllBlocks.size() << IMMIX_BLOCK_BITS)>>10);
      }
      else
      {
         GCLOG("Blocks %d = %d k\n", mAllBlocks.size(), (mAllBlocks.size() << IMMIX_BLOCK_BITS)>>10);
      }
      #endif


      return true;
   }

   bool allowMoreBlocks()
   {
      #ifdef HXCPP_GC_GENERATIONAL
      return sGcMode==gcmFull;
      #else
      return true;
      #endif
   }

   void repoolReclaimedBlock(BlockDataInfo *block)
   {
      // The mMaxHoleSize has changed - possibly return to one of the pools
   }


   BlockDataInfo *GetFreeBlock(size_t inRequiredBytes, hx::ImmixAllocator *inAlloc)
   {
      while(true)
      {
         BlockDataInfo *result = GetNextFree(inRequiredBytes);
         if (result)
         {
            result->zeroAndUnlock();

            // After zero/reclaim, it might be that the hole size is smaller than we thought.
            if (result->mMaxHoleSize>=inRequiredBytes)
               return result;

            repoolReclaimedBlock(result);
            continue;
         }

         if (hx::gPauseForCollect)
         {
            hx::PauseForCollect();
