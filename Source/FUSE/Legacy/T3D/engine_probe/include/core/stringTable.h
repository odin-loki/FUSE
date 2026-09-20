#ifndef _STRINGTABLE_H_
#define _STRINGTABLE_H_

#ifndef _PLATFORM_H_
#include "platform/platform.h"
#endif

typedef const char* StringTableEntry;

class _StringTable
{
   StringTableEntry _EmptyString = "";

public:
   StringTableEntry EmptyString() const { return _EmptyString; }

   StringTableEntry insert(const char* string, bool caseSens = false);
   StringTableEntry insertn(const char* string, S32 len, bool caseSens = false);
   StringTableEntry lookup(const char* string, bool caseSens = false);
   StringTableEntry lookupn(const char* string, S32 len, bool caseSens = false);
};

extern _StringTable* StringTable;

#endif // _STRINGTABLE_H_
