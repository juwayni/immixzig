

         if (mMoreHoles)
         {
            #ifdef HXCPP_GC_NURSERY
            spaceFirst = allocBase + mCurrentRange[mCurrentHole].start + sizeof(int);
            spaceOversize = spaceFirst + mCurrentRange[mCurrentHole].length;
            #else
            spaceStart = mCurrentRange[mCurrentHole].start;
            spaceEnd = spaceStart + mCurrentRange[mCurrentHole].length;
            #endif
            mCurrentHole = saturating_add<uint8_t>(mCurrentHole, 1);
            mMoreHoles = mCurrentHole<mCurrentHoles;

         }
         else
         {
            // For opmtimized windows 64 builds, this dummy var technique does not
            //  quite work, since the compiler might recycle one of the earlier stack
            //  slots, and place dummy behind the stack values we are actually trying to
            //  capture.  Moving the dummy into the GetFreeBlock seems to have fixed this.
            //  Not 100% sure this is the best answer, but it is working.
            //volatile int dummy = 1;
            //mBottomOfStack = (int *)&dummy;
            //CAPTURE_REGS;

            BlockDataInfo *info = sGlobalAlloc->GetFreeBlock(allocSize,this);

            allocBase = reinterpret_cast<unsigned char*>(info->mPtr);
            mCurrentRange = info->mRanges;
            allocStartFlags = info->allocStart;
            mCurrentHoles = info->mHoles;
            mFraggedRows = &info->mFraggedRows;
            #ifdef HXCPP_GC_NURSERY
            spaceFirst = allocBase + mCurrentRange->start + sizeof(int);
            spaceOversize = spaceFirst + mCurrentRange->length;
            #else
            spaceStart = mCurrentRange->start;
            spaceEnd = spaceStart + mCurrentRange->length;
            #endif
            mCurrentHole = 1;
            mMoreHoles = mCurrentHole<mCurrentHoles;
         }

         // Other thread may have started collect, in which case we may just
         //  overwritted the 'mMoreHoles' and 'spaceEnd' termination attempt
         if (hx::gPauseForCollect)
         {
            mMoreHoles = 0;
            #ifdef HXCPP_GC_NURSERY
            spaceOversize = 0;
            #else
            spaceEnd = 0;
            #endif
         }
      }
      return 0;
   }

   #ifdef HXCPP_VISIT_ALLOCS
   void Visit(hx::VisitContext *__inCtx)
   {
      #ifdef HXCPP_COMBINE_STRINGS
      if (stringSet)
      {
         __inCtx->visitObject( (hx::Object **)&stringSet);
      }
      #endif
   }
   #endif


   void Mark(hx::MarkContext *__inCtx)
   {
      #ifndef HXCPP_SINGLE_THREADED_APP
      if (!mTopOfStack)
      {
         Reset();
         return;
      }
      #endif

      #ifdef SHOW_MEM_EVENTS
      //int here = 0;
      //GCLOG("=========== Mark Stack ==================== %p ... %p (%p)\n",mBottomOfStack,mTopOfStack,&here);
      #endif

      #ifdef HXCPP_DEBUG
         MarkPushClass("Stack",__inCtx);
         MarkSetMember("Stack",__inCtx);

         #ifdef HXCPP_EXPLICIT_STACK_EXTENT
           hx::MarkConservative( (int *)emscripten_stack_get_current(),(int *)emscripten_stack_get_base(), __inCtx);
         #else
         if (mTopOfStack && mBottomOfStack)
            hx::MarkConservative(mBottomOfStack, mTopOfStack , __inCtx);
         #endif

         #ifdef HXCPP_SCRIPTABLE
            MarkSetMember("ScriptStack",__inCtx);
            hx::MarkConservative((int *)(stack), (int *)(pointer),__inCtx);
         #endif
         MarkSetMember("Registers",__inCtx);
         hx::MarkConservative(CAPTURE_REG_START, CAPTURE_REG_END, __inCtx);


         MarkPopClass(__inCtx);
      #else

         #ifdef HXCPP_EXPLICIT_STACK_EXTENT
           hx::MarkConservative( (int *)emscripten_stack_get_current(), (int *) emscripten_stack_get_base(), __inCtx);
         #else
            if (mTopOfStack && mBottomOfStack)
               hx::MarkConservative(mBottomOfStack, mTopOfStack , __inCtx);
            hx::MarkConservative(CAPTURE_REG_START, CAPTURE_REG_END, __inCtx);
         #endif

         #ifdef HXCPP_SCRIPTABLE
            hx::MarkConservative((int *)(stack), (int *)(pointer),__inCtx);
         #endif
      #endif

      #ifdef HXCPP_COMBINE_STRINGS
         if (stringSet)
            MarkMember( *(hx::Object **)&stringSet, __inCtx);
      #endif

      Reset();

   }

};




