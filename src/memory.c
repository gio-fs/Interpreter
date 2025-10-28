#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <inttypes.h>
#include <windows.h>
#include <assert.h>
#include "clox_compiler.h"
#include "memory.h"
#include "common.h"
#include "vm.h"

#ifdef DEBUG_LOG_GC
#include <stdio.h>
#include "clox_debug.h"
#endif

#define IS_IN_NURSERY(obj) \
                ((uint8_t*)obj >= vHeap.nursery.start \
                            && (uint8_t*)obj < vHeap.nursery.curr)
#define IS_IN_AGING(obj) \
                ((uint8_t*)obj >= vHeap.aging.from.start \
                            && (uint8_t*)obj < vHeap.aging.from.start + vHeap.aging.from.bytesAllocated)
#define IS_IN_OLD(obj) \
                ((uint8_t*)obj >= vHeap.oldGen.from.start \
                            && (uint8_t*)obj < vHeap.oldGen.from.start + vHeap.oldGen.from.bytesAllocated)

GenerationalHeap vHeap;

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

// there are three possible states for memory pages:
// - uncommitted, neither virtual nor physical address mapped
// - mapped, mapped with a virtual address but not in use
// - committedd, mapped and currently in use

// let the os search for a page
void* reserve(size_t size) {
    return VirtualAlloc(NULL, size, MEM_RESERVE, PAGE_NOACCESS);
}

// map to the virtual address
bool commit(void* addr, size_t size) {
    void* result = VirtualAlloc(addr, size, MEM_COMMIT, PAGE_READWRITE);
    return result != NULL;
}

// unmap the page
bool decommit(void* addr, size_t size) {
    return VirtualFree(addr, size, MEM_DECOMMIT);
}

void release(void* addr, size_t size) {
    VirtualFree(addr, 0, MEM_RELEASE);
}



static void updateFields(Obj* obj) {

    #define ADJUST_INTERNAL(obj) \
        do { \
            if (obj != NULL && ((Obj*)obj)->forwarded != NULL) { \
                obj = (__typeof__(obj))((Obj*)obj)->forwarded; \
            } \
        } while (0)

    #define ADJUST_INTERNAL_VALUE(value) \
        do { \
            if (IS_OBJ(*value)) { \
                Obj* obj = AS_OBJ(*value); \
            if (obj != NULL && ((Obj*)obj)->forwarded != NULL) { \
                    *value = OBJ_VAL(obj->forwarded);\
                } \
            } \
    } while (0)

    switch (obj->type) {
        case OBJ_UPVALUE: {
            ObjUpvalue* upval = (ObjUpvalue*)obj;
            ADJUST_INTERNAL_VALUE(&upval->closed);
            ADJUST_INTERNAL(upval->next);
            break;
        }
        case OBJ_FUNCTION: {
            ObjFunction* func = (ObjFunction*)obj;
            ADJUST_INTERNAL(func->name);

            for (int i = 0; i < func->chunk.constants.count; i++) {
                ADJUST_INTERNAL_VALUE(&func->chunk.constants.values[i]);
            }

            break;
        }
        case OBJ_CLOSURE: {
            ObjClosure* closure = (ObjClosure*)obj;
            ADJUST_INTERNAL(closure->function);

            for (int j = 0; j < closure->upvalueCount; j++) {
                if (closure->upvalues[j] != NULL) {
                    ADJUST_INTERNAL(closure->upvalues[j]);
                }
            }
            break;
        }
        case OBJ_DICTIONARY: {
            ObjDictionary* dict = (ObjDictionary*)obj;
            ADJUST_INTERNAL(dict->klass);
            size_t entriesCount = 0;

            for (int i = 0; i < dict->map.capacity; i++) {
                if (dict->map.entries[i].key != NULL) {
                    ADJUST_INTERNAL(dict->map.entries[i].key);
                    ADJUST_INTERNAL_VALUE(&dict->map.entries[i].value);

                    dict->entries.entries[entriesCount].key = dict->map.entries[i].key;
                    dict->entries.entries[entriesCount++].value = dict->map.entries[i].value;
                }

            }

            break;
        }
        case OBJ_ARRAY: {
            ObjArray* arr = (ObjArray*)obj;
            ADJUST_INTERNAL(arr->klass);

            for (int i = 0; i < arr->values.count; i++) {
                ADJUST_INTERNAL_VALUE(&arr->values.values[i]);
            }

            break;
        }
        case OBJ_CLASS: {
            ObjClass* klass = (ObjClass*)obj;
            ADJUST_INTERNAL(klass->name);

            for (int i = 0; i < klass->methods.capacity; i++) {
                if (klass->methods.entries[i].key != NULL) {
                    ADJUST_INTERNAL(klass->methods.entries[i].key);
                    ADJUST_INTERNAL_VALUE(&klass->methods.entries[i].value);
                }
            }

            for (int i = 0; i < klass->fields.capacity; i++) {
                if (klass->fields.entries[i].key != NULL) {
                    ADJUST_INTERNAL(klass->fields.entries[i].key);
                    ADJUST_INTERNAL_VALUE(&klass->fields.entries[i].value);
                }
            }
            break;
        }
        case OBJ_INSTANCE: {
            ObjInstance* instance = (ObjInstance*)obj;
            ADJUST_INTERNAL(instance->klass);

            for (int i = 0; i < instance->fields.capacity; i++) {
                if (instance->fields.entries[i].key != NULL) {
                    ADJUST_INTERNAL(instance->fields.entries[i].key);
                    ADJUST_INTERNAL_VALUE(&instance->fields.entries[i].value);
                }
            }
            break;
        }
        case OBJ_BOUND_METHOD: {
            ObjBoundMethod* bound = (ObjBoundMethod*)obj;
            ADJUST_INTERNAL_VALUE(&bound->receiver);
            ADJUST_INTERNAL(bound->method);
            break;
        }
        case OBJ_NATIVE:
        case OBJ_STRING:
        case OBJ_RANGE:

            break;
    }

    #undef ADJUST_INTERNAL
    #undef ADJUST_INTERNAL_VALUE
}

