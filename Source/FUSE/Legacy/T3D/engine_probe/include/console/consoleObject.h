#ifndef _CONSOLEOBJECT_H_
#define _CONSOLEOBJECT_H_

#ifndef _TORQUE_TYPES_H_
#include "platform/types.h"
#endif

enum NetClassTypes
{
    NetClassTypeObject = 0,
    NetClassTypeDataBlock,
    NetClassTypeEvent,
    NetClassTypesCount,
};

enum NetClassGroups
{
    NetClassGroupGame = 0,
    NetClassGroupCommunity,
    NetClassGroup3,
    NetClassGroup4,
    NetClassGroupsCount,
};

class AbstractClassRep
{
public:
    static U32 NetClassCount[NetClassGroupsCount][NetClassTypesCount];
    static U32 NetClassBitSize[NetClassGroupsCount][NetClassTypesCount];
};

#endif // _CONSOLEOBJECT_H_
