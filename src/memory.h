#ifndef clox_memory_h
#define clox_memory_h
#include "common.h"
#include "object.h"
typedef struct {
    uint8_t* start;
    size_t size;
    uint8_t* curr;
    uint8_t* reserved;
    size_t reservedSize;
    size_t reservedBytesAllocated;
} Nursery;

typedef struct {
    uint8_t* start;
    size_t size;
    size_t bytesAllocated;
} Heap;

typedef struct {
    Nursery nursery;
    ValueArray worklist;
    Heap aging;
    Heap oldGen;
} GenerationalHeap;

extern GenerationalHeap heap;

#define FRAMES_INIT_CAPACITY 64
#define STACK_INIT_CAPACITY (FRAMES_INIT_CAPACITY * UINT8_COUNT)

#define ALLOCATE(type, count) (type*)reallocate(NULL, 0, sizeof(type) * count)
#define GROW_CAPACITY(capacity) ((capacity) < 8 ? 8 : (capacity) * 2)
#define GROW_STACK_CAPACITY(capacity) ((capacity) < STACK_INIT_CAPACITY ? STACK_INIT_CAPACITY : (capacity) * 2)
#define GROW_FRAMES_CAPACITY(capacity) ((capacity) < FRAMES_INIT_CAPACITY ? FRAMES_INIT_CAPACITY : (capacity) * 2)
#define GROW_ARRAY(type, ptr, oldCount, newCount) (type*)reallocate(ptr, sizeof(type) * (oldCount), sizeof(type) * (newCount))
#define FREE(objType, ptr) reallocate(ptr, sizeof(objType), 0)
#define FREE_ARRAY(type, ptr, oldCount) reallocate(ptr, sizeof(type) * (oldCount), 0)

#define GC_HEAP_GROW_FACTOR 2
#define HEAP_SIZE 1024 * 1024 * 2
#define NURSERY_SIZE HEAP_SIZE / 4
#define ALIGNMENT 8
#define PROMOTING_AGE 2

void* reallocate(void* ptr, size_t oldSize, size_t newSize);
void markObj(Obj* obj);
void markValue(Value slot);
void markTable(Table* table);
void collectGarbage();
void freeObjects();
void initGenHeap();
void* writeNursery(Nursery* nursery, size_t size);
void* writeHeap(Heap* heap, size_t size);
size_t align(size_t size, size_t alignment);
const char* objTypeName(int t);
#endif