static void scanAndUpdate(Heap* heap) {
    uint8_t* start = heap->start;
    uint8_t* end = heap->start + heap->bytesAllocated;

    while (start < end) {
        Obj* obj = (Obj*)start;
        updateFields(obj);
        start += obj->size;
    }
}

static void scanAndUpdateNursery() {
    uint8_t* start = vHeap.nursery.start;
    uint8_t* end = vHeap.nursery.curr;

    while (start < end) {
        Obj* obj = (Obj*)start;
        updateFields(obj);
        start += obj->size;
    }
}

#define ADJUST_VALUE(value) \
        do { \
            if (IS_OBJ(*value)) { \
                Obj* obj = AS_OBJ(*value); \
                if (obj != NULL && obj->forwarded != NULL) { \
                    *value = OBJ_VAL(obj->forwarded);\
                } \
            } \
        } while (0)

#define ADJUST_REF(obj) \
        do { \
            if (obj != NULL && ((Obj*)obj)->forwarded != NULL) { \
                obj = (__typeof__(obj))((Obj*)obj)->forwarded; \
            } \
        } while (0)

static void updateReferences() {
    // ValueArray worklist;
    // initValueArray(&worklist);

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
        if (entry->key != NULL) {
            ADJUST_REF(entry->key);
            ADJUST_VALUE(&entry->value);
        }

    }

    for (int i = 0; i < vm.constGlobals.capacity; i++) {
        Entry* entry = &vm.constGlobals.entries[i];
        if (entry->key != NULL) {
            ADJUST_REF(entry->key);
            ADJUST_VALUE(&entry->value);
        }

    }

    for (int i = 0; i < vm.strings.capacity; i++) {
        Entry* entry = &vm.strings.entries[i];
        if (entry->key != NULL) {
            ADJUST_REF(entry->key);
        }

    }

    ADJUST_REF(vm.array_NativeString);
    ADJUST_REF(vm.dict_NativeString);
    ADJUST_REF(vm.initString);
    ADJUST_REF(vm.arrayClass);
    ADJUST_REF(vm.dictClass);


    for (int i = 0; i < vHeap.aging.dirty.count; i++) {
        ADJUST_REF(vHeap.aging.dirty.objects[i]);
        updateFields(vHeap.aging.dirty.objects[i]);
    }

    for (int i = 0; i < vHeap.oldGen.dirty.count; i++) {
        ADJUST_REF(vHeap.oldGen.dirty.objects[i]);
        updateFields(vHeap.oldGen.dirty.objects[i]);
    }


    if (vm.isInMinor) {
        #pragma omp parallel sections
        {
            #pragma omp section
            scanAndUpdate(&vHeap.aging.to);

            #pragma omp section
            scanAndUpdate(&vHeap.oldGen.from);
        }
    } else if (vm.isInMajor) {
        #pragma omp parallel sections
        {
            #pragma omp section
            scanAndUpdateNursery();

            #pragma omp section
            scanAndUpdate(&vHeap.aging.from);

            #pragma omp section
            scanAndUpdate(&vHeap.oldGen.to);
        }

    }

}

