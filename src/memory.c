#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include "clox_compiler.h"
#include "memory.h"
#include "common.h"
#include "vm.h"

#ifdef DEBUG_LOG_GC
#include <stdio.h>
#include "clox_debug.h"
#endif

#define IS_IN_NURSERY(obj) \
                ((uint8_t*)obj >= heap.nursery.start \
                            && (uint8_t*)obj < heap.nursery.curr)
#define IS_IN_RESERVED(obj) \
                ((uint8_t*)obj >= heap.nursery.reserved \
                            && (uint8_t*)obj < heap.nursery.reserved + heap.nursery.reservedBytesAllocated)

GenerationalHeap heap;


#ifdef DEBUG_LOG_GC
const char* objTypeName(int t) {
    switch (t) {
        case OBJ_UPVALUE:    return "UPVALUE";
        case OBJ_FUNCTION:   return "FUNCTION";
        case OBJ_CLOSURE:    return "CLOSURE";
        case OBJ_DICTIONARY: return "DICTIONARY";
        case OBJ_ARRAY:      return "ARRAY";
        case OBJ_CLASS:      return "CLASS";
        case OBJ_INSTANCE:   return "INSTANCE";
        case OBJ_BOUND_METHOD:return "BOUND_METHOD";
        case OBJ_NATIVE:     return "NATIVE";
        case OBJ_STRING:     return "STRING";
        case OBJ_RANGE:      return "RANGE";
        default:             return "UNKNOWN";
    }
}
#endif

static void updateFields(Value* value, ptrdiff_t diff, Heap* heap) {
    #define IS_IN_MOVED(obj) \
                ((uint8_t*)obj >= heap->start \
                            && (uint8_t*)obj <= heap->start + heap->bytesAllocated)

    #define ADJUST_INTERNAL_REF(obj) \
        do { \
            if (IS_IN_MOVED(obj) && obj != NULL) { \
                obj = (__typeof__(obj))((uint8_t*)(obj) + diff); \
            } \
        } while (0)

    #define ADJUST_INTERNAL_VAL(value) \
        do { \
            if (IS_OBJ(*value)) { \
                Obj* obj = AS_OBJ(*value); \
                if (IS_IN_MOVED(obj)) { \
                    obj = (__typeof__(obj))((uint8_t*)(obj) + diff); \
                    *value = OBJ_VAL(obj); \
                } \
            } \
        } while (0)

    if (value->type != VAL_OBJ) return;
    Obj* obj = AS_OBJ(*value);

    switch (obj->type) {
        case OBJ_UPVALUE: {
            ObjUpvalue* upvalue = (ObjUpvalue*)obj;
            ADJUST_INTERNAL_VAL(&upvalue->closed);
            ADJUST_INTERNAL_REF(upvalue->next);
            break;
        }
        case OBJ_FUNCTION: {
            ObjFunction* func = (ObjFunction*)obj;
            ADJUST_INTERNAL_REF(func->name);

            for (int i = 0; i < func->chunk.constants.count; i++) {
                ADJUST_INTERNAL_VAL(&func->chunk.constants.values[i]);
            }

            break;
        }
        case OBJ_CLOSURE: {
            ObjClosure* closure = (ObjClosure*)obj;
            ADJUST_INTERNAL_REF(closure->function);
            ObjFunction* func = closure->function;

            if (func->name != NULL) {
                ADJUST_INTERNAL_REF(func->name);
            }

            for (int i = 0; i < func->chunk.constants.count; i++) {
                ADJUST_INTERNAL_VAL(&func->chunk.constants.values[i]);
            }

            for (int j = 0; j < closure->upvalueCount; j++) {
                if (closure->upvalues[j] != NULL) {
                    ADJUST_INTERNAL_REF(closure->upvalues[j]);
                }
            }
            break;
        }
        case OBJ_DICTIONARY: {
            ObjDictionary* dict = (ObjDictionary*)obj;
            for (int i = 0; i < dict->map.capacity; i++) {
                ADJUST_INTERNAL_REF(dict->map.entries[i].key);
                ADJUST_INTERNAL_VAL(&dict->map.entries[i].value);
            }

            for (int i = 0; i < dict->entries.capacity; i++) {
                ADJUST_INTERNAL_REF(dict->entries.entries[i].key);
                ADJUST_INTERNAL_VAL(&dict->entries.entries[i].value);
            }

            ADJUST_INTERNAL_REF(dict->klass);
            break;
        }
        case OBJ_ARRAY: {
            ObjArray* arr = (ObjArray*)obj;

            for (int i = 0; i < arr->values.count; i++) {
                ADJUST_INTERNAL_VAL(&arr->values.values[i]);
            }

            ADJUST_INTERNAL_REF(arr->klass);
            break;
        }
        case OBJ_CLASS: {
            ObjClass* klass = (ObjClass*)obj;
            ADJUST_INTERNAL_REF(klass->name);

            for (int i = 0; i < klass->methods.capacity; i++) {
                ADJUST_INTERNAL_REF(klass->methods.entries[i].key);
                ADJUST_INTERNAL_VAL(&klass->methods.entries[i].value);
            }

            for (int i = 0; i < klass->fields.capacity; i++) {
                ADJUST_INTERNAL_REF(klass->fields.entries[i].key);
                ADJUST_INTERNAL_VAL(&klass->fields.entries[i].value);
            }
            break;
        }
        case OBJ_INSTANCE: {
            ObjInstance* instance = (ObjInstance*)obj;
            ADJUST_INTERNAL_REF(instance->klass);

            for (int i = 0; i < instance->fields.capacity; i++) {
                ADJUST_INTERNAL_REF(instance->fields.entries[i].key);
                ADJUST_INTERNAL_VAL(&instance->fields.entries[i].value);
            }
            break;
        }
        case OBJ_BOUND_METHOD: {
            ObjBoundMethod* bound = (ObjBoundMethod*)obj;
            ADJUST_INTERNAL_VAL(&bound->receiver);
            ADJUST_INTERNAL_REF(bound->method);
            break;
        }
        case OBJ_NATIVE:
        case OBJ_STRING:
        case OBJ_RANGE:
            // No references to update
            break;
    }
}

