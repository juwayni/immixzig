
public:
    enum { StackSize = 8192 };

    bool isGenerational;

    MarkContext(int inThreadId = -1)
    {
       #ifdef HXCPP_DEBUG
       mInfo = new MarkInfo[StackSize];
       mPos = 0;
       #endif
       mThreadId = inThreadId;
       marking = sGlobalChunks.alloc();

       isGenerational = false;
    }
    ~MarkContext()
    {
       if (marking) sGlobalChunks.free(marking);
       #ifdef HXCPP_DEBUG
       delete [] mInfo;
       #endif
       // TODO: Free slabs
    }


    #ifdef HXCPP_DEBUG
    void PushClass(const char *inClass)
    {
       if (mPos<StackSize-1)
       {
          mInfo[mPos].mClass = inClass;
          mInfo[mPos].mMember = 0;
       }
       mPos++;
    }


    void SetMember(const char *inMember)
    {
       // Should not happen...
       if (mPos==0)
          return;
       if (mPos<StackSize)
          mInfo[mPos-1].mMember = inMember ? inMember : "Unknown";
    }
    void PopClass() { mPos--; }

    void Trace()
    {
       int n = mPos < StackSize ? mPos : StackSize;
       #ifdef ANDROID
       __android_log_print(ANDROID_LOG_ERROR, "trace", "Class referenced from");
       #else
       printf("Class referenced from:\n");
       #endif

       for(int i=0;i<n;i++)
          #ifdef ANDROID
          __android_log_print(ANDROID_LOG_INFO, "trace", "%s.%s",  mInfo[i].mClass, mInfo[i].mMember );
          #else
          printf("%s.%s\n",  mInfo[i].mClass, mInfo[i].mMember );
          #endif

       if (mPos>=StackSize)
       {
          #ifdef ANDROID
          __android_log_print(ANDROID_LOG_INFO, "trace", "... + deeper");
          #else
          printf("... + deeper\n");
          #endif
       }
    }
    #endif


    void pushObj(hx::Object *inObject)
    {
       if (marking->count < MarkChunk::SIZE)
       {
          marking->push(inObject);
       }
       else
       {
          marking = sGlobalChunks.pushJob(marking,true);
          marking->push(inObject);
       }
    }

    void init()
    {
       if (!marking)
          marking = sGlobalChunks.getInitJob();
    }

    void releaseJobs()
    {
       if (marking && marking->count)
       {
          sGlobalChunks.pushJob(marking,false);
       }
       else if (marking)
       {
          sGlobalChunks.release(marking);
       }
       marking = 0;
    }

    void processMarkStack()
    {
       while(true)
       {
          if (!marking || !marking->count)
          {
             #ifdef HX_MULTI_THREAD_MARKING
             if (sgThreadPoolAbort)
             {
                releaseJobs();
                return;
             }
             #endif

             marking = sGlobalChunks.popJobOrFinish(marking,mThreadId);
             if (!marking)
                break;

             if (marking->count==MarkChunk::OBJ_ARRAY_JOB)
             {
                int n = marking->arrayElements;
                hx::Object **elems = marking->arrayBase;
                marking->count = 0;
                MarkObjectArray(elems, n, this);
                continue;
             }
          }

          while(marking)
          {
             hx::Object *obj = marking->pop();
             if (obj)
             {
                obj->__Mark(this);
                #ifdef HX_MULTI_THREAD_MARKING
                // Load balance
                if (sLazyThreads && marking->count>32)
                {
                   MarkChunk *c = sGlobalChunks.alloc();
                   marking->count -= 16;
                   c->count = 16;
                   memcpy( c->stack, marking->stack + marking->count, 16*sizeof(hx::Object *));
                   sGlobalChunks.pushJob(c,false);
                }
                #ifdef PROFILE_THREAD_USAGE
                sThreadMarkCount[mThreadId]++;
                #endif
                #endif
             }
             else
                break;
          }
       }
    }
};


/*
void MarkerReleaseWorkerLocked( )
{
   //printf("Release...\n");
   for(int i=0;i<sAllThreads;i++)
   {
      if ( ! (sRunningThreads & (1<<i) ) )
      {
         //printf("Wake %d\n",i);
         sRunningThreads |= (1<<i);
         sThreadWake[i]->Set();
         return;
      }
   }
}
*/



#ifdef HXCPP_DEBUG
static bool breakOnce = 1;
void MarkSetMember(const char *inName,hx::MarkContext *__inCtx)
{
   if (gCollectTrace)
      __inCtx->SetMember(inName);
}

