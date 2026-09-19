#ifndef _ENGINEAPI_H_
#define _ENGINEAPI_H_

// FUSE_T3D_LEGACY_ENGINE_PROBE shadow — real engineAPI pulls SimObject/console closure.
// color.cpp DefineEngineFunction blocks compile to impl-only static functions.

#define DefineEngineFunction(name, returnType, args, defaultArgs, usage) \
    static inline returnType _fn##name##impl args

#endif // _ENGINEAPI_H_
