#ifndef clox_object_h
#define clox_object_h
#include "common.h"
#include "chunk.h"
#include "value.h"
#include "table.h"

typedef enum {
    OBJ_BOUND_METHOD,
    OBJ_FUNCTION,
    OBJ_NATIVE,
    OBJ_STRING,
    OBJ_ARRAY,
    OBJ_CLOSURE,
    OBJ_UPVALUE,
    OBJ_DICTIONARY,
    OBJ_RANGE,
    OBJ_CLASS,
    OBJ_INSTANCE
} ObjType;

struct Obj {
    ObjType type;
    bool isMarked;
    bool isDirty;
    int age;
    size_t size;
    struct Obj* forwarded;
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


typedef Value (*NativeFn) (int argCount, Value* args);

typedef struct {
    Obj obj;
    bool isBuiltIn;
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
    ObjString* name;
    Table fields;
    Table methods;
} ObjClass;

typedef struct {
    Obj obj;
    Value receiver;
    ObjClosure* method;
} ObjBoundMethod;

typedef struct {
    Obj obj;
    ObjClass* klass;
    Table fields;
} ObjInstance;

typedef struct {
    Obj obj;
    ObjClass* klass;
    ValueType type;
    ValueArray values;
} ObjArray;

typedef struct {
    Obj obj;
    ObjClass* klass;
    Table map;
    EntryList entries;
} ObjDictionary;

typedef struct {
    Obj obj;
    double current;
    double start;
    double end;
} ObjRange;

#define OBJ_TYPE(value)     ((AS_OBJ(value))->type)

#define IS_FUNCTION(value)      isObjType(value, OBJ_FUNCTION)
#define IS_STRING(value)        isObjType(value, OBJ_STRING)
#define IS_NATIVE(vlaue)        isObjType(value, OBJ_NATIVE)
#define IS_ARRAY(value)         isObjType(value, OBJ_ARRAY)
#define IS_CLOSURE(value)       isObjType(value, OBJ_CLOSURE)
#define IS_MAP(value)           isObjType(value, OBJ_DICTIONARY)
#define IS_CLASS(value)         isObjType(value, OBJ_CLASS)
#define IS_INSTANCE(value)      isObjType(value, OBJ_INSTANCE)
#define IS_BOUND_METHOD(value)  isObjType(value, OBJ_BOUND_METHOD)
#define IS_RANGE(value)         isObjType(value, OBJ_RANGE)

#define AS_FUNCTION(value)      ((ObjFunction*)AS_OBJ(value))
#define AS_STRING(value)        ((ObjString*)AS_OBJ(value)) //points to an objstring on heap
#define AS_CSTRING(value)       (((ObjString*)AS_OBJ(value))->chars) //points directly to the chars array of the string
#define AS_NATIVE(value)        (((ObjNative*)AS_OBJ(value))->function)
#define AS_NATIVE_OBJ(value)    (((ObjNative*)AS_OBJ(value)))
#define AS_ARRAY(value)         ((ObjArray*)AS_OBJ(value))
#define AS_CLOSURE(value)       ((ObjClosure*)AS_OBJ(value))
#define AS_MAP(value)           ((ObjDictionary*)AS_OBJ(value))
#define AS_CLASS(value)         ((ObjClass*)AS_OBJ(value))
#define AS_INSTANCE(value)      ((ObjInstance*)AS_OBJ(value))
#define AS_BOUND_METHOD(value)  ((ObjBoundMethod*)AS_OBJ(value))
#define AS_RANGE(value)         ((ObjRange*)AS_OBJ(value))

ObjFunction* newFunction();
ObjArray* newArray();
ObjClosure* newClosure(ObjFunction* function);
ObjUpvalue*newUpvalue(Value* value);
ObjDictionary* newDictionary();
ObjRange* newRange(double start, double end);
ObjClass* newClass(ObjString* name);
ObjInstance* newInstance(ObjClass* klass);
ObjBoundMethod* newBoundMethod(Value receiver, ObjClosure* method);
bool appendArray(ObjArray* arr, Value value);
bool arraySet(ObjArray* array, int index, Value value);
bool arrayGet(ObjArray* arr, int index, Value* value);
Value arrayPop(ObjArray* arr);
ObjNative* newNative(NativeFn function, bool isBuiltIn);
ObjString* copyString(const char* chars, int length);
ObjString* allocateString(char* chars, int length, uint32_t hash);
ObjString* takeString(char* chars, int length);
void printObject(Value value);
uint32_t hashString(const char* chars, int length);

//declared as inline because the function body uses two times value,
//which the preprocessor would convert in the expression in brackets.
//Let's say value is pop, it would pop two times from the stack

static inline bool isObjType(Value value, ObjType type) {
    return IS_OBJ(value) && AS_OBJ(value)->type == type;
}

#endif
