            continue;
         }

         #ifndef HXCPP_SINGLE_THREADED_APP
         hx::EnterGCFreeZone();
         gThreadStateChangeLock->lock();
         hx::ExitGCFreeZoneLocked();

         result = GetNextFree(inRequiredBytes);
         #endif

         bool forceCompact = false;
         if (!result && allowMoreBlocks() && (!sgInternalEnable || GetWorkingMemory()<sWorkingMemorySize))
         {
            if (AllocMoreBlocks(forceCompact,false))
               result = GetNextFree(inRequiredBytes);
         }

         if (!result)
         {
            inAlloc->SetupStackAndCollect(false,forceCompact,true,true);
            result = GetNextFree(inRequiredBytes);
         }

         if (!result && !forceCompact)
         {
            // Try with compact this time...
            forceCompact = true;
            inAlloc->SetupStackAndCollect(false,forceCompact,true,true);
            result = GetNextFree(inRequiredBytes);
         }

         if (!result)
         {
            if (AllocMoreBlocks(forceCompact,false))
               result = GetNextFree(inRequiredBytes);
         }

         if (!result)
         {
            GCLOG("Memory exhausted.\n");
            #ifndef HXCPP_M64
            GCLOG(" try 64 bit build.\n");
            #endif
            #ifndef HXCPP_GC_BIG_BLOCKS
            GCLOG(" try HXCPP_GC_BIG_BLOCKS.\n");
            #endif
            DebuggerTrap();
         }

         // Assume all wil be used
         mCurrentRowsInUse += result->GetFreeRows();

         #ifndef HXCPP_SINGLE_THREADED_APP
         gThreadStateChangeLock->unlock();
         #endif


         result->zeroAndUnlock();

         // After zero/reclaim, it might be that the hole size is smaller than we thought.
         if (result->mMaxHoleSize>=inRequiredBytes)
            return result;

         repoolReclaimedBlock(result);
      }
  }

   #if  defined(HXCPP_VISIT_ALLOCS) // {

  void MoveSpecial(hx::Object *inTo, hx::Object *inFrom, int size)
   {
      #ifdef HX_WATCH
      if (hxInWatchList(reinterpret_cast<uintptr_t>(inFrom)))
         GCLOG("****** watch MOVE from %p\n",inFrom);
      if (hxInWatchList(reinterpret_cast<uintptr_t>(inTo)))
         GCLOG("****** watch MOVE to %p\n",inTo);
      #endif
       // FinalizerList will get visited...

       hx::FinalizerMap::iterator i = hx::sFinalizerMap.find(inFrom);
       if (i!=hx::sFinalizerMap.end())
       {
          hx::finalizer f = i->second;
          hx::sFinalizerMap.erase(i);
          hx::sFinalizerMap[inTo] = f;
       }

       hx::HaxeFinalizerMap::iterator h = hx::sHaxeFinalizerMap.find(inFrom);
       if (h!=hx::sHaxeFinalizerMap.end())
       {
          hx::HaxeFinalizer f = h->second;
          hx::sHaxeFinalizerMap.erase(h);
          hx::sHaxeFinalizerMap[inTo] = f;
       }

       hx::MakeZombieSet::iterator mz = hx::sMakeZombieSet.find(inFrom);
       if (mz!=hx::sMakeZombieSet.end())
       {
          hx::sMakeZombieSet.erase(mz);
          hx::sMakeZombieSet.insert(inTo);
       }

       hx::ObjectIdMap::iterator id = hx::sObjectIdMap.find(inFrom);
       if (id!=hx::sObjectIdMap.end())
       {
          hx::sIdObjectMap[id->second] = inTo;
          hx::sObjectIdMap[inTo] = id->second;
          hx::sObjectIdMap.erase(id);
       }

       // Maybe do something faster with weakmaps

#ifdef HXCPP_TELEMETRY
       //printf(" -- moving %018x to %018x, size %d\n", inFrom, inTo, size);
       __hxt_gc_realloc(inFrom, inTo, size);
#endif

   }



   bool MoveBlocks(MoveBlockJob &inJob,BlockDataStats &ioStats)
   {
      BlockData *dest = 0;
      BlockDataInfo *destInfo = 0;
      unsigned int *destStarts = 0;
      int hole = -1;
      int holes = 0;
      int destPos = 0;
      int destLen = 0;

      int moveObjs = 0;
      int clearedBlocks = 0;

      while(true)
      {
         BlockDataInfo *from = inJob.getFrom();
         if (!from)
            break;
         if (from->calcFragScore()>FRAG_THRESH)
            ioStats.fraggedBlocks--;
         ioStats.rowsInUse -= from->mUsedRows;
         if (!from->isEmpty())
            ioStats.emptyBlocks ++;

         unsigned int *allocStart = from->allocStart;
         #ifdef SHOW_MEM_EVENTS
         //GCLOG("Move from %p (%d x %d)\n", from, from->mUsedRows, from->mHoles );
         #endif

         const unsigned char *rowMarked = from->mPtr->mRowMarked;
         for(int r=IMMIX_HEADER_LINES;r<IMMIX_LINES;r++)
         {
            if (rowMarked[r])
            {
               unsigned int starts = allocStart[r];
               if (!starts)
                  continue;
               for(int i=0;i<32;i++)
               {
                  if ( starts & (1<<i))
                  {
                     unsigned int* row{ reinterpret_cast<unsigned int*>(from->mPtr->row(static_cast<size_t>(r))) };
                     unsigned int &header{ row[i] };

                     if ((header&IMMIX_ALLOC_MARK_ID) == hx::gMarkID)
                     {
                        int size = ((header & IMMIX_ALLOC_SIZE_MASK) >> IMMIX_ALLOC_SIZE_SHIFT);
                        int allocSize = size + sizeof(int);

                        while(allocSize + ALIGN_PADDING(destPos)>destLen)
                        {
                           hole++;
                           if (hole<holes)
                           {
                              destPos = destInfo->mRanges[hole].start;
                              destLen = destInfo->mRanges[hole].length;
                           }
                           else
                           {
                              if (destInfo)
                                 destInfo->makeFull();
                              do
                              {
                                 destInfo = inJob.getTo();
                                 if (!destInfo)
                                    goto all_done;
                              } while(destInfo->mHoles==0);


                              ioStats.rowsInUse += IMMIX_USEFUL_LINES - destInfo->mUsedRows;
                              //destInfo->zero();
                              dest = destInfo->mPtr;
                              destStarts = destInfo->allocStart;

                              hole = 0;
                              holes = destInfo->mHoles;
                              destPos = destInfo->mRanges[hole].start;
                              destLen = destInfo->mRanges[hole].length;
                           }
                        }

                        #ifdef HXCPP_ALIGN_ALLOC
                        destPos += ALIGN_PADDING(destPos);
                        #endif

                        int startRow = destPos>>IMMIX_LINE_BITS;

                        destStarts[ startRow ] |= hx::gImmixStartFlag[destPos&127];

                        unsigned int *buffer = (unsigned int *)((char *)dest + destPos);

                        unsigned int headerPreserve = header & IMMIX_HEADER_PRESERVE;

                        int end = destPos + allocSize;

                        *buffer++ =  (( (end+(IMMIX_LINE_LEN-1))>>IMMIX_LINE_BITS) -startRow) |
                                        (size<<IMMIX_ALLOC_SIZE_SHIFT) |
                                        headerPreserve |
                                        hx::gMarkID;
                        destPos = end;
                        destLen -= allocSize;

                        unsigned int *src = row + i + 1;

                        MoveSpecial((hx::Object *)buffer,(hx::Object *)src, size);

                        // Result has moved - store movement in original position...
                        memcpy(buffer, src, size);
                        //GCLOG("   move %p -> %p %d (%08x %08x )\n", src, buffer, size, buffer[-1], buffer[1] );

                        *(unsigned int **)src = buffer;
                        header = IMMIX_OBJECT_HAS_MOVED;
                        moveObjs++;

                        starts &= ~(1<<i);
                        if (!starts)
                           break;
                     }
                  }
               }
            }
         }

         all_done:
         if (destInfo)
         {
            // Still have space, which means from finished.
            destInfo->makeFull();
            from->clear();
            clearedBlocks++;
            // Could remove some used rows from stats
         }
         else
         {
            // Partialy cleared, then ran out of to blocks - remove from allocation this round
            if (from)
            {
               // Could maybe be a bit smarter here
               ioStats.rowsInUse += from->mUsedRows;
               from->makeFull();
            }
         }
      }

      #ifdef SHOW_FRAGMENTATION
      GCLOG("Moved %d objects (%d/%d blocks)\n", moveObjs, clearedBlocks, mAllBlocks.size());
      #endif

      if (moveObjs)
      {
         AdjustPointers(0);
      }


      return moveObjs;
   }

   void AdjustPointers(hx::QuickVec<hx::Object *> *inRemembered)
   {
      class AdjustPointer : public hx::VisitContext
      {
         GlobalAllocator *mAlloc;
      public:
         AdjustPointer(GlobalAllocator *inAlloc) : mAlloc(inAlloc) {  }

         void visitObject(hx::Object **ioPtr) HXCPP_OVERRIDE
         {
            if ( ((*(unsigned int **)ioPtr)[-1]) == IMMIX_OBJECT_HAS_MOVED )
            {
               //GCLOG("  patch object to  %p -> %p\n", *ioPtr,  (*(hx::Object ***)ioPtr)[0]);
               *ioPtr = (*(hx::Object ***)ioPtr)[0];
               //GCLOG("    %08x %08x ...\n", ((int *)(*ioPtr))[0], ((int *)(*ioPtr))[1] );
            }
         }

         void visitAlloc(void **ioPtr) HXCPP_OVERRIDE
         {
            if ( ((*(unsigned int **)ioPtr)[-1]) == IMMIX_OBJECT_HAS_MOVED )
            {
               //GCLOG("  patch reference to  %p -> %p\n", *ioPtr,  (*(void ***)ioPtr)[0]);
               *ioPtr = (*(void ***)ioPtr)[0];
               //GCLOG("    %08x %08x ...\n", ((int *)(*ioPtr))[0], ((int *)(*ioPtr))[1] );
            }
         }
      };


      AdjustPointer adjust(this);
      VisitAll(&adjust,true, inRemembered);
   }


   #ifdef HXCPP_GC_GENERATIONAL
   BlockDataInfo *getNextBlockWithoutSurvivors(int &ioPosition)
   {
      while(ioPosition<mAllBlocks.size())
      {
         BlockDataInfo *b = mAllBlocks[ioPosition++];
         if (b->mHoles>0 && !b->mHasSurvivor)
            return b;
      }
      return 0;
   }



   int MoveSurvivors(hx::QuickVec<hx::Object *> *inRemembered)
   {
      int sourceScan = 0;
      int destScan = 0;
      int moveObjs = 0;
      int clearedBlocks = 0;

      BlockDataInfo *destInfo = 0;
      BlockData *dest = 0;
      int destHole = 0;
      int destPos = 0;
      int destLen = 0;
      unsigned int *destStarts = 0;

      BlockDataInfo *from = 0;

      int tested = 0;

      int all = mAllBlocks.size();
      for(int srcBlock=0; srcBlock<all; srcBlock++)
      {
         from = mAllBlocks[srcBlock];
         if (from->mPinned || !from->mHasSurvivor)
         {
            from = 0;
            continue;
         }
         tested++;
         unsigned int *srcStart = from->allocStart;

         // Scan nursery for survivors
         for (uint8_t hole{ 0 }; hole < from->mHoles; hole++)
         {
            int start = from->mRanges[hole].start;
            int len = from->mRanges[hole].length;
            int startRow = start >> IMMIX_LINE_BITS;
            int end = startRow + (len >> IMMIX_LINE_BITS);
            for(int r = startRow; r<end;r++)
            {
               unsigned int &startFlags = srcStart[r];
               if (startFlags)
               {
                  for(int loc=0;loc<32;loc++)
                     if (startFlags & (1<<loc))
                     {
                        unsigned int* row{ reinterpret_cast<unsigned int*>(from->mPtr->row(static_cast<size_t>(r))) };
                        unsigned int &header{ row[loc] };

                        if ((header&IMMIX_ALLOC_MARK_ID) == hx::gMarkID)
                        {
                           int size = ((header & IMMIX_ALLOC_SIZE_MASK) >> IMMIX_ALLOC_SIZE_SHIFT);
                           int allocSize = size + sizeof(int);

                           // Find dest reqion ...
                           while(destHole==0 || destLen < ALIGN_PADDING(destPos) + allocSize)
                           {
                              if (destHole==0)
                              {
                                 if (destInfo)
                                    destInfo->makeFull();
                                 destInfo = getNextBlockWithoutSurvivors(destScan);
                                 if (!destInfo)
                                    goto no_more_moves;

                                 destHole = -1;
                                 destLen = 0;
                              }
                              if (destLen + ALIGN_PADDING(0)<allocSize)
                              {
                                 destHole++;
                                 if (destHole>=destInfo->mHoles)
                                    destHole = 0;
                                 else
                                 {
                                    dest = destInfo->mPtr;
                                    destPos = destInfo->mRanges[destHole].start;
                                    destLen = destInfo->mRanges[destHole].length;
                                    destStarts = destInfo->allocStart;
                                 }
                              }
                           }

                           // TODO - not copy + paste

                        printf("Move!\n");
                           #ifdef HXCPP_ALIGN_ALLOC
                           destPos += ALIGN_PADDING(destPos);
                           #endif

                           int startRow = destPos>>IMMIX_LINE_BITS;

                           destStarts[ startRow ] |= hx::gImmixStartFlag[destPos&127];

                           unsigned int *buffer = (unsigned int *)((char *)dest + destPos);

                           unsigned int headerPreserve = header & IMMIX_HEADER_PRESERVE;

                           int end = destPos + allocSize;

                           *buffer++ =  (( (end+(IMMIX_LINE_LEN-1))>>IMMIX_LINE_BITS) -startRow) |
                                           (size<<IMMIX_ALLOC_SIZE_SHIFT) |
                                           headerPreserve |
                                           hx::gMarkID;
                           destPos = end;
                           destLen -= allocSize;

                           unsigned int *src = row + loc + 1;

                           MoveSpecial((hx::Object *)buffer,(hx::Object *)src, size);

                           // Result has moved - store movement in original position...
                           memcpy(buffer, src, size);
                           //GCLOG("   move %p -> %p %d (%08x %08x )\n", src, buffer, size, buffer[-1], buffer[1] );

                           *(unsigned int **)src = buffer;
                           header = IMMIX_OBJECT_HAS_MOVED;
                           moveObjs++;

                           startFlags &= ~(1<<loc);
                           if (!startFlags)
                              break;
                        }
