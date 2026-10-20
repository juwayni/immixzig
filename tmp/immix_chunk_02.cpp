   bool isEmpty() const { return mUsedRows == 0; }
   int getUsedRows() const { return mUsedRows; }

   void getStats(BlockDataStats &outStats)
   {
      outStats.rowsInUse += mUsedRows;
      outStats.bytesInUse += mUsedBytes;
      outStats.fraggedRows += mFraggedRows;
      //outStats.bytesInUse += 0;
      outStats.fragScore += mPinned ? (mMoveScore>0?1:0) : mMoveScore;

      if (mUsedRows==0)
         outStats.emptyBlocks++;
      if (mMoveScore> FRAG_THRESH )
         outStats.fraggedBlocks++;
   }

   #ifdef HX_GC_VERIFY_ALLOC_START
   void verifyAllocStart()
   {
      unsigned char *rowMarked = mPtr->mRowMarked;

      for(int r = IMMIX_HEADER_LINES; r<IMMIX_LINES; r++)
      {
         if (!rowMarked[r] && allocStart[r])
         {
            printf("allocStart set without marking\n");
            DebuggerTrap();
         }
      }
   }
   #endif

   void countRows(BlockDataStats &outStats)
   {
      unsigned char *rowMarked = mPtr->mRowMarked;
      unsigned int *rowTotals = ((unsigned int *)rowMarked) + 1;

      // TODO - sse/neon
      #ifdef HXCPP_GC_BIG_BLOCKS
      unsigned int total = 0;
      #else
      unsigned int total = rowMarked[2] + rowMarked[3];
      #endif

      total +=
       rowTotals[0]  + rowTotals[1]  + rowTotals[2]  + rowTotals[3]  + rowTotals[4] +
       rowTotals[5]  + rowTotals[6]  + rowTotals[7]  + rowTotals[8]  + rowTotals[9] +
       rowTotals[10] + rowTotals[11] + rowTotals[12] + rowTotals[13] + rowTotals[14] +
       rowTotals[15] + rowTotals[16] + rowTotals[17] + rowTotals[18] + rowTotals[19] +
       rowTotals[20] + rowTotals[21] + rowTotals[22] + rowTotals[23] + rowTotals[24] +
       rowTotals[25] + rowTotals[26] + rowTotals[27] + rowTotals[28] + rowTotals[29] +
       rowTotals[30] + rowTotals[31] + rowTotals[32] + rowTotals[33] + rowTotals[34] +
       rowTotals[35] + rowTotals[36] + rowTotals[37] + rowTotals[38] + rowTotals[39] +
       rowTotals[40] + rowTotals[41] + rowTotals[42] + rowTotals[43] + rowTotals[44] +
       rowTotals[45] + rowTotals[46] + rowTotals[47] + rowTotals[48] + rowTotals[49] +
       rowTotals[50] + rowTotals[51] + rowTotals[52] + rowTotals[53] + rowTotals[54] +
       rowTotals[55] + rowTotals[56] + rowTotals[57] + rowTotals[58] + rowTotals[59] +
       rowTotals[60] + rowTotals[61] + rowTotals[62];


      #ifdef HXCPP_GC_BIG_BLOCKS
      rowTotals += 63;
      total +=
       rowTotals[0]  + rowTotals[1]  + rowTotals[2]  + rowTotals[3]  + rowTotals[4] +
       rowTotals[5]  + rowTotals[6]  + rowTotals[7]  + rowTotals[8]  + rowTotals[9] +
       rowTotals[10] + rowTotals[11] + rowTotals[12] + rowTotals[13] + rowTotals[14] +
       rowTotals[15] + rowTotals[16] + rowTotals[17] + rowTotals[18] + rowTotals[19] +
       rowTotals[20] + rowTotals[21] + rowTotals[22] + rowTotals[23] + rowTotals[24] +
       rowTotals[25] + rowTotals[26] + rowTotals[27] + rowTotals[28] + rowTotals[29] +
       rowTotals[30] + rowTotals[31] + rowTotals[32] + rowTotals[33] + rowTotals[34] +
       rowTotals[35] + rowTotals[36] + rowTotals[37] + rowTotals[38] + rowTotals[39] +
       rowTotals[40] + rowTotals[41] + rowTotals[42] + rowTotals[43] + rowTotals[44] +
       rowTotals[45] + rowTotals[46] + rowTotals[47] + rowTotals[48] + rowTotals[49] +
       rowTotals[50] + rowTotals[51] + rowTotals[52] + rowTotals[53] + rowTotals[54] +
       rowTotals[55] + rowTotals[56] + rowTotals[57] + rowTotals[58] + rowTotals[59] +
       rowTotals[60] + rowTotals[61] + rowTotals[62] + rowTotals[63];

      #endif

      mUsedRows = (total & 0xff) + ((total>>8) & 0xff) + ((total>>16)&0xff) + ((total>>24)&0xff);
      mUsedBytes = uint16_t{ mUsedRows } << IMMIX_LINE_BITS;

      mZeroLock = 0;
      mOwned = false;
      outStats.rowsInUse += mUsedRows;
      outStats.bytesInUse += mUsedBytes;
      outStats.fraggedRows += mFraggedRows;
      mFraggedRows = 0;
      mHoles = 0;

      if (mUsedRows==IMMIX_USEFUL_LINES)
      {
         // All rows used - write the block off
         mMoveScore = 0;
         mZeroed = ZEROED_AUTO;
         mReclaimed = true;
      }
      else
      {
         mZeroed = ZEROED_NOT;
         mReclaimed = false;
      }

      uint16_t left{ static_cast<uint16_t>((IMMIX_USEFUL_LINES - mUsedRows) << IMMIX_LINE_BITS) };
      if (left<mMaxHoleSize)
         mMaxHoleSize = left;
   }

   template<bool FULL>
   void reclaim(BlockDataStats *outStats)
   {
      HoleRange *ranges = mRanges;
      ranges[0].length = 0;
      mZeroed = ZEROED_NOT;
      int usedBytes = 0;

      unsigned char *rowMarked = mPtr->mRowMarked;

      int r = IMMIX_HEADER_LINES;
      // Count unused rows ....

      // start on 4-byte boundary...
      #ifdef HXCPP_ALIGN_ALLOC
      while(r<4 && rowMarked[r]==0)
         r++;
      if (!rowMarked[r])
      #endif
      {
         while(r<(IMMIX_LINES-4) && *(int *)(rowMarked+r)==0 )
            r += 4;
         while(r<(IMMIX_LINES) && rowMarked[r]==0)
            r++;
      }

      if (r==IMMIX_LINES)
      {
         ranges[0].start  = IMMIX_HEADER_LINES<<IMMIX_LINE_BITS;
         ranges[0].length = (IMMIX_USEFUL_LINES)<<IMMIX_LINE_BITS;
         mMaxHoleSize = static_cast<uint16_t>(IMMIX_USEFUL_LINES << IMMIX_LINE_BITS);
         mUsedRows = 0;
         mHoles = 1;
         mMoveScore = 0;
         ZERO_MEM(allocStart+IMMIX_HEADER_LINES, IMMIX_USEFUL_LINES*sizeof(int));
      }
      else
      {
         int hole = 0;

         if (r>IMMIX_HEADER_LINES)
         {
            ranges[hole].start  = IMMIX_HEADER_LINES;
            ranges[hole].length = r-IMMIX_HEADER_LINES;
            hole++;
         }

         while(r<IMMIX_LINES)
         {
            if (rowMarked[r])
            {
               if (FULL)
               {
                  unsigned int &starts = allocStart[r];
                  if (starts)
                  {
                     unsigned int* headerPtr{ reinterpret_cast<unsigned int*>(mPtr->row(static_cast<size_t>(r))) };
                     #define CHECK_FLAG(i,byteMask) \
                     { \
                        unsigned int mask = 1<<i; \
                        if ( starts & mask ) \
                        { \
                           unsigned int header = headerPtr[i]; \
                           if ( (header & IMMIX_ALLOC_MARK_ID) != hx::gMarkID ) \
                           { \
                              starts ^= mask; \
                              if (!(starts & byteMask)) \
                                 break; \
                           } \
                           else \
                              usedBytes += sizeof(int) + ((header & IMMIX_ALLOC_SIZE_MASK) >> IMMIX_ALLOC_SIZE_SHIFT); \
                        } \
                     }

                     if (starts & 0x000000ff)
                        for(int i=0;i<8;i++)
                           CHECK_FLAG(i,0x000000ff);

                     if (starts & 0x0000ff00)
                        for(int i=8;i<16;i++)
                           CHECK_FLAG(i,0x0000ff00);

                     if (starts & 0x00ff0000)
                        for(int i=16;i<24;i++)
                           CHECK_FLAG(i,0x00ff0000);

                     if (starts & 0xff000000)
                        for(int i=24;i<32;i++)
                           CHECK_FLAG(i,0xff000000);
                  }
               }
               r++;
            }
            else
            {
               int start = r;
               ranges[hole].start = start;

               #ifdef HXCPP_ALIGN_ALLOC
               int alignR = (r+3) & ~3;
               while(r<alignR && rowMarked[r]==0)
                  r++;
               if (!rowMarked[r])
               #endif
               {
                  while(r<(IMMIX_LINES-4) && *(int *)(rowMarked+r)==0 )
                     r += 4;
                  while(r<(IMMIX_LINES) && rowMarked[r]==0)
                     r++;
               }
               ranges[hole].length = r-start;
               hole++;
            }
         }


         int freeLines = 0;
         // Convert hole rows to hole bytes...
         mMaxHoleSize = 0;
         for(int h=0;h<hole;h++)
         {
            uint16_t s{ ranges->start };
            uint16_t l{ ranges->length };
            freeLines += l;
            ZERO_MEM(allocStart+s, l*sizeof(int));

            uint16_t sBytes{ static_cast<uint16_t>(s << IMMIX_LINE_BITS) };
            ranges->start = sBytes;

            uint16_t lBytes{ static_cast<uint16_t>(l << IMMIX_LINE_BITS) };
            ranges->length = lBytes;

            if (lBytes>mMaxHoleSize)
              mMaxHoleSize = lBytes;

            ranges++;
         }
         mUsedRows = IMMIX_USEFUL_LINES - freeLines;
         mHoles = hole;
      }

      mUsedBytes = FULL ? usedBytes : uint16_t{ mUsedRows } << IMMIX_LINE_BITS;
      mMoveScore = calcFragScore();
      mReclaimed = true;

      if (outStats)
      {
         outStats->rowsInUse += mUsedRows;
         outStats->bytesInUse += mUsedBytes;
         outStats->fragScore += mMoveScore;
         outStats->fraggedRows += mFraggedRows;

         if (mUsedRows==0)
            outStats->emptyBlocks++;
         if (mMoveScore> FRAG_THRESH )
            outStats->fraggedBlocks++;
      }

      mFraggedRows = 0;
   }

   int calcFragScore() const
   {
      return mPinned ? 0 : saturating_sub<uint8_t>(mHoles, 3) + 8 * (mUsedRows<<IMMIX_LINE_BITS) / (mUsedBytes+IMMIX_LINE_LEN);
   }


   // When known to be an actual object start...
   AllocType GetAllocTypeChecked(uintptr_t inOffset, bool allowPrevious) const
   {
      unsigned char time{ mPtr->mData[inOffset + HX_ENDIAN_MARK_ID_BYTE_HEADER] };
      if (((time + 1) & MARK_BYTE_MASK) != (gByteMarkID & MARK_BYTE_MASK))
      {
         // Object is either out-of-date, or already marked....
         return time==gByteMarkID ? allocMarked : allocNone;
      }

      if (!allowPrevious)
         return allocNone;

      if (*reinterpret_cast<unsigned int *>(mPtr->mData + inOffset) & IMMIX_ALLOC_IS_CONTAINER)
      {
         // See if object::new has been called, but not constructed yet ...
         void** vtable{ reinterpret_cast<void**>(mPtr->mData + inOffset + sizeof(int)) };
         if (nullptr == vtable[0])
         {
            // GCLOG("Partially constructed object.");
            return allocString;
         }
         return allocObject;
      }

      return allocString;
   }

   #ifdef HXCPP_GC_NURSERY
   AllocType GetEnclosingNurseryType(uintptr_t inOffset, uintptr_t* outPtr) const
   {
      // The block did not get used in the previous cycle, so allocStart is invalid and
      //  no new objects should be in here
      if (!mZeroed)
         return allocNone;
      // For the nursery(generational) case, the allocStart markers are not set
      // So trace tne new object links through the new allocation holes
      for (uint8_t h{ 0 }; h < mHoles; h++)
      {
         size_t scan{ mRanges[h].start };
         if (inOffset<scan)
            break;

         size_t last{ scan + mRanges[h].length };
         if (inOffset<last)
         {
            #ifdef HXCPP_ALIGN_ALLOC
            // Make sure header scan is odd-int aligned so the following object will be even-int
            // aligned
            if (!(scan & 0x4))
               scan += 4;
            #endif

            // Found hole that the object was possibly allocated in
            while(scan<=inOffset)
            {
               // Trace along the hole...
               unsigned int header{ *reinterpret_cast<unsigned int*>(mPtr->mData + scan) };
               unsigned int size{
                  !(header & 0xff000000)
                     ? (header & 0x0000ffff)
                     : ((header & IMMIX_ALLOC_SIZE_MASK) >> IMMIX_ALLOC_SIZE_SHIFT) };

               size_t end{ scan + size + sizeof(int) };
               if (!size || end > last)
                  return allocNone;

               if (inOffset>=scan && inOffset<end)
               {
                  if (inOffset > scan + sgCheckInternalOffset)
                     return allocNone;

                  if (nullptr != outPtr)
                  {
                     *outPtr = reinterpret_cast<uintptr_t>(mPtr->mData + scan + sizeof(int));
                  }

                  if (header & IMMIX_ALLOC_IS_CONTAINER)
                  {
                     // See if object::new has been called, but not constructed yet ...
                     void **vtable{ reinterpret_cast<void **>(mPtr->mData + scan + sizeof(int)) };
                     if (vtable[0]==0)
                     {
                        // GCLOG("Partially constructed object.");
                        return allocString;
                     }
                     return allocObject;
                  }
                  return allocString;
               }
               scan = end;
               #ifdef HXCPP_ALIGN_ALLOC
               // Make sure scan is odd-int aligned so the following object will be even-int
               // aligned
               if (!(scan & 0x4))
                  scan += 4;
               #endif
            }
            break;
         }
      }
      return allocNone;
}
   #endif

   AllocType GetAllocType(uintptr_t inOffset, bool inAllowPrevious) const
   {
      // Row that the header would be on
      uintptr_t r{ inOffset >> IMMIX_LINE_BITS };

      // Out of bounds - can't be a new object start
      if (r < IMMIX_HEADER_LINES || r >= IMMIX_LINES)
      {
         return allocNone;
      }

      // Does a live object start on this row
      if ( !( allocStart[r] & hx::gImmixStartFlag[inOffset &127]) )
      {
         #ifdef HXCPP_GC_NURSERY
         return GetEnclosingNurseryType(inOffset, nullptr);
         #endif
         //Not a actual start...
         return allocNone;
      }

      return GetAllocTypeChecked(inOffset,inAllowPrevious);
   }

   AllocType GetEnclosingAllocType(uintptr_t inOffset, uintptr_t* outPtr, bool inAllowPrevious) const
   {
      for (uintptr_t dx{ 0 }; dx <= sgCheckInternalOffset; dx += 4)
      {
         uintptr_t blockOffset{ inOffset - dx };
         if (blockOffset >= 0)
         {
            uintptr_t r{ blockOffset >> IMMIX_LINE_BITS };
            if (r >= IMMIX_HEADER_LINES && r < IMMIX_LINES)
            {
               // Normal, good alloc
               unsigned int rowPos{ hx::gImmixStartFlag[blockOffset & 127] };
               if ( allocStart[r] & rowPos )
               {
                  // Found last valid object - is it big enough?
                  unsigned int header{ *reinterpret_cast<unsigned int*>(reinterpret_cast<char*>(mPtr) + blockOffset) };
                  size_t size{ (header & IMMIX_ALLOC_SIZE_MASK) >> IMMIX_ALLOC_SIZE_SHIFT };
                  // Valid object not big enough...
                  if (blockOffset + size + sizeof(int) <= inOffset )
                     break;

                  // If the object is old, it could be the tail end of an old row that
                  //  thinks is is covering this row, but it is not because this row is reused.
                  // So we can say there is no other object that could be covering
                  //  this spot, but not that it is not a nursery object.
                  AllocType result{ GetAllocTypeChecked(blockOffset,inAllowPrevious) };
                  if (result!=allocNone)
                  {
                     if (nullptr != outPtr)
                     {
                        *outPtr = reinterpret_cast<uintptr_t>(mPtr->mData + blockOffset + sizeof(int));
                     }

                     return result;
                  }
                  break;
               }
            }
          }
      }

      #ifdef HXCPP_GC_NURSERY
      return GetEnclosingNurseryType(inOffset,outPtr);
      #endif
      // Not a actual start...
