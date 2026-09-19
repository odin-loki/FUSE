#ifndef _STRINGTABLE_H_
#define _STRINGTABLE_H_

#ifndef _PLATFORM_H_
#include "platform/platform.h"
#endif

typedef const char* StringTableEntry;

class _StringTable
{
public:
   StringTableEntry insert(const char* string, bool /*caseSens*/ = false)
   {
      return string ? string : "";
   }

   StringTableEntry insertn(const char* string, S32 /*len*/, bool /*caseSens*/ = false)
   {
      return string ? string : "";
   }

   StringTableEntry lookup(const char* string, bool /*caseSens*/ = false)
   {
      return string ? string : nullptr;
   }

   StringTableEntry lookupn(const char* string, S32 /*len*/, bool /*caseSens*/ = false)
   {
      return string ? string : nullptr;
   }
};

extern _StringTable* StringTable;

#endif // _STRINGTABLE_H_
