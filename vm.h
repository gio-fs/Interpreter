#ifndef clox_vm_h
#define clox_vm_h
#include "chunk.h"
#include "table.h"
#include "value.h"
#include "object.h"



typedef struct {
    ObjFunction* function;
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

    Obj* objects;
} VM;

typedef enum {
    INTERPRET_OK,
    INTERPRET_COMPILE_ERROR,
    INTERPRET_RUNTIME_ERROR,
} InterpretResult;

extern VM vm;

void initVM();
void freeVM();
InterpretResult interpret(const char* source);
void runtimeError(const char* format, ...);
ObjString* valueTypeToString(ValueType type);
#endif
