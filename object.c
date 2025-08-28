#include <stdio.h>
#include <string.h>
#include "memory.h"
#include "object.h"
#include "value.h"
#include "table.h"
#include "vm.h"

#define ALLOCATE_OBJ(type, objectType) ((type*)allocateObject(sizeof(type), objectType))

Obj* allocateObject(size_t size, ObjType type) {
    Obj* obj = reallocate(NULL, 0, size);
    obj->type = type;

    obj->next = vm.objects;
    vm.objects = obj;

    return obj;
}

//FNV-1a non-criptographic hash algorithm
uint32_t hashString(const char* chars, int length) {
    uint32_t hash = 2166136261;

    for(int i=0; i<length; i++) {
        hash ^= chars[i];
        hash *= 16777619;
    }

    return hash;
}

ObjString* allocateString(char* chars, int length, uint32_t hash) {
    //contiguous allocation for the chars array
    ObjString* string = (ObjString*)allocateObject(sizeof(ObjString) + sizeof(char) * length + 1, OBJ_STRING);
    string->length = length;
    memcpy(string->chars, chars, length);
    string->chars[length] = '\0';
    string->hash = hash;

    tableSet(&vm.strings, string, NIL_VAL);

    return string;
}

ObjString* takeString(char* chars, int length) {

    uint32_t hash = hashString(chars, length);

    ObjString* interned = tableFindString(&vm.strings, chars, length, hash);

    if (interned != NULL) {
        FREE_ARRAY(char, chars, length + 1);
        return interned;
    }

    return allocateString(chars, length, hash);
}

ObjString* copyString(const char* chars, int length) {

    uint32_t hash = hashString(chars, length);

    ObjString* interned = tableFindString(&vm.strings, chars, length, hash);
    //if interned we just return the reference
    if (interned != NULL) return interned;
    
    char* heapChars = ALLOCATE(char, length + 1); 
    memcpy(heapChars, chars, length);
    heapChars[length] = '\0';

    return allocateString(heapChars, length, hash);
}



void printObject(Value value) {
    switch(OBJ_TYPE(value)) {
        case OBJ_STRING:
            printf("%s", AS_CSTRING(value));
            break;
    }
} 