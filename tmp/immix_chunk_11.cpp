      STAMP(t2)


      // Sweep blocks

      // Update table entries?  This needs to be done before the gMarkID count clocks
      //  back to the same number
      if (!generational)
         sgTimeToNextTableUpdate--;

      bool full = inMajor || (sgTimeToNextTableUpdate<=0) || inForceCompact;

      // Setup memory target ...
      // Count free rows, and prep blocks for sorting
      BlockDataStats stats;

      /*
       This reduces the stall time, but adds a bit of background cpu usage
       Might be good to just countRows for non-generational too
      */
      if (!full && generational)
      {
         countRows(stats);
         size_t currentRows = stats.rowsInUse + stats.fraggedRows + freeFraggedRows;
         double filled = (double)(currentRows) / (double)(mAllBlocks.size()*IMMIX_USEFUL_LINES);
         if (filled>0.85)
         {
            // Failure of generational estimation
            int retained = currentRows - mRowsInUse;
            int space = mAllBlocks.size()*IMMIX_USEFUL_LINES - mRowsInUse;
            if (space<retained)
               space = retained;
            if (space<1)
               space = 1;
            mGenerationalRetainEstimate = (double)retained/(double)space;
            #ifdef SHOW_MEM_EVENTS
            GCLOG("Generational retention/fragmentation too high %f, do normal collect\n", mGenerationalRetainEstimate);
            #endif

            #ifdef HX_GC_VERIFY_ALLOC_START
            verifyAllocStart();
            #endif

            generational = false;
            MarkAll(generational);

            sgTimeToNextTableUpdate--;
            full = sgTimeToNextTableUpdate<=0;

            stats.clear();
            reclaimBlocks(full,stats);
         }
      }
      else
      {
         reclaimBlocks(full,stats);
      }


      #ifdef HXCPP_GC_GENERATIONAL
      if (compactSurviors)
      {
         MoveSurvivors(&rememberedSet);
      }
      #endif


      #ifdef HXCPP_GC_MOVING
      if (!full)
      {
         double useRatio = (double)(mRowsInUse<<IMMIX_LINE_BITS) / (sWorkingMemorySize);
         #if defined(SHOW_FRAGMENTATION) || defined(SHOW_MEM_EVENTS)
            GCLOG("Row use ratio:%f\n", useRatio);
         #endif
         // Could be either expanding, or fragmented...
         if (useRatio>0.75)
         {
            #if defined(SHOW_FRAGMENTATION) || defined(SHOW_MEM_EVENTS)
               GCLOG("Do full stats\n", useRatio);
            #endif
            full = true;
            stats.clear();
            reclaimBlocks(full,stats);
         }
      }
      #endif

      if (full)
      {
         #ifdef HXCPP_GC_MOVING
         sgTimeToNextTableUpdate = 7;
         #else
         sgTimeToNextTableUpdate = 15;
         #endif
      }

      size_t oldRowsInUse = mRowsInUse;
      mRowsInUse = stats.rowsInUse + stats.fraggedRows + freeFraggedRows;

      bool moved = false;

      #if defined(SHOW_FRAGMENTATION) || defined(SHOW_MEM_EVENTS)
      GCLOG("Total memory : %s\n", formatBytes(GetWorkingMemory()).c_str());
      GCLOG("  reserved bytes : %s\n", formatBytes(mRowsInUse*IMMIX_LINE_LEN).c_str());
      if (full)
      {
         GCLOG("  active bytes   : %s\n", formatBytes(stats.bytesInUse).c_str());
         GCLOG("  active ratio   : %f\n", (double)stats.bytesInUse/( mRowsInUse*IMMIX_LINE_LEN));
         GCLOG("  fragged blocks : %d (%.1f%%)\n", stats.fraggedBlocks, stats.fraggedBlocks*100.0/mAllBlocks.size() );
         GCLOG("  fragged score  : %f\n", (double)stats.fragScore/mAllBlocks.size() );
      }
      GCLOG("  large size   : %s\n", formatBytes(mLargeAllocated).c_str());
      GCLOG("  empty blocks : %d (%.1f%%)\n", stats.emptyBlocks, stats.emptyBlocks*100.0/mAllBlocks.size());
      #endif

      size_t bytesInUse = mRowsInUse<<IMMIX_LINE_BITS;

      STAMP(t3)


      #ifdef HXCPP_TELEMETRY
      // Detect deallocations - TODO: add STAMP() ?
      __hxt_gc_after_mark(gByteMarkID, HX_ENDIAN_MARK_ID_BYTE);
      #endif

      // Sweep large

      // Manage recycle size ?
      //  clear old frames recycle objects
      int l2 = largeObjectRecycle.size();
      for(int i=0;i<largeObjectRecycle.size();i++)
         HxFree(largeObjectRecycle[i]);
      largeObjectRecycle.setSize(0);

      size_t recycleRemaining = 0;
      #ifdef RECYCLE_LARGE
      if (!inForceCompact)
         recycleRemaining = mLargeAllocForceRefresh;
      #endif

      int idx = 0;
      int l0 = mLargeList.size();
      while(idx<mLargeList.size())
      {
         unsigned int *blob = mLargeList[idx];
         if ( (blob[1] & IMMIX_ALLOC_MARK_ID) != hx::gMarkID )
         {
            unsigned int size = *blob;
            mLargeAllocated -= size;
            if (size < recycleRemaining)
            {
               recycleRemaining -= size;
               largeObjectRecycle.push(blob);
            }
            else
            {
               HxFree(blob);
            }

            mLargeList.qerase(idx);
         }
         else
            idx++;
      }

      int l1 = mLargeList.size();


      STAMP(t4)

      bool defragged = false;

      // Compact/Defrag?
      #if defined(HXCPP_GC_MOVING) && defined(HXCPP_VISIT_ALLOCS)
      if (full)
      {
         bool doRelease = false;

         if (inForceCompact)
            doRelease = true;
         else
         {
            size_t mem = mRowsInUse<<IMMIX_LINE_BITS;
            size_t targetFree = std::max(hx::sgMinimumFreeSpace, mem / 100 * hx::sgTargetFreeSpacePercentage );
            targetFree = std::min(targetFree, sgMaximumFreeSpace );
            sWorkingMemorySize = std::max( mem + targetFree, hx::sgMinimumWorkingMemory);

            size_t allMem = GetWorkingMemory();
            // 8 Meg too much?
            size_t allowExtra = std::max(size_t{ 8 } * 1024 * 1024, sWorkingMemorySize * 5 / 4);

            if ( allMem > sWorkingMemorySize + allowExtra )
            {
               #if defined(SHOW_FRAGMENTATION) || defined(SHOW_MEM_EVENTS)
               int releaseGroups = (int)((allMem - sWorkingMemorySize) / (IMMIX_BLOCK_SIZE<<IMMIX_BLOCK_GROUP_BITS));
               if (releaseGroups)
                  GCLOG("Try to release %d groups\n", releaseGroups );
               #endif
               doRelease = true;
            }
         }


         bool isFragged = stats.fragScore > mAllBlocks.size()*FRAG_THRESH;
         if (doRelease || isFragged || hx::gAlwaysMove)
         {
            if (isFragged && sgTimeToNextTableUpdate>3)
               sgTimeToNextTableUpdate = 3;
            calcMoveOrder( );

            // Borrow some blocks to ensuure space to defrag into
            int workingBlocks = mAllBlocks.size()*3/2 - stats.emptyBlocks;
            int borrowed = 0;
            while( mAllBlocks.size()<workingBlocks )
            {
               bool dummy = false;
               if (!AllocMoreBlocks(dummy, true))
                  break;
               doRelease = true;
               borrowed++;
            }
            stats.emptyBlocks += borrowed;
            #if defined(SHOW_FRAGMENTATION)
               GCLOG("Borrowed %d groups for %d target blocks\n", borrowed, workingBlocks);
            #endif

            MoveBlockJob job(mAllBlocks);

            if (MoveBlocks(job,stats) || doRelease)
            {
               if (doRelease)
               {
                  size_t mem = mRowsInUse<<IMMIX_LINE_BITS;
                  size_t targetFree = std::max(hx::sgMinimumFreeSpace, bytesInUse/100 *hx::sgTargetFreeSpacePercentage );
                  targetFree = std::min(targetFree, sgMaximumFreeSpace );
                  size_t targetMem = std::max( mem + targetFree, hx::sgMinimumWorkingMemory) +
                                        (2<<(IMMIX_BLOCK_GROUP_BITS+IMMIX_BLOCK_BITS));

                  if (inForceCompact)
                     targetMem = 0;
                  size_t have = GetWorkingMemory();
                  if (targetMem<have)
                  {
                     size_t releaseSize = have - targetMem;
                     #ifdef SHOW_FRAGMENTATION
                     GCLOG(" Release %s bytes to leave %s\n", formatBytes(releaseSize).c_str(), formatBytes(targetMem).c_str() );
                     #endif

                     int releasedBlocks = releaseEmptyGroups(stats, releaseSize);
                     #ifdef SHOW_FRAGMENTATION
                     int releasedGroups = releasedBlocks >> IMMIX_BLOCK_GROUP_BITS;
                     GCLOG(" Released %s, %d groups\n", formatBytes((size_t)releasedBlocks<<IMMIX_BLOCK_BITS).c_str(), releasedGroups );
                     #endif
                  }
               }

               // Reduce sWorkingMemorySize now we have defragged
               #if defined(SHOW_FRAGMENTATION) || defined(SHOW_MEM_EVENTS)
               GCLOG("After compacting---\n");
               GCLOG("  total memory : %s\n", formatBytes(GetWorkingMemory()).c_str() );
               GCLOG("  total needed : %s\n", formatBytes((size_t)mRowsInUse*IMMIX_LINE_LEN).c_str() );
               GCLOG("  for bytes    : %s\n", formatBytes(bytesInUse).c_str() );
               GCLOG("  empty blocks : %d (%.1f%%)\n", stats.emptyBlocks, stats.emptyBlocks*100.0/mAllBlocks.size());
               GCLOG("  fragged blocks : %d (%.1f%%)\n", stats.fraggedBlocks, stats.fraggedBlocks*100.0/mAllBlocks.size() );
               #endif
            }

            std::stable_sort(&mAllBlocks[0], &mAllBlocks[0] + mAllBlocks.size(), SortByBlockPtr );

            #ifdef HXCPP_GC_VERIFY
            VerifyBlockOrder();
            #endif
         }
      }
      #endif


      STAMP(t5)

      size_t mem = mRowsInUse<<IMMIX_LINE_BITS;
      size_t baseMem = full ? bytesInUse : mem;
      #ifdef HXCPP_GC_DYNAMIC_SIZE
      size_t targetFree = std::max(hx::sgMinimumFreeSpace, baseMem * profileCollectSummary.spaceFactor );
      #else
      size_t targetFree = std::max(hx::sgMinimumFreeSpace, baseMem / 100 *hx::sgTargetFreeSpacePercentage );
      #endif
      targetFree = std::min(targetFree, sgMaximumFreeSpace );
      // Only adjust if non-generational
      if (!generational)
         sWorkingMemorySize = std::max( mem + targetFree, hx::sgMinimumWorkingMemory);

      #if defined(SHOW_FRAGMENTATION) || defined(SHOW_MEM_EVENTS)
      GCLOG("Target memory %s, using %s\n",  formatBytes(sWorkingMemorySize).c_str(), formatBytes(mem).c_str() );
      #endif

      // Large alloc target
      int blockSize =  mAllBlocks.size()<<IMMIX_BLOCK_BITS;
      if (blockSize > mLargeAllocSpace)
         mLargeAllocSpace = blockSize;
      mLargeAllocForceRefresh = mLargeAllocated + mLargeAllocSpace;

      mTotalAfterLastCollect = MemUsage();

      #ifdef HXCPP_GC_GENERATIONAL
      if (generational)
      {
         // TODO - include large too?
         int retained = mRowsInUse - oldRowsInUse;
         int space = mAllBlocks.size()*IMMIX_USEFUL_LINES - oldRowsInUse;
         if (space<retained)
            space = retained;

         mGenerationalRetainEstimate = (double)retained/(double)space;
      }
      else
      {
         // move towards 0.2
         mGenerationalRetainEstimate += (0.2-mGenerationalRetainEstimate)*0.25;
      }

      double filled_ratio = (double)mRowsInUse/(double)(mAllBlocksCount*IMMIX_USEFUL_LINES);
      double after_gen = filled_ratio + (1.0-filled_ratio)*mGenerationalRetainEstimate;

      if (after_gen<0.75)
      {
         sGcMode = gcmGenerational;
      }
      else
      {
         sGcMode = gcmFull;
         // What was I thinking here?  This breaks #851
         //gByteMarkID |= 0x30;
      }

      #ifdef SHOW_MEM_EVENTS
      GCLOG("filled=%.2f%% + estimate = %.2f%% = %.2f%% -> %s\n",
            filled_ratio*100, mGenerationalRetainEstimate*100, after_gen*100, sGcMode==gcmFull?"Full":"Generational");
      #endif

      #endif

      createFreeList();

      // This saves some running/stall time, but increases the total CPU usage
      // Delaying it until just before the block is used to improve the cache locality
      backgroundProcessFreeList(true);

      mAllBlocksCount   = mAllBlocks.size();
      mCurrentRowsInUse = mRowsInUse;

      #ifdef SHOW_MEM_EVENTS
      GCLOG("Collect Done\n");
      #endif

      #ifdef PROFILE_COLLECT
      STAMP(t6)
      double period = t6-sLastCollect;
      sLastCollect=t6;
      GCLOG("Collect time %s total=%.2fms =%.1f%%\n  setup=%.2f\n  %s=%.2f(init=%.2f/roots=%.2f %d+%d/loc=%.2f*%d %d+%d/mark=%.2f %d+%d/fin=%.2f*%d/ids=%.2f)\n  reclaim=%.2f\n  large(%d->%d, recyc %d)=%.2f\n  defrag=%.2f\n",
               generational ? "gen" : "std",
              (t6-t0)*1000, (t6-t0)*100.0/period, // total %
              (t1-t0)*1000, // sync/setup
              generational ? "mark gen" : "mark", (t2-t1)*1000,
                      (tMarkInit-t1)*1000,
                      (tMarkLocal-tMarkInit)*1000,  hx::rootObjects, hx::rootAllocs,
                      (tMarkLocalEnd-tMarkLocal)*1000, hx::localCount, hx::localObjects-hx::rootObjects, hx::localAllocs-hx::rootAllocs,
                      (tMarked-tMarkLocalEnd)*1000, sObjectMarks-hx::localObjects, sAllocMarks-hx::localAllocs,
                      (hx::tFinalizers-tMarked)*1000, hx::finalizerCount,
                      (t2-hx::tFinalizers)*1000,
              (t3-t2)*1000, // reclaim
              l0, l1, l2, (t4-t3)*1000, // large
              (t5-t4)*1000 // defrag
              );
      sObjectMarks = sAllocMarks = 0;

      #endif

      #ifdef PROFILE_THREAD_USAGE
      GCLOG("Thread chunks:%d, wakes=%d\n", sThreadChunkPushCount, sThreadChunkWakes);
      for(int i=-1;i<MAX_GC_THREADS;i++)
        GCLOG(" thread %d] %d + %d\n", i, sThreadMarkCount[i], sThreadArrayMarkCount[i]);
      GCLOG("Locking spins  : %d\n", sSpinCount);
      sSpinCount = 0;
      #endif

      #ifdef HXCPP_GC_GENERATIONAL
      if (sGcMode==gcmGenerational)
         for(int i=0;i<mLocalAllocs.size();i++)
         {
            hx::StackContext *ctx = (hx::StackContext *)mLocalAllocs[i];
            ctx->mOldReferrers = hx::sGlobalChunks.alloc();
         }
      #endif


      #ifdef HXCPP_GC_VERIFY
      VerifyBlockOrder();
      #endif

      for(int i=0;i<LOCAL_POOL_SIZE;i++)
      {
         LocalAllocator *l = mLocalPool[i];
         if (l)
            ClearPooledAlloc(l);
      }

      #ifdef HXCPP_TELEMETRY
      __hxt_gc_end();
      #endif

      sgIsCollecting = false;


      hx::gPauseForCollect = 0x00000000;
      #ifndef HXCPP_SINGLE_THREADED_APP
         for(int i=0;i<mLocalAllocs.size();i++)
         {
            #ifdef HXCPP_SCRIPTABLE
            ((hx::StackContext *)mLocalAllocs[i])->byteMarkId = hx::gByteMarkID;
            #endif
            if (mLocalAllocs[i]!=this_local)
               ReleaseFromSafe(mLocalAllocs[i]);
         }

         if (!inLocked)
            gThreadStateChangeLock->unlock();
      #else
        #ifdef HXCPP_SCRIPTABLE
        hx::gMainThreadContext->byteMarkId = hx::gByteMarkID;
        #endif
      #endif


      PROFILE_COLLECT_SUMMARY_END;
   }

   void reclaimBlocks(bool full, BlockDataStats &outStats)
   {
      if (MAX_GC_THREADS>1)
      {
         for(int i=0;i<MAX_GC_THREADS;i++)
            sThreadBlockDataStats[i].clear();
         StartThreadJobs(full ? tpjReclaimFull : tpjReclaim, mAllBlocks.size(), true);
         outStats = sThreadBlockDataStats[0];
         for(int i=1;i<MAX_GC_THREADS;i++)
            outStats.add(sThreadBlockDataStats[i]);
      }
      else
      {
         outStats.clear();
