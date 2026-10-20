               finishThreadJob(inId);
         }
         else
         {
            if (sgThreadPoolJob==tpjReclaimFull || sgThreadPoolJob==tpjReclaim)
               ReclaimAsync(sThreadBlockDataStats[inId]);

            else if (sgThreadPoolJob==tpjCountRows)
               CountAsync(sThreadBlockDataStats[inId]);

            else if (sgThreadPoolJob==tpjGetStats)
               GetStatsAsync(sThreadBlockDataStats[inId]);

            #ifdef HXCPP_VISIT_ALLOCS
            else if (sgThreadPoolJob==tpjVisitBlocks)
               VisitBlockAsync(sThreadVisitContext);
            #endif

            else if (sgThreadPoolJob==tpjAsyncZero)
               ZeroAsync();

            finishThreadJob(inId);
         }
      }
   }

   static void SThreadLoop( void *inInfo )
   {
      sGlobalAlloc->ThreadLoop((int)(size_t)inInfo);
   }

   void CreateWorker(int inId)
   {
      void *info = (void *)(size_t)inId;

      #if defined(__EMSCRIPTEN__)
         // Only one thread
      #else
         sThreadWake[inId] = new std::condition_variable_any();
         if (0 == inId)
         {
             sThreadJobDone = new std::condition_variable_any();
         }

         sThreadSleeping[inId] = false;

         std::thread thread(SThreadLoop, info);

         thread.detach();
      #endif
   }

   void StopThreadJobs(bool inKill)
   {
      bool maybeExtraWake = false;

      if (sAllThreads)
      {
         if (inKill)
         {
            sgThreadPoolAbort = true;
            if (sgThreadPoolJob==tpjAsyncZeroJit)
            {
               std::lock_guard<std::mutex> l(sThreadPoolLock);
               // Thread will be waiting, but not finished
               if (sRunningThreads & 0x1)
               {
                  sgThreadPoolJob = tpjNone;
                  maybeExtraWake = true;
                  wakeThreadLocked(0);
               }
            }
         }


         std::lock_guard<std::mutex> l(sThreadPoolLock);
         sThreadJobDoneSleeping = true;
         sThreadJobDone->wait(sThreadPoolLock, []() { return sRunningThreads == false; });
         sThreadJobDoneSleeping = false;
         sgThreadPoolAbort = false;
         sAllThreads = 0;
         sgThreadPoolJob = tpjNone;
         sLazyThreads = 0;
      }
   }


   void StartThreadJobs(ThreadPoolJob inJob, int inWorkers, bool inWait, int inThreadLimit = -1)
   {
      mThreadJobId = 0;

      if (!inWorkers)
         return;

      if (!sThreadPoolInit)
      {
         sThreadPoolInit = true;
         for(int i=0;i<MAX_GC_THREADS;i++)
            CreateWorker(i);
      }

      std::lock_guard<std::mutex> l(sThreadPoolLock);

      sgThreadPoolJob = inJob;

      sgThreadCount = inThreadLimit<0 ? MAX_GC_THREADS : std::min((int)MAX_GC_THREADS, inThreadLimit) ;

      int start = std::min(inWorkers, sgThreadCount );

      sAllThreads = (1<<sgThreadCount) - 1;

      sRunningThreads = (1<<start) - 1;

      sLazyThreads = sRunningThreads != sAllThreads;

      for(int i=0;i<start;i++)
         SignalThreadPool(sThreadWake[i],sThreadSleeping[i]);

      if (inWait)
      {
         // Join the workers...
         sThreadJobDoneSleeping = true;
         sThreadJobDone->wait(sThreadPoolLock, []() { return sRunningThreads == false; });
         sThreadJobDoneSleeping = false;

         sAllThreads = 0;
         sgThreadPoolJob = tpjNone;
         sLazyThreads = 0;

         if (sRunningThreads)
         {
            printf("Bad thread stop %d\n", sRunningThreads);
            DebuggerTrap();
         }
      }
   }


   #if defined(SHOW_FRAGMENTATION) || defined(SHOW_MEM_EVENTS)
   std::string formatBytes(size_t bytes)
   {
      size_t k = 1<<10;
      size_t meg = 1<<20;

      char strBuf[100];
      if (bytes<k)
         snprintf(strBuf,sizeof(strBuf),"%d", (int)bytes);
      else if (bytes<meg)
         snprintf(strBuf,sizeof(strBuf),"%.2fk", (double)bytes/k);
      else
         snprintf(strBuf,sizeof(strBuf),"%.2fmb", (double)bytes/meg);
      return strBuf;
   }
   #endif


   double tMarkInit;
   double tMarkLocal;
   double tMarkLocalEnd;
   double tMarked;
   void MarkAll(bool inGenerational)
   {
      if (!inGenerational)
      {
         hx::gPrevByteMarkID = hx::gByteMarkID;

         // The most-significant header byte looks like:
         // C nH Odd Even  c c c c
         //  C = "is const alloc bit" - in this case Odd and Even will be false
         // nH = non-hashed const string bit
         // Odd = true if cycle is odd
         // Even = true if cycle is even
         // c c c c = 4 bit cycle code
         //
         hx::gPrevMarkIdMask = ((~hx::gMarkID) & 0x30000000) | HX_GC_CONST_ALLOC_BIT;

         // 4 bits of cycle
         gByteMarkID = (gByteMarkID + 1) & 0x0f;
         if (gByteMarkID & 0x1)
            gByteMarkID |= 0x20;
         else
            gByteMarkID |= 0x10;

         hx::gMarkID = gByteMarkID << 24;
         hx::gMarkIDWithContainer = (gByteMarkID << 24) | IMMIX_ALLOC_IS_CONTAINER;
         gRememberedByteMarkID = gByteMarkID | HX_GC_REMEMBERED;

         #ifdef HX_WATCH
         GCLOG(" non-gen mark byte -> %02x\n", hx::gByteMarkID);
         #endif
         gBlockStack = 0;

         ClearRowMarks();
      }
      else
      {
         #ifdef HX_WATCH
         GCLOG(" generational mark byte -> %02x\n", hx::gByteMarkID);
         #endif
         ClearBlockMarks();
      }

      MEM_STAMP(tMarkInit);

      #ifdef PROFILE_THREAD_USAGE
      for(int i=-1;i<MAX_GC_THREADS;i++)
         sThreadChunkPushCount = sThreadChunkWakes = sThreadMarkCount[i] = sThreadArrayMarkCount[i] = 0;
      #endif



      mMarker.init();

      hx::MarkClassStatics(&mMarker);

      {
      hx::AutoMarkPush info(&mMarker,"Roots","root");

      for(hx::RootSet::iterator i = hx::sgRootSet.begin(); i!=hx::sgRootSet.end(); ++i)
      {
         hx::Object *&obj = **i;
         if (obj)
            hx::MarkObjectAlloc(obj , &mMarker );
      }

      if (hx::sgOffsetRootSet)
         for(hx::OffsetRootSet::iterator i = hx::sgOffsetRootSet->begin(); i!=hx::sgOffsetRootSet->end(); ++i)
         {
            char *ptr = *(char **)(i->first);
            int offset = i->second;
            hx::Object *obj = (hx::Object *)(ptr - offset);

            if (obj)
               hx::MarkObjectAlloc(obj , &mMarker );
         }
      } // automark

      #ifdef PROFILE_COLLECT
      hx::rootObjects = sObjectMarks;
      hx::rootAllocs = sAllocMarks;
      #endif


      {
      hx::AutoMarkPush info(&mMarker,"Zombies","zombie");
      // Mark zombies too....
      for(int i=0;i<hx::sZombieList.size();i++)
         hx::MarkObjectAlloc(hx::sZombieList[i] , &mMarker );
      } // automark

      MEM_STAMP(tMarkLocal);
      hx::localCount = 0;

      mMarker.isGenerational = inGenerational;

      // Mark local stacks
      for(int i=0;i<mLocalAllocs.size();i++)
         MarkLocalAlloc(mLocalAllocs[i] , &mMarker);

      #ifdef PROFILE_COLLECT
      hx::localObjects = sObjectMarks;
      hx::localAllocs = sAllocMarks;
      #endif

      MEM_STAMP(tMarkLocalEnd);

      #ifdef HX_MULTI_THREAD_MARKING
         mMarker.releaseJobs();

         // Unleash the workers...
         StartThreadJobs(tpjMark, MAX_GC_THREADS, true);
      #else
         mMarker.processMarkStack();
      #endif



      MEM_STAMP(tMarked);

      hx::FindZombies(mMarker);

      hx::RunFinalizers();

      #ifdef HXCPP_GC_VERIFY
      for(int i=0;i<mAllBlocks.size();i++)
         mAllBlocks[i]->verify("After mark");
      #endif

      #ifdef HX_WATCH
      for (uintptr_t watch : hxWatchList)
      {
         GCLOG("********* Watch mark : %p %08x\n", reinterpret_cast<void*>(watch), (reinterpret_cast<unsigned int*>(watch))[-1]);
         GCLOG(" ******** is marked  : %d\n", (reinterpret_cast<unsigned char*>(watch)[HX_ENDIAN_MARK_ID_BYTE] == gByteMarkID));
      }
      #endif
   }



   #ifdef HX_GC_VERIFY_ALLOC_START
   void verifyAllocStart()
   {
      for(int i=1;i<mAllBlocks.size();i++)
         mAllBlocks[i]->verifyAllocStart();
   }
   #endif

   #ifdef HXCPP_GC_VERIFY
   void VerifyBlockOrder()
   {
      for(int i=1;i<mAllBlocks.size();i++)
      {
         if ( mAllBlocks[i-1]->mPtr >= mAllBlocks[i]->mPtr)
         {
            printf("Bad block order block[%d]=%p >= block[%d]=%p / %zu\n", i-1, mAllBlocks[i-1]->mPtr,
                    i, mAllBlocks[i]->mPtr, mAllBlocks.size() );
            DebuggerTrap();
         }
      }
   }
   #endif

   void Collect(bool inMajor, bool inForceCompact, bool inLocked,bool inFreeIsFragged)
   {
      PROFILE_COLLECT_SUMMARY_START;

      #ifndef HXCPP_SINGLE_THREADED_APP
      // If we set the flag from 0 -> 0xffffffff then we are the collector
      //  otherwise, someone else is collecting at the moment - so wait...
      if (false == atomic_compare_exchange_strong_discard(&hx::gPauseForCollect, 0, std::numeric_limits<unsigned int>::max()))
      {
         if (inLocked)
         {
            gThreadStateChangeLock->unlock();

            hx::PauseForCollect();

            hx::EnterGCFreeZone();
            gThreadStateChangeLock->lock();
            hx::ExitGCFreeZoneLocked();
         }
         else
         {
            hx::PauseForCollect();
         }
         return;
      }
      #endif

      STAMP(t0)

      // We are the collector - all must wait for us
      LocalAllocator *this_local = 0;
      #ifndef HXCPP_SINGLE_THREADED_APP
      this_local = (LocalAllocator *)(hx::ImmixAllocator *)hx::tlsStackContext;

      if (!inLocked)
         gThreadStateChangeLock->lock();

      for(int i=0;i<mLocalAllocs.size();i++)
         if (mLocalAllocs[i]!=this_local)
            WaitForSafe(mLocalAllocs[i]);
      #endif

      sgIsCollecting = true;

      StopThreadJobs(true);
      #ifdef HXCPP_DEBUG
      sgAllocsSinceLastSpam = 0;
      #endif

      HX_STACK_FRAME("GC", "collect", 0, "GC::collect", __FILE__, __LINE__,0)
      #ifdef SHOW_MEM_EVENTS
      int here = 0;
      GCLOG("=== Collect === %p\n",&here);
      #endif


      #ifdef PROFILE_THREAD_USAGE
      GCLOG("Thread zero waits %d/%d/%d, misses=%d, hits=%d\n", sThreadZeroPokes, sThreadZeroWaits, mFreeBlocks.size(), sThreadZeroMisses, sThreadBlockZeroCount);
      sThreadZeroWaits = 0;
      sThreadZeroPokes = 0;
      sThreadZeroMisses = 0;
      sThreadBlockZeroCount = 0;
      #endif



      #ifdef HXCPP_TELEMETRY
      __hxt_gc_start();
      #endif

      size_t freeFraggedRows = 0;
      if (inFreeIsFragged)
      {
         for(int i=mNextFreeBlockOfSize[0]; i<mFreeBlocks.size(); i++)
            freeFraggedRows += mFreeBlocks[i]->GetFreeRows();
      }

      // Now all threads have mTopOfStack & mBottomOfStack set.
      bool generational = false;

      #ifdef HXCPP_GC_GENERATIONAL
      bool compactSurviors = false;

      if (sGcMode==gcmGenerational)
      {
         for(int i=0;i<mLocalAllocs.size();i++)
         {
            hx::StackContext *ctx = (hx::StackContext *)mLocalAllocs[i];
            if( ctx->mOldReferrers->count )
                hx::sGlobalChunks.addLocked( ctx->mOldReferrers );
            else
                hx::sGlobalChunks.free( ctx->mOldReferrers );
            ctx->mOldReferrers = 0;
         }
      }

      hx::QuickVec<hx::Object *> rememberedSet;
      generational = !inMajor && !inForceCompact && sGcMode == gcmGenerational;
      if (sGcMode==gcmGenerational)
      {
         hx::sGlobalChunks.copyPointers(rememberedSet,!generational);
         #ifdef SHOW_MEM_EVENTS
         GCLOG("Patch remembered set marks %d\n", rememberedSet.size());
         #endif
         for(int i=0;i<rememberedSet.size();i++)
            ((unsigned char *)rememberedSet[i])[HX_ENDIAN_MARK_ID_BYTE] = gByteMarkID;
      }
      #endif

      STAMP(t1)

      MarkAll(generational);

      #ifdef HX_GC_VERIFY_GENERATIONAL
      {
         #ifdef SHOW_MEM_EVENTS
         GCLOG("verify generational [\n");
         #endif
         sGcVerifyGenerational = true;
         sgTimeToNextTableUpdate--;
         MarkAll(false);
         sGcVerifyGenerational = false;
         #ifdef SHOW_MEM_EVENTS
         GCLOG("] verify generational\n");
         #endif
      }
      #endif
