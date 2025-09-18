#include <stdlib.h>
#include "clox_compiler.h"
#include "memory.h"
#include "common.h"
#include "vm.h"


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

    if(newSize == 0) {
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
    printf("%p mark ", (void*)obj);
    printValue(OBJ_VAL(obj));
    printf("\n");
#endif

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
    printf("%p free type %d\n", (void*)obj, obj->type);
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
            FREE(ObjString, obj);
            break;     
        }
        case OBJ_ARRAY: {
            ObjArray* arr = (ObjArray*)obj;
            FREE(ObjArray, obj);
            break;     
        }
        case OBJ_NATIVE: {
            FREE(ObjNative, obj);
            break;     
        }
        case OBJ_CLOSURE: {
            ObjClosure* closure = (ObjClosure*)obj;
            FREE(ObjClosure, obj);
            break;     
        }
        case OBJ_UPVALUE: {
            ObjUpvalue* upvalue = (ObjUpvalue*)obj;
            FREE(ObjUpvalue, obj);
            break;     
        }
        case OBJ_DICTIONARY: {
            ObjDictionary* upvalue = (ObjDictionary*)obj;
            FREE(ObjDictionary, obj);
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

    markTable(&vm.globals);
    markTable(&vm.constGlobals);
    markCompilerRoots();
}

static void blackenObject(Obj* obj) {
#ifdef DEBUG_LOG_GC
    printf("%p blacken ", (void*)obj);
    printValue(OBJ_VAL(obj));
    printf("/n");
#endif

    switch (obj->type) {
        case OBJ_UPVALUE: 
            markValue(((ObjUpvalue*)obj)->closed);
            break;
        case OBJ_FUNCTION:
            ObjFunction* func = (ObjFunction*)obj;
            markObj((Obj*)func->name);
            markArray(&func->chunk.constants);
            break;
        case OBJ_CLOSURE: 
            ObjClosure* closure = (ObjClosure*)closure;
            markObj((Obj*)closure->function);
            for (int i = 0; i < closure->upvalueCount; i++) {
                markObj((Obj*)closure->upvalues[i]);
            }
            break;
        case OBJ_DICTIONARY:
            ObjDictionary* dict = (ObjDictionary*)dict;
            markTable(&dict->map);
            for (int i = 0; i < dict->map.count; i++) {
                markObj((Obj*)dict->entries.entries[i].key);
                markValue(dict->entries.entries[i].value);
            }
            break;
        case OBJ_ARRAY:
            markArray(&((ObjArray*)obj)->values);
            break;

        case OBJ_NATIVE:
        case OBJ_STRING:
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
    Obj* prev = NULL;
    Obj* obj = vm.objects;
    while (obj != NULL) {
        if (obj->isMarked) {
            obj->isMarked = false; // we need the objects to be white in the next collection
            prev = obj;
            obj = obj->next;
        } else {
            Obj* unreached = obj;

            // updating linked list when finding an unreached obj
            obj = obj->next;
            if (prev != NULL) {
                prev->next = obj;
            } else {
                vm.objects = obj;
            }

            freeObject(unreached);
        }
    }
}

void collectGarbage() {
#ifdef DEBUG_LOG_GC
    printf("--gc begin\n");
#endif

    markRoots();
    traceReferences();
    tableRemoveWhite(&vm.strings);
    sweep();


#ifdef DEBUG_LOG_GC
    printf("--end\n");
#endif

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

