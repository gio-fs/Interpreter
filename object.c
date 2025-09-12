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

ObjFunction* newFunction() {
    ObjFunction* function = ALLOCATE_OBJ(ObjFunction, OBJ_FUNCTION);

    function->arity = 0;
    function->upvalueCount = 0;
    function->name = NULL;
    initChunk(&function->chunk);
    return function;
}

ObjArray* newArray(ValueType type) {
    ObjArray* arr = ALLOCATE_OBJ(ObjArray, OBJ_ARRAY);

    arr->type = type;
    arr->values.values = NULL;
    arr->values.count = 0;
    arr->values.capacity = 0;
    return arr;
}

bool appendArray(ObjArray* arr, Value value) {
    if (value.type != arr->type) {
        // error, vm handles this
        return false;
    }

    writeValueArray(&arr->values, value);
    return true;
}

bool setArray(ObjArray* arr, int index, Value value) {
    if ( index < 0 || index >= arr->values.count || value.type != arr->type) {
        return false;
    }

    arr->values.values[index] = value;
    return true;
}

bool getArray(ObjArray* arr, int index, Value* value) {
    if (index < 0 || index >= arr->values.count) {
        return false;
    }

    *value = arr->values.values[index];
    return true;

}

ObjClosure* newClosure(ObjFunction* function) {
    ObjClosure* closure = ALLOCATE_OBJ(ObjClosure, OBJ_CLOSURE);
    ObjUpvalue** upvalues = ALLOCATE(ObjUpvalue*, function->upvalueCount);
    
    for (int i = 0; i <= function->upvalueCount - 1; i++) {
        upvalues[i] = NULL;
    }

    closure->upvalues = upvalues;
    closure->upvalueCount = function->upvalueCount;
    closure->function = function;
    return closure;

}

ObjNative* newNative(NativeFn function) {
    ObjNative* native = ALLOCATE_OBJ(ObjNative, OBJ_NATIVE);
    native->function = function;
    return native;
}

ObjUpvalue* newUpvalue(Value* slot) {
    ObjUpvalue* upvalue = ALLOCATE_OBJ(ObjUpvalue, OBJ_UPVALUE);
    upvalue->location = slot;
    upvalue->closed = NIL_VAL;
    upvalue->next = NULL;
    return upvalue;
}

ObjDictionary* newDictionary() {
    ObjDictionary* dict = ALLOCATE_OBJ(ObjDictionary, OBJ_DICTIONARY);
    initTable(&dict->map);
    return dict;
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
    //if interned we just return the rerence
    if (interned != NULL) return interned;

    char* heapChars = ALLOCATE(char, length + 1);
    memcpy(heapChars, chars, length);
    heapChars[length] = '\0';

    return allocateString(heapChars, length, hash);
}

static void printFunction(ObjFunction* function) {
    if (function->name == NULL) {
        printf("<script>");
        
        return;
    }
    printf("<fn %s>", function->name->chars);
}

void printObject(Value value) {
    switch(OBJ_TYPE(value)) {
        case OBJ_FUNCTION: {
            printf("%s", AS_FUNCTION(value));
            break;
        }
        case OBJ_STRING: {
            printf("%s", AS_CSTRING(value));
            break;
        }
        case OBJ_NATIVE: {
            printf("<native func>");
            break;
        }
        case OBJ_ARRAY: {
            ObjString* arrType = valueTypeToString(AS_ARRAY(value)->type);
            printf("<%s array>", arrType->chars);
            break;
        }
        case OBJ_CLOSURE: {
            printFunction(AS_CLOSURE(value)->function);
            break;
        }
        case OBJ_UPVALUE: {
            printf("upvalue");
            break;
        }

             
    }
}