void MarkPushClass(const char *inName,hx::MarkContext *__inCtx)
{
   if (gCollectTrace)
      __inCtx->PushClass(inName);
}

void MarkPopClass(hx::MarkContext *__inCtx)
{
   if (gCollectTrace)
      __inCtx->PopClass();
}
#endif




struct AutoMarkPush
{
   hx::MarkContext *mCtx;
   AutoMarkPush(hx::MarkContext *ctx, const char *cls, const char *member)
   {
      #ifdef HXCPP_DEBUG
      mCtx = ctx;
      MarkPushClass(cls,mCtx);
      MarkSetMember(member,mCtx);
      #endif
   }
   ~AutoMarkPush()
   {
      #ifdef HXCPP_DEBUG
      MarkPopClass(mCtx);
      #endif
   }
};






void MarkAllocUnchecked(void *inPtr,hx::MarkContext *__inCtx)
{
   #ifdef PROFILE_COLLECT
   sAllocMarks++;
   #endif

   size_t ptr_i = ((size_t)inPtr)-sizeof(int);
   unsigned int flags =  *((unsigned int *)ptr_i);

   #ifdef HXCPP_GC_NURSERY
   if (!(flags & 0xff000000))
   {
      #ifdef HX_GC_VERIFY_GENERATIONAL
      if (sGcVerifyGenerational)
      {
         printf("Nursery alloc escaped generational collection %p\n", inPtr);
         DebuggerTrap();
      }
      #endif

      int size = flags & 0xffff;
      // Size will be 0 for large allocs -> no need to mark block
      if (size)
      {
         int start = (int)(ptr_i & IMMIX_BLOCK_OFFSET_MASK);
         int startRow = start>>IMMIX_LINE_BITS;
         int blockId = *(BlockIdType *)(ptr_i & IMMIX_BLOCK_BASE_MASK);
         BlockDataInfo *info = (*gBlockInfo)[blockId];

         int endRow = (start + size + sizeof(int) + IMMIX_LINE_LEN-1)>>IMMIX_LINE_BITS;
         *(unsigned int *)ptr_i = flags = (flags & IMMIX_HEADER_PRESERVE) |
                                          (endRow -startRow) |
                                          (size<<IMMIX_ALLOC_SIZE_SHIFT) |
                                          gMarkID;

         unsigned int *pos = info->allocStart + startRow;
         unsigned int val = *pos;
         while(_hx_atomic_compare_exchange((volatile int *)pos, val,val|gImmixStartFlag[start&127]) != val)
            val = *pos;

         #ifdef HXCPP_GC_GENERATIONAL
         info->mHasSurvivor = true;
         #endif
      }
      else
      {
         // Large nursury object
         ((unsigned char *)inPtr)[HX_ENDIAN_MARK_ID_BYTE] = gByteMarkID;
      }
   }
   else
   #endif
   {
      #ifdef HX_GC_VERIFY_GENERATIONAL
      if (sGcVerifyGenerational && ((unsigned char *)inPtr)[HX_ENDIAN_MARK_ID_BYTE] != gPrevByteMarkID)
      {
         printf("Alloc missed int generational collection %p\n", inPtr);
         DebuggerTrap();
      }
      #endif
      ((unsigned char *)inPtr)[HX_ENDIAN_MARK_ID_BYTE] = gByteMarkID;
   }

   int rows = flags & IMMIX_ALLOC_ROW_COUNT;
   if (rows)
   {
      #if HXCPP_GC_DEBUG_LEVEL>0
      if ( ((ptr_i & IMMIX_BLOCK_OFFSET_MASK)>>IMMIX_LINE_BITS) + rows > IMMIX_LINES) DebuggerTrap();
      #endif

      char *block = (char *)(ptr_i & IMMIX_BLOCK_BASE_MASK);
      char *rowMark = block + ((ptr_i & IMMIX_BLOCK_OFFSET_MASK)>>IMMIX_LINE_BITS);
      *rowMark = 1;
      if (rows>1)
      {
         rowMark[1] = 1;
         if (rows>2)
         {
            rowMark[2] = 1;
            if (rows>3)
            {
               rowMark[3] = 1;
               for(int r=4; r<rows; r++)
                  rowMark[r]=1;
            }
         }
      }

   // MARK_ROWS_UNCHECKED_END
   }
}

