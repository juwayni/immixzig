   hx::Object **end = ptrI + inLength;


   #define MARK_PTR_I \
   tmp = *ptrI++; \
   if (tmp) MarkObjectAlloc(tmp,__inCtx);



   #ifdef HX_MULTI_THREAD_MARKING
   if (sAllThreads && inLength>4096)
   {
      hx::Object **dishOffEnd = end - 4096;
      while(ptrI<end)
      {
         // Are the other threads slacking off?
         if ((sRunningThreads != sAllThreads) && ptrI<dishOffEnd)
         {
            ptrI += sGlobalChunks.takeArrayJob(ptrI, end-ptrI);
         }
         else
         {
            MARK_PTR_I;
            MARK_PTR_I;
            MARK_PTR_I;
            MARK_PTR_I;

            MARK_PTR_I;
            MARK_PTR_I;
            MARK_PTR_I;
            MARK_PTR_I;

            MARK_PTR_I;
            MARK_PTR_I;
            MARK_PTR_I;
            MARK_PTR_I;

            MARK_PTR_I;
            MARK_PTR_I;
            MARK_PTR_I;
            MARK_PTR_I;
         }
      }
   }
   else
   #endif
   {
      while(ptrI<end)
      {
         MARK_PTR_I;
         MARK_PTR_I;
         MARK_PTR_I;
         MARK_PTR_I;

         MARK_PTR_I;
         MARK_PTR_I;
         MARK_PTR_I;
         MARK_PTR_I;

         MARK_PTR_I;
         MARK_PTR_I;
         MARK_PTR_I;
         MARK_PTR_I;

         MARK_PTR_I;
         MARK_PTR_I;
         MARK_PTR_I;
         MARK_PTR_I;
      }
   }
}

void MarkStringArray(String *inPtr, int inLength, hx::MarkContext *__inCtx)
{
   #if 0
   if (MAX_GC_THREADS>1 && sAllThreads && inLength>4096)
   {
      int extra = inLength & 0x0f;
      for(int i=0;i<extra;i++)
         if (inPtr[i]) MarkObjectAlloc(inPtr[i],__inCtx);

      hx::Object **ptrI = inPtr + extra;
      hx::Object **end = ptrI + (inLength& ~0x0f);
      hx::Object **dishOffEnd = end - 4096;
      while(ptrI<end)
      {
         // Are the other threads slacking off?
         if ((sRunningThreads != sAllThreads) && ptrI<dishOffEnd)
         {
            ptrI += sGlobalChunks.takeArrayJob(ptrI, end-ptrI);
         }

         if (*ptrI) MarkObjectAlloc(*ptrI,__inCtx); ptrI++;
         if (*ptrI) MarkObjectAlloc(*ptrI,__inCtx); ptrI++;
         if (*ptrI) MarkObjectAlloc(*ptrI,__inCtx); ptrI++;
         if (*ptrI) MarkObjectAlloc(*ptrI,__inCtx); ptrI++;
         if (*ptrI) MarkObjectAlloc(*ptrI,__inCtx); ptrI++;
         if (*ptrI) MarkObjectAlloc(*ptrI,__inCtx); ptrI++;
         if (*ptrI) MarkObjectAlloc(*ptrI,__inCtx); ptrI++;
         if (*ptrI) MarkObjectAlloc(*ptrI,__inCtx); ptrI++;
         if (*ptrI) MarkObjectAlloc(*ptrI,__inCtx); ptrI++;
         if (*ptrI) MarkObjectAlloc(*ptrI,__inCtx); ptrI++;
         if (*ptrI) MarkObjectAlloc(*ptrI,__inCtx); ptrI++;
         if (*ptrI) MarkObjectAlloc(*ptrI,__inCtx); ptrI++;
         if (*ptrI) MarkObjectAlloc(*ptrI,__inCtx); ptrI++;
         if (*ptrI) MarkObjectAlloc(*ptrI,__inCtx); ptrI++;
         if (*ptrI) MarkObjectAlloc(*ptrI,__inCtx); ptrI++;
         if (*ptrI) MarkObjectAlloc(*ptrI,__inCtx); ptrI++;
      }
   }
   else
   #endif
   {
      for(int i=0;i<inLength;i++)
      {
         const char *str = inPtr[i].raw_ptr();
         HX_MARK_STRING(str);
      }
   }
}

#ifdef HXCPP_DEBUG
#define FILE_SCOPE
#else
#define FILE_SCOPE static
#endif

// --- Roots -------------------------------

FILE_SCOPE std::mutex* sGCRootLock = nullptr;
typedef std::unordered_set<hx::Object **> RootSet;
static RootSet sgRootSet;

