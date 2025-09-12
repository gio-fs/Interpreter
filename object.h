#ifndef clox_object_h
#define clox_object_h
#include "common.h"
#include "chunk.h"
#include "value.h"
#include "table.h"

typedef enum {
    OBJ_FUNCTION,
    OBJ_NATIVE,
    OBJ_STRING,
    OBJ_ARRAY,
    OBJ_CLOSURE,
    OBJ_UPVALUE,
    OBJ_DICTIONARY
} ObjType;

struct Obj {
    ObjType type;
    struct Obj* next; //singly-linked list to keep track of the objs for the GC
};

struct ObjString {
    Obj obj;
    int length;
    uint32_t hash;
    char chars[]; //flexible array member
};

typedef struct {
    Obj obj;
    int arity;
    int upvalueCount;
    Chunk chunk;
    ObjString* name;
} ObjFunction;

typedef struct {
    Obj obj;
    ValueType type;
    ValueArray values;
} ObjArray;

typedef Value (*NativeFn) (int argCount, Value* args);

typedef struct {
    Obj obj;
    NativeFn function;
} ObjNative;

typedef struct ObjUpvalue {
    Obj obj;
    Value* location;
    Value closed;
    struct ObjUpvalue* next;
} ObjUpvalue;

typedef struct {
    Obj obj;
    ObjFunction* function;
    ObjUpvalue** upvalues;
    int upvalueCount;
} ObjClosure;

typedef struct {
    Obj obj;
    Table map;
} ObjDictionary;


#define OBJ_TYPE(value)     ((AS_OBJ(value))->type)

#define IS_FUNCTION(value)  isObjType(value, OBJ_FUNCTION)
#define IS_STRING(value)    isObjType(value, OBJ_STRING)
#define IS_NATIVE(vlaue)    isObjType(value, OBJ_NATIVE)
#define IS_ARRAY(value)     isObjType(value, OBJ_ARRAY)
#define IS_CLOSURE(value)   isObjType(value, OBJ_CLOSURE)
#define IS_MAP(value)       isObjType(value, OBJ_DICTIONARY)

#define AS_FUNCTION(value)  ((ObjFunction*)AS_OBJ(value))
#define AS_STRING(value)    ((ObjString*)AS_OBJ(value)) //points to an objstring on heap
#define AS_CSTRING(value)   (((ObjString*)AS_OBJ(value))->chars) //points directly to the chars array of the string
#define AS_NATIVE(value)    (((ObjNative*)AS_OBJ(value))->function)
#define AS_ARRAY(value)     ((ObjArray*)AS_OBJ(value))
#define AS_CLOSURE(value)   ((ObjClosure*)AS_OBJ(value))
#define AS_MAP(value)       ((ObjDictionary*)AS_OBJ(value))

ObjFunction* newFunction(); 
ObjArray* newArray(ValueType type);
ObjClosure* newClosure(ObjFunction* function);
ObjUpvalue*newUpvalue(Value* value);
ObjDictionary* newDictionary();
bool appendArray(ObjArray* arr, Value value);
bool setArray(ObjArray* array, int index, Value value);
bool getArray(ObjArray* arr, int index, Value* value);
ObjNative* newNative(NativeFn function);
ObjString* copyString(const char* chars, int length);
ObjString* allocateString(char* chars, int length, uint32_t hash);
ObjString* takeString(char* chars, int length);
void printObject(Value value);

//declared as inline because the function body uses two times value,
//which the preprocessor would convert in the expression in brackets.
//Let's say value is pop, it would pop two times from the stack

static inline bool isObjType(Value value, ObjType type) {
    return IS_OBJ(value) && AS_OBJ(value)->type == type;
}

#endif