static void clearMarkBits() {
    uint8_t* start = vHeap.aging.from.start;
    uint8_t* end = vHeap.aging.from.start + vHeap.aging.from.bytesAllocated;

    while (start < end) {
        Obj* obj = (Obj*)start;
        obj->isMarked = false;
        start += obj->size;
    }

    start = vHeap.oldGen.from.start;
    end = vHeap.oldGen.from.start + vHeap.oldGen.from.bytesAllocated;

    while (start < end) {
        Obj* obj = (Obj*)start;
        obj->isMarked = false;
        start += obj->size;
    }
}

static void clearForwardings() {
    uint8_t* start = vHeap.aging.from.start;
    uint8_t* end = vHeap.aging.from.start + vHeap.aging.from.bytesAllocated;

    while (start < end) {
        Obj* obj = (Obj*)start;
        obj->forwarded = NULL;
        start += obj->size;
    }

    start = vHeap.oldGen.from.start;
    end = vHeap.oldGen.from.start + vHeap.oldGen.from.bytesAllocated;

    while (start < end) {
        Obj* obj = (Obj*)start;
        obj->forwarded = NULL;
        start += obj->size;
    }
}

static void compactOldGen() {
    uint8_t* start = vHeap.oldGen.from.start;
    uint8_t* end = vHeap.oldGen.from.start + vHeap.oldGen.from.bytesAllocated;

    if (!commit(vHeap.oldGen.to.start, vHeap.oldGenCommit)) {
        printf("OldGen.to commit failed. Exiting process...\n");
        exit(1);
    }

    while (start < end) {
        Obj* curr = (Obj*)start;

        if (curr->isMarked) {
            Obj* survived = writeHeap(&vHeap.oldGen.to, curr->size);
            memcpy(survived, curr, curr->size);
            curr->forwarded = survived;

        }

        start += curr->size;
    }

    updateReferences();

    uint8_t* temp = vHeap.oldGen.from.start;
    vHeap.oldGen.from.start = vHeap.oldGen.to.start;
    vHeap.oldGen.to.start = temp;
    vHeap.oldGen.from.bytesAllocated = vHeap.oldGen.to.bytesAllocated;
    vHeap.oldGen.to.bytesAllocated = 0;

    decommit(vHeap.oldGen.to.start, vHeap.oldGenCommit);
}

static void promoteObjects() {
    uint8_t* start = vHeap.aging.from.start;
    uint8_t* end = vHeap.aging.from.start + vHeap.aging.from.bytesAllocated;

    while (start < end) {
        Obj* curr = (Obj*)start;
        size_t currSize = curr->size;

        if (curr->age == PROMOTING_AGE) {
            Obj* oldObj = (Obj*)writeHeap(&vHeap.oldGen.from, curr->size);
            memcpy(oldObj, curr, curr->size);
            curr->forwarded = oldObj;

        } else {
            Obj* young = (Obj*)writeHeap(&vHeap.aging.to, curr->size);
            memcpy(young, curr, curr->size);
            curr->forwarded = young;
            young->age++;
        }

        start += currSize;
    }



    updateReferences();

    uint8_t* temp = vHeap.aging.from.start;
    vHeap.aging.from.start = vHeap.aging.to.start;
    vHeap.aging.to.start = temp;
    vHeap.aging.from.bytesAllocated = vHeap.aging.to.bytesAllocated;
    vHeap.aging.to.bytesAllocated = 0;
}


size_t align(size_t size, size_t alignment) {
    size_t aligned = (size + alignment - 1) & ~(alignment - 1);
    return aligned;
}

void minorCollection();
void* writeNursery(Nursery* nursery, size_t size) {
    size_t aligned = align(size, ALIGNMENT);

    if (vHeap.oldGen.from.bytesAllocated > vm.nextGC
        && !vm.isCollecting) {

        majorCollection();
    }

    if ((nursery->curr + aligned) > (nursery->start + NURSERY_SIZE)) {
        if (vm.isCollecting) {
            fprintf(stderr, "FATAL: Nursery overflow while GC disabled\n");
            exit(1);
        }
        minorCollection();
    }

    void* result = nursery->curr;
    nursery->curr += aligned;
    return result;
}

