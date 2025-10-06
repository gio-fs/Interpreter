#include <stdlib.h>
#include <stdio.h>
#include "clox_compiler.h"
#include "memory.h"
#include "common.h"
#include "vm.h"
#define GC_HEAP_GROW_FACTOR 2

#ifdef DEBUG_LOG_GC
#include <stdio.h>
#include "clox_debug.h"
#endif

void* reallocate(void* ptr, size_t oldSize, size_t newSize) {
    vm.bytesAllocated += newSize - oldSize;


    if (newSize > oldSize) {
    #ifdef DEBUG_STRESS_GC
        collectGarbage();
    #endif
    }


    if (vm.bytesAllocated > vm.nextGC && !vm.isCollecting) {
        collectGarbage();
    }

    if (newSize == 0) {
        free(ptr);
        return NULL;
    }

    void* result = realloc(ptr, newSize);
    if (result == NULL) exit(1);
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

void freeObject(Obj* obj) {
#ifdef DEBUG_LOG_GC
    // printf("[FREE_OBJ] %p type=%d\n", (void*)obj, obj->type);
#endif

    switch (obj->type) {
        case OBJ_FUNCTION: {
            ObjFunction* function = (ObjFunction*)obj;
            freeChunk(&function->chunk);
            FREE(ObjFunction, obj);
            break;
        }

        case OBJ_STRING: {
            ObjString* string = (ObjString*)obj;
#ifdef DEBUG_LOG_GC
            // printf("  len=%d size=%zu\n", string->length,
            //        sizeof(ObjString) + string->length + 1);
#endif
            // chars[] is a flexible array member: it's part of the same
            // memory and should be deallocated only one time
            reallocate(string, sizeof(ObjString) + string->length + 1, 0);
            break;
        }

        case OBJ_ARRAY: {
            ObjArray* arr = (ObjArray*)obj;
            reallocate(arr->values.values, sizeof(Value) * arr->values.capacity, 0);
            reallocate(arr, sizeof(ObjArray), 0);
            break;
        }

        case OBJ_NATIVE: {
            reallocate(obj, sizeof(ObjNative), 0);
            break;
        }

        case OBJ_CLOSURE: {
            ObjClosure* closure = (ObjClosure*)obj;
            reallocate(closure, sizeof(ObjClosure), 0);
            break;
        }

        case OBJ_UPVALUE: {
            reallocate(obj, sizeof(ObjUpvalue), 0);
            break;
        }

        case OBJ_DICTIONARY: {
            ObjDictionary* dict = (ObjDictionary*)obj;
            freeTable(&dict->map);
            reallocate(dict->entries.entries, sizeof(Entry) * dict->entries.capacity, 0);
            reallocate(dict, sizeof(ObjDictionary), 0);

            break;
        }
        case OBJ_CLASS: {
            ObjClass* klass = (ObjClass*)obj;
            freeTable(&klass->methods);
            freeTable(&klass->fields);
            FREE(ObjClass, obj);
            break;
        }
        case OBJ_INSTANCE: {
            ObjInstance* instance = (ObjInstance*)obj;
            freeTable(&instance->fields);
            FREE(ObjInstance, obj);
            break;
        }
        case OBJ_BOUND_METHOD: {
            FREE(ObjBoundMethod, obj);
            break;
        }
        case OBJ_RANGE: {
            FREE(ObjRange, obj);
            break;
        }
    }
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
    markCompilerRoots();
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

static void sweep(void) {
    Obj* previous = NULL;
    Obj* object = vm.objects;
    while (object != NULL) {
        if (object->isMarked) {
            object->isMarked = false;
            previous = object;
            object = object->next;
        } else {
            Obj* unreached = object;

            object = object->next;
            if (previous != NULL) {
                previous->next = object;
            } else {
                vm.objects = object;
            }

            freeObject(unreached);
        }
    }
}


void collectGarbage() {
#ifdef DEBUG_LOG_GC
    // for (int i = 0; i < 10000; i++) printf("--gc begin\n");
#endif
    vm.isCollecting = true;
    unsigned long long before = vm.bytesAllocated;

    markRoots();
    traceReferences();
    tableRemoveWhite(&vm.strings);
    sweep();

    unsigned long long survived = vm.bytesAllocated;
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

void freeObjects() {
    Obj* obj = vm.objects;
    while (obj != NULL) {
        Obj* next = obj->next;
        freeObject(obj);
        obj = next;
    }

    // the memory for the gray stack is not managed by the GC
    free(vm.grayStack);
}

