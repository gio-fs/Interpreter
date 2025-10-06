#ifndef clox_memory_h
#define clox_memory_h
#include "common.h"
#include "object.h"
typedef struct {
    Value* start;
    size_t nurserySize;
    Value* end;
} Heap;

#define FRAMES_INIT_CAPACITY 64
#define STACK_INIT_CAPACITY (FRAMES_INIT_CAPACITY * UINT8_COUNT)

#define ALLOCATE(type, count) (type*)reallocate(NULL, 0, sizeof(type) * count)
#define GROW_CAPACITY(capacity) ((capacity) < 8 ? 8 : (capacity) * 2)
#define GROW_STACK_CAPACITY(capacity) ((capacity) < STACK_INIT_CAPACITY ? STACK_INIT_CAPACITY : (capacity) * 2)
#define GROW_FRAMES_CAPACITY(capacity) ((capacity) < FRAMES_INIT_CAPACITY ? FRAMES_INIT_CAPACITY : (capacity) * 2)
#define GROW_ARRAY(type, ptr, oldCount, newCount) (type*)reallocate(ptr, sizeof(type) * (oldCount), sizeof(type) * (newCount))
#define FREE(objType, ptr) reallocate(ptr, sizeof(objType), 0)
#define FREE_ARRAY(type, ptr, oldCount) reallocate(ptr, sizeof(type) * (oldCount), 0)

void* reallocate(void* ptr, size_t oldSize, size_t newSize);
void markObj(Obj* obj);
void markValue(Value slot);
void markTable(Table* table);
void collectGarbage();
void freeObjects();
#endif
