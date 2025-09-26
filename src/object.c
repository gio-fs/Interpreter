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
    obj->isMarked = false;

    obj->next = vm.objects;
    vm.objects = obj;

#ifdef DEBUG_LOG_GC
    printf("%p allocate %ld for %d\n", (void*)obj, size, type);
#endif

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

ObjArray* newArray() {
    ObjArray* arr = ALLOCATE_OBJ(ObjArray, OBJ_ARRAY);

    arr->type = VAL_NIL;
    arr->values.values = NULL;
    arr->values.count = 0;
    arr->values.capacity = 0;

    Value arrClass;
    tableGet(&vm.globals, copyString("__Array__", 9), &arrClass);
    arr->instance = newInstance(AS_CLASS(arrClass));
    return arr;
}

bool appendArray(ObjArray* arr, Value value) {
    if (arr->values.count == 0) {
        arr->type = value.type;
    }
    if (value.type != arr->type) {
        // error, vm handles this
        return false;
    }

    push(OBJ_VAL(arr));
    writeValueArray(&arr->values, value);
    pop();

    return true;
}

bool arraySet(ObjArray* arr, int index, Value value) {
    if ( index < 0 || index >= arr->values.count || value.type != arr->type) {
        return false;
    }

    arr->values.values[index] = value;
    return true;
}

bool arrayGet(ObjArray* arr, int index, Value* value) {
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
    initEntryList(&dict->entries);
    Value dictClass;
    tableGet(&vm.globals, copyString("__Dict__", 8), &dictClass);
    dict->instance = newInstance(AS_CLASS(dictClass));
    return dict;
}

ObjClass* newClass(ObjString* name) {
    ObjClass* cclass = ALLOCATE_OBJ(ObjClass, OBJ_CLASS);
    cclass->name = name;
    initTable(&cclass->methods);
    initTable(&cclass->fields);
    return cclass;
}

ObjInstance* newInstance(ObjClass* klass) {
    ObjInstance* instance = ALLOCATE_OBJ(ObjInstance, OBJ_INSTANCE);
    instance->klass = klass;
    initTable(&instance->fields);
    tableAddAll(&klass->fields, &instance->fields);
    tableAddAll(&klass->methods, &instance->fields);
    return instance;
}

ObjBoundMethod* newBoundMethod(Value receiver, ObjClosure* method) {
    ObjBoundMethod* bound = ALLOCATE_OBJ(ObjBoundMethod, OBJ_BOUND_METHOD);
    bound->receiver = receiver;
    bound->method = method;
    return bound;
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

    push(OBJ_VAL(string));
    tableSet(&vm.strings, string, NIL_VAL);
    pop();

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
            printFunction(AS_FUNCTION(value));
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
        case OBJ_DICTIONARY: {
            printf("<dict>");
        // #ifdef DEBUG_TRACE_EXECUTION
        //     printf(" ");
        //     ObjDictionary* dict = AS_MAP(value);
        //     Entry first = dict->entries.entries[0];
        //     printf("Key: ");
        //     printValue(OBJ_VAL(first.key));
        //     printf(" Value: ");
        //     printValue(first.value);
        // #endif

        break;
        }
        case OBJ_CLASS: {
            printf("class %s", AS_CLASS(value)->name->chars);
            break;
        }
        case OBJ_INSTANCE: {
            printf("%s instance", AS_INSTANCE(value)->klass->name->chars);
            break;
        }
        case OBJ_BOUND_METHOD: {
            printFunction(AS_BOUND_METHOD(value)->method->function);
            break;
        }


    }
}