static void promoteObjects() {
    uint8_t* start = heap.aging.start;
    uint8_t* end = heap.aging.start + heap.aging.bytesAllocated;

    while (start < end) {
        Obj* curr = (Obj*)start;

        if (curr->age == PROMOTING_AGE) {
            Obj* oldObj = (Obj*)writeHeap(&heap.oldGen, curr->size);
            memcpy(oldObj, curr, curr->size);

            ptrdiff_t diff = (uint8_t*)(curr) - start;
            updateFields(&OBJ_VAL(oldObj), diff, &heap.oldGen);
            oldObj->forwarded = oldObj;

        }

        start += curr->size;
    }
}

static void updateReferences(uint8_t* old, Heap* heap) {
    ptrdiff_t diff = heap->start - old;

    #define IS_IN_MOVED(obj) \
                ((uint8_t*)obj >= heap->start \
                            && (uint8_t*)obj <= heap->start + heap->bytesAllocated)

    #define ADJUST_REF(obj) \
        do { \
            if (IS_IN_MOVED(obj) && obj != NULL) { \
                obj = (__typeof__(obj))((uint8_t*)(obj) + diff); \
            } \
        } while (0)

    #define ADJUST_VALUE(value) \
        do { \
            if (IS_OBJ(*value)) { \
                Obj* obj = AS_OBJ(*value); \
                if (IS_IN_MOVED(obj)) { \
                    obj = (__typeof__(obj))((uint8_t*)(obj) + diff); \
                    *value = OBJ_VAL(obj); \
                } \
            } \
        } while (0)


    for (Value* slot = vm.stack.values; slot < vm.stackTop; slot++) {
        ADJUST_VALUE(slot);
    }

    for (int i = 0; i < vm.frameArray.count; i++) {
        ADJUST_REF(vm.frameArray.frames[i].closure);
    }

    ObjUpvalue** upval = &vm.openUpvalues; // pointer to upvalue list
    while(*upval != NULL) {
        ADJUST_REF(*upval);
        upval = &(*upval)->next;
    }

    for (int i = 0; i <= vm.nestingLevel; i++) {
        for (int j = 0; j < vm.queueCount[i]; j++) {
            ADJUST_VALUE(&vm.queue[i].values[j]);
        }
    }

    for (int i = 0; i < vm.globals.capacity; i++) {
        Entry* entry = &vm.globals.entries[i];
        ADJUST_REF(entry->key);
        ADJUST_VALUE(&entry->value);
    }

    for (int i = 0; i < vm.constGlobals.capacity; i++) {
        Entry* entry = &vm.constGlobals.entries[i];
        ADJUST_REF(entry->key);
        ADJUST_VALUE(&entry->value);
    }

    for (int i = 0; i < vm.strings.capacity; i++) {
        Entry* entry = &vm.strings.entries[i];
        ADJUST_REF(entry->key);
        ADJUST_VALUE(&entry->value);
    }

    ADJUST_REF(vm.array_NativeString);
    ADJUST_REF(vm.dict_NativeString);
    ADJUST_REF(vm.initString);
    ADJUST_REF(vm.arrayClass);
    ADJUST_REF(vm.dictClass);

    uint8_t* end = heap->start + heap->bytesAllocated;
    uint8_t* start = heap->start;

    while (start < end) {
        Obj* obj = (Obj*)start;
        updateFields(&OBJ_VAL(obj), diff, heap);
        obj->forwarded = obj;
        start += obj->size;
    }

}

size_t align(size_t size, size_t alignment) {
    size_t aligned = (size + (alignment - 1)) & ~(alignment - 1);
    return aligned;
}