void MarkObjectAllocUnchecked(hx::Object *inPtr,hx::MarkContext *__inCtx)
{
   size_t ptr_i = ((size_t)inPtr)-sizeof(int);
   unsigned int flags =  *((unsigned int *)ptr_i);
   #ifdef HXCPP_GC_NURSERY
   if (!(flags & 0xff000000))
   {
      #if defined(HX_GC_VERIFY_GENERATIONAL)
         if (sGcVerifyGenerational)
         {
            printf("Nursery object escaped generational collection %p\n", inPtr);
            DebuggerTrap();
         }
      #endif


      int size = flags & 0xffff;
      int start = (int)(ptr_i & IMMIX_BLOCK_OFFSET_MASK);
      int startRow = start>>IMMIX_LINE_BITS;
      int blockId = *(BlockIdType *)(ptr_i & IMMIX_BLOCK_BASE_MASK);
      BlockDataInfo *info = (*gBlockInfo)[blockId];

      int endRow = (start + size + sizeof(int) + IMMIX_LINE_LEN-1)>>IMMIX_LINE_BITS;
      *(unsigned int *)ptr_i = flags = (flags & IMMIX_HEADER_PRESERVE) |
                                       (endRow -startRow) |
                                       (size<<IMMIX_ALLOC_SIZE_SHIFT) |
                                       gMarkID;

      unsigned int *pos = info->allocStart + startRow;
      unsigned int val = *pos;
      while(_hx_atomic_compare_exchange( (volatile int *)pos, val, val|gImmixStartFlag[start&127]) != val)
         val = *pos;
      #ifdef HXCPP_GC_GENERATIONAL
      info->mHasSurvivor = true;
      #endif
   }
   else
   #endif
      ((unsigned char *)inPtr)[HX_ENDIAN_MARK_ID_BYTE] = gByteMarkID;

   int rows = flags & IMMIX_ALLOC_ROW_COUNT;
   if (rows)
   {
      char *block = (char *)(ptr_i & IMMIX_BLOCK_BASE_MASK);
      char *rowMark = block + ((ptr_i & IMMIX_BLOCK_OFFSET_MASK)>>IMMIX_LINE_BITS);
      #if HXCPP_GC_DEBUG_LEVEL>0
      if ( ((ptr_i & IMMIX_BLOCK_OFFSET_MASK)>>IMMIX_LINE_BITS) + rows > IMMIX_LINES) DebuggerTrap();
      #endif

      *rowMark = 1;
      if (rows>1)
      {
         rowMark[1] = 1;
         if (rows>2)
         {
            rowMark[2] = 1;
            if (rows>3)
            {
               rowMark[3] = 1;
               for(int r=4; r<rows; r++)
                  rowMark[r]=1;
            }
         }
      }

      if (flags & IMMIX_ALLOC_IS_CONTAINER)
      {
         #ifdef PROFILE_COLLECT
         sObjectMarks++;
         #endif

         #ifdef HXCPP_DEBUG
            if (gCollectTrace && gCollectTrace==inPtr->__GetClass().GetPtr())
            {
               gCollectTraceCount++;
               if (gCollectTraceDoPrint)
                   __inCtx->Trace();
            }

            // Recursive mark so stack stays intact..
            #if (HXCPP_GC_DEBUG_LEVEL>0)
            inPtr->__Mark(__inCtx);
            #else
            if (gCollectTrace)
               inPtr->__Mark(__inCtx);
            else
               __inCtx->pushObj(inPtr);
            #endif
         #else // Not debug

            // There is a slight performance gain by calling recursively, but you
            //   run the risk of stack overflow.  Also, a parallel mark algorithm could be
            //   done when the marking is stack based.
            //inPtr->__Mark(__inCtx);
            #if (HXCPP_GC_DEBUG_LEVEL>0)
            inPtr->__Mark(__inCtx);
            #else
            __inCtx->pushObj(inPtr);
            #endif
        #endif
      }
      else
      {
         #ifdef PROFILE_COLLECT
         sAllocMarks++;
         #endif
      }

   // MARK_ROWS_UNCHECKED_END
   }
}


void MarkObjectArray(hx::Object **inPtr, int inLength, hx::MarkContext *__inCtx)
{
   hx::Object *tmp;

   int extra = inLength & 0x0f;
   for(int i=0;i<extra;i++)
      if (inPtr[i]) MarkObjectAlloc(inPtr[i],__inCtx);
   if (inLength==extra)
      return;

   inLength -= extra;
   hx::Object **ptrI = inPtr + extra;
