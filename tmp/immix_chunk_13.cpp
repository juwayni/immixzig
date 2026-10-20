      onThreadDetach();

      #ifndef HXCPP_SINGLE_THREADED_APP
      if (!mGCFreeZone)
         EnterGCFreeZone();
      #endif

      std::lock_guard<std::recursive_mutex> lock(*gThreadStateChangeLock);

      #ifdef HX_WINDOWS
      mID = 0;
      #endif

      #ifdef HXCPP_GC_GENERATIONAL
      if (mOldReferrers)
      {
         if ( mOldReferrers->count )
            hx::sGlobalChunks.pushJob( mOldReferrers, false );
         else
            hx::sGlobalChunks.free( mOldReferrers );
         mOldReferrers = 0;
      }
      #endif

      #ifndef HXCPP_EXPLICIT_STACK_EXTENT
      mTopOfStack = mBottomOfStack = 0;
      #endif

      sGlobalAlloc->RemoveLocalLocked(this);

      hx::tlsStackContext = 0;

      if (!sGlobalAlloc->ReturnToPoolLocked(this))
         delete this;
   }

   void Reset()
   {
      allocBase = 0;
      mCurrentHole = 0;
      mCurrentHoles = 0;
      mFraggedRows = 0;
      #ifdef HXCPP_GC_NURSERY
      spaceFirst = 0;
      spaceOversize = 0;
      #else
      spaceEnd = 0;
      spaceStart = 0;
      #endif
      mMoreHoles = false;
   }

   // The main of haxe calls SetTopOfStack(top,false)
   //   via hxcpp_set_top_of_stack or HX_TOP_OF_STACK in HxcppMain.cpp/Macros.h
   // The means "register current thread indefinitely"
   // Other places may call this to ensure the the current thread is registered
   //  indefinitely (until forcefully revoked)
   //
   // Normally libraries/mains will then let this dangle.
   //
   // However after the main, on android it calls SetTopOfStack(0,true), to unregister the thread,
   // because it is likely to be the ui thread, and the remaining call will be from
   // callbacks from the render thread.
   //
   // When threads want to attach temporarily, they will call
   // gc_set_top_of_stack(top,true)
   //  -> SetTopOfStack(top,true)
   //    do stuff...
   // gc_set_top_of_stack(0,true)
   //  -> SetTopOfStack(0,true)
   //
   //  OR
   //
   //  PushTopOfStack(top)
   //   ...
   //  PopTopOfStack
   //
   //  However, we really want the gc_set_top_of_stack(top,true) to allow recursive locks so:
   //
   //  SetTopOfStack(top,false) -> ensure global stack lock exists
   //  SetTopOfStack(top,true) -> add stack lock
   //  SetTopOfStack(0,_) -> pop stack lock. If all gone, clear global stack lock
   //

   #ifdef HXCPP_EXPLICIT_STACK_EXTENT // {

   void SetTopOfStack(int *inTop,bool inPush) { }
   void PushTopOfStack(void *inTop) { }
   void PopTopOfStack() { }
   void SetBottomOfStack(int *inBottom) { }
   void PauseForCollect() { }
   void EnterGCFreeZone() { }
   bool TryGCFreeZone() { return true; }
   bool TryExitGCFreeZone() { return false; }
   void ExitGCFreeZoneLocked() { }


   #else // } !HXCPP_EXPLICIT_STACK_EXTENT {
   void SetTopOfStack(int *inTop,bool inPush)
   {
      if (inTop)
      {
         if (!mTopOfStack)
            mTopOfStack = inTop;
         // EMSCRIPTEN the stack grows upwards - not wasm.
         // It could be that the main routine was called from deep with in the stack,
         //  then some callback was called from a higher location on the stack
         #ifdef HXCPP_STACK_UP
         else if (inTop < mTopOfStack)
            mTopOfStack = inTop;
         #else
         else if (inTop > mTopOfStack)
            mTopOfStack = inTop;
         #endif

         if (inPush)
            mStackLocks++;
         else
            mGlobalStackLock = true;

         #ifndef HXCPP_SINGLE_THREADED_APP
         if (mGCFreeZone)
            ExitGCFreeZone();
         #endif
      }
      else
      {
         if (mStackLocks>0)
            mStackLocks--;
         else
            mGlobalStackLock = false;

         if (!mStackLocks && !mGlobalStackLock)
         {
            Release();
         }
      }

      #ifdef VerifyStackRead
      VerifyStackRead(mBottomOfStack, mTopOfStack)
      #endif
   }


   void PushTopOfStack(void *inTop)
   {
      SetTopOfStack((int *)inTop,true);
   }


   void PopTopOfStack()
   {
      mStackLocks--;
      if (mStackLocks<=0 && !mGlobalStackLock)
      {
         Release();
      }
   }



   void SetBottomOfStack(int *inBottom)
   {
      mBottomOfStack = inBottom;
      #ifdef VerifyStackRead
      VerifyStackRead(mBottomOfStack, mTopOfStack)
      #endif
   }


   void PauseForCollect()
   {
      #ifndef HXCPP_SINGLE_THREADED_APP
      volatile int dummy = 1;
      mBottomOfStack = (int *)&dummy;
      CAPTURE_REGS;
      #ifdef VerifyStackRead
      VerifyStackRead(mBottomOfStack, mTopOfStack)
      #endif

      if (sgIsCollecting)
         CriticalGCError("Bad Allocation while collecting - from finalizer?");

      mReadyForCollect.Set();
      mCollectDone.Wait();
      #endif
   }

   void EnterGCFreeZone()
   {
      #ifndef HXCPP_SINGLE_THREADED_APP
      volatile int dummy = 1;
      mBottomOfStack = (int *)&dummy;
      if (mTopOfStack)
      {
         CAPTURE_REGS;
      }
      #ifdef VerifyStackRead
      VerifyStackRead(mBottomOfStack, mTopOfStack)
      #endif

      mGCFreeZone = true;
      mReadyForCollect.Set();
      #endif
   }

   bool TryGCFreeZone()
   {
      #ifndef HXCPP_SINGLE_THREADED_APP
      if (mGCFreeZone)
         return false;
      EnterGCFreeZone();
      #endif
      return true;
   }

   bool TryExitGCFreeZone()
   {
      #ifndef HXCPP_SINGLE_THREADED_APP
      if (!mGCFreeZone)
         return false;
      ExitGCFreeZone();
      return true;
      #endif
      return false;
   }


   void ExitGCFreeZone()
   {
      #ifndef HXCPP_SINGLE_THREADED_APP
      if (!mGCFreeZone)
         CriticalGCError("GCFree Zone mismatch");

      std::lock_guard<std::recursive_mutex> lock(*gThreadStateChangeLock);
      mReadyForCollect.Reset();
      mGCFreeZone = false;
      #endif
   }
        // For when we already hold the lock
   void ExitGCFreeZoneLocked()
   {
      #ifndef HXCPP_SINGLE_THREADED_APP
      mReadyForCollect.Reset();
      mGCFreeZone = false;
      #endif
   }



   // Called by the collecting thread to make sure this allocator is paused.
   // The collecting thread has the lock, and will not be releasing it until
   //  it has finished the collect.
   //
   //  The mGCFreeZone is set without a lock in the EnterGCFreeZone code, and then
   //   mReadyForCollect is set.  So it is possible the mGCFreeZone check may or may not
   //   trigger.  If this call happens first, mGCFreeZone will be zero, and mReadyForCollect
   //   will wait.  By this time mGCFreeZone will be set and the next call not check
   //   mReadyForCollect again.  If this one happens later, it is possible mReadyForCollect
   //   will not be waited on. mReadyForCollect will be cleared when the zone is left.
   //
   //  The mMoreHoles/spaceOversize/spaceEnd get zeroed without a lock.  The timing should
   //   not be critical since the allocation code shold expect that these are volatile.
   //   If the allocation works, all is good.  If it fails then the collection will happed soon.
   #if defined(__has_feature)
     #if __has_feature(thread_sanitizer)
      __attribute__((no_sanitize("thread")))
     #endif
   #endif
   void WaitForSafe()
   {
      #ifndef HXCPP_SINGLE_THREADED_APP
      if (!mGCFreeZone)
      {
         // Cause allocation routines to fail ...
         mMoreHoles = false;
         #ifdef HXCPP_GC_NURSERY
         spaceOversize = 0;
         #else
         spaceEnd = 0;
         #endif
         mReadyForCollect.Wait();
      }
      #endif
   }

   void ReleaseFromSafe()
   {
      #ifndef HXCPP_SINGLE_THREADED_APP
      if (!mGCFreeZone)
         mCollectDone.Set();
      #endif
   }

   #endif // }  HXCPP_EXPLICIT_STACK_EXTENT


   void SetupStackAndCollect(bool inMajor, bool inForceCompact, bool inLocked=false,bool inFreeIsFragged=false) HXCPP_OVERRIDE
   {
      #ifndef HXCPP_SINGLE_THREADED_APP
        #if HXCPP_DEBUG
        if (mGCFreeZone)
           CriticalGCError("Collecting from a GC-free thread");
        #endif
      #endif

      #ifndef HXCPP_EXPLICIT_STACK_EXTENT
      volatile int dummy = 1;
      mBottomOfStack = (int *)&dummy;

      CAPTURE_REGS;

      if (!mTopOfStack)
         mTopOfStack = mBottomOfStack;

      // EMSCRIPTEN the stack grows upwards
      #ifdef HXCPP_STACK_UP
      if (mBottomOfStack < mTopOfStack)
         mTopOfStack = mBottomOfStack;
      #else
      if (mBottomOfStack > mTopOfStack)
         mTopOfStack = mBottomOfStack;
      #endif

      #ifdef VerifyStackRead
      VerifyStackRead(mBottomOfStack, mTopOfStack)
      #endif

      #endif


      sGlobalAlloc->Collect(inMajor, inForceCompact, inLocked, inFreeIsFragged);
   }



   void ExpandAlloc(size_t& ioSize)
   {
      #ifdef HXCPP_ALIGN_ALLOC
      // Do nothing here - aligning to the end of the row will bump the
      // next allocation, so it's not clear if its a good idea.
      #else
         #ifdef HXCPP_GC_NURSERY
         int spaceStart = spaceFirst - allocBase - 4;
         int spaceEnd = spaceOversize - allocBase - 4;
         #endif

         size_t size{ ioSize + sizeof(int) };
         size_t end{ spaceStart + size };
         if (end <= spaceEnd)
         {
            size_t linePad{ IMMIX_LINE_LEN - (end & (IMMIX_LINE_LEN - 1)) };
            if (linePad>0 && linePad<=64)
               ioSize += linePad;
         }
      #endif
   }


   void *CallAlloc(size_t inSize, unsigned int inObjectFlags) HXCPP_OVERRIDE
   {
      #ifndef HXCPP_SINGLE_THREADED_APP
      #if HXCPP_DEBUG
      if (mGCFreeZone)
         CriticalGCError("Allocating from a GC-free thread");
      #endif
      if (hx::gPauseForCollect)
         PauseForCollect();
      #endif

      if (inSize==0)
         return hx::emptyAlloc;

      #if defined(HXCPP_VISIT_ALLOCS) && (defined(HXCPP_M64)||defined(HXCPP_ARM64))
      // Make sure we can fit a relocation pointer
      // I think we could use something like sizeof(void*) instead of 8.
      size_t allocSize{ sizeof(int) + std::max(size_t{ 8 }, inSize) };
      #else
      size_t allocSize{ sizeof(int) + inSize };
      #endif

      #if HXCPP_GC_DEBUG_LEVEL>0
      if (inSize & 3) DebuggerTrap();
      #endif

      while(1)
      {
         #ifdef HXCPP_GC_NURSERY
            unsigned char* buffer{ spaceFirst };
            #ifdef HXCPP_ALIGN_ALLOC
            if (reinterpret_cast<uintptr_t>(buffer) & 0x4 )
               buffer += 4;
            #endif
            unsigned char *end = buffer + allocSize;

            if ( end <= spaceOversize )
            {
               spaceFirst = end;

               reinterpret_cast<unsigned int*>(buffer)[-1] = static_cast<unsigned int>(allocSize) - 4 | inObjectFlags;

               #if defined(HXCPP_GC_CHECK_POINTER) && defined(HXCPP_GC_DEBUG_ALWAYS_MOVE)
               hx::GCOnNewPointer(buffer);
               #endif

               return buffer;
            }
            // spaceOversize might have been set to zero for quick-termination of alloc.
            unsigned char* s{ spaceOversize };
            if (s>spaceFirst && mFraggedRows)
               *mFraggedRows += static_cast<uint8_t>((s - spaceFirst) >> IMMIX_LINE_BITS);
         #else
            #ifdef HXCPP_ALIGN_ALLOC
            if (!(size_t{ spaceStart } & 0x4))
               spaceStart += 4;
            #endif

            size_t end{ allocSize + spaceStart };
            if (end <= spaceEnd)
            {
               unsigned int* buffer{ reinterpret_cast<unsigned int*>(allocBase + spaceStart) };

               size_t startRow{ spaceStart >> IMMIX_LINE_BITS };
               size_t endRow{ (end + (IMMIX_LINE_LEN - 1)) >> IMMIX_LINE_BITS };

               allocStartFlags[startRow] |= hx::gImmixStartFlag[spaceStart & 127];

               *buffer++ =
                   inObjectFlags |
                   hx::gMarkID |
                   static_cast<unsigned int>(inSize << IMMIX_ALLOC_SIZE_SHIFT) |
                   static_cast<unsigned int>(endRow - startRow);

               spaceStart = end;

               #if defined(HXCPP_GC_CHECK_POINTER) && defined(HXCPP_GC_DEBUG_ALWAYS_MOVE)
               hx::GCOnNewPointer(buffer);
               #endif

               #ifdef HXCPP_TELEMETRY
               __hxt_gc_alloc(buffer, inSize);
               #endif

               return buffer;
            }
            if (mFraggedRows && spaceEnd > spaceStart)
            {
               *mFraggedRows += static_cast<uint8_t>((spaceEnd - spaceStart) >> IMMIX_LINE_BITS);
            }
         #endif