void minorCollection();
void* writeNursery(Nursery* nursery, size_t size) {
    size_t aligned = align(size, ALIGNMENT);

    if (vm.isCollecting) {
        void* reserved = nursery->reserved + nursery->reservedBytesAllocated;
        nursery->reservedBytesAllocated += aligned;
        return reserved;
    }

    if (nursery->curr + aligned > nursery->start + NURSERY_SIZE && !vm.isCollecting) {
#ifdef DEBUG_LOG_GC
        fprintf(stderr, "[GC] writeNursery: insufficient space, triggering minorCollection\n");
#endif
        minorCollection();
    }

    void* result = nursery->curr;
    nursery->curr += aligned;
    return result;
}

static void growHeap(Heap* heap, size_t newSize) {
    uint8_t* old = heap->start;
    heap->start = realloc(heap->start, newSize);

    if (heap->start == NULL) {
        printf("growHeap failed");
        exit(1);
    }

    // updateReferences(old, heap);
    heap->size = newSize;
    printf("Heap grown");
}

void* writeHeap(Heap* heap, size_t size) {
    size_t aligned = align(size, ALIGNMENT);
#ifdef DEBUG_LOG_GC
    fprintf(stderr, "[GC] writeHeap: heap=%p size=%zu aligned=%zu bytesAllocated=%zu capacity=%zu\n",
            (void*)heap->start, size, size, heap->bytesAllocated, heap->size);
#endif
    heap->bytesAllocated += aligned;
    if (heap->bytesAllocated >= heap->size) {
#ifdef DEBUG_LOG_GC
        fprintf(stderr, "[GC] writeHeap: need grow, bytesAllocated=%zu > size=%zu\n",
                heap->bytesAllocated, heap->size);
#endif
        growHeap(heap, heap->size * 2);
#ifdef DEBUG_LOG_GC
        fprintf(stderr, "[GC] writeHeap: after grow, capacity=%zu base=%p\n", heap->size, (void*)heap->start);
#endif
    }

    void* result = heap->start + heap->bytesAllocated - aligned;
    if (result == NULL) {
        printf("heapAlloc failed");
        exit(1);
    }
#ifdef DEBUG_LOG_GC
    fprintf(stderr, "[GC] writeHeap: allocated block=%p size=%zu new-bytesAllocated=%zu\n",
            (void*)result, size, heap->bytesAllocated);
#endif
    return result;
}

static Obj* copyObject(Obj* obj) {
#ifdef DEBUG_LOG_GC
    fprintf(stderr, "[GC] copyObject: old=%p type=%s size=%zu age=%u fwd=%p\n",
            (void*)obj, objTypeName(obj->type), obj->size, obj->age, (void*)obj->forwarded);
    if (IS_IN_RESERVED(obj)) fprintf(stderr, "Object is in reserved space");
#endif
    if (obj->size == 0 || obj->type > 10 || obj->type < 0) {
        printf("Error: object size is 0 or type is unknown\n");
        exit(1);
    }

    if (obj->forwarded != NULL) {
#ifdef DEBUG_LOG_GC
        fprintf(stderr, "[GC] copyObject: already forwarded to %p\n", (void*)obj->forwarded);
#endif
        return obj->forwarded;
    }

    obj->age++;
    Obj* newObj = (Obj*)writeHeap(&heap.aging, obj->size);
    obj->forwarded = newObj;
#ifdef DEBUG_LOG_GC
    fprintf(stderr, "[GC] copyObject: memcpy new=%p <- old=%p bytes=%zu\n",
            (void*)newObj, (void*)obj, obj->size);
#endif
    memcpy(newObj, obj, obj->size);

#ifdef DEBUG_LOG_GC
    fprintf(stderr, "[GC] copyObject: enqueued new object %p (type=%s)\n",
            (void*)newObj, objTypeName(newObj->type));
#endif
    writeValueArray(&heap.worklist, OBJ_VAL(newObj));
    return newObj;
}

static void copyValue(Value* value) {
#ifdef DEBUG_LOG_GC
    fprintf(stderr, "[GC] copyValue: slot=%p type=%d\n", (void*)value, value->type);
#endif
    if (value->type != VAL_OBJ) return;
    Obj* old = AS_OBJ(*value);
#ifdef DEBUG_LOG_GC
    fprintf(stderr, "[GC] copyValue: oldObj=%p type=%s\n", (void*)old, objTypeName(old->type));
#endif
    Obj* newLoc = copyObject((Obj*)old);
    *value = OBJ_VAL(newLoc);
#ifdef DEBUG_LOG_GC
    fprintf(stderr, "[GC] copyValue: updated slot=%p old=%p -> new=%p\n",
            (void*)value, (void*)old, (void*)newLoc);
#endif
}

