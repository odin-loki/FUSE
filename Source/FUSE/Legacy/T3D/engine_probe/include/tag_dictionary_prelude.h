// Force-include for Engine/source/core/tagDictionary.cpp under FUSE_T3D_LEGACY_ENGINE_PROBE.
// tagDictionary.h uses DataChunker without including dataChunker.h (PCH/transitive in full Engine).
#ifndef FUSE_T3D_TAG_DICTIONARY_PRELUDE_H
#define FUSE_T3D_TAG_DICTIONARY_PRELUDE_H

#include "core/dataChunker.h"

// Probe smoke uses stack-local TagDictionary only; skip unused global singleton.
#define FUSE_PROBE_NO_GLOBAL_TAG_DICTIONARY 1

#endif