static void growOldGen(size_t newSize) {
    size_t pageAligned = align(newSize, PAGE_SIZE);

    if (vHeap.oldGenCommit + pageAligned > vHeap.oldGen.from.size) {
        printf("Not enough virtual space. OldGen size is: %d, OldGenCommit is: %d, newSize is %d.\nExiting process...",
                vHeap.oldGen.from.size, vHeap.oldGenCommit, pageAligned);
        exit(1);
    }

    if (!commit(vHeap.oldGen.from.start, pageAligned)) {
        printf("OldGen.from growth failed\n");
        exit(1);
    }

    if (!commit(vHeap.oldGen.to.start, pageAligned)) {
        printf("OldGen.to growth failed\n");
        exit(1);
    }

    vHeap.oldGenCommit = pageAligned;
    printf("OldGen grown\n");
}

void* writeHeap(Heap* heap, size_t size) {
    size = align(size, ALIGNMENT);
#ifdef DEBUG_LOG_GC
    fprintf(stderr, "[GC] writeHeap: Heap=%p size=%zu aligned=%zu bytesAllocated=%zu capacity=%zu\n",
            (void*)heap->start, size, size, heap->bytesAllocated, heap->size);
#endif
    heap->bytesAllocated += size;
    if (heap->bytesAllocated >= vHeap.oldGenCommit) {
#ifdef DEBUG_LOG_GC
        fprintf(stderr, "[GC] writeHeap: need grow, bytesAllocated=%zu > size=%zu\n",
                heap->bytesAllocated, heap->size);
#endif
        printf("Entering growOldGen...\n");
        if (heap->type == TYPE_OLDGEN) growOldGen(vHeap.oldGenCommit * OLDGEN_GROW_FACTOR);
        else {
            printf("Fatal error: aging overflow");
            exit(1);
        }

#ifdef DEBUG_LOG_GC
        fprintf(stderr, "[GC] writeHeap: after grow, capacity=%zu base=%p\n", heap->size, (void*)heap->start);
#endif
    }

    void* result = heap->start + heap->bytesAllocated - size;
    if (result == NULL) {
        printf("HeapAlloc failed");
        exit(1);
    }
#ifdef DEBUG_LOG_GC
    fprintf(stderr, "[GC] writeHeap: allocated block=%p size=%zu new-bytesAllocated=%zu\n",
            (void*)result, size, heap->bytesAllocated);
#endif
    return result;
}

void markDirty(Obj* obj) {
    if (obj->isDirty == true) return;
    if (IS_IN_AGING(obj)) {

        if (vHeap.aging.dirty.capacity < vHeap.aging.dirty.count + 1) {
            vHeap.aging.dirty.capacity = GROW_CAPACITY(vHeap.aging.dirty.capacity);
            vHeap.aging.dirty.objects = realloc(vHeap.aging.dirty.objects, sizeof(Obj*) * vHeap.aging.dirty.capacity);

            if (vHeap.aging.dirty.objects == NULL) {
                printf("Failed to realloc dirty.objects\n");
                exit(1);
            }
        }

        vHeap.aging.dirty.objects[vHeap.aging.dirty.count++] = obj;
        obj->isDirty = true;

    } else if (IS_IN_OLD(obj)) {

        if (vHeap.oldGen.dirty.capacity < vHeap.oldGen.dirty.count + 1) {
            vHeap.oldGen.dirty.capacity = GROW_CAPACITY(vHeap.oldGen.dirty.capacity);
            vHeap.oldGen.dirty.objects = realloc(vHeap.oldGen.dirty.objects, sizeof(Obj*) * vHeap.oldGen.dirty.capacity);

            if (vHeap.aging.dirty.objects == NULL) {
                printf("Failed to realloc dirty.objects\n");
                exit(1);
            }
        }

        vHeap.oldGen.dirty.objects[vHeap.oldGen.dirty.count++] = obj;
        obj->isDirty = true;
    } else {
        // nothing, obj is in nursery
    }
}

static Obj* copyObject(Obj* obj) {
#ifdef DEBUG_LOG_GC
    fprintf(stderr, "[GC] copyObject: old=%p type=%s size=%zu age=%u fwd=%p\n",
            (void*)obj, objTypeName(obj->type), obj->size, obj->age, (void*)obj->forwarded);
#endif

    if (IS_IN_AGING(obj) || IS_IN_OLD(obj)) {
#ifdef DEBUG_LOG_GC
        fprintf(stderr, "[GC] copyObject: is in aging or oldGen %p\n", (void*)obj->forwarded);
#endif
        return obj;
    }

    if (obj->forwarded != NULL) {
#ifdef DEBUG_LOG_GC
        fprintf(stderr, "[GC] copyObject: already forwarded to %p\n", (void*)obj->forwarded);
#endif
        return obj->forwarded;
    }

    Obj* newObj = (Obj*)writeHeap(&vHeap.aging.from, obj->size);
    obj->forwarded = newObj;
#ifdef DEBUG_LOG_GC
    fprintf(stderr, "[GC] copyObject: memcpy new=%p <- old=%p bytes=%zu\n",
            (void*)newObj, (void*)obj, obj->size);
#endif
    memcpy(newObj, obj, obj->size);
    newObj->forwarded = NULL;

#ifdef DEBUG_LOG_GC
    fprintf(stderr, "[GC] copyObject: enqueued new object %p (type=%s)\n",
            (void*)newObj, objTypeName(newObj->type));
#endif
    writeValueArray(&vHeap.worklist, OBJ_VAL(newObj));
    return newObj;
}

