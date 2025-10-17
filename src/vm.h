#ifndef clox_vm_h
#define clox_vm_h
#include "chunk.h"
#include "table.h"
#include "value.h"
#include "object.h"


typedef struct {
    ObjClosure* closure;
    uint8_t* ip;
    Value* slots;
} CallFrame;

typedef struct {
    int capacity;
    int count;
    CallFrame* frames;
} CallFrameArray;

typedef struct {
    CallFrameArray frameArray;

    ValueArray stack;
    Value* stackTop;
    Table strings;
    Table globals;
    Table constGlobals;
    ValueArray queue[64];
    int queueCount[64];
    int firstIn[64];
    int nestingLevel;
    ObjUpvalue* openUpvalues;
    ObjString* array_NativeString;
    ObjString* dict_NativeString;
    ObjString* initString;
    ObjClass* arrayClass;
    ObjClass* dictClass;
    bool canSetConstProp;

    unsigned long long bytesAllocated;
    unsigned long long nextGC;

    Obj* objects;
    int grayCount;
    int grayCapacity;
    Obj** grayStack;
    bool isCollecting;
} VM;

typedef enum {
    INTERPRET_OK,
    INTERPRET_COMPILE_ERROR,
    INTERPRET_RUNTIME_ERROR,
} InterpretResult;

extern VM vm;

void initVM();
void freeVM();
Value pop();
void push(Value value);
InterpretResult interpret(const char* source);
void runtimeError(const char* format, ...);
ObjString* valueTypeToString(ValueType type);
#endif