typedef std::unordered_map<void *,int> OffsetRootSet;
static OffsetRootSet *sgOffsetRootSet=0;

void GCAddRoot(hx::Object **inRoot)
{
   std::lock_guard<std::mutex> lock(*sGCRootLock);
   sgRootSet.insert(inRoot);
}

void GCRemoveRoot(hx::Object **inRoot)
{
   std::lock_guard<std::mutex> lock(*sGCRootLock);
   sgRootSet.erase(inRoot);
}


void GcAddOffsetRoot(void *inRoot, int inOffset)
{
   std::lock_guard<std::mutex> lock(*sGCRootLock);
   if (!sgOffsetRootSet)
      sgOffsetRootSet = new OffsetRootSet();
   (*sgOffsetRootSet)[inRoot] = inOffset;
}

void GcSetOffsetRoot(void *inRoot, int inOffset)
{
   std::lock_guard<std::mutex> lock(*sGCRootLock);
   (*sgOffsetRootSet)[inRoot] = inOffset;
}

void GcRemoveOffsetRoot(void *inRoot)
{
   std::lock_guard<std::mutex> lock(*sGCRootLock);
   OffsetRootSet::iterator r = sgOffsetRootSet->find(inRoot);
   sgOffsetRootSet->erase(r);
}




// --- Finalizers -------------------------------


class WeakRef;
typedef hx::QuickVec<WeakRef *> WeakRefs;

FILE_SCOPE std::mutex *sFinalizerLock = 0;
FILE_SCOPE WeakRefs sWeakRefs;

class WeakRef : public hx::Object
{
public:
   HX_IS_INSTANCE_OF enum { _hx_ClassId = hx::clsIdWeakRef };

   WeakRef(Dynamic inRef)
   {
      mRef = inRef;
      if (mRef.mPtr)
      {
         std::lock_guard<std::mutex> lock(*sFinalizerLock);
         sWeakRefs.push(this);
      }
   }

   // Don't mark our ref !

   #ifdef HXCPP_VISIT_ALLOCS
   void __Visit(hx::VisitContext *__inCtx) HXCPP_OVERRIDE { HX_VISIT_MEMBER(mRef); }
   #endif

   Dynamic mRef;
};



typedef hx::QuickVec<InternalFinalizer *> FinalizerList;

FILE_SCOPE FinalizerList *sgFinalizers = 0;

typedef std::unordered_map<hx::Object *,hx::finalizer> FinalizerMap;
FILE_SCOPE FinalizerMap sFinalizerMap;

typedef void (*HaxeFinalizer)(Dynamic);
typedef std::unordered_map<hx::Object *,HaxeFinalizer> HaxeFinalizerMap;
FILE_SCOPE HaxeFinalizerMap sHaxeFinalizerMap;

hx::QuickVec<int> sFreeObjectIds;
typedef std::unordered_map<hx::Object *,int> ObjectIdMap;
typedef hx::QuickVec<hx::Object *> IdObjectMap;
FILE_SCOPE ObjectIdMap sObjectIdMap;
FILE_SCOPE IdObjectMap sIdObjectMap;

typedef std::unordered_set<hx::Object *> MakeZombieSet;
FILE_SCOPE MakeZombieSet sMakeZombieSet;

typedef hx::QuickVec<hx::Object *> ZombieList;
FILE_SCOPE ZombieList sZombieList;

typedef hx::QuickVec<hx::HashRoot *> WeakHashList;
FILE_SCOPE WeakHashList sWeakHashList;


InternalFinalizer::InternalFinalizer(hx::Object *inObj, finalizer inFinalizer)
{
   mValid = true;
   mObject = inObj;
   mFinalizer = inFinalizer;

   // Ensure this survives generational collect
   std::lock_guard<std::mutex> lock(*gSpecialObjectLock);
   sgFinalizers->push(this);
}

#ifdef HXCPP_VISIT_ALLOCS
void InternalFinalizer::Visit(VisitContext *__inCtx)
{
   HX_VISIT_OBJECT(mObject);
}
#endif

void InternalFinalizer::Detach()
{
   mValid = false;
}

void FindZombies(MarkContext &inContext)
{
   for(MakeZombieSet::iterator i=sMakeZombieSet.begin(); i!=sMakeZombieSet.end(); )
   {
      hx::Object *obj = *i;
      MakeZombieSet::iterator next = i;
      ++next;

      unsigned char mark = ((unsigned char *)obj)[HX_ENDIAN_MARK_ID_BYTE];
      if ( mark!=gByteMarkID )
      {
         sZombieList.push(obj);
         sMakeZombieSet.erase(i);

         // Mark now to prevent secondary zombies...
         inContext.init();
         hx::MarkObjectAlloc(obj , &inContext );
         inContext.processMarkStack();
      }

      i = next;
   }
}