static void copyValue(Value* value) {
#ifdef DEBUG_LOG_GC
    fprintf(stderr, "[GC] copyValue: slot=%p type=%d\n", (void*)value, value->type);
#endif
    if (value->type != VAL_OBJ) return;
    if (IS_IN_AGING(AS_OBJ(*value)) || IS_IN_OLD(AS_OBJ(*value))) return;

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

            break;
    }
}


static void scanOldGenerations() {
#ifdef DEBUG_LOG_GC
    fprintf(stderr, "[GC] Scanning old generations for nursery references\n");
#endif

    for (int i = 0; i < vHeap.aging.dirty.count; i++) {
        Obj* dirty = vHeap.aging.dirty.objects[i];
        scanObjectFields(dirty);
        dirty->isDirty = false;
    }

    for (int i = 0; i < vHeap.oldGen.dirty.count; i++) {
        Obj* dirty = vHeap.oldGen.dirty.objects[i];
        scanObjectFields(dirty);
        dirty->isDirty = false;
    }

    vHeap.aging.dirty.count = 0;
    vHeap.oldGen.dirty.count = 0;

}

static void copyReferences() {
#ifdef DEBUG_LOG_GC
    fprintf(stderr, "[GC] copyReferences: worklist.count=%d\n", vHeap.worklist.count);
#endif

    for (int i = 0; i < vHeap.worklist.count; i++) {
        Value* value = &vHeap.worklist.values[i];

        if ((*value).type != VAL_OBJ) continue;
        scanObjectFields(AS_OBJ(*value));
    }
}


void minorCollection() {
    vm.isCollecting = true;
    vm.isInMinor = true;

#ifdef DEBUG_LOG_GC
    fprintf(stderr, "\n[GC] ===== Minor collection begin =====\n");
#endif

#ifdef DEBUG_LOG_GC
    size_t used = (size_t)(vHeap.nursery.curr - vHeap.nursery.start);
    fprintf(stderr, "[GC] Nursery before: used=%zu free=%zu\n", used, vHeap.nursery.size - used);
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
    for (int i = 0; i < 5000; i++) fprintf(stderr, "[GC] Roots: openUpvalues head=%p\n", (void*)vm.openUpvalues);
#endif
    for (ObjUpvalue* upvalue = NULL; vm.openUpvalues != NULL; upvalue = upvalue->next) {
        copyObject((Obj*)upvalue);
    }


#ifdef DEBUG_LOG_GC
    for (int i = 0; i < 5000; i++) fprintf(stderr, "[GC] Roots: queues levels=%d\n", vm.nestingLevel + 1);
#endif
    for (int i = 0; i <= vm.nestingLevel; i++) {
        for (int j = 0; j < vm.queueCount[i]; j++) {
            copyValue(&vm.queue[i].values[j]);
#ifdef DEBUG_LOG_GC
            fprintf(stderr, "[GC] Root queue[%d][%d] updated\n", i, j);
#endif
        }
    }


#ifdef DEBUG_LOG_GC
    for (int i = 0; i < 5000; i++)  fprintf(stderr, "[GC] Roots: globals\n");
#endif
    copyTable(&vm.globals);

#ifdef DEBUG_LOG_GC
    for (int i = 0; i < 5000; i++) fprintf(stderr, "[GC] Roots: constGlobals\n");
#endif
    copyTable(&vm.constGlobals);

#ifdef DEBUG_LOG_GC
    for (int i = 0; i < 5000; i++) fprintf(stderr, "[GC ROOT] Roots: strings\n");
#endif
    copyTable(&vm.strings);


    ObjString* oldArr = vm.array_NativeString;
    vm.array_NativeString = (ObjString*)copyObject((Obj*)vm.array_NativeString);
#ifdef DEBUG_LOG_GC
    for (int i = 0; i < 5000; i++)  fprintf(stderr, "[GC ROOT] Root array_NativeString: old=%p -> new=%p\n", (void*)oldArr, (void*)vm.array_NativeString);
#endif

    ObjString* oldDict = vm.dict_NativeString;
    vm.dict_NativeString = (ObjString*)copyObject((Obj*)vm.dict_NativeString);
#ifdef DEBUG_LOG_GC
    for (int i = 0; i < 5000; i++) fprintf(stderr, "[GC ROOT] Root dict_NativeString: old=%p -> new=%p\n", (void*)oldDict, (void*)vm.dict_NativeString);
#endif

    ObjString* oldInit = vm.initString;
    vm.initString = (ObjString*)copyObject((Obj*)vm.initString);
#ifdef DEBUG_LOG_GC
    for (int i = 0; i < 5000; i++) fprintf(stderr, "[GC ROOT] Root initString: old=%p -> new=%p\n", (void*)oldInit, (void*)vm.initString);
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

    scanOldGenerations();
    copyReferences();

    vHeap.nursery.curr = vHeap.nursery.start;
    promoteObjects();

    clearForwardings();
    initValueArray(&vHeap.worklist);

#ifdef DEBUG_LOG_GC
    fprintf(stderr, "[GC] Nursery after reset: curr=%p used=0 free=%zu\n",
            (void*)vHeap.nursery.curr, vHeap.nursery.size);
    for (int i = 0; i < 10000; i++) fprintf(stderr, "[GC] ===== Minor collection end =====\n\n");
#endif
    vm.isCollecting = false;
    vm.isInMinor = false;
    printf("Ended minor collection. Aging size is %lld. OldGen size is %lld.\n", vHeap.aging.from.bytesAllocated, vHeap.oldGen.from.bytesAllocated);
}