static void copyTable(Table* table) {
#ifdef DEBUG_LOG_GC
    fprintf(stderr, "[GC] copyTable: table=%p capacity=%d count=%d entries=%p\n",
            (void*)table, table->capacity, table->count, (void*)table->entries);
#endif
    for (int i = 0; i < table->capacity; i++) {
        Entry* entry = &table->entries[i];
        if (entry->key != NULL) {
#ifdef DEBUG_LOG_GC
            fprintf(stderr, "[GC] copyTable: slot=%d key(old)=%p val.type=%d\n",
                    i, (void*)entry->key, entry->value.type);
#endif
            ObjString* newKey = (ObjString*)copyObject((Obj*)entry->key);
            entry->key = newKey;
#ifdef DEBUG_LOG_GC
            fprintf(stderr, "[GC] copyTable: slot=%d key(new)=%p\n", i, (void*)entry->key);
#endif
            copyValue(&entry->value);
#ifdef DEBUG_LOG_GC
            fprintf(stderr, "[GC] copyTable: slot=%d value updated\n", i);
#endif
        }
    }
#ifdef DEBUG_LOG_GC
    fprintf(stderr, "[GC] copyTable: done table=%p\n", (void*)table);
#endif
}

static void copyArray(ValueArray* arr) {
#ifdef DEBUG_LOG_GC
    fprintf(stderr, "[GC] copyArray: arr=%p count=%d values=%p\n",
            (void*)arr, arr->count, (void*)arr->values);
#endif
    for (int i = 0; i < arr->count; i++) {
#ifdef DEBUG_LOG_GC
        fprintf(stderr, "[GC] copyArray: idx=%d\n", i);
#endif
        copyValue(&arr->values[i]);
    }
#ifdef DEBUG_LOG_GC
    fprintf(stderr, "[GC] copyArray: done arr=%p\n", (void*)arr);
#endif
}

static void scanObjectFields(Obj* obj) {
    switch (obj->type) {
        case OBJ_UPVALUE: {
            copyValue(&((ObjUpvalue*)obj)->closed);
            break;
        }
        case OBJ_FUNCTION: {
            ObjFunction* func = (ObjFunction*)obj;
            if (func->name != NULL) {
                func->name = (ObjString*)copyObject((Obj*)func->name);
            }
            copyArray(&func->chunk.constants);
            break;
        }
        case OBJ_CLOSURE: {
            ObjClosure* closure = (ObjClosure*)obj;
            closure->function = (ObjFunction*)copyObject((Obj*)closure->function);

            ObjFunction* func = closure->function;
            if (func->name != NULL) {
                func->name = (ObjString*)copyObject((Obj*)func->name);
            }
            copyArray(&func->chunk.constants);

            for (int j = 0; j < closure->upvalueCount; j++) {
                if (closure->upvalues[j] != NULL) {
                    closure->upvalues[j] = (ObjUpvalue*)copyObject((Obj*)closure->upvalues[j]);
                }
            }
            break;
        }
        case OBJ_DICTIONARY: {
            ObjDictionary* dict = (ObjDictionary*)obj;
            copyTable(&dict->map);
            for (int j = 0; j < dict->entries.count; j++) {
                dict->entries.entries[j].key =
                    (ObjString*)copyObject((Obj*)dict->entries.entries[j].key);
                copyValue(&dict->entries.entries[j].value);
            }

            dict->klass = (ObjClass*)copyObject((Obj*)dict->klass);
            break;
        }
        case OBJ_ARRAY: {
            ObjArray* arr = (ObjArray*)obj;
            copyArray(&arr->values);
            arr->klass = (ObjClass*)copyObject((Obj*)arr->klass);
            break;
        }
        case OBJ_CLASS: {
            ObjClass* klass = (ObjClass*)obj;
            klass->name = (ObjString*)copyObject((Obj*)klass->name);
            copyTable(&klass->methods);
            copyTable(&klass->fields);
            break;
        }
        case OBJ_INSTANCE: {
            ObjInstance* instance = (ObjInstance*)obj;
            instance->klass = (ObjClass*)copyObject((Obj*)instance->klass);
            copyTable(&instance->fields);
            break;
        }
        case OBJ_BOUND_METHOD: {
            ObjBoundMethod* bound = (ObjBoundMethod*)obj;
            copyValue(&bound->receiver);
            bound->method = (ObjClosure*)copyObject((Obj*)bound->method);
            break;
        }
        case OBJ_NATIVE:
        case OBJ_STRING:
        case OBJ_RANGE:
            // No references to update
            break;
    }
}

static void scanOldGenerations() {
#ifdef DEBUG_LOG_GC
    fprintf(stderr, "[GC] Scanning old generations for nursery references\n");
#endif

    uint8_t* start = heap.aging.start;
    uint8_t* end = heap.aging.start + heap.aging.bytesAllocated;

#ifdef DEBUG_LOG_GC
    fprintf(stderr, "[GC] Scanning aging space: %p to %p (%zu bytes)\n",
            (void*)start, (void*)end, heap.aging.bytesAllocated);
#endif

    while (start < end) {
        Obj* obj = (Obj*)start;

#ifdef DEBUG_LOG_GC
        fprintf(stderr, "[GC] Aging obj=%p type=%s size=%zu\n",
                (void*)obj, objTypeName(obj->type), obj->size);
#endif
        scanObjectFields(obj);
        start += obj->size;
    }

    uint8_t* oldStart = heap.oldGen.start;
    uint8_t* oldEnd = heap.oldGen.start + heap.oldGen.bytesAllocated;

#ifdef DEBUG_LOG_GC
    fprintf(stderr, "[GC] Scanning oldGen space: %p to %p (%zu bytes)\n",
            (void*)start, (void*)end, heap.aging.bytesAllocated);
#endif

    while (oldStart < oldEnd) {
        Obj* obj = (Obj*)oldStart;

#ifdef DEBUG_LOG_GC
        fprintf(stderr, "[GC] OldGen obj=%p type=%s size=%zu\n",
                (void*)obj, objTypeName(obj->type), obj->size);
#endif

        scanObjectFields(obj);
        oldStart += obj->size;
    }
    printf("Ended scanOldGEnerations\n");
}

