#ifndef _ENGINETYPEINFO_H_
#define _ENGINETYPEINFO_H_

// FUSE_T3D_LEGACY_ENGINE_PROBE shadow — real engineTypeInfo pulls engineExports/console closure.
// uuid.h only needs EngineFieldTable::Field for UUIDEngineExport declarations (methods live in uuid.cpp).

class EngineFieldTable
{
public:
    struct Field {};
};

#endif // _ENGINETYPEINFO_H_
