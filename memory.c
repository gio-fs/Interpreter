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
        case OBJ_STRING:
            ObjString* string = (ObjString*)obj;
            FREE(ObjString, obj);
            break;
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