static void copyReferences() {
#ifdef DEBUG_LOG_GC
    fprintf(stderr, "[GC] copyReferences: worklist.count=%d\n", heap.worklist.count);
#endif
    for (int i = heap.worklist.count - 1; i >= 0; i--) {
        Value* value = &heap.worklist.values[i];

        if ((*value).type != VAL_OBJ) continue;
        scanObjectFields(AS_OBJ(*value));
    }
    initValueArray(&heap.worklist);
}

static void scanReservedNursery() {
    uint8_t* start = heap.nursery.reserved;
    uint8_t* end = start + heap.nursery.reservedBytesAllocated;

    while (start < end) {
        Obj* curr = (Obj*)start;
        scanObjectFields(curr);
        start += curr->size;
    }
}

static void moveReservedNursery() {
    memcpy(heap.nursery.start, heap.nursery.reserved, heap.nursery.reservedBytesAllocated);
    heap.nursery.curr += heap.nursery.reservedBytesAllocated;
    heap.nursery.reservedBytesAllocated = 0;
}

void minorCollection() {
    vm.isCollecting = true;
#ifdef DEBUG_LOG_GC
    fprintf(stderr, "\n[GC] ===== Minor collection begin =====\n");
#endif

#ifdef DEBUG_LOG_GC
    size_t used = (size_t)(heap.nursery.curr - heap.nursery.start);
    fprintf(stderr, "[GC] Nursery before: used=%zu free=%zu\n", used, heap.nursery.size - used);
#endif


#ifdef DEBUG_LOG_GC
    fprintf(stderr, "[GC] Roots: stack scan from %p to %p\n",
            (void*)vm.stack.values, (void*)vm.stackTop);
#endif
    for (Value* slot = vm.stack.values; slot < vm.stackTop; slot++) {
        copyValue(slot);
    }


#ifdef DEBUG_LOG_GC
    fprintf(stderr, "[GC] Roots: frameArray count=%d frames=%p\n",
            vm.frameArray.count, (void*)vm.frameArray.frames);
#endif
    for (int i = 0; i < vm.frameArray.count; i++) {
        ObjClosure* old = vm.frameArray.frames[i].closure;
        vm.frameArray.frames[i].closure =
            (ObjClosure*)copyObject((Obj*)old);
#ifdef DEBUG_LOG_GC
        fprintf(stderr, "[GC] Root frame[%d]: old=%p -> new=%p\n",
                i, (void*)old, (void*)vm.frameArray.frames[i].closure);
#endif
    }


#ifdef DEBUG_LOG_GC
    for (int i = 0; i < 50000; i++) fprintf(stderr, "[GC] Roots: openUpvalues head=%p\n", (void*)vm.openUpvalues);
#endif
    ObjUpvalue** upval = &vm.openUpvalues; // pointer to upvalue list
    while(*upval != NULL) {
        ObjUpvalue* old = *upval;
        *upval = (ObjUpvalue*)copyObject((Obj*)old);
        upval = &(*upval)->next;
    }


#ifdef DEBUG_LOG_GC
    for (int i = 0; i < 50000; i++) fprintf(stderr, "[GC] Roots: queues levels=%d\n", vm.nestingLevel + 1);
#endif
    for (int i = 0; i <= vm.nestingLevel; i++) {
        for (int j = 0; j < vm.queueCount[i]; j++) {
            copyValue(&vm.queue[i].values[j]);
#ifdef DEBUG_LOG_GC
            fprintf(stderr, "[GC] Root queue[%d][%d] updated\n", i, j);
#endif
        }
    }

    // Globals
#ifdef DEBUG_LOG_GC
    for (int i = 0; i < 50000; i++)  fprintf(stderr, "[GC] Roots: globals\n");
#endif
    copyTable(&vm.globals);

#ifdef DEBUG_LOG_GC
    for (int i = 0; i < 50000; i++) fprintf(stderr, "[GC] Roots: constGlobals\n");
#endif
    copyTable(&vm.constGlobals);

#ifdef DEBUG_LOG_GC
    for (int i = 0; i < 50000; i++) fprintf(stderr, "[GC ROOT] Roots: strings\n");
#endif
    copyTable(&vm.strings);

    // Well-known strings/classes
    ObjString* oldArr = vm.array_NativeString;
    vm.array_NativeString = (ObjString*)copyObject((Obj*)vm.array_NativeString);
#ifdef DEBUG_LOG_GC
    for (int i = 0; i < 50000; i++)  fprintf(stderr, "[GC ROOT] Root array_NativeString: old=%p -> new=%p\n", (void*)oldArr, (void*)vm.array_NativeString);
#endif

    ObjString* oldDict = vm.dict_NativeString;
    vm.dict_NativeString = (ObjString*)copyObject((Obj*)vm.dict_NativeString);
#ifdef DEBUG_LOG_GC
    for (int i = 0; i < 50000; i++) fprintf(stderr, "[GC ROOT] Root dict_NativeString: old=%p -> new=%p\n", (void*)oldDict, (void*)vm.dict_NativeString);
#endif

    ObjString* oldInit = vm.initString;
    vm.initString = (ObjString*)copyObject((Obj*)vm.initString);
#ifdef DEBUG_LOG_GC
    for (int i = 0; i < 50000; i++) fprintf(stderr, "[GC ROOT] Root initString: old=%p -> new=%p\n", (void*)oldInit, (void*)vm.initString);
#endif

    ObjClass* oldArrClass = vm.arrayClass;
    vm.arrayClass = (ObjClass*)copyObject((Obj*)vm.arrayClass);
#ifdef DEBUG_LOG_GC
    for (int i = 0; i < 5000; i++) fprintf(stderr, "[GC ROOT] Root arrayClass: old=%p -> new=%p\n", (void*)oldArrClass, (void*)vm.arrayClass);
#endif

    ObjClass* oldDictClass = vm.dictClass;
    vm.dictClass = (ObjClass*)copyObject((Obj*)vm.dictClass);
#ifdef DEBUG_LOG_GC
    for (int i = 0; i < 5000; i++) fprintf(stderr, "[GC ROOT] Root dictClass: old=%p -> new=%p\n", (void*)oldDictClass, (void*)vm.dictClass);
#endif

    // scanReservedNursery();
    scanOldGenerations();
    copyReferences();
    // promoteObjects();

    heap.nursery.curr = heap.nursery.start;
    moveReservedNursery();

#ifdef DEBUG_LOG_GC
    fprintf(stderr, "[GC] Nursery after reset: curr=%p used=0 free=%zu\n",
            (void*)heap.nursery.curr, heap.nursery.size);
    for (int i = 0; i < 10000; i++) fprintf(stderr, "[GC] ===== Minor collection end =====\n\n");
#endif
    vm.isCollecting = false;
}


