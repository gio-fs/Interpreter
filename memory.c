#include <stdlib.h>
#include "memory.h"
#include "vm.h"

void* reallocate(void* ptr, size_t oldSize, size_t newSize) {

    if(newSize == 0) {
        free(ptr);
        return NULL;
    }

    void* result = realloc(ptr, newSize);
    if (result == NULL) exit(1);
    return result;
}

void freeObject(Obj* obj) {
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
    }
}

void freeObjects() {
    Obj* obj = vm.objects;
    while (obj != NULL) {
        Obj* next = obj->next;
        freeObject(obj);
        obj = next;
    }
}

