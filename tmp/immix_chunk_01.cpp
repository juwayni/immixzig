
unsigned int gImmixStartFlag[128];

unsigned int gMarkID = 0x10 << 24;
unsigned int gMarkIDWithContainer = (0x10 << 24) | IMMIX_ALLOC_IS_CONTAINER;

int gPrevByteMarkID = 0x2f;
unsigned int gPrevMarkIdMask = ((~0x2f000000) & 0x30000000) | HX_GC_CONST_ALLOC_BIT;




void ExitGCFreeZoneLocked();


DECLARE_FAST_TLS_DATA(StackContext, tlsStackContext);

#ifdef HXCPP_SCRIPTABLE
extern void scriptMarkStack(hx::MarkContext *);
#endif
}

//#define DEBUG_ALLOC_PTR ((char *)0xb68354)


#ifdef HX_WINDOWS
#define ZERO_MEM(ptr, n) ZeroMemory(ptr,n)
#else
#define ZERO_MEM(ptr, n) memset(ptr,0,n)
#endif


// ---  Internal GC - IMMIX Implementation ------------------------------



// Some inline implementations ...
// Use macros to allow for mark/move



/*
  IMMIX block size, and various masks for converting addresses

*/

#ifdef HXCPP_GC_BIG_BLOCKS
   constexpr uint8_t IMMIX_BLOCK_BITS{ 16 };
   using BlockIdType = unsigned int;
#else
   constexpr uint8_t IMMIX_BLOCK_BITS{ 15 };
   using BlockIdType = unsigned short;
#endif

constexpr uint16_t  IMMIX_BLOCK_SIZE{ 1 << IMMIX_BLOCK_BITS };
constexpr uint16_t  IMMIX_BLOCK_OFFSET_MASK{ IMMIX_BLOCK_SIZE - 1 };
constexpr uint8_t   IMMIX_LINE_COUNT_BITS{ IMMIX_BLOCK_BITS - IMMIX_LINE_BITS };
constexpr uint16_t  IMMIX_LINES{ 1 << IMMIX_LINE_COUNT_BITS };
constexpr uintptr_t IMMIX_BLOCK_BASE_MASK{ ~uintptr_t{ IMMIX_BLOCK_OFFSET_MASK } };
constexpr uint8_t   IMMIX_HEADER_LINES{ IMMIX_LINES >> IMMIX_LINE_BITS };
constexpr uint8_t   IMMIX_USEFUL_LINES{ IMMIX_LINES - IMMIX_HEADER_LINES };
constexpr uint8_t   IMMIX_MAX_ALLOC_GROUPS_SIZE{ 1 << IMMIX_BLOCK_GROUP_BITS };

// Every second line used
constexpr uint8_t   MAX_HOLES{ IMMIX_USEFUL_LINES >> 1 };

/*

 IMMIX Alloc Header - 32 bits


 The header is placed in the 4 bytes before the object pointer, and it is placed there using a uint32.
 When addressed as the uint32, you can use the bitmasks below to extract the various data.

 The "mark id" can conveniently be interpreted as a byte, however the offsets well
  be different depending on whether the system is little endian or big endian.


  Little endian - lsb first

 7   0 15 8 23 16 31 24
 -----------------------
 |    |    |     | MID  | obj start here .....
 -----------------------



  Big endian - msb first

 31 24 23 16 15 8 7   0
 -----------------------
 |MID |    |     |      | obj start here .....
 -----------------------

MID = HX_ENDIAN_MARK_ID_BYTE = is measured from the object pointer
      HX_ENDIAN_MARK_ID_BYTE_HEADER = is measured from the header pointer (4 bytes before object)


*/

#define HX_ENDIAN_MARK_ID_BYTE_HEADER (HX_ENDIAN_MARK_ID_BYTE + 4)


// Used by strings
// HX_GC_CONST_ALLOC_BIT  0x80000000

//#define HX_GC_REMEMBERED          0x40000000
#define IMMIX_ALLOC_MARK_ID         0x3f000000
//#define IMMIX_ALLOC_IS_CONTAINER  0x00800000
//#define IMMIX_ALLOC_IS_PINNED     not used at object level
//#define HX_GX_STRING_EXTENDED     0x00200000
//#define HX_GC_STRING_HASH         0x00100000
// size will shift-right IMMIX_ALLOC_SIZE_SHIFT (6).  Low two bits are 0
#define IMMIX_ALLOC_SIZE_MASK       0x000fff00
#define IMMIX_ALLOC_ROW_COUNT       0x000000ff

#define IMMIX_HEADER_PRESERVE       0x00f00000


#define IMMIX_OBJECT_HAS_MOVED 0x000000fe