void initGenHeap() {
    heap.nursery.start = (uint8_t*)malloc(NURSERY_SIZE);
    heap.nursery.size = NURSERY_SIZE;
    heap.nursery.curr = heap.nursery.start;
    heap.nursery.reservedSize = NURSERY_SIZE / 2;
    heap.nursery.reservedBytesAllocated = 0;
    heap.nursery.reserved = (uint8_t*)malloc(NURSERY_SIZE / 2);

    heap.oldGen.start = (uint8_t*)malloc(HEAP_SIZE);
    heap.oldGen.size = HEAP_SIZE;
    heap.oldGen.bytesAllocated = 0;

    heap.aging.start = (uint8_t*)malloc(HEAP_SIZE / 2);
    heap.aging.size = HEAP_SIZE / 2;
    heap.aging.bytesAllocated = 0;

    initValueArray(&heap.worklist);
}

void* reallocate(void* ptr, size_t oldSize, size_t newSize) {
    vm.bytesAllocated += newSize - oldSize;

    if (newSize > oldSize) {
    #ifdef DEBUG_STRESS_GC
        collectGarbage();
    #endif
    }

    // if (vm.bytesAllocated > vm.nextGC && !vm.isCollecting) {
    //     collectGarbage();
    // }

    if (newSize == 0) {
        free(ptr);
        return NULL;
    }

    void* result = realloc(ptr, newSize);
    if (result == NULL) exit(1);
    return result;
}

// void markObj(Obj* obj) {
//     if (obj == NULL) return;
//     if (obj->isMarked) return;

// #ifdef DEBUG_LOG_GC
//     // printf("%p mark ", (void*)obj);
//     // printValue(OBJ_VAL(obj));
//     // printf("\n");
// #endif
//     // for (int i = 0; i < 9000; i++) printf("[MARKOBJ] %p type=%d marked=%d grayCount=%d\n",
//     //   (void*)obj, obj->type, obj->isMarked, vm.grayCount);


//     obj->isMarked = true;

//     if (vm.grayCapacity < vm.grayCount + 1) {
//         vm.grayCapacity = GROW_CAPACITY(vm.grayCapacity);
//         vm.grayStack = realloc(vm.grayStack, sizeof(Obj*) * vm.grayCapacity);

//         if (vm.grayStack == NULL) exit(1);
//     }

//     vm.grayStack[vm.grayCount++] = obj;
// }

// void freeObject(Obj* obj) {
// #ifdef DEBUG_LOG_GC
//     // printf("[FREE_OBJ] %p type=%d\n", (void*)obj, obj->type);
// #endif