void initGenHeap() {
    vHeap.reservedSize = RESERVED_SIZE;
    vHeap.baseAddr = (uint8_t*)reserve(vHeap.reservedSize);

    if (vHeap.baseAddr == NULL) {
        printf("baseAddr reserve failed. Exiting process...\n");
        exit(1);
    }

    vHeap.nursery.size = NURSERY_SIZE;
    vHeap.nurseryOffset = 0;

    vHeap.nursery.start = vHeap.baseAddr;
    if (!commit((void*)vHeap.baseAddr, NURSERY_SIZE)) {
        printf("Nursery commit failed. Exiting process...\n");
        exit(1);
    }

    vHeap.aging.from.size = AGING_SIZE;
    vHeap.agingOffset = NURSERY_SIZE;
    vHeap.aging.from.type = TYPE_AGING;

    vHeap.aging.from.start = vHeap.baseAddr + vHeap.agingOffset;
    if (!commit((void*)vHeap.baseAddr + vHeap.agingOffset, AGING_SIZE)) {
        printf("Aging commit failed. Exiting process...\n");
        exit(1);
    }

    vHeap.aging.to.size = AGING_SIZE;
    vHeap.aging.to.type = TYPE_AGING;

    vHeap.aging.to.start = vHeap.baseAddr + vHeap.agingOffset + AGING_SIZE;
    if (!commit((void*)vHeap.baseAddr + vHeap.agingOffset + AGING_SIZE, AGING_SIZE)) {
        printf("Semi space commit failed. Exiting process...\n");
        exit(1);
    }

    vHeap.oldGen.from.size = OLDGEN_SIZE / 2;
    vHeap.oldGenOffset = NURSERY_SIZE + 2 * AGING_SIZE;
    vHeap.oldGenCommit = OLDGEN_INITIAL_COMMIT;
    vHeap.oldGen.from.type = TYPE_OLDGEN;

    vHeap.oldGen.from.start = vHeap.baseAddr + vHeap.oldGenOffset;
    if (!commit((void*)vHeap.baseAddr + vHeap.oldGenOffset, OLDGEN_INITIAL_COMMIT)) {
        printf("OldGen commit failed. Exiting process...\n");
        exit(1);
    }

    vHeap.oldGen.to.size = OLDGEN_SIZE / 2;
    vHeap.oldGen.to.type = TYPE_OLDGEN;
    vHeap.oldGen.to.start = vHeap.baseAddr + vHeap.oldGenOffset + vHeap.oldGen.from.size;

    vHeap.nursery.curr = vHeap.nursery.start;
    vHeap.aging.from.bytesAllocated = 0;
    vHeap.aging.to.bytesAllocated = 0;
    vHeap.oldGen.from.bytesAllocated = 0;
    vHeap.oldGen.to.bytesAllocated = 0;

    vHeap.aging.dirty.capacity = 8192;
    vHeap.aging.dirty.count = 0;
    vHeap.aging.dirty.objects = realloc(vHeap.aging.dirty.objects, sizeof(Obj*) * vHeap.aging.dirty.capacity);

    vHeap.oldGen.dirty.capacity = 8192;
    vHeap.oldGen.dirty.count = 0;
    vHeap.oldGen.dirty.objects = realloc(vHeap.oldGen.dirty.objects, sizeof(Obj*) * vHeap.oldGen.dirty.capacity);


    initValueArray(&vHeap.worklist);
}