// Bigger than this, and they go in the large object pool
#define IMMIX_LARGE_OBJ_SIZE 4000

#ifdef allocString
#undef allocString
#endif


void CriticalGCError(const char *inMessage)
{
   // Can't perfrom normal handling because it needs the GC system
   #ifdef ANDROID
   __android_log_print(ANDROID_LOG_ERROR, "HXCPP", "Critical Error: %s", inMessage);
   #elif defined(HX_WINRT)
      WINRT_LOG("HXCPP Critical Error: %s\n", inMessage);
   #else
   printf("Critical Error: %s\n", inMessage);
   #endif

   DebuggerTrap();
}





enum AllocType { allocNone, allocString, allocObject, allocMarked };

struct BlockDataInfo *gBlockStack = 0;
typedef hx::QuickVec<hx::Object *> ObjectStack;

static std::mutex sThreadPoolLock;

// For threaded marking/block reclaiming
static unsigned int sRunningThreads = 0;
static unsigned int sAllThreads = 0;
static bool sLazyThreads = false;
static bool sThreadPoolInit = false;

enum ThreadPoolJob
{
   tpjNone,
   tpjMark,
   tpjReclaim,
   tpjReclaimFull,
   tpjCountRows,
   tpjAsyncZero,
   tpjAsyncZeroJit,
   tpjGetStats,
   tpjVisitBlocks,
};

int sgThreadCount = 0;
static ThreadPoolJob sgThreadPoolJob = tpjNone;
static bool sgThreadPoolAbort = false;

// Pthreads enters the sleep state while holding a mutex, so it no cost to update
//  the sleeping state and thereby avoid over-signalling the condition
bool                         sThreadSleeping[MAX_GC_THREADS];
std::condition_variable_any* sThreadWake[MAX_GC_THREADS];
bool                         sThreadJobDoneSleeping = false;
std::condition_variable_any* sThreadJobDone;


static inline void SignalThreadPool(std::condition_variable_any* ioSignal, bool sThreadSleeping)
{
    if (sThreadSleeping)
        ioSignal->notify_one();
}

static void wakeThreadLocked(int inThreadId)
{
   sRunningThreads |= (1<<inThreadId);
   sLazyThreads = sRunningThreads != sAllThreads;
   SignalThreadPool(sThreadWake[inThreadId],sThreadSleeping[inThreadId]);
}

union BlockData
{
   // First 2/4 bytes are not needed for row markers (first 2/4 rows are for flags)
   BlockIdType mId;

   // First 2/4 rows contain a byte-flag-per-row
   unsigned char mRowMarked[IMMIX_LINES];
   // Row data as union - don't use first 2/4 rows
   unsigned char mData[IMMIX_LINES * IMMIX_LINE_LEN];

   inline unsigned char* row(size_t r)
   {
       return mData + (r * IMMIX_LINE_LEN);
   }
};

struct BlockDataStats
{
   void clear() { ZERO_MEM(this, sizeof(BlockDataStats)); }
   void add(const BlockDataStats &inOther)
   {
      rowsInUse += inOther.rowsInUse;
      bytesInUse += inOther.bytesInUse;
      emptyBlocks += inOther.emptyBlocks;
      fraggedBlocks += inOther.fraggedBlocks;
      fragScore += inOther.fragScore;
      fraggedRows += inOther.fraggedRows;
   }

   size_t rowsInUse;
   size_t bytesInUse;
   size_t emptyBlocks;
   size_t fragScore;
   size_t fraggedBlocks;
   size_t fraggedRows;
};

static BlockDataStats sThreadBlockDataStats[MAX_GC_THREADS];
#ifdef HXCPP_VISIT_ALLOCS
static hx::VisitContext *sThreadVisitContext = 0;
#endif



namespace hx { void MarkerReleaseWorkerLocked(); }




struct GroupInfo
{
   int  blocks;
   char *alloc;

   bool pinned;
   bool isEmpty;
   int  usedBytes;
   int  usedSpace;

   void clear()
   {
      pinned = false;
      usedBytes = 0;
      usedSpace = 0;
      isEmpty = true;
   }
   int getMoveScore()
   {
      return pinned ?  0 : usedSpace-usedBytes;
   }

};

hx::QuickVec<GroupInfo> gAllocGroups;



struct HoleRange
{
   unsigned short start;
   unsigned short length;
};


hx::QuickVec<struct BlockDataInfo *> *gBlockInfo = 0;
static int gBlockInfoEmptySlots = 0;

#define FRAG_THRESH 14

#define ZEROED_NOT    0
#define ZEROED_THREAD 1
#define ZEROED_AUTO   2

// Align padding based on block offset
#ifdef HXCPP_ALIGN_ALLOC
    #define ALIGN_PADDING(x) (4-(x&4))