inline LocalAllocator *GetLocalAlloc(bool inAllowEmpty=false)
{
   #ifndef HXCPP_SINGLE_THREADED_APP
      LocalAllocator *result = (LocalAllocator *)(hx::ImmixAllocator *)hx::tlsStackContext;
      if (!result && !inAllowEmpty)
         hx::BadImmixAlloc();

      return result;
   #else
      return (LocalAllocator *)hx::gMainThreadContext;
   #endif
}

#ifndef HXCPP_SINGLE_THREADED_APP
void WaitForSafe(LocalAllocator *inAlloc)
{
   inAlloc->WaitForSafe();
}

void ReleaseFromSafe(LocalAllocator *inAlloc)
{
   inAlloc->ReleaseFromSafe();
}
#endif

void MarkLocalAlloc(LocalAllocator *inAlloc,hx::MarkContext *__inCtx)
{
   inAlloc->Mark(__inCtx);
}

void ClearPooledAlloc(LocalAllocator *inAlloc)
{
   inAlloc->Reset();
}

#ifdef HXCPP_VISIT_ALLOCS
void VisitLocalAlloc(LocalAllocator *inAlloc,hx::VisitContext *__inCtx)
{
   inAlloc->Visit(__inCtx);
}
#endif



void CollectFromThisThread(bool inMajor,bool inForceCompact)
{
   LocalAllocator *la = GetLocalAlloc();
   la->SetupStackAndCollect(inMajor,inForceCompact);
}