bool IsWeakRefValid(const HX_CHAR *inPtr)
{
   unsigned char mark = ((unsigned char *)inPtr)[HX_ENDIAN_MARK_ID_BYTE];

    // Special case of member closure - check if the 'this' pointer is still alive
   return  mark==gByteMarkID;
}

bool IsWeakRefValid(hx::Object *inPtr)
{
   unsigned char mark = ((unsigned char *)inPtr)[HX_ENDIAN_MARK_ID_BYTE];

    // Special case of member closure - check if the 'this' pointer is still alive
    bool isCurrent = mark==gByteMarkID;
    if ( !isCurrent && inPtr->__GetType()==vtFunction)
    {
        hx::Object *thiz = (hx::Object *)inPtr->__GetHandle();
        if (thiz)
        {
            mark = ((unsigned char *)thiz)[HX_ENDIAN_MARK_ID_BYTE];
            if (mark==gByteMarkID)
            {
               // The object is still alive, so mark the closure and continue
               MarkAlloc(inPtr,0);
               return true;
            }
         }
    }
    return isCurrent;
}

static void someHackyFunc(hx::Object *)
{
}

static void (*hackyFunctionCall)(hx::Object *) = someHackyFunc;

struct Finalizable
{
   union
   {
      _hx_member_finalizer member;
      _hx_alloc_finalizer  alloc;
   };
   void *base;
   bool pin;
   bool isMember;

   Finalizable(hx::Object *inBase, _hx_member_finalizer inMember, bool inPin)
   {
      base = inBase;
      member = inMember;
      pin = inPin;
      isMember = true;
   }

   Finalizable(void *inBase, _hx_alloc_finalizer inAlloc, bool inPin)
   {
      base = inBase;
      alloc = inAlloc;
      pin = inPin;
      isMember = false;
   }
   void run()
   {
      if (isMember)
      {
         hx::Object *object = (hx::Object *)base;
         // I can't tell if it is msvc over-optimizing this code, to I am not
         //  quite calling things right, but this seems to fix it...
         hackyFunctionCall(object);
         (object->*member)();
      }
      else
         alloc( base );
   }
};
typedef hx::QuickVec< Finalizable > FinalizableList;
FILE_SCOPE FinalizableList sFinalizableList;


static double tFinalizers;
static int finalizerCount;
static int localCount;
static int localObjects;
static int localAllocs;
static int rootObjects;
static int rootAllocs;

void RunFinalizers()
{
   finalizerCount = 0;

   FinalizerList &list = *sgFinalizers;
   int idx = 0;
   while(idx<list.size())
   {
      InternalFinalizer *f = list[idx];
      if (!f->mValid)
      {
         list.qerase(idx);
         delete f;
      }
      else if (((unsigned char *)(f->mObject))[HX_ENDIAN_MARK_ID_BYTE] != gByteMarkID)
      {
         if (f->mFinalizer)
         {
            f->mFinalizer(f->mObject);
            finalizerCount++;
         }
         list.qerase(idx);
         delete f;
      }
      else
      {
         idx++;
      }
   }

   idx = 0;
   while(idx<sFinalizableList.size())
   {
      Finalizable &f = sFinalizableList[idx];
      unsigned char mark = ((unsigned char *)f.base)[HX_ENDIAN_MARK_ID_BYTE];
      if ( mark!=gByteMarkID )
      {
         finalizerCount++;
         f.run();
         sFinalizableList.qerase(idx);
      }
      else
         idx++;
   }

   for(FinalizerMap::iterator i=sFinalizerMap.begin(); i!=sFinalizerMap.end(); )
   {
      hx::Object *obj = i->first;
      FinalizerMap::iterator next = i;
      ++next;

      unsigned char mark = ((unsigned char *)obj)[HX_ENDIAN_MARK_ID_BYTE];
      if ( mark!=gByteMarkID )
      {
         finalizerCount++;
         (*i->second)(obj);
         sFinalizerMap.erase(i);
      }

      i = next;
   }


   for(HaxeFinalizerMap::iterator i=sHaxeFinalizerMap.begin(); i!=sHaxeFinalizerMap.end(); )
   {
      hx::Object *obj = i->first;
      HaxeFinalizerMap::iterator next = i;
      ++next;

      unsigned char mark = ((unsigned char *)obj)[HX_ENDIAN_MARK_ID_BYTE];
      if ( mark!=gByteMarkID )
      {
         finalizerCount++;
         (*i->second)(obj);
         sHaxeFinalizerMap.erase(i);
      }

      i = next;
   }