#else
    #define ALIGN_PADDING(x) 0
#endif

struct BlockDataInfo
{
   int             mId;
   int             mGroupId;
   BlockData       *mPtr;

   unsigned int allocStart[IMMIX_LINES];

   HoleRange    mRanges[MAX_HOLES];
   uint8_t      mHoles;

   uint8_t      mUsedRows;
   uint16_t     mMaxHoleSize;
   int          mMoveScore;
   uint16_t     mUsedBytes;
   uint8_t      mFraggedRows;
   bool         mPinned;
   unsigned char mZeroed;
   bool         mReclaimed;
   bool         mOwned;
   #ifdef HXCPP_GC_GENERATIONAL
   bool         mHasSurvivor;
   #endif
   std::atomic_int mZeroLock;


   BlockDataInfo(int inGid, BlockData *inData)
   {
      if (gBlockInfoEmptySlots)
      {
         for(int i=0;i<gBlockInfo->size();i++)
            if ( !(*gBlockInfo)[i] )
            {
               gBlockInfoEmptySlots--;
               mId = i;
               (*gBlockInfo)[i] = this;
               break;
            }
      }
      else
      {
         if (gBlockInfo==0)
            gBlockInfo = new hx::QuickVec<BlockDataInfo *>;
         mId = gBlockInfo->size();
         gBlockInfo->push( this );
      }


      mZeroLock = 0;
      mOwned = false;
      mGroupId = inGid;
      mPtr     = inData;
      inData->mId = mId;
      #ifdef SHOW_MEM_EVENTS
      //GCLOG("  create block %d : %p -> %p\n",  mId, this, mPtr );
      #endif
      clear();
   }

   void clear()
   {
      mUsedRows = 0;
      mUsedBytes = 0;
      mFraggedRows = 0;
      mPinned = false;
      ZERO_MEM(allocStart,sizeof(int)*IMMIX_LINES);
      ZERO_MEM(mPtr->mRowMarked+IMMIX_HEADER_LINES, IMMIX_USEFUL_LINES);
      mRanges[0].start = IMMIX_HEADER_LINES << IMMIX_LINE_BITS;
      mRanges[0].length = IMMIX_USEFUL_LINES << IMMIX_LINE_BITS;
      mMaxHoleSize = mRanges[0].length;
      mMoveScore = 0;
      mHoles = 1;
      mZeroed = ZEROED_NOT;
      mReclaimed = true;
      mZeroLock = 0;
      mOwned = false;
   }

   void makeFull()
   {
      mUsedRows = IMMIX_USEFUL_LINES;
      mUsedBytes = uint16_t{ mUsedRows } << IMMIX_LINE_BITS;
      mFraggedRows = 0;
      memset(mPtr->mRowMarked+IMMIX_HEADER_LINES, 1,IMMIX_USEFUL_LINES);
      mRanges[0].start = 0;
      mRanges[0].length = 0;
      mMaxHoleSize = 0;
      mMoveScore = 0;
      mHoles = 0;
      mZeroed = ZEROED_AUTO;
      mReclaimed = true;
      mZeroLock = 0;
      mOwned = false;
   }


   void clearBlockMarks()
   {
      mPinned = false;
      #ifdef HXCPP_GC_GENERATIONAL
      mHasSurvivor = false;
      #endif
   }

   void clearRowMarks()
   {
      clearBlockMarks();
      ZERO_MEM((char *)mPtr+IMMIX_HEADER_LINES, IMMIX_USEFUL_LINES);
   }

   inline int GetFreeRows() const { return (IMMIX_USEFUL_LINES - mUsedRows); }
   inline int GetFreeData() const { return (IMMIX_USEFUL_LINES - mUsedRows)<<IMMIX_LINE_BITS; }


   bool zeroAndUnlock()
   {
      bool doZero = !mZeroed;
      if (doZero)
      {
         if (!mReclaimed)
            reclaim<false>(0);

         for (uint8_t i{ 0 }; i < mHoles; i++)
             ZERO_MEM( (char *)mPtr+mRanges[i].start, mRanges[i].length );
         mZeroed = ZEROED_THREAD;
      }
      mZeroLock = 0;
      return doZero;
   }

   bool tryZero()
   {
      if (mZeroed)
         return false;

      if (atomic_compare_exchange_strong_discard(&mZeroLock, 0, 1))
         return zeroAndUnlock();

      return false;
   }

   void destroy()
   {
      #ifdef SHOW_MEM_EVENTS_VERBOSE
      GCLOG("  release block %d : %p\n",  mId, this );
      #endif
      (*gBlockInfo)[mId] = 0;
      gBlockInfoEmptySlots++;
      delete this;
   }
