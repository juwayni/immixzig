                     }
               }
            }
         }

         no_more_moves:
            if (destInfo)
            {
               destInfo->makeFull();
               clearedBlocks++;
            }
            else
            {
               // Partialy cleared, then ran out of to blocks - remove from allocation this round
               if (from && moveObjs)
               {
                  // Could maybe be a bit smarter here
                  //ioStats.rowsInUse += from->mUsedRows;
                  from->makeFull();
               }
            }
      }

      #ifdef SHOW_FRAGMENTATION
      //GCLOG("Compacted nursery %d objects (%d/%d blocks)\n", moveObjs, clearedBlocks, mAllBlocks.size());
      #endif

      if (moveObjs)
      {
         AdjustPointers(inRemembered);
      }

      return moveObjs;
   }
   #endif

   int releaseEmptyGroups(BlockDataStats &outStats, size_t releaseSize)
   {
      int released=0;
      int groups=0;
      for(int i=0; i<mAllBlocks.size(); i++ )
      {
         if (!mAllBlocks[i]->isEmpty())
             gAllocGroups[mAllBlocks[i]->mGroupId].isEmpty=false;
      }

      #ifdef HXCPP_GC_VERIFY
      typedef std::pair< void *, void * > ReleasedRange;
      std::vector<ReleasedRange> releasedRange;
      std::vector<int> releasedGids;
      #endif

      for(int i=0;i<gAllocGroups.size();i++)
      {
         GroupInfo &g = gAllocGroups[i];
         if (g.alloc && g.isEmpty && !g.pinned)
         {
            size_t groupBytes = g.blocks << (IMMIX_BLOCK_BITS);

            #ifdef SHOW_MEM_EVENTS_VERBOSE
            GCLOG("Release group %d: %p -> %p\n", i, g.alloc, g.alloc+groupBytes);
            #endif
            #ifdef HXCPP_GC_VERIFY
            releasedRange.push_back( ReleasedRange(g.alloc, g.alloc+groupBytes) );
            releasedGids.push_back(i);
            #endif

            HxFreeGCBlock(g.alloc);
            g.alloc = 0;

            groups++;

            if (groupBytes > releaseSize)
               break;

            releaseSize -= groupBytes;
         }
      }

      // Release blocks
      for(int i=0; i<mAllBlocks.size();  )
      {
         if (!gAllocGroups[mAllBlocks[i]->mGroupId].alloc)
         {
            outStats.emptyBlocks--;
            released++;
            mAllBlocks[i]->destroy();
            mAllBlocks.erase(i);
         }
         else
         {
            #ifdef HXCPP_GC_VERIFY
            for(int g=0;g<releasedGids.size();g++)
               if (mAllBlocks[i]->mGroupId == releasedGids[g])
               {
                  printf("Group %d should be released.\n", mAllBlocks[i]->mGroupId);
                  DebuggerTrap();
               }
            #endif
            i++;
         }
      }

      #ifdef HXCPP_GC_VERIFY
      for(int i=0;i<mAllBlocks.size();i++)
      {
         BlockDataInfo *info = mAllBlocks[i];
         void *ptr = info->mPtr;
         for(int r=0;r<releasedRange.size();r++)
         if ( ptr>=releasedRange[r].first && ptr<releasedRange[r].second )
         {
            printf("Released block %p(%d:%p) still in list\n", info, info->mId, info->mPtr );
            DebuggerTrap();
         }
      }

      VerifyBlockOrder();
      #endif


      #if defined(SHOW_MEM_EVENTS) || defined(SHOW_FRAGMENTATION)
      GCLOG("Release %d blocks, %d groups,  %s\n", released, groups, formatBytes((size_t)released<<(IMMIX_BLOCK_BITS)).c_str());
      #endif
      return released;
   }


   void calcMoveOrder()
   {
      for(int i=0;i<gAllocGroups.size();i++)
         gAllocGroups[i].clear();

      for(int i=0; i<mAllBlocks.size(); i++ )
      {
         BlockDataInfo &block = *mAllBlocks[i];
         GroupInfo &g = gAllocGroups[block.mGroupId];
         if (block.mPinned)
            g.pinned = true;
         g.usedBytes += block.mUsedBytes;
         g.usedSpace += block.mUsedRows<<IMMIX_LINE_BITS;
      }

      for(int i=0; i<mAllBlocks.size(); i++ )
      {
         BlockDataInfo &block = *mAllBlocks[i];
         GroupInfo &g = gAllocGroups[block.mGroupId];
         // Base on group wasted space (when not pinned)
         block.mMoveScore = g.getMoveScore();
      }
   }

   #endif // } defined(HXCPP_VISIT_ALLOCS)

   void *GetIDObject(int inIndex)
   {
      std::lock_guard<std::mutex> lock(*gSpecialObjectLock);
      if (inIndex<0 || inIndex>hx::sIdObjectMap.size())
         return 0;
      return hx::sIdObjectMap[inIndex];
   }

   int GetObjectID(void * inPtr)
   {
      std::lock_guard<std::mutex> lock(*gSpecialObjectLock);
      hx::ObjectIdMap::iterator i = hx::sObjectIdMap.find( (hx::Object *)inPtr );
      if (i!=hx::sObjectIdMap.end())
         return i->second;

      int id = 0;
      if (hx::sFreeObjectIds.size()>0)
      {
         id = hx::sFreeObjectIds.pop();
      }
      else
      {
         id = hx::sObjectIdMap.size();
         hx::sIdObjectMap.push(0);
      }
      hx::sObjectIdMap[(hx::Object *)inPtr] = id;
      hx::sIdObjectMap[id] = (hx::Object *)inPtr;
      return id;
   }


   void ClearRowMarks()
   {
      for(int i=0;i<mAllBlocks.size();i++)
         mAllBlocks[i]->clearRowMarks();
   }


   void ClearBlockMarks()
   {
      for(int i=0;i<mAllBlocks.size();i++)
         mAllBlocks[i]->clearBlockMarks();
   }

   #ifdef HXCPP_VISIT_ALLOCS
   void VisitAll(hx::VisitContext *inCtx,bool inMultithread = false,hx::QuickVec<hx::Object *> *inRemembered = 0)
   {
      if (inRemembered)
      {
         int n = inRemembered->size();
         for(int i=0;i<n;i++)
            (*inRemembered)[i]->__Visit(inCtx);
      }
      else if (MAX_GC_THREADS>1 && inMultithread && mAllBlocks.size())
      {
         sThreadVisitContext = inCtx;
         StartThreadJobs(tpjVisitBlocks, mAllBlocks.size(), true);
         sThreadVisitContext = 0;
      }
      else
         for(int i=0;i<mAllBlocks.size();i++)
            mAllBlocks[i]->VisitBlock(inCtx);

      for(int i=0;i<mLocalAllocs.size();i++)
         VisitLocalAlloc(mLocalAllocs[i], inCtx);

      hx::VisitClassStatics(inCtx);

      for(hx::RootSet::iterator i = hx::sgRootSet.begin(); i!=hx::sgRootSet.end(); ++i)
      {
         hx::Object **obj = *i;
         if (*obj)
            inCtx->visitObject(obj);
      }


      if (hx::sgOffsetRootSet)
         for(hx::OffsetRootSet::iterator i = hx::sgOffsetRootSet->begin(); i!=hx::sgOffsetRootSet->end(); ++i)
         {
            char *ptr = *(char **)(i->first);
            int offset = i->second;
            hx::Object *obj = (hx::Object *)(ptr - offset);

            if (obj)
            {
               hx::Object *obj0 = obj;
               inCtx->visitObject(&obj);
               if (obj!=obj0)
                  *(char **)(i->first) = (char *)(obj) + offset;
            }
         }

      if (hx::sgFinalizers)
         for(int i=0;i<hx::sgFinalizers->size();i++)
            (*hx::sgFinalizers)[i]->Visit(inCtx);

      for(int i=0;i<hx::sFinalizableList.size();i++)
         inCtx->visitAlloc( &hx::sFinalizableList[i].base );

      for(int i=0;i<hx::sZombieList.size();i++)
         inCtx->visitObject( &hx::sZombieList[i] );

      for(int i=0;i<hx::sWeakRefs.size(); i++)
         inCtx->visitObject( (hx::Object **) &hx::sWeakRefs[i] );

      for(int i=0;i<hx::sWeakHashList.size();i++)
         inCtx->visitObject( (hx::Object **) &hx::sWeakHashList[i] );

   }

   #endif

   void ReclaimAsync(BlockDataStats &outStats)
   {
      while(!sgThreadPoolAbort)
      {
         size_t blockId{ mThreadJobId.fetch_add(1) };
         if (blockId>=mAllBlocks.size())
            break;

         if ( sgThreadPoolJob==tpjReclaimFull)
            mAllBlocks[blockId]->reclaim<true>(&outStats);
         else
            mAllBlocks[blockId]->reclaim<false>(&outStats);
      }
   }

   void CountAsync(BlockDataStats &outStats)
   {
      while(!sgThreadPoolAbort)
      {
         size_t blockId{ mThreadJobId.fetch_add(1) };
         if (blockId>=mAllBlocks.size())
            break;

         mAllBlocks[blockId]->countRows(outStats);
      }
   }


   void GetStatsAsync(BlockDataStats &outStats)
   {
      while(!sgThreadPoolAbort)
      {
         size_t blockId{ mThreadJobId.fetch_add(1) };
         if (blockId>=mAllBlocks.size())
            break;

         mAllBlocks[blockId]->getStats( outStats );
      }
   }



   #ifdef HXCPP_VISIT_ALLOCS
   void VisitBlockAsync(hx::VisitContext *inCtx)
   {
      while(!sgThreadPoolAbort)
      {
         size_t blockId{ mThreadJobId.fetch_add(1) };
         if (blockId>=mAllBlocks.size())
            break;

         mAllBlocks[blockId]->VisitBlock(inCtx);
      }
   }
   #endif


   void ZeroAsync()
   {
      while(!sgThreadPoolAbort)
      {
         size_t zeroListId{ mThreadJobId.fetch_add(1) };
         if (zeroListId>=mZeroList.size())
            break;

         BlockDataInfo *info = mZeroList[zeroListId];
         info->tryZero();
      }
   }

   bool ZeroAsyncJit()
   {
      int spinCount = 0;
      while(!sgThreadPoolAbort)
      {
         if (mZeroListQueue>=sMaxZeroQueueSize)
         {
            spinCount++;
            if (spinCount<10000)
               // Spin
               continue;
            // Full for now, so sleep...
            return true;
         }
         spinCount = 0;

         // Look at next block...
         size_t zeroListId{ mThreadJobId.fetch_add(1) };
         if (zeroListId>=mZeroList.size())
         {
            // Done, so sleep...
            return true;
         }

         BlockDataInfo *info = mZeroList[zeroListId];
         if (info->tryZero())
         {
            // We zeroed it, so increase queue count
            mZeroListQueue++;
            #ifdef PROFILE_THREAD_USAGE
            sThreadBlockZeroCount++;
            #endif
         }
      }

      return true;
   }

   // Try to maintain between sMinZeroQueueSize and sMaxZeroQueueSize pre-zeroed blocks
   void onZeroedBlockDequeued()
   {
      // Wake the thread?
      if (mZeroListQueue.fetch_sub(1) < sMinZeroQueueSize && !sRunningThreads)
      {
         if (mZeroListQueue + mThreadJobId < mZeroList.size())
         {
            // Wake zeroing thread
            std::lock_guard<std::mutex> l(sThreadPoolLock);
            if (!(sRunningThreads & 0x01))
            {
               #ifdef PROFILE_THREAD_USAGE
               sThreadZeroPokes++;
               #endif
               wakeThreadLocked(0);
            }
         }
      }
   }


   void finishThreadJob(int inId)
   {
      std::lock_guard<std::mutex> l(sThreadPoolLock);
      if (sRunningThreads & (1<<inId))
      {
         sRunningThreads &= ~(1<<inId);
         sLazyThreads = sRunningThreads != sAllThreads;

         if (!sRunningThreads)
            SignalThreadPool(sThreadJobDone,sThreadJobDoneSleeping);
      }
      else
      {
         printf("Finishe non-runnning thread?\n");
         DebuggerTrap();
      }
   }

   void waitForThreadWake(int inId)
   {
        std::lock_guard<std::mutex> l(sThreadPoolLock);
        int count = 0;

        // May be woken multiple times if sRunningThreads is set to 0 then 1 before we sleep
        sThreadSleeping[inId] = true;
        // Spurious wake?
        sThreadWake[inId]->wait(sThreadPoolLock, [&inId]() { return sRunningThreads & (1 << inId); });

        sThreadSleeping[inId] = false;
   }


   void ThreadLoop(int inId)
   {
      hx::MarkContext context(inId);

      while(true)
      {
         waitForThreadWake(inId);

         #ifdef HXCPP_GC_VERIFY
         if (! (sRunningThreads & (1<<inId)) )
            printf("Bad running threads!\n");
         #endif

         if (sgThreadPoolJob==tpjMark)
         {
            context.processMarkStack();
         }
         else if (sgThreadPoolJob==tpjAsyncZeroJit)
         {
            #ifdef PROFILE_THREAD_USAGE
            sThreadZeroWaits++;
            #endif
            if (ZeroAsyncJit())
