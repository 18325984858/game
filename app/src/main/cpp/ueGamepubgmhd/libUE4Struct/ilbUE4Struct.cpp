#include "ilbUE4Struct.h"

namespace ue4 {

// =====================================================================
//  FNameEntry
// =====================================================================

const char* FNameEntry::getName() const {
    return AnsiName;
}

bool FNameEntry::isWide() const {
    return (Index & 1) != 0;
}

// =====================================================================
//  TNameEntryArray
// =====================================================================

FNameEntry* TNameEntryArray::getEntry(int index) const {
    if (index < 0 || index >= NumElements) return nullptr;
    int ci = index / NAMES_ELEMENTS_PER_CHUNK;
    int wi = index % NAMES_ELEMENTS_PER_CHUNK;
    if (!Chunks[ci]) return nullptr;
    return Chunks[ci][wi];
}

// =====================================================================
//  FUObjectArray
// =====================================================================

void* FUObjectArray::getChunkPtr(int ci) const {
    return ChunkPtrs[ci];
}

int32_t FUObjectArray::getChunkCount(int ci) const {
    return ChunkElementCounts[ci];
}

int32_t FUObjectArray::getNumChunks() const {
    return NumChunks;
}

int32_t FUObjectArray::getTotalNum() const {
    return TotalNumElements;
}

} // namespace ue4

OBFU_ATTRS_END