void* reallocate(void* ptr, size_t oldSize, size_t newSize) {

    if (newSize == 0) {
        free(ptr);
        return NULL;
    }

    void* result = realloc(ptr, newSize);
    if (result == NULL) {
        printf("realloc didn't realloc");
        exit(1);
    }
    return result;
}


void clearDirtyBits() {
    uint8_t* ptr = vHeap.aging.from.start;
    while (ptr < vHeap.aging.from.start + vHeap.aging.from.bytesAllocated) {
        Obj* obj = (Obj*)ptr;
        obj->isDirty = false;
        ptr += obj->size;
    }

    ptr = vHeap.oldGen.from.start;
    while (ptr < vHeap.oldGen.from.start + vHeap.oldGen.from.bytesAllocated) {
        Obj* obj = (Obj*)ptr;
        obj->isDirty = false;
        ptr += obj->size;
    }
}

void markObj(Obj* obj) {
    if (obj == NULL) return;
    if (obj->isMarked) return;

#ifdef DEBUG_LOG_GC
    // printf("%p mark ", (void*)obj);
    // printValue(OBJ_VAL(obj));
    // printf("\n");
#endif
    // for (int i = 0; i < 9000; i++) printf("[MARKOBJ] %p type=%d marked=%d grayCount=%d\n",
    //   (void*)obj, obj->type, obj->isMarked, vm.grayCount);


    obj->isMarked = true;

    if (vm.grayCapacity < vm.grayCount + 1) {
        vm.grayCapacity = GROW_CAPACITY(vm.grayCapacity);
        vm.grayStack = realloc(vm.grayStack, sizeof(Obj*) * vm.grayCapacity);

        if (vm.grayStack == NULL) {
            printf("Failed to realloc grayStack\n");
            exit(1);
        }
    }

    vm.grayStack[vm.grayCount++] = obj;
}


void markValue(Value value) {
    if (!IS_OBJ(value)) return;
    markObj(AS_OBJ(value));
}

void markTable(Table* table) {
    for (int i = 0; i < table->capacity; i++) {
        Entry* entry = &table->entries[i];
        markObj((Obj*)entry->key);
        markValue(entry->value);
    }
}

void markArray(ValueArray* arr) {
    for (int i = 0; i < arr->count; i++) {
        markValue(arr->values[i]);
    }
}

static void blackenObject(Obj* obj) {
#ifdef DEBUG_LOG_GC
    // fprintf(stderr, "[BLACKEN] %p type=%s\n", (void*)obj, typeName(obj->type));
#endif

    switch (obj->type) {
        case OBJ_UPVALUE: {
#ifdef DEBUG_LOG_GC
            // fprintf(stderr, "  -> mark upvalue->closed\n");
#endif
            markValue(((ObjUpvalue*)obj)->closed);
            break;
        }
        case OBJ_FUNCTION: {
            ObjFunction* func = (ObjFunction*)obj;
#ifdef DEBUG_LOG_GC
            // fprintf(stderr, "  -> mark name=%p\n", (void*)func->name);
            // fprintf(stderr, "  -> mark %d constants\n", func->chunk.constants.count);
#endif
            markObj((Obj*)func->name);
            markArray(&func->chunk.constants);
            break;
        }
        case OBJ_CLOSURE: {
            ObjClosure* closure = (ObjClosure*)obj;
#ifdef DEBUG_LOG_GC
            // fprintf(stderr, "  -> mark function=%p\n", (void*)closure->function);
            // fprintf(stderr, "  -> mark %d upvalues\n", closure->upvalueCount);
#endif
            markObj((Obj*)closure->function);
            for (int i = 0; i < closure->upvalueCount; i++) {
                markObj((Obj*)closure->upvalues[i]);
            }
            break;
        }
        case OBJ_DICTIONARY: {
            ObjDictionary* dict = (ObjDictionary*)obj;
#ifdef DEBUG_LOG_GC
            // fprintf(stderr, "  -> mark dictionary map\n");
#endif
            markTable(&dict->map);
            for (int i = 0; i < dict->entries.count; i++) {
                markObj((Obj*)dict->entries.entries[i].key);
                markValue(dict->entries.entries[i].value);
            }
            markObj((Obj*)dict->klass);
            break;
        }
        case OBJ_ARRAY: {
            ObjArray* arr = (ObjArray*)obj;
#ifdef DEBUG_LOG_GC
            // fprintf(stderr, "  -> mark array values (%d)\n", ((ObjArray*)obj)->values.count);
#endif
            markArray(&arr->values);
            markObj((Obj*)arr->klass);
            break;
        }
        case OBJ_CLASS: {
            ObjClass* klass = (ObjClass*)obj;
            markObj((Obj*)klass->name);
            markTable(&klass->methods);
            markTable(&klass->fields);
            break;
        }
        case OBJ_INSTANCE: {
            ObjInstance* instance = (ObjInstance*)obj;
            markObj((Obj*)instance->klass);
            markTable(&instance->fields);
            break;
        }
        case OBJ_BOUND_METHOD: {
            ObjBoundMethod* bound = (ObjBoundMethod*)obj;
            markValue(bound->receiver);
            markObj((Obj*)bound->method);
            break;
        }
        case OBJ_NATIVE:
        case OBJ_STRING:
        case OBJ_RANGE:

            break;
    }
}


