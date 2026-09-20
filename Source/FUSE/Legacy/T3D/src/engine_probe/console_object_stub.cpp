#include "console/consoleObject.h"

U32 AbstractClassRep::NetClassCount[NetClassGroupsCount][NetClassTypesCount] = {
    {32, 32, 32},
    {32, 32, 32},
    {32, 32, 32},
    {32, 32, 32},
};

U32 AbstractClassRep::NetClassBitSize[NetClassGroupsCount][NetClassTypesCount] = {
    {8, 8, 8},
    {8, 8, 8},
    {8, 8, 8},
    {8, 8, 8},
};
