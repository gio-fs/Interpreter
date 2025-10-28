#ifndef clox_value_h
#define clox_value_h
#include <string.h>
#include <stdlib.h>
#include "common.h"

typedef struct ObjString ObjString;
typedef struct Obj Obj;

typedef enum {
    VAL_BOOL,
    VAL_NIL,
    VAL_NUMBER,
    VAL_OBJ,
} ValueType;



#ifdef NAN_BOXING

#define QNAN     ((uint64_t)0x7ffc000000000000) // number values
#define SIGN_BIT ((uint64_t)0x8000000000000000) // indicates Obj*
#define TAG_NIL   1 // 01.
#define TAG_FALSE 2 // 10.
#define TAG_TRUE  3 // 11.

typedef uint64_t Value;

static inline Value numToVal(double num) {
    Value value;
    memcpy(&value, &num, sizeof(double));
    return value;
}


#define AS_NUMBER(value) numToVal(value)
#define AS_BOOL(value)      ((value) == TRUE_VAL)
#define AS_OBJ(value) \
    ((Obj*)(uintptr_t)((value) & ~(SIGN_BIT | QNAN)))

static inline double valueToNum(Value value) {
    double num;
    memcpy(&num, &value, sizeof(Value));
    return num;
}

#define NIL_VAL             ((Value)(uint64_t)(QNAN | TAG_NIL))
#define FALSE_VAL           ((Value)(uint64_t)(QNAN | TAG_FALSE))
#define TRUE_VAL            ((Value)(uint64_t)(QNAN | TAG_TRUE))
#define BOOL_VAL(b)         ((b) ? TRUE_VAL : FALSE_VAL)
#define NUMBER_VAL(value)   valueToNum(value)
#define OBJ_VAL(obj)        (Value)(SIGN_BIT | QNAN | (uint64_t)(uintptr_t)(obj))

#define IS_NIL(value)       (((value)) == NIL_VAL)
#define IS_BOOL(value)      (((value) | 1) == TRUE_VAL)
#define IS_NUMBER(value)    ((value & QNAN) != QNAN)
#define IS_OBJ(value) \
    (((value) & (QNAN | SIGN_BIT)) == (QNAN | SIGN_BIT))

ValueType valueType(Value value);
#define TYPEOF(value)       valueType(value)

#else
typedef struct {
    ValueType type;
    union {
        bool boolean;
        double number;
        Obj* obj;
    } as;
} Value;

#define IS_BOOL(value)      ((value).type == VAL_BOOL)
#define IS_NIL(value)       ((value).type == VAL_NIL)
#define IS_NUMBER(value)    ((value).type == VAL_NUMBER)
#define IS_OBJ(value)       ((value).type == VAL_OBJ)

#define AS_BOOL(value)      ((value).as.boolean)
#define AS_NUMBER(value)    ((value).as.number)
#define AS_OBJ(value)       ((value).as.obj)

#define BOOL_VAL(value)     ((Value){VAL_BOOL, {.boolean = value}})
#define NIL_VAL             ((Value){VAL_NIL, {.number = 0}})
#define NUMBER_VAL(value)   ((Value){VAL_NUMBER, {.number = value}})
#define OBJ_VAL(object)     ((Value){VAL_OBJ, {.obj = (Obj*)object}})

#endif
typedef struct {
    int capacity;
    int count;
    Value* values;
} ValueArray;

void initValueArray(ValueArray* arr);
void writeValueArray(ValueArray* arr, Value value);
void freeValueArray(ValueArray* arr);
void printValue(Value value);
bool valuesEqual(Value a, Value b);

#endif
