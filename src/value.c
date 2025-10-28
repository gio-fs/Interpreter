#include<stdio.h>
#include<string.h>

#include "value.h"
#include "memory.h"
#include "object.h"
#include "vm.h"

void initValueArray(ValueArray* arr) {

    arr->capacity = 0;
    arr->count = 0;
    arr->values = NULL;
}

void writeValueArray(ValueArray* arr,  Value value) {
    if (arr->capacity < arr->count + 1) {
        int oldCapacity = arr->capacity;
        arr->capacity = GROW_CAPACITY(oldCapacity);
        bool wasCollecting = vm.isCollecting;
        vm.isCollecting = true;
        arr->values = GROW_ARRAY(Value, arr->values, oldCapacity, arr->capacity);
        vm.isCollecting = wasCollecting;
    }

    arr->values[arr->count++] = value;
}


void freeValueArray(ValueArray* arr) {
    FREE_ARRAY(uint8_t, arr->values, arr->capacity);
    initValueArray(arr);
}



bool valuesEqual(Value a, Value b) {
#ifdef NAN_BOXING
    return a == b;
#else
    if (a.type != b.type) return false;

    switch (a.type) {
        case VAL_BOOL:      return AS_BOOL(a) == AS_BOOL(b);
        case VAL_NIL:       return true;
        case VAL_NUMBER:    return AS_NUMBER(a) == AS_NUMBER(b);
        case VAL_OBJ:       return AS_OBJ(a) == AS_OBJ(b);

        default:            return false;
    }

    return false;
#endif
}

ValueType valueType(Value value) {
    if (IS_BOOL(value)) return VAL_BOOL;
    else if (IS_NUMBER(value)) return VAL_NUMBER;
    else if (IS_OBJ(value)) return VAL_OBJ;
    else return VAL_NIL;
}

void printValue(Value value) {

#ifdef NAN_BOXING
    if (IS_BOOL(value)) {
        printf(AS_BOOL(value) ? "true" : "false");
    } else if (IS_NIL(value)) {
        printf("nil");
    } else if (IS_NUMBER(value)) {
        printf("%g", AS_NUMBER(value));
    } else if (IS_OBJ(value)) {
        printObject(value);
    }
#else
    switch (value.type) {
        case VAL_BOOL:
            printf(AS_BOOL(value) ? "true" : "false");
            break;
        case VAL_NIL:
            printf("nil");
            break;
        case VAL_NUMBER:
            printf("%g", AS_NUMBER(value));
            break;
        case VAL_OBJ:
            printObject(value);
            break;
    }
    #endif
}
