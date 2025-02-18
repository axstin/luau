// This file is part of the Luau programming language and is licensed under MIT License; see LICENSE.txt for details
#include "Luau/CodeAllocator.h"

#include "Luau/Common.h"

#include <string.h>

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <Windows.h>

const size_t kPageSize = 4096;
#else
#include <sys/mman.h>
#include <unistd.h>

const size_t kPageSize = sysconf(_SC_PAGESIZE);
#endif

static size_t alignToPageSize(size_t size)
{
    return (size + kPageSize - 1) & ~(kPageSize - 1);
}

#if defined(_WIN32)
static uint8_t* allocatePages(size_t size)
{
    return (uint8_t*)VirtualAlloc(nullptr, alignToPageSize(size), MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
}

static void freePages(uint8_t* mem, size_t size)
{
    if (VirtualFree(mem, 0, MEM_RELEASE) == 0)
        LUAU_ASSERT(!"failed to deallocate block memory");
}

static void makePagesExecutable(uint8_t* mem, size_t size)
{
    LUAU_ASSERT((uintptr_t(mem) & (kPageSize - 1)) == 0);
    LUAU_ASSERT(size == alignToPageSize(size));

    DWORD oldProtect;
    if (VirtualProtect(mem, size, PAGE_EXECUTE_READ, &oldProtect) == 0)
        LUAU_ASSERT(!"failed to change page protection");
}

static void flushInstructionCache(uint8_t* mem, size_t size)
{
    if (FlushInstructionCache(GetCurrentProcess(), mem, size) == 0)
        LUAU_ASSERT(!"failed to flush instruction cache");
}
#else
static uint8_t* allocatePages(size_t size)
{
    return (uint8_t*)mmap(nullptr, alignToPageSize(size), PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);
}

static void freePages(uint8_t* mem, size_t size)
{
    if (munmap(mem, alignToPageSize(size)) != 0)
        LUAU_ASSERT(!"failed to deallocate block memory");
}

static void makePagesExecutable(uint8_t* mem, size_t size)
{
    LUAU_ASSERT((uintptr_t(mem) & (kPageSize - 1)) == 0);
    LUAU_ASSERT(size == alignToPageSize(size));

    if (mprotect(mem, size, PROT_READ | PROT_EXEC) != 0)
        LUAU_ASSERT(!"failed to change page protection");
}

static void flushInstructionCache(uint8_t* mem, size_t size)
{
    __builtin___clear_cache((char*)mem, (char*)mem + size);
}
#endif

namespace Luau
{
namespace CodeGen
{

CodeAllocator::CodeAllocator(size_t blockSize, size_t maxTotalSize)
    : blockSize(blockSize)
    , maxTotalSize(maxTotalSize)
{
    LUAU_ASSERT(blockSize > kMaxUnwindDataSize);
    LUAU_ASSERT(maxTotalSize >= blockSize);
}

CodeAllocator::~CodeAllocator()
{
    if (destroyBlockUnwindInfo)
    {
        for (void* unwindInfo : unwindInfos)
            destroyBlockUnwindInfo(context, unwindInfo);
    }

    for (uint8_t* block : blocks)
        freePages(block, blockSize);
}

bool CodeAllocator::allocate(
    uint8_t* data, size_t dataSize, uint8_t* code, size_t codeSize, uint8_t*& result, size_t& resultSize, uint8_t*& resultCodeStart)
{
    // 'Round up' to preserve 16 byte alignment
    size_t alignedDataSize = (dataSize + 15) & ~15;

    size_t totalSize = alignedDataSize + codeSize;

    // Function has to fit into a single block with unwinding information
    if (totalSize > blockSize - kMaxUnwindDataSize)
        return false;

    size_t unwindInfoSize = 0;

    // We might need a new block
    if (totalSize > size_t(blockEnd - blockPos))
    {
        if (!allocateNewBlock(unwindInfoSize))
            return false;

        LUAU_ASSERT(totalSize <= size_t(blockEnd - blockPos));
    }

    LUAU_ASSERT((uintptr_t(blockPos) & (kPageSize - 1)) == 0); // Allocation starts on page boundary

    size_t dataOffset = unwindInfoSize + alignedDataSize - dataSize;
    size_t codeOffset = unwindInfoSize + alignedDataSize;

    if (dataSize)
        memcpy(blockPos + dataOffset, data, dataSize);
    if (codeSize)
        memcpy(blockPos + codeOffset, code, codeSize);

    size_t pageSize = alignToPageSize(unwindInfoSize + totalSize);

    makePagesExecutable(blockPos, pageSize);
    flushInstructionCache(blockPos + codeOffset, codeSize);

    result = blockPos + unwindInfoSize;
    resultSize = totalSize;
    resultCodeStart = blockPos + codeOffset;

    blockPos += pageSize;
    LUAU_ASSERT((uintptr_t(blockPos) & (kPageSize - 1)) == 0); // Allocation ends on page boundary

    return true;
}

bool CodeAllocator::allocateNewBlock(size_t& unwindInfoSize)
{
    // Stop allocating once we reach a global limit
    if ((blocks.size() + 1) * blockSize > maxTotalSize)
        return false;

    uint8_t* block = allocatePages(blockSize);

    if (!block)
        return false;

    blockPos = block;
    blockEnd = block + blockSize;

    blocks.push_back(block);

    if (createBlockUnwindInfo)
    {
        void* unwindInfo = createBlockUnwindInfo(context, block, blockSize, unwindInfoSize);

        // 'Round up' to preserve 16 byte alignment of the following data and code
        unwindInfoSize = (unwindInfoSize + 15) & ~15;

        LUAU_ASSERT(unwindInfoSize <= kMaxUnwindDataSize);

        if (!unwindInfo)
            return false;

        unwindInfos.push_back(unwindInfo);
    }

    return true;
}

} // namespace CodeGen
} // namespace Luau
