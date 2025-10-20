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
                ((uint8_t*)obj >= vHeap.aging.start \
                            && (uint8_t*)obj < vHeap.aging.start + vHeap.aging.bytesAllocated)
#define IS_IN_OLD(obj) \
                ((uint8_t*)obj >= vHeap.oldGen.start \
                            && (uint8_t*)obj < vHeap.oldGen.start + vHeap.oldGen.bytesAllocated)

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
                if (obj != NULL && obj->forwarded != NULL) { \
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
            if (func->name != NULL) {
                printValue(OBJ_VAL(func->name));
                printf("\n");
            }

            for (int i = 0; i < func->chunk.constants.count; i++) {
                ADJUST_INTERNAL_VALUE(&func->chunk.constants.values[i]);
            }

            break;
        }
        case OBJ_CLOSURE: {
            ObjClosure* closure = (ObjClosure*)obj;
            ADJUST_INTERNAL(closure->function);
            ADJUST_INTERNAL(closure->function->name);

            for (int i = 0; i < closure->function->chunk.constants.count; i++) {
                ADJUST_INTERNAL_VALUE(&closure->function->chunk.constants.values[i]);
            }

            for (int j = 0; j < closure->upvalueCount; j++) {
                if (closure->upvalues[j] != NULL) {
                    ADJUST_INTERNAL(closure->upvalues[j]);
                }
            }
            break;
        }
        case OBJ_DICTIONARY: {
            ObjDictionary* dict = (ObjDictionary*)obj;
            int entriesCount = 0;

            ADJUST_INTERNAL(dict->klass);

            for (int i = 0; i < dict->map.capacity; i++) {
                ADJUST_INTERNAL(dict->map.entries[i].key);
                ADJUST_INTERNAL_VALUE(&dict->map.entries[i].value);

                if (dict->map.entries[i].key != NULL) {
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
                ADJUST_INTERNAL(klass->methods.entries[i].key);
                ADJUST_INTERNAL_VALUE(&klass->methods.entries[i].value);
            }

            for (int i = 0; i < klass->fields.capacity; i++) {
                ADJUST_INTERNAL(klass->fields.entries[i].key);
                ADJUST_INTERNAL_VALUE(&klass->fields.entries[i].value);
            }
            break;
        }
        case OBJ_INSTANCE: {
            ObjInstance* instance = (ObjInstance*)obj;
            ADJUST_INTERNAL(instance->klass);

            for (int i = 0; i < instance->fields.capacity; i++) {
                ADJUST_INTERNAL(instance->fields.entries[i].key);
                ADJUST_INTERNAL_VALUE(&instance->fields.entries[i].value);
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



static void updateReferences() {
    // ValueArray worklist;
    // initValueArray(&worklist);

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

    uint8_t* start = vHeap.aging.start;
    uint8_t* end = vHeap.aging.start + vHeap.aging.bytesAllocated;

    while (start < end) {
        Obj* obj = (Obj*)start;
        ADJUST_REF(obj);
        updateFields(obj);
        start += obj->size;
    }

    uint8_t* oldStart = vHeap.oldGen.start;
    uint8_t* oldEnd = vHeap.oldGen.start + vHeap.oldGen.bytesAllocated;

    while (oldStart < oldEnd) {
        Obj* obj = (Obj*)oldStart;
        ADJUST_REF(obj);
        updateFields(obj);
        oldStart += obj->size;
    }

    // for (int i = 0; i < worklist.count; i++) {
    //    updateFields(AS_OBJ(worklist.values[i]), &worklist);
    // }

    // initValueArray(&worklist);
    #undef ADJUST_VALUE
    #undef ADJUST_REF
}

static void compactOldGen() {
    uint8_t* start = vHeap.oldGen.start;
    uint8_t* end = vHeap.oldGen.start + vHeap.oldGen.bytesAllocated;
    uint8_t* dest = vHeap.oldGen.start;

    // first we compute the forwarding destination,
    // then we update all refs and then we actually move
    // to forwarded. Another strategy is to use a semispace

    while (start < end) {
        Obj* curr = (Obj*)start;

        if (curr->isMarked) {
            curr->forwarded = (Obj*)dest;
            dest += curr->size;
        } else {
            curr->forwarded = NULL;
        }

        start += curr->size;
    }

    updateReferences();

    while (start < end) {
        Obj* curr = (Obj*)start;
        size_t currSize = curr->size;

        if (curr->forwarded != NULL) {
            memmove(curr->forwarded, curr, curr->size);
        }

        start += currSize;
    }

    vHeap.oldGen.bytesAllocated = dest - start;
}

static void promoteObjects() {
    uint8_t* start = vHeap.aging.start;
    uint8_t* end = vHeap.aging.start + vHeap.aging.bytesAllocated;
    uint8_t* dest = vHeap.semiSpace.start;


    while (start < end) {
        Obj* curr = (Obj*)start;
        size_t currSize = curr->size;

        if (curr->age == PROMOTING_AGE) {
            Obj* oldObj = (Obj*)writeHeap(&vHeap.oldGen, curr->size);
            memcpy(oldObj, curr, curr->size);
            curr->forwarded = oldObj;

        } else {
            Obj* young = (Obj*)writeHeap(&vHeap.semiSpace, curr->size);
            memcpy(young, curr, curr->size);
            curr->forwarded = young;
            young->age++;
            dest += currSize;
        }

        start += currSize;
    }

    updateReferences();

    uint8_t* temp = vHeap.aging.start;
    vHeap.aging.start = vHeap.semiSpace.start;
    vHeap.semiSpace.start = temp;
    vHeap.aging.bytesAllocated = vHeap.semiSpace.bytesAllocated;
    vHeap.semiSpace.bytesAllocated = 0;
}


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

size_t align(size_t size, size_t alignment) {
    size_t aligned = (size + alignment - 1) & ~(alignment - 1);
    return aligned;
}

void minorCollection();
void* writeNursery(Nursery* nursery, size_t size) {
    size_t aligned = align(size, ALIGNMENT);

    if (vHeap.oldGen.bytesAllocated > vm.nextGC
        && !vm.isCollecting) {

        collectGarbage();
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

static void growOldGen(size_t newSize) {
    size_t pageAligned = align(newSize, PAGE_SIZE);

    if (vHeap.oldGenCommit + pageAligned > vHeap.oldGen.size) {
        printf("Not enough virtual space. OldGen size is: %d, OldGenCommit is: %d, newSize is %d.\nExiting process...",
                vHeap.oldGen.size, vHeap.oldGenCommit, pageAligned);
        exit(1);
    }

    if (!commit(vHeap.oldGen.start, pageAligned)) {
        printf("OldGen growth failed\n");
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
            printf("Fatal error: aging overflow\n");
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

    Obj* newObj = (Obj*)writeHeap(&vHeap.aging, obj->size);
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

    uint8_t* start = vHeap.aging.start;
    uint8_t* end = vHeap.aging.start + vHeap.aging.bytesAllocated;

#ifdef DEBUG_LOG_GC
    fprintf(stderr, "[GC] Scanning aging space: %p to %p (%zu bytes)\n",
            (void*)start, (void*)end, vHeap.aging.bytesAllocated);
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

    uint8_t* oldStart = vHeap.oldGen.start;
    uint8_t* oldEnd = vHeap.oldGen.start + vHeap.oldGen.bytesAllocated;

#ifdef DEBUG_LOG_GC
    fprintf(stderr, "[GC] Scanning oldGen space: %p to %p (%zu bytes)\n",
            (void*)start, (void*)end, vHeap.aging.bytesAllocated);
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
    ObjUpvalue** upval = &vm.openUpvalues; // pointer to upvalue list
    while(*upval != NULL) {
        ObjUpvalue* old = *upval;
        *upval = (ObjUpvalue*)copyObject((Obj*)old);
        upval = &(*upval)->next;
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

    initValueArray(&vHeap.worklist);

#ifdef DEBUG_LOG_GC
    fprintf(stderr, "[GC] Nursery after reset: curr=%p used=0 free=%zu\n",
            (void*)vHeap.nursery.curr, vHeap.nursery.size);
    for (int i = 0; i < 10000; i++) fprintf(stderr, "[GC] ===== Minor collection end =====\n\n");
#endif
    vm.isCollecting = false;
    printf("Ended minor collection\n");
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

    vHeap.aging.size = AGING_SIZE;
    vHeap.agingOffset = NURSERY_SIZE;
    vHeap.aging.type = TYPE_AGING;

    vHeap.aging.start = vHeap.baseAddr + vHeap.agingOffset;
    if (!commit((void*)vHeap.baseAddr + vHeap.agingOffset, AGING_SIZE)) {
        printf("Aging commit failed. Exiting process...\n");
        exit(1);
    }

    vHeap.semiSpace.size = AGING_SIZE;
    vHeap.semiSpace.type = TYPE_AGING;

    vHeap.semiSpace.start = vHeap.baseAddr + vHeap.agingOffset + AGING_SIZE;
    if (!commit((void*)vHeap.baseAddr + vHeap.agingOffset + AGING_SIZE, AGING_SIZE)) {
        printf("Semi space commit failed. Exiting process...\n");
        exit(1);
    }

    vHeap.oldGen.size = OLDGEN_SIZE;
    vHeap.oldGenOffset = NURSERY_SIZE + 2 * AGING_SIZE;
    vHeap.oldGenCommit = OLDGEN_INITIAL_COMMIT;
    vHeap.oldGen.type = TYPE_OLDGEN;

    vHeap.oldGen.start = vHeap.baseAddr + vHeap.oldGenOffset;
    if (!commit((void*)vHeap.baseAddr + vHeap.oldGenOffset, OLDGEN_INITIAL_COMMIT)) {
        printf("OldGen commit failed. Exiting process...\n");
        exit(1);
    }

    vHeap.nursery.curr = vHeap.nursery.start;
    vHeap.aging.bytesAllocated = 0;
    vHeap.semiSpace.bytesAllocated = 0;
    vHeap.oldGen.bytesAllocated = 0;

    initValueArray(&vHeap.worklist);
}

void* reallocate(void* ptr, size_t oldSize, size_t newSize) {

    // if (vHeap.oldGen.bytesAllocated > vm.nextGC
    //     && !vm.isCollecting) {

    //     collectGarbage();
    // }

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

        if (vm.grayStack == NULL) exit(1);
    }

    vm.grayStack[vm.grayCount++] = obj;
}


void markValue(Value value) {
    if (!IS_OBJ(value)) return;
    markObj(AS_OBJ(value));
}

void markTable(Table* table) {
    for (int i = 0; i< table->capacity; i++) {
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
    markObj((Obj*)vm.array_NativeString);
    markObj((Obj*)vm.dict_NativeString);
    markObj((Obj*)vm.initString);
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

static void traceReferences() {
    while (vm.grayCount > 0) {
        Obj* obj = vm.grayStack[--vm.grayCount];
        blackenObject(obj);
    }
}

static void sweep() {
    compactOldGen();
}


void collectGarbage() {
#ifdef DEBUG_LOG_GC
    // for (int i = 0; i < 10000; i++) printf("--gc begin\n");
#endif
    vm.isCollecting = true;
    unsigned long long before = vHeap.oldGen.bytesAllocated;

    markRoots();
    traceReferences();
    tableRemoveWhite(&vm.strings);
    sweep();

    unsigned long long survived = vHeap.oldGen.bytesAllocated;
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


// #ifdef DEBUG_LOG_GC
    // for (int i = 0; i < 10000; i++) printf("--end\n");
    printf("    Collected %lld bytes (from %lld to %lld), next at %lld\n",
                    before - vm.bytesAllocated, before, vm.bytesAllocated, vm.nextGC);
// #endif

}