//     switch (obj->type) {
//         case OBJ_FUNCTION: {
//             ObjFunction* function = (ObjFunction*)obj;
//             freeChunk(&function->chunk);
//             FREE(ObjFunction, obj);
//             break;
//         }

//         case OBJ_STRING: {
//             ObjString* string = (ObjString*)obj;
// #ifdef DEBUG_LOG_GC
//             // printf("  len=%d size=%zu\n", string->length,
//             //        sizeof(ObjString) + string->length + 1);
// #endif
//             // chars[] is a flexible array member: it's part of the same
//             // memory and should be deallocated only one time
//             reallocate(string, sizeof(ObjString) + string->length + 1, 0);
//             break;
//         }

//         case OBJ_ARRAY: {
//             ObjArray* arr = (ObjArray*)obj;
//             reallocate(arr->values.values, sizeof(Value) * arr->values.capacity, 0);
//             reallocate(arr, sizeof(ObjArray), 0);
//             break;
//         }

//         case OBJ_NATIVE: {
//             reallocate(obj, sizeof(ObjNative), 0);
//             break;
//         }

//         case OBJ_CLOSURE: {
//             ObjClosure* closure = (ObjClosure*)obj;
//             reallocate(closure, sizeof(ObjClosure), 0);
//             break;
//         }

//         case OBJ_UPVALUE: {
//             reallocate(obj, sizeof(ObjUpvalue), 0);
//             break;
//         }

//         case OBJ_DICTIONARY: {
//             ObjDictionary* dict = (ObjDictionary*)obj;
//             freeTable(&dict->map);
//             reallocate(dict->entries.entries, sizeof(Entry) * dict->entries.capacity, 0);
//             reallocate(dict, sizeof(ObjDictionary), 0);
//             break;
//         }
//         case OBJ_CLASS: {
//             ObjClass* klass = (ObjClass*)obj;
//             freeTable(&klass->methods);
//             freeTable(&klass->fields);
//             FREE(ObjClass, obj);
//             break;
//         }
//         case OBJ_INSTANCE: {
//             ObjInstance* instance = (ObjInstance*)obj;
//             freeTable(&instance->fields);
//             FREE(ObjInstance, obj);
//             break;
//         }
//         case OBJ_BOUND_METHOD: {
//             FREE(ObjBoundMethod, obj);
//             break;
//         }
//         case OBJ_RANGE: {
//             FREE(ObjRange, obj);
//             break;
//         }
//     }
// }

// void markValue(Value value) {
//     if (!IS_OBJ(value)) return;
//     markObj(AS_OBJ(value));
// }

// void markTable(Table* table) {
//     for (int i = 0; i< table->capacity; i++) {
//         Entry* entry = &table->entries[i];
//         markObj((Obj*)entry->key);
//         markValue(entry->value);
//     }
// }

// void markArray(ValueArray* arr) {
//     for (int i = 0; i < arr->count; i++) {
//         markValue(arr->values[i]);
//     }
// }

// static void markRoots() {
//     // traversing stack
//     for (Value* slot = vm.stack.values; slot < vm.stackTop; slot++) {
//         markValue(*slot);
//     }

//     // traversing callframe array
//     for (int i = 0; i < vm.frameArray.count; i++) {
//         markObj((Obj*)vm.frameArray.frames[i].closure);
//     }

//     // traversing upvalue linked list
//     for (ObjUpvalue* upvalue = NULL; vm.openUpvalues != NULL; upvalue = upvalue->next) {
//         markObj((Obj*)upvalue);
//     }

//     for (int i = 0; i <= vm.nestingLevel; i++) {
//         for (int j = 0; j < vm.queueCount[i]; j++) {
//             markValue(vm.queue[i].values[j]);
//         }
//     }

//     markTable(&vm.globals);
//     markTable(&vm.constGlobals);
//     markObj((Obj*)vm.array_NativeString);
//     markObj((Obj*)vm.dict_NativeString);
//     markObj((Obj*)vm.initString);
//     markCompilerRoots();
// }

// static const char* typeName(ObjType type) {
//     switch (type) {
//         case OBJ_FUNCTION:   return "function";
//         case OBJ_STRING:     return "string";
//         case OBJ_ARRAY:      return "array";
//         case OBJ_NATIVE:     return "native";
//         case OBJ_CLOSURE:    return "closure";
//         case OBJ_UPVALUE:    return "upvalue";
//         case OBJ_DICTIONARY: return "dictionary";
//         case OBJ_CLASS:      return "class";
//         default:             return "unknown";
//     }
// }

// static void blackenObject(Obj* obj) {
// #ifdef DEBUG_LOG_GC
//     // fprintf(stderr, "[BLACKEN] %p type=%s\n", (void*)obj, typeName(obj->type));
// #endif