static void markRoots() {
    // traversing stack
    for (Value* slot = vm.stack.values; slot < vm.stackTop; slot++) {
        markValue(*slot);
    }

    // traversing callframe array
    for (int i = 0; i < vm.frameArray.count; i++) {
        markObj((Obj*)vm.frameArray.frames[i].closure);
    }

    // traversing upvalue linked list
    for (ObjUpvalue* upvalue = NULL; vm.openUpvalues != NULL; upvalue = upvalue->next) {
        markObj((Obj*)upvalue);
    }

    for (int i = 0; i <= vm.nestingLevel; i++) {
        for (int j = 0; j < vm.queueCount[i]; j++) {
            markValue(vm.queue[i].values[j]);
        }
    }

    markTable(&vm.globals);
    markTable(&vm.constGlobals);
    markTable(&vm.strings);
    markObj((Obj*)vm.array_NativeString);
    markObj((Obj*)vm.dict_NativeString);
    markObj((Obj*)vm.initString);
    markObj((Obj*)vm.dictClass);
    markObj((Obj*)vm.arrayClass);
}

static const char* typeName(ObjType type) {
    switch (type) {
        case OBJ_FUNCTION:   return "function";
        case OBJ_STRING:     return "string";
        case OBJ_ARRAY:      return "array";
        case OBJ_NATIVE:     return "native";
        case OBJ_CLOSURE:    return "closure";
        case OBJ_UPVALUE:    return "upvalue";
        case OBJ_DICTIONARY: return "dictionary";
        case OBJ_CLASS:      return "class";
        default:             return "unknown";
    }
}


static void traceReferences() {
    while (vm.grayCount > 0) {
        Obj* obj = vm.grayStack[--vm.grayCount];
        blackenObject(obj);
    }
}

static void sweep() {
    compactOldGen();
}

static void markFromYoung() {

    uint8_t* start = vHeap.nursery.start;
    uint8_t* end = vHeap.nursery.curr;

    while (end > start) {
        Obj* curr = (Obj*)start;
        blackenObject(curr);
        start += curr->size;
    }

    start = vHeap.aging.from.start;
    end = vHeap.aging.from.start + vHeap.aging.from.bytesAllocated;

    while (end > start) {
        Obj* curr = (Obj*)start;
        blackenObject(curr);
        start += curr->size;
    }

}

void majorCollection() {
#ifdef DEBUG_LOG_GC
    // for (int i = 0; i < 10000; i++) printf("--gc begin\n");
#endif
    vm.isCollecting = true;
    vm.isInMajor = true;
    size_t before = vHeap.oldGen.from.bytesAllocated;

    markFromYoung();
    markRoots();
    traceReferences();
    sweep();

    size_t survived = vHeap.oldGen.from.bytesAllocated;
    double rate = (double)(survived / before);

    if (rate > 0.75) {
        vm.nextGC = survived * 4;
    } else if (rate > 0.5) {
        vm.nextGC = survived * 3;
    } else {
        vm.nextGC = survived * 2;
    }

    if (vm.nextGC < 1024 * 1024) {
        vm.nextGC = 1024 * 1024;
    }

    clearForwardings();
    clearMarkBits();
    vm.isCollecting = false;
    vm.isInMajor = false;

// #ifdef DEBUG_LOG_GC
    // for (int i = 0; i < 10000; i++) printf("--end\n");
    printf("    Collected %lld bytes (from %lld to %lld), next at %lld\n",
                    before - survived, before, survived, vm.nextGC);
// #endif

}