namespace hx
{

void *ImmixAllocator::CallAlloc(size_t inSize,unsigned int inObjectFlags)
{
   return reinterpret_cast<LocalAllocator *>(this)->CallAlloc(inSize, inObjectFlags);
}



void PauseForCollect()
{
   GetLocalAlloc()->PauseForCollect();
}



void EnterGCFreeZone()
{
   #ifndef HXCPP_SINGLE_THREADED_APP
      LocalAllocator *tla = GetLocalAlloc();
      tla->EnterGCFreeZone();
   #endif
}


bool TryGCFreeZone()
{
   #ifndef HXCPP_SINGLE_THREADED_APP
      LocalAllocator *tla = GetLocalAlloc();
      return tla->TryGCFreeZone();
   #else
      return false;
   #endif
}

bool TryExitGCFreeZone()
{
   #ifndef HXCPP_SINGLE_THREADED_APP
      LocalAllocator *tla = GetLocalAlloc(true);
      if (!tla)
         return 0;
      bool left = tla->TryExitGCFreeZone();
      return left;
   #else
      return false;
   #endif
}


void ExitGCFreeZone()
{
   #ifndef HXCPP_SINGLE_THREADED_APP
      LocalAllocator *tla = GetLocalAlloc();
      tla->ExitGCFreeZone();
   #endif
}

void ExitGCFreeZoneLocked()
{
   #ifndef HXCPP_SINGLE_THREADED_APP
      LocalAllocator *tla = GetLocalAlloc();
      tla->ExitGCFreeZoneLocked();
   #endif
}

void InitAlloc()
{
   for(int i=0;i<IMMIX_LINE_LEN;i++)
      gImmixStartFlag[i] = 1<<( i>>2 ) ;

   hx::CommonInitAlloc();
   sgAllocInit = true;
   sGlobalAlloc = new GlobalAllocator();
   sgFinalizers = new FinalizerList();
   sFinalizerLock = new std::mutex();
   sGCRootLock = new std::mutex();

   gMainThreadContext =  new LocalAllocator();

   tlsStackContext = gMainThreadContext;

   ExitGCFreeZone();

   // Setup main thread ...
   hx::thread::Thread_obj::current();

   gMainThreadContext->onThreadAttach();
}


void GCPrepareMultiThreaded()
{
   #ifdef HXCPP_SINGLE_THREADED_APP
   CriticalGCError("GCPrepareMultiThreaded called with HXCPP_SINGLE_THREADED_APP");
   #endif
}



void SetTopOfStack(int *inTop,bool inForce)
{
   bool threadAttached = false;
   if (inTop)
   {
      if (!sgAllocInit)
         InitAlloc();
      else
      {
         if (tlsStackContext==0)
         {
            GCPrepareMultiThreaded();
            RegisterCurrentThread(inTop);
            threadAttached = true;
         }
      }
   }

   LocalAllocator *tla = (LocalAllocator *)(hx::ImmixAllocator *)tlsStackContext;

   if (tla)
   {
      tla->SetTopOfStack(inTop,inForce);
      if (threadAttached)
         tla->onThreadAttach();
   }
}


void *InternalNew(size_t inSize,bool inIsObject)
{
   // HX_STACK_FRAME("GC", "new", 0, "GC::new", __FILE__, __LINE__, 0)

   #ifdef HXCPP_DEBUG
   if (sgSpamCollects && sgAllocsSinceLastSpam>=sgSpamCollects)
   {
      //GCLOG("InternalNew spam\n");
      CollectFromThisThread(false,false);
   }
   sgAllocsSinceLastSpam++;
   #endif

   if (inSize>=IMMIX_LARGE_OBJ_SIZE)
   {
      void* result{ sGlobalAlloc->AllocLarge(inSize, true) };
      return result;
   }
   else
   {
      LocalAllocator* tla{ GetLocalAlloc() };

      if (inIsObject)
      {
         void* result{ tla->CallAlloc(inSize,IMMIX_ALLOC_IS_CONTAINER) };
         return result;
      }
      else
      {
         #if defined(HXCPP_GC_MOVING) && defined(HXCPP_M64)
         if (inSize<8)
            return tla->CallAlloc(8,0);
         #endif

         void* result{ tla->CallAlloc((inSize + 3) & ~size_t{3},0) };
         return result;
      }
   }
}


// Force global collection - should only be called from 1 thread.
int InternalCollect(bool inMajor,bool inCompact)
{
   if (!sgAllocInit)
       return 0;

   GetLocalAlloc()->SetupStackAndCollect(inMajor, inCompact);

   return sGlobalAlloc->MemUsage();
}

inline unsigned int ObjectSize(void *inData)
{
   unsigned int header = ((unsigned int *)(inData))[-1];

   return (header & IMMIX_ALLOC_ROW_COUNT) ?
            ( (header & IMMIX_ALLOC_SIZE_MASK) >> IMMIX_ALLOC_SIZE_SHIFT) :
             ((unsigned int *)(inData))[-2];
}


unsigned int ObjectSizeSafe(void *inData)
{
   unsigned int header = ((unsigned int *)(inData))[-1];
   if (header & HX_GC_CONST_ALLOC_BIT)
      return 0;

   #ifdef HXCPP_GC_NURSERY
   if (!(header & 0xff000000))
   {
      // Small object
      if (header & 0x00ffffff)
         return header & 0x0000ffff;
      // Large object
   }
   #endif


   return (header & IMMIX_ALLOC_ROW_COUNT) ?
            ( (header & IMMIX_ALLOC_SIZE_MASK) >> IMMIX_ALLOC_SIZE_SHIFT) :
             ((unsigned int *)(inData))[-2];
}

void GCChangeManagedMemory(int inDelta, const char *inWhy)
{
   sGlobalAlloc->onMemoryChange(inDelta, inWhy);
}

void InternalReleaseMem(void *inMem)
{
   if (inMem)
   {
      unsigned int s = ObjectSizeSafe(inMem);
      if (s>=IMMIX_LARGE_OBJ_SIZE)
      {
         //Can release asap
         sGlobalAlloc->FreeLarge(inMem);
      }
   }
}



void *InternalRealloc(size_t inFromSize, void *inData, size_t inSize, bool inExpand)
{
   if (inData==0 || inFromSize==0)
   {
      if (inData)
         InternalReleaseMem(inData);
      return hx::InternalNew(inSize,false);
   }

   // HX_STACK_FRAME("GC", "realloc", 0, "GC::relloc", __FILE__ , __LINE__, 0)

   #ifdef HXCPP_DEBUG
   if (sgSpamCollects && sgAllocsSinceLastSpam>=sgSpamCollects)
   {
      //GCLOG("InternalNew spam\n");
      CollectFromThisThread(false,false);
   }
   sgAllocsSinceLastSpam++;
   #endif

   void* new_data{};
   if (inSize==0)
   {
      new_data = hx::emptyAlloc;
   }
   else if (inSize>=IMMIX_LARGE_OBJ_SIZE)
   {
      new_data = sGlobalAlloc->AllocLarge(inSize, false);
      if (inSize > inFromSize)
         ZERO_MEM(static_cast<char*>(new_data) + inFromSize, inSize - inFromSize);
   }