//     switch (obj->type) {
//         case OBJ_UPVALUE: {
// #ifdef DEBUG_LOG_GC
//             // fprintf(stderr, "  -> mark upvalue->closed\n");
// #endif
//             markValue(((ObjUpvalue*)obj)->closed);
//             break;
//         }
//         case OBJ_FUNCTION: {
//             ObjFunction* func = (ObjFunction*)obj;
// #ifdef DEBUG_LOG_GC
//             // fprintf(stderr, "  -> mark name=%p\n", (void*)func->name);
//             // fprintf(stderr, "  -> mark %d constants\n", func->chunk.constants.count);
// #endif
//             markObj((Obj*)func->name);
//             markArray(&func->chunk.constants);
//             break;
//         }
//         case OBJ_CLOSURE: {
//             ObjClosure* closure = (ObjClosure*)obj;
// #ifdef DEBUG_LOG_GC
//             // fprintf(stderr, "  -> mark function=%p\n", (void*)closure->function);
//             // fprintf(stderr, "  -> mark %d upvalues\n", closure->upvalueCount);
// #endif
//             markObj((Obj*)closure->function);
//             for (int i = 0; i < closure->upvalueCount; i++) {
//                 markObj((Obj*)closure->upvalues[i]);
//             }
//             break;
//         }
//         case OBJ_DICTIONARY: {
//             ObjDictionary* dict = (ObjDictionary*)obj;
// #ifdef DEBUG_LOG_GC
//             // fprintf(stderr, "  -> mark dictionary map\n");
// #endif
//             markTable(&dict->map);
//             for (int i = 0; i < dict->entries.count; i++) {
//                 markObj((Obj*)dict->entries.entries[i].key);
//                 markValue(dict->entries.entries[i].value);
//             }
//             markObj((Obj*)dict->klass);
//             break;
//         }
//         case OBJ_ARRAY: {
//             ObjArray* arr = (ObjArray*)obj;
// #ifdef DEBUG_LOG_GC
//             // fprintf(stderr, "  -> mark array values (%d)\n", ((ObjArray*)obj)->values.count);
// #endif
//             markArray(&arr->values);
//             markObj((Obj*)arr->klass);
//             break;
//         }
//         case OBJ_CLASS: {
//             ObjClass* klass = (ObjClass*)obj;
//             markObj((Obj*)klass->name);
//             markTable(&klass->methods);
//             markTable(&klass->fields);
//             break;
//         }
//         case OBJ_INSTANCE: {
//             ObjInstance* instance = (ObjInstance*)obj;
//             markObj((Obj*)instance->klass);
//             markTable(&instance->fields);
//             break;
//         }
//         case OBJ_BOUND_METHOD: {
//             ObjBoundMethod* bound = (ObjBoundMethod*)obj;
//             markValue(bound->receiver);
//             markObj((Obj*)bound->method);
//             break;
//         }
//         case OBJ_NATIVE:
//         case OBJ_STRING:
//         case OBJ_RANGE:

//             break;
//     }
// }

// static void traceReferences() {
//     while (vm.grayCount > 0) {
//         Obj* obj = vm.grayStack[--vm.grayCount];
//         blackenObject(obj);
//     }
// }

// // static void sweep(void) {
// //     Obj* previous = NULL;
// //     Obj* object = vm.objects;
// //     while (object != NULL) {
// //         if (object->isMarked) {
// //             object->isMarked = false;
// //             previous = object;
// //             object = object->next;
// //         } else {
// //             Obj* unreached = object;

// //             object = object->next;
// //             if (previous != NULL) {
// //                 previous->next = object;
// //             } else {
// //                 vm.objects = object;
// //             }

// //             freeObject(unreached);
// //         }
// //     }
// // }


// // void collectGarbage() {
// // #ifdef DEBUG_LOG_GC
// //     // for (int i = 0; i < 10000; i++) printf("--gc begin\n");
// // #endif
// //     vm.isCollecting = true;
// //     unsigned long long before = vm.bytesAllocated;

// //     markRoots();
// //     traceReferences();
// //     tableRemoveWhite(&vm.strings);
// //     sweep();

// //     unsigned long long survived = vm.bytesAllocated;
// //     double rate = (double)(survived / before);

// //     if (rate > 0.75) {
// //         vm.nextGC = survived * 4;
// //     } else if (rate > 0.5) {
// //         vm.nextGC = survived * 3;
// //     } else {
// //         vm.nextGC = survived * 2;
// //     }

// //     if (vm.nextGC < 1024 * 1024) {
// //         vm.nextGC = 1024 * 1024;
// //     }


// // // #ifdef DEBUG_LOG_GC
// //     // for (int i = 0; i < 10000; i++) printf("--end\n");
// //     printf("    Collected %lld bytes (from %lld to %lld), next at %lld\n",
// //                     before - vm.bytesAllocated, before, vm.bytesAllocated, vm.nextGC);
// // // #endif

// // }

// // void freeObjects() {
// //     Obj* obj = vm.objects;
// //     while (obj != NULL) {
// //         Obj* next = obj->next;
// //         freeObject(obj);
// //         obj = next;
// //     }

// //     // the memory for the gray stack is not managed by the GC
// //     free(vm.grayStack);
// // }

