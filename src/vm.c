#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <string.h>
#include <time.h>
#include <inttypes.h>
#include "common.h"
#include "memory.h"
#include "object.h"
#include "value.h"
#include "clox_compiler.h"
#include "clox_debug.h"
#include "vm.h"

#define MAX_NESTING_LVL 64

VM vm;


#include <time.h>
#include <stdint.h>

double highres_time() {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec / 1e9;
}

static Value clockNative(int argCount, Value* args) {
    return NUMBER_VAL(highres_time());
}


void initCallFrameArray(CallFrameArray* arr) {
    arr->capacity = FRAMES_INIT_CAPACITY;
    arr->count = 0;
    arr->frames = ALLOCATE(CallFrame, vm.frameArray.capacity);
}

void freeCallFrameArray(CallFrameArray* arr) {
    FREE_ARRAY(CallFrame, arr->frames, arr->capacity);
    initCallFrameArray(arr);
}

static void growStack() {
    if (vm.stack.capacity <= (int)(vm.stackTop - vm.stack.values) || vm.frameArray.capacity <= vm.frameArray.count) {
        int oldStackCapacity = vm.stack.capacity;
        int oldFramesCapacity = vm.frameArray.capacity;
        int stackSize = (int)(vm.stackTop - vm.stack.values);

        Value* oldStack = vm.stack.values;

        vm.stack.capacity = GROW_STACK_CAPACITY(oldStackCapacity);
        vm.frameArray.capacity = GROW_FRAMES_CAPACITY(oldFramesCapacity);
        vm.stack.values = GROW_ARRAY(Value, vm.stack.values, oldStackCapacity, vm.stack.capacity);
        vm.frameArray.frames = GROW_ARRAY(CallFrame, vm.frameArray.frames, oldFramesCapacity, vm.frameArray.capacity);

        vm.stackTop = vm.stack.values + stackSize;

        int reallocDiff = (int)(vm.stack.values - oldStack); // diff between new reallocated address and previous
        for (int i = vm.frameArray.count - 1; i >= 0; i--) {
            if (vm.frameArray.frames[i].slots != NULL) {
                vm.frameArray.frames[i].slots += reallocDiff;
            }
        }

        printf("Stack grown\n");
    }
}

void push(Value value) {
    bool wasCollecting = vm.isCollecting;
    vm.isCollecting = true;
    growStack();
    vm.isCollecting = wasCollecting;
    *vm.stackTop = value;
    vm.stackTop++;
}

Value pop() {
    vm.stackTop--;
    return *vm.stackTop;
}

static void resetStack() {
    vm.stackTop = vm.stack.values;
}

void runtimeError(const char* format, ...) {

    va_list args; // pointer to the first argument
    va_start(args, format); // init args to format
    vfprintf(stderr, format, args);
    va_end(args); // frees args
    fputs("\n", stderr);

    // printing stack trace
    for (int i = vm.frameArray.count - 1; i >= 0; i--) {
        CallFrame* frame = &vm.frameArray.frames[i];
        ObjFunction* function = frame->closure->function;

        // -1 because the IP is sitting on the next instruction to be
        // executed.
        size_t instruction = frame->ip - function->chunk.code - 1;
        int line = getLine(&function->chunk, (int)instruction);
        fprintf(stderr, "[line %d] in ", line);

        if (function->name == NULL) {
            fprintf(stderr, "script\n");
        } else {
            fprintf(stderr, "%s()\n", function->name->chars);
        }
    }

    resetStack();
}

static Value peek(int distance) {
    return vm.stackTop[-1 - distance];
}

static bool isBuiltInAndSet(Value value, ObjClass** klass) {
    switch (AS_OBJ(value)->type) {
        case OBJ_ARRAY:
            *klass = AS_ARRAY(value)->klass;
            return true;
        case OBJ_DICTIONARY:
            *klass = AS_MAP(value)->klass;
            return true;
    }
    return false;
}


static bool isBuiltIn(Value value) {
    if (value.type != VAL_OBJ) return false;
    switch (AS_OBJ(value)->type) {
        case OBJ_ARRAY:
        case OBJ_DICTIONARY:
            return true;
    }

    return false;
}


static bool call(ObjClosure* closure, int argc) {
    if (argc != closure->function->arity) {
        runtimeError("Expected %d arguments but got %d" , closure->function->arity, argc);
        return false;
    }

    growStack();

    CallFrame* frame = &vm.frameArray.frames[vm.frameArray.count++];
    frame->closure= closure;
    frame->ip = closure->function->chunk.code;

    // give the frame it's window from the stack: the "- 1" it's to skip the function obj
    frame->slots = vm.stackTop - argc - 1;
    return true;
}


static bool callValue(Value callee, int argCount) {
    if (IS_OBJ(callee)) {
        switch (OBJ_TYPE(callee)) {
            case OBJ_CLOSURE: return call(AS_CLOSURE(callee), argCount);
            case OBJ_NATIVE: {
                NativeFn native = AS_NATIVE(callee);
                Value result = native(argCount, vm.stackTop - argCount);
                vm.stackTop -= argCount + 1;
                if (AS_NATIVE_OBJ(callee)->isBuiltIn) pop();
                push(result);
                return true;
            }
            case OBJ_CLASS: {
                ObjClass* klass = AS_CLASS(callee);
                vm.stackTop[-argCount - 1] = OBJ_VAL(newInstance(klass));
                return true;
            }
            case OBJ_BOUND_METHOD: {
                ObjBoundMethod* bound = AS_BOUND_METHOD(callee);
                return call(bound->method, argCount);
            }
            default:
            // handling non callable objects
            break;
        }
    }

    runtimeError("Callee must be a function or a class.");
    return false;
}

static void closeUpvalues(Value* last) {
    // we close all upvalues above last and save the value pointed
    // by upvalue->location in upvalue->closed
    while (vm.openUpvalues != NULL &&
           vm.openUpvalues->location >= last) {

            ObjUpvalue* upvalue = vm.openUpvalues;
            upvalue->closed = *upvalue->location;
            upvalue->location = &upvalue->closed;
            vm.openUpvalues = upvalue->next;
        }
}

ObjUpvalue* captureUpvalue(Value* local) {
    ObjUpvalue* previous = NULL;
    ObjUpvalue* upvalue = vm.openUpvalues;

    // traversing the list until we either find a null upvalue
    // (we reached the head) or find an upvalue which location
    // is less than or equal our local target
    while (upvalue != NULL && upvalue->location > local) {
        previous = upvalue;
        upvalue = upvalue->next;
    }

    // if the location is equal we've found the upvalue
    // and we capture it
    if (upvalue != NULL && upvalue->location == local) {
        return upvalue;
    }

    ObjUpvalue* created = newUpvalue(local);
    created->next = upvalue;

    // if its null, we add to the head the new upvalue, else
    // we add the upvalue in the middle of two nodes (upvalues)
    if (previous == NULL) {
        vm.openUpvalues = created;
    } else {
        previous->next = created;
    }

    // this way we have an ordered linked list of value slots
    return created;
}

static void defineMethod(ObjString* name) {
    // printValue(OBJ_VAL(name));
    // printf("\n");
    Value method = peek(0);
    ObjClass* klass = AS_CLASS(peek(1));
    tableSet(&klass->methods, name, method);
    pop();
}

static void defineProperty(ObjString* name, bool isConst) {
    // printValue(OBJ_VAL(name));
    // printf("\n");
    // printf("Const: %d\n", isConst);
    ObjClass* klass = AS_CLASS(peek(0));
    isConst? tableSet(&klass->fields, name, NUMBER_VAL(-1))
                : tableSet(&klass->fields, name, NIL_VAL);
}

static void defineNative(const char* name, NativeFn function) {
    push(OBJ_VAL(copyString(name, (int)strlen(name))));
    push(OBJ_VAL(newNative(function, false)));
    tableSet(&vm.globals, AS_STRING(vm.stack.values[0]), vm.stack.values[1]);
    pop();
    pop();
}

static ObjClass* defineBuiltinClass(ObjString* name) {
    push(OBJ_VAL(newClass(name)));
    ObjClass* klass = AS_CLASS(peek(0));
    tableSet(&vm.globals, name, peek(0));
    pop();
    return klass;
}

static void defineBuiltinMethod(ObjClass* klass, const char* name, NativeFn function) {
    push(OBJ_VAL(copyString(name, (int)strlen(name))));
    push(OBJ_VAL(newNative(function, true)));
    tableSet(&klass->methods, AS_STRING(peek(1)), peek(0));
    pop();
    pop();
}


static bool bindMethod(ObjClass* klass, ObjString* name) {
    Value method;
// #ifdef DEBUG_TRACE_EXECUTION
//     printValue(OBJ_VAL(klass->name));
//     printf("\n");
// #endif
    if (!tableGet(&klass->methods, name, &method)) {
        runtimeError("Undefined property");
        return false;
    }
    bool wasCollecting = vm.isCollecting;
    vm.isCollecting = true;
    ObjBoundMethod* bound = newBoundMethod(peek(0), AS_CLOSURE(method));
    vm.isCollecting = wasCollecting;
    pop();

    push(OBJ_VAL(bound));
    return true;
}

static bool bindNativeMethod(ObjClass* klass, ObjString* name) {
    Value method;
// #ifdef DEBUG_TRACE_EXECUTION
//     printValue(OBJ_VAL(klass->name));
//     printf("\n");
// #endif
    if (!tableGet(&klass->methods, name, &method)) {
        runtimeError("Undefined property");
        return false;
    }

    ObjNative* native = AS_NATIVE_OBJ(method);

    push(OBJ_VAL(native));
    return true;
}

ObjString* valueTypeToString(ValueType type) {
    switch (type) {
        case VAL_BOOL: return copyString("bool", 4);
        case VAL_NIL: return copyString("nil", 3);
        case VAL_NUMBER: return copyString("number", 6);
        case VAL_OBJ: return copyString("obj", 3);

        default:
        // unreacheable
        break;
    }

    // unreacheable
    return copyString("error", 5);
}

static bool isFalsey(Value value) {
    return IS_NIL(value) || (IS_BOOL(value) && !AS_BOOL(value));
}

static ObjString* intToString(int num) {
    // Handle negative
    bool negative = num < 0;
    if (negative) num = -num;

    // Convert digits (backwards)
    char buffer[16];
    int pos = 15;
    buffer[pos--] = '\0';

    do {
        buffer[pos--] = '0' + (num % 10);
        num /= 10;
    } while (num > 0);

    if (negative) buffer[pos--] = '-';

    return copyString(&buffer[pos + 1], 14 - pos);
}

static ObjString* valueToString(Value value) {
    if (IS_STRING(value)) {
        return AS_STRING(value);

    } else if (IS_NUMBER(value)) {
        double num = AS_NUMBER(value);

        // snprintf copies a value (num in this case) as a string literal to buffer with a specific format
        if (num == (int)num) return intToString((int)num);

        char buffer[40];
        snprintf(buffer, sizeof(buffer), "%g", num);
        return copyString(buffer, strlen(buffer));

    } else if (IS_BOOL(value)) {
        const char* boolStr = AS_BOOL(value) ? "true" : "false";
        return copyString(boolStr, strlen(boolStr));

    } else if (IS_NIL(value)) {
        return copyString("nil", 3);
    }

    // temporary fallback for obj types
    return copyString("object", 6);
}


static bool queue(ValueArray* arr) {
    if (vm.nestingLevel < 0) {
        return false;
    }

    if (arr->capacity < arr->count + 1) {
        int oldCapacity = arr->capacity;
        arr->capacity = GROW_CAPACITY(oldCapacity);
        vm.queueCount[vm.nestingLevel] = 0;
        arr->values = GROW_ARRAY(Value, arr->values, oldCapacity, arr->capacity);
    }

    arr->values[arr->count] = pop();
    arr->count++;
    vm.queueCount[vm.nestingLevel] = arr->count;
    // vm.lastIn[vm.nestingLevel] = arr->count;
    return true;
}

static bool deque(ValueArray* arr, Value* value) {
    if (vm.nestingLevel < 0) {
        return false;
    }

    *value = arr->values[--arr->count];
    return true;
}


static void concatenate() {
    vm.isCollecting = true;
    ObjString* b = valueToString(pop());
    vm.isCollecting = true;
    ObjString* a = valueToString(pop());

    vm.isCollecting = true;
    int length = a->length + b->length;
    char* newString = ALLOCATE(char, length + 1);

    memcpy(newString, a->chars, a->length);
    memcpy(newString + a->length, b->chars, b->length);
    newString[length] = '\0';

    // newString is a temp value on heap:
    // the string object (result) itself is in the heap, and it's chars field points
    // to the same memory location of temp, thus taking the ownership of the memory.
    // Now temp doesn't have anymore the responsibility of freeing that memory
    // and will be handled by the GC when result will be freed. The actual array of characters
    // never changes place, only the ownership switches between temp and result

    ObjString* result = takeString(newString, length);
    push(OBJ_VAL(result));
    vm.isCollecting = false;
}


static bool isIterable(Value value) {
    if (IS_OBJ(value)) {
        return AS_OBJ(value)->type == OBJ_ARRAY
                || AS_OBJ(value)->type == OBJ_DICTIONARY
                || AS_OBJ(value)->type == OBJ_RANGE;

    } else return false;
}



static Value array_AddNative(int argCount, Value* args) {
    if (!IS_ARRAY(args[-2])) {
        runtimeError("Value is not an array");
        return NIL_VAL;
    }
    if (argCount != 1) {
        runtimeError("Array.add() expects only one argument");
        return NIL_VAL;
    }

    ObjArray* arr = AS_ARRAY(args[-2]);
    appendArray(arr, args[0]);
    return args[0];
}

static Value array_SetNative(int argCount, Value* args) {
    if (!IS_ARRAY(args[-2])) {
        runtimeError("Value is not an array");
        return NIL_VAL;
    }
    if (argCount != 2) {
        runtimeError("Array.set() expects two arguments: idx, value");
        return NIL_VAL;
    }

    ObjArray* arr = AS_ARRAY(args[-2]);
    arraySet(arr, AS_NUMBER(args[0]), args[1]);
    return args[1];
}

static Value array_GetNative(int argCount, Value* args) {
    if (!IS_ARRAY(args[-2])) {
        runtimeError("Value is not an array");
        return NIL_VAL;
    }
    if (argCount != 1) {
        runtimeError("Array.get() expects one argument: idx");
        return NIL_VAL;
    }

    ObjArray* arr = AS_ARRAY(args[-2]);
    Value value;
    if (!arrayGet(arr, AS_NUMBER(args[0]), &value)) {
        // runtimeError("Element '%d' of array doesn't exist", AS_NUMBER(args[0]));
        return NUMBER_VAL(0);
    }
    return value;
}

static Value array_PopNative(int argCount, Value* args) {
    if (!IS_ARRAY(args[-2])) {
        runtimeError("Object is not an array");
        return NIL_VAL;
    }
    if (argCount != 1) {
        runtimeError("Array.pop() doesn't expect arguments");
        return NIL_VAL;
    }

    return arrayPop(AS_ARRAY(args[0]));
}

static Value dict_AddNative(int argCount, Value* args) {
    if (!IS_MAP(args[-2])) {
        runtimeError("Value is not a map");
        return NIL_VAL;
    }
    if (argCount != 2) {
        runtimeError("Dict.add() expects two arguments: key, value");
        return NIL_VAL;
    }
    vm.isCollecting = true;

    ObjDictionary* dict = AS_MAP(args[-2]);
    ObjString* key = valueToString(args[0]);

    Value value;
    tableGet(&dict->map, key, &value);

    if (!tableSet(&dict->map, key, args[1])) {
        runtimeError("Entry already exists in dictionary");
        tableDelete(&dict->map, key);
        tableSet(&dict->map, key, value);
        return NIL_VAL;
    }

    Entry entry = {.key = key, .value = args[1]};
    writeEntryList(&dict->entries, entry);

    vm.isCollecting = false;
    return NIL_VAL;
}

static Value dict_SetNative(int argCount, Value* args) {
    if (!IS_MAP(args[-2])) {
        runtimeError("Value is not a map");
        return NIL_VAL;
    }
    if (argCount != 2) {
        runtimeError("Dict.set() expects two arguments: key, value");
        return NIL_VAL;
    }

    vm.isCollecting = true;
    ObjDictionary* dict = AS_MAP(args[-2]);
    ObjString* key = valueToString(args[0]);

    tableSet(&dict->map, key, args[1]);
    vm.isCollecting = false;
    return args[1];
}

static Value dict_GetNative(int argCount, Value* args) {
    if (!IS_MAP(args[-2])) {
        runtimeError("Value is not a map");
        return NIL_VAL;
    }
    if (argCount != 1) {
        runtimeError("Dict.get() expects one argument: key");
        return NIL_VAL;
    }
    vm.isCollecting = true;
    ObjDictionary* dict = AS_MAP(args[-2]);
    ObjString* key = valueToString(args[0]);

    Value value;
    if (!tableGet(&dict->map, key, &value)) {
        // runtimeError("Key not found");
        return NUMBER_VAL(0);
    }

    vm.isCollecting = false;
    return value;
}

void initVM() {
    vm.stack.count = 0;
    vm.stack.capacity = STACK_INIT_CAPACITY;
    vm.stack.values = ALLOCATE(Value, vm.stack.capacity);
    vm.stackTop = vm.stack.values;
    vm.openUpvalues = NULL;
    vm.objects = NULL;
    vm.bytesAllocated = 0;
    vm.nextGC = 1024 * 1024;

    vm.array_NativeString = NULL;
    vm.array_NativeString = copyString("__Array__", 9);
    vm.dict_NativeString = NULL;
    vm.dict_NativeString = copyString("__Dict__", 8);

    vm.grayCount = 0;
    vm.grayCapacity = 0;
    vm.grayStack = NULL;

    vm.isCollecting = false;


    initValueArray(&vm.queue[0]);
    initCallFrameArray(&vm.frameArray);

    initTable(&vm.strings);
    initTable(&vm.globals);
    initTable(&vm.constGlobals);


    defineNative("clock", clockNative);
    ObjClass* array = defineBuiltinClass(vm.array_NativeString);
    defineBuiltinMethod(array, "add", array_AddNative);
    defineBuiltinMethod(array, "set", array_SetNative);
    defineBuiltinMethod(array, "get", array_GetNative);
    defineBuiltinMethod(array, "pop", array_PopNative);
    ObjClass* dict = defineBuiltinClass(vm.dict_NativeString);
    defineBuiltinMethod(dict, "add", dict_AddNative);
    defineBuiltinMethod(dict, "set", dict_SetNative);
    defineBuiltinMethod(dict, "get", dict_GetNative);
}

void freeVM() {

    freeCallFrameArray(&vm.frameArray);

    for (int i = 0; i < vm.nestingLevel; i++) {
        // vm.lastIn[i] = 0;
        freeValueArray(&vm.queue[i]);
    }

    vm.nestingLevel = 0;

    FREE_ARRAY(Value, vm.stack.values, vm.stack.capacity);
    vm.stack.capacity = 0;
    vm.stackTop = NULL;

    freeTable(&vm.strings);
    freeTable(&vm.globals);
    freeTable(&vm.constGlobals);

    freeObjects();
}

static InterpretResult run() {
    CallFrame* frame = &vm.frameArray.frames[vm.frameArray.count - 1];

    #define READ_BYTE() (*frame->ip++)
    #define READ_WORD() (frame->ip += 2, (uint16_t)(frame->ip[-2] << 8 | frame->ip[-1]))
    #define READ_LONG() (frame->ip += 3, (uint32_t)(frame->ip[-3] | frame->ip[-2] << 8 | frame->ip[-1] << 16))
    #define READ_CONSTANT() (frame->closure->function->chunk.constants.values[READ_BYTE()])
    #define READ_STRING() AS_STRING(READ_CONSTANT())

    #define BINARY_OP(valueType, op)\
        do {\
            if (!IS_NUMBER(peek(0)) || !IS_NUMBER(peek(1))) {\
                runtimeError("Operands must be numbers.");\
                return INTERPRET_RUNTIME_ERROR;\
            }\
            double b = AS_NUMBER(pop());\
            double a = AS_NUMBER(pop());\
            push(valueType(a op b));\
        } while(0)

    #ifdef DEBUG_TRACE_EXECUTION
        int count = 0;
    #endif
    for (;;) {

        #ifdef DEBUG_TRACE_EXECUTION
            count++;
            if (count % 50 == 0) {
            disassembleInstruction(&frame->closure->function->chunk,
                (int)(frame->ip - frame->closure->function->chunk.code));

            printf("\n");

            for (Value* slot = vm.stack.values; slot < vm.stackTop; slot++) {
                printf("[ ");
                // printf(" Type: ");
                // ObjString* type = valueTypeToString(slot->type);
                // printValue(OBJ_VAL(type));
                // printf(" , ");
                printValue(*slot);
                printf(" ] ");
            }

            printf("\n\n");
            }

        #endif

        uint8_t instruction = READ_BYTE();

        switch (instruction) {
            case OP_CONSTANT: {
                Value constant = READ_CONSTANT();
                push(constant);
                break;
            }
            case OP_CONSTANT_LONG: {
                uint32_t longIndex = READ_LONG(); // longIndex is 3 bytes
                Value constantLong = frame->closure->function->chunk.constants.values[longIndex];
                push(constantLong);
                break;
            }
            case OP_NIL:
                push(NIL_VAL); break;
            case OP_TRUE:
                push(BOOL_VAL(true));
                break;
            case OP_FALSE:
                push(BOOL_VAL(false));
                break;
            case OP_POP:
                pop();
                break;
            case OP_JUMP: {
                uint16_t offset = READ_WORD();
                frame->ip += offset;
                break;
            }
            case OP_LOOP: {
                uint16_t offset = READ_WORD();
                frame->ip -= offset;
                break;
            }
            case OP_JUMP_IF_FALSE: {
                uint16_t offset = READ_WORD();
                // checking the if condtition on top of the stack
                if (isFalsey(peek(0))) frame->ip += offset;
                break;
            }
            case OP_GET_LOCAL: {
                // we need to push the local's value on top of the stack since
                // other bytecode instructions only look for stackTop - 1
                uint8_t slot = READ_BYTE();
                push(frame->slots[slot]);
                break;
            }
            case OP_GET_GLOBAL: {
                ObjString* name = READ_STRING();
                Value value;
                if(!tableGet(&vm.globals, name, &value)) {
                    runtimeError("Undefined variable '%s' .", name->chars);
                    return INTERPRET_RUNTIME_ERROR;
                }
                push(value);
                break;
            }
            case OP_GET_GLOBAL_LONG: {
                uint32_t longIndex = READ_LONG();
                ObjString* name = AS_STRING(frame->closure->function->chunk.constants.values[longIndex]);
                Value valueLong;
                if(!tableGet(&vm.globals, name, &valueLong)) {
                    runtimeError("Undefined variable '%s' .", name->chars);
                    return INTERPRET_RUNTIME_ERROR;
                }
                push(valueLong);
                break;
            }
            case OP_DEFINE_GLOBAL: {
                ObjString* name = READ_STRING();
                Value value = peek(0);
                if (!tableSet(&vm.globals, name, value) || tableGet(&vm.constGlobals, name, &value)) {
                    runtimeError("Variable '%s' is already defined. '%s'", name->chars);
                    return INTERPRET_RUNTIME_ERROR;
                }
                pop();
                break;
            }
            case OP_DEFINE_CONST_GLOBAL: {
                ObjString* name = READ_STRING();
                Value value = peek(0);
                if (!tableSet(&vm.constGlobals, name, value) || tableGet(&vm.globals, name, &value)) {
                    runtimeError("Variable '%s' is already defined.", name->chars);
                    return INTERPRET_RUNTIME_ERROR;
                }
                pop();
                break;
            }
            case OP_DEFINE_GLOBAL_LONG: {
                uint32_t longIndex = READ_LONG();
                ObjString* name = AS_STRING(frame->closure->function->chunk.constants.values[longIndex]);
                Value value = peek(0);
                if (!tableSet(&vm.globals, name, value) || tableGet(&vm.constGlobals, name, &value)) {
                    runtimeError("Variable '%s' is already defined.", name->chars);
                    return INTERPRET_RUNTIME_ERROR;
                }
                pop();
                break;
            }
            case OP_DEFINE_CONST_GLOBAL_LONG: {
                uint32_t longIndex = READ_LONG();
                ObjString* name = AS_STRING(frame->closure->function->chunk.constants.values[longIndex]);
                Value value = peek(0);
                if (!tableSet(&vm.constGlobals, name, value) || tableGet(&vm.globals, name, &value)) {
                    runtimeError("Variable '%s' is already defined.", name->chars);
                    return INTERPRET_RUNTIME_ERROR;
                }
                pop();
                break;
            }
            case OP_SET_LOCAL: {
                // we need to push the local's value on top of the stack since
                // other bytecode instructions only look for stackTop - 1
                uint8_t slot = READ_BYTE();
                frame->slots[slot] = peek(0);

                break;
            }
            case OP_SET_GLOBAL: {
                ObjString* name = READ_STRING();
                if (tableFindString(&vm.constGlobals, name->chars, name->length, name->hash)) {
                    runtimeError("Variable '%s' is const.", name->chars);
                    return INTERPRET_RUNTIME_ERROR;
                }
                if (tableSet(&vm.globals, name, peek(0))) {
                    tableDelete(&vm.globals, name);
                    runtimeError("Undefined variable '%s'", name->chars);
                    return INTERPRET_RUNTIME_ERROR;
                }
                break;
            }
            case OP_SET_GLOBAL_LONG: {
                uint32_t longIndex = READ_LONG();
                ObjString* name = AS_STRING(frame->closure->function->chunk.constants.values[longIndex]);
                if (tableFindString(&vm.constGlobals, name->chars, name->length, name->hash)) {
                    runtimeError("Variable '%s' is const.", name->chars);
                    return INTERPRET_RUNTIME_ERROR;
                }
                if (tableSet(&vm.globals, name, peek(0))) {
                    tableDelete(&vm.globals, name);
                    runtimeError("Undefined variable '%s'", name->chars);
                    return INTERPRET_RUNTIME_ERROR;
                }
                break;
            }

            case OP_ARRAY: {
                int length = READ_BYTE();
                // -1 because peek already returns last element so this way
                // distance becomes -1 -(length - 1),  thus length elements from top
                ObjArray* arr = newArray();
                push(OBJ_VAL(arr));

                for (int i = length - 1; i >= 0; i--) {
                    if (!appendArray(arr, peek(i + 1))) {
                        ObjString* errorType = valueTypeToString(peek(i).type);
                        ObjString* arrType = valueTypeToString(VAL_NUMBER);
                        runtimeError("Expected a value of type %s but tried to append %s", arrType->chars, errorType->chars);
                        return INTERPRET_RUNTIME_ERROR;
                    }
                }

                // pop();

                for (int i = length; i >= 0; i--) {
                    pop();
                }

                push(OBJ_VAL(arr));
                break;
            }
            case OP_ARRAY_LONG: {
                int length = READ_LONG();
                // -1 because peek already returns last element so this way
                // distance becomes -1 -(length - 1),  thus length elements from top
                ObjArray* arr = newArray();
                push(OBJ_VAL(arr));

                for (int i = length - 1; i >= 0; i--) {
                    if (!appendArray(arr, peek(i + 1))) {
                        ObjString* errorType = valueTypeToString(peek(i).type);
                        ObjString* arrType = valueTypeToString(VAL_NUMBER);
                        runtimeError("Expected a value of type %s but tried to append %s", arrType->chars, errorType->chars);
                        return INTERPRET_RUNTIME_ERROR;
                    }
                }

                pop();

                for (int i = length - 1; i >= 0; i--) {
                    pop();
                }

                push(OBJ_VAL(arr));
                break;
            }
            case OP_MAP: {
                int count = READ_BYTE();
                ObjDictionary* dict = newDictionary();
                push(OBJ_VAL(dict));

                for (int i = count - 1; i > 0; i -= 2) {
                    vm.isCollecting = true;
                    ObjString* key = valueToString(peek(i));
                    vm.isCollecting = false;
                    Value elem = peek(i - 1);

                    Entry curr = {.key = key, .value = elem};
                    tableSet(&dict->map, key, elem);
                    writeEntryList(&dict->entries, curr);
                }

                pop();

                for (int i = 0; i < count; i++) {
                    pop();
                }

                push(OBJ_VAL(dict));
                break;
            }
            case OP_MAP_LONG: {
                uint32_t count = READ_LONG();
                ObjDictionary* dict = newDictionary();
                push(OBJ_VAL(dict));

                for (int i = count - 1; i > 0; i -= 2) {
                    Value elem = peek(i - 1);
                    vm.isCollecting = true;
                    ObjString* key = valueToString(peek(i));
                    vm.isCollecting = false;

                    Entry curr = {.key = key, .value = elem};
                    tableSet(&dict->map, key, elem);
                    writeEntryList(&dict->entries, curr);
                }

                for (int i = 0; i < count; i++) {
                    pop();
                }

                pop();
                push(OBJ_VAL(dict));
                break;
            }
            case OP_GET_ELEMENT: {
                int slot = READ_BYTE();
                Value elementIndex = pop();
                Value value;

                if (!isIterable(frame->slots[slot])) {
                    runtimeError("Value must be of indexeable type");
                    return INTERPRET_RUNTIME_ERROR;
                }

                switch (AS_OBJ(frame->slots[slot])->type) {
                    case OBJ_ARRAY: {
                        if (!IS_NUMBER(elementIndex)) {
                            runtimeError("Indexing expression must evaluate to positive integer for arrays");
                            return INTERPRET_RUNTIME_ERROR;
                        }

                        ObjArray* arr = AS_ARRAY(frame->slots[slot]);
                        int index = AS_NUMBER(elementIndex);

                        if (!arrayGet(arr, index, &value)) {
                            // ObjString* name = valueTypeToString(arr->type);
                            // runtimeError("Error in getting element %g of array. Array type is %s", index, name->chars);
                            goto endBranch;
                        }

                        break;
                    }
                    case OBJ_DICTIONARY: {
                        ObjDictionary* dict = AS_MAP(frame->slots[slot]);
                        bool wasCollecting = vm.isCollecting;
                        vm.isCollecting = true;
                        ObjString* key = valueToString(elementIndex);
                        vm.isCollecting = wasCollecting;

                        if (!tableGet(&dict->map, key, &value)) {
                            // runtimeError("Key '%s' not found in dictionary\n", key->chars);
                            goto endBranch;
                        }

                        break;
                    }
                }

                endBranch: break;

                push(value);
                break;
            }
            case OP_SET_ELEMENT: {
                int slot = READ_BYTE();
                Value setVal = pop();
                Value elementIndex = peek(0);

                if (!isIterable(frame->slots[slot])) {
                    runtimeError("Value must be of indexeable type");
                    return INTERPRET_RUNTIME_ERROR;
                }

                switch (AS_OBJ(frame->slots[slot])->type) {
                    case OBJ_ARRAY: {
                        if (!IS_NUMBER(elementIndex)) {
                            runtimeError("Indexing expression must evaluate to positive integer for arrays");
                            return INTERPRET_RUNTIME_ERROR;
                        }

                        ObjArray* arr = AS_ARRAY(frame->slots[slot]);
                        int index = AS_NUMBER(elementIndex);

                        if (!arraySet(arr, index, setVal)) {
                            ObjString* name = valueTypeToString(arr->type);
                            runtimeError("Error in setting element %g of array. Array type is %s", index, name->chars);
                            return INTERPRET_RUNTIME_ERROR;
                        }

                        break;
                    }
                    case OBJ_DICTIONARY: {
                        if (!IS_STRING(elementIndex)) {
                            runtimeError("Indexing expression must evaluate to string for maps");
                            return INTERPRET_RUNTIME_ERROR;
                        }

                        ObjDictionary* dict = AS_MAP(frame->slots[slot]);
                        ObjString* key = AS_STRING(elementIndex);

                        if (tableSet(&dict->map, key, setVal)) {
                            runtimeError("Key '%s' not found in dictionary\n", key->chars);
                            return INTERPRET_RUNTIME_ERROR;
                        }

                        break;
                    }

                }

                push(setVal);
                break;
            }

            case OP_GET_ELEMENT_GLOBAL: {
                ObjString* name = READ_STRING();
                Value elementIndex = pop();
                Value arr;

                if (!IS_STRING(elementIndex) && !IS_NUMBER(elementIndex)) {
                    runtimeError("Array index must evaluate to positive integer.");
                    return INTERPRET_RUNTIME_ERROR;
                }

                if(!tableGet(&vm.globals, name, &arr)) {
                    runtimeError("Undefined variable '%s' .", name->chars);
                    return INTERPRET_RUNTIME_ERROR;
                }

                if (IS_STRING(elementIndex)) {
                    if (!IS_MAP(arr)) {
                        runtimeError("Element must be a dictionary");
                        return INTERPRET_RUNTIME_ERROR;
                    }
                    ObjDictionary* dict = AS_MAP(arr);
                    ObjString* key = AS_STRING(elementIndex);
                    Value value;

                    if (!tableGet(&dict->map, key, &value)) {
                        runtimeError("Key not found");
                        return INTERPRET_RUNTIME_ERROR;
                    }

                    push(value);
                    break;
                }

                if (!IS_ARRAY(arr)) {
                    runtimeError("Indexed variable is not an array");
                    return INTERPRET_RUNTIME_ERROR;
                }

                ObjArray* cachedArr = AS_ARRAY(arr);
                Value element;
                if (!arrayGet(cachedArr, AS_NUMBER(elementIndex), &element)){
                    runtimeError("Index out of bounds.");
                    return INTERPRET_RUNTIME_ERROR;
                }

                push(element);
                break;
            }
            case OP_SET_ELEMENT_GLOBAL: {
                ObjString* name = READ_STRING();
                Value setValue = pop();
                Value elementIndex = peek(0);
                Value arr;

                if (!IS_STRING(elementIndex) && !IS_NUMBER(elementIndex)) {
                    runtimeError("Array index expression must evaluate to positive integer.");
                    return INTERPRET_RUNTIME_ERROR;
                }

                if (tableFindString(&vm.constGlobals, name->chars, name->length, name->hash)) {
                    runtimeError("Variable '%s' is const.", name->chars);
                    return INTERPRET_RUNTIME_ERROR;
                }

                if (!tableGet(&vm.globals, name, &arr)) {
                    runtimeError("Variable '%s' is not defined", name->chars);
                    return INTERPRET_RUNTIME_ERROR;
                }

                if (IS_STRING(elementIndex)) {
                    if (!IS_MAP(arr)) {
                        runtimeError("Element must be a dictionary");
                        return INTERPRET_RUNTIME_ERROR;
                    }
                    ObjDictionary* dict = AS_MAP(arr);
                    ObjString* key = AS_STRING(elementIndex);
                    Value value;

                    if (tableSet(&dict->map, key, setValue)) {
                        runtimeError("'%s' doesn't exist in this dictionary", key->chars);
                        return INTERPRET_RUNTIME_ERROR;
                    }

                    push(value);
                    break;
                }

                if (!IS_ARRAY(arr)) {
                    runtimeError("Indexed variable is not an array");
                    return INTERPRET_RUNTIME_ERROR;
                }

                ObjArray* cachedArray = AS_ARRAY(arr);

                if (!arraySet(cachedArray, AS_NUMBER(elementIndex), setValue)) {
                    ObjString* name = valueTypeToString(cachedArray->type);
                    runtimeError("Error in setting element %g of array. Array type is %s", AS_NUMBER(elementIndex), name->chars);
                    return INTERPRET_RUNTIME_ERROR;
                }

                break;

            }
            case OP_FOR_EACH: {
                int arg = READ_BYTE();
                Value iterable = pop();
                Value item;
                int count = (int)AS_NUMBER(frame->slots[arg]);

                if (!isIterable(iterable)) {
                    runtimeError("Object is not iterable");
                    return INTERPRET_RUNTIME_ERROR;
                }

                switch (AS_OBJ(iterable)->type) {
                    case OBJ_ARRAY: {
                        if (count >= AS_ARRAY(iterable)->values.count) {
                            push(NUMBER_VAL(AS_ARRAY(iterable)->values.count));
                            break;
                        }

                        item = AS_ARRAY(iterable)->values.values[count];
                        frame->slots[arg - 1] = item;
                        frame->slots[arg] = NUMBER_VAL(count);

                        push(NUMBER_VAL(AS_ARRAY(iterable)->values.count));
                        break;
                    }
                    case OBJ_DICTIONARY: {
                        if (count >= AS_MAP(iterable)->map.count) {
                            push(NUMBER_VAL(AS_MAP(iterable)->map.count));
                            break;
                        }

                        item = OBJ_VAL(AS_MAP(iterable)->entries.entries[count].key);
                        frame->slots[arg - 1] = item;
                        frame->slots[arg] = NUMBER_VAL(count);

                        push(NUMBER_VAL(AS_MAP(iterable)->map.count));
                        break;
                    }
                    case OBJ_RANGE: {
                        if (count >= AS_RANGE(iterable)->end) {
                            push(NUMBER_VAL(AS_RANGE(iterable)->end));
                            break;
                        }

                        item = NUMBER_VAL(AS_RANGE(iterable)->current++);
                        frame->slots[arg - 1] = item;
                        frame->slots[arg] = NUMBER_VAL(count);

                        push(NUMBER_VAL(AS_RANGE(iterable)->current));
                        break;
                    }
                    default:
                    // unreacheable
                    runtimeError("Fatal error: unreacheable branch");
                    return INTERPRET_RUNTIME_ERROR;
                }

                break;
            }
            case OP_EQUAL: {
                Value b = pop();
                Value a = pop();

                push(BOOL_VAL(valuesEqual(a, b)));
                break;
            }
            case OP_EQUAL_AND: {
                Value b = pop();
                Value a = pop();
                if (IS_BOOL(a) && IS_BOOL(b)) {
                    bool first = AS_BOOL(b);
                    bool second = AS_BOOL(a);
                    push(BOOL_VAL(first & second));
                    break;
                }

                push(BOOL_VAL(valuesEqual(a, b)));
                break;
            }
            case OP_ADD: {
                if (IS_NUMBER(peek(0)) && IS_NUMBER(peek(1))) {
                    double b = AS_NUMBER(pop());
                    double a = AS_NUMBER(pop());
                    push(NUMBER_VAL(a + b));
                } else {
                    concatenate();
                }
                break;
            }
            case OP_SUBTRACT:
                BINARY_OP(NUMBER_VAL, -);
                break;
            case OP_MULTIPLY:
                BINARY_OP(NUMBER_VAL, *);
                break;
            case OP_DIVIDE:
                BINARY_OP(NUMBER_VAL, /);
                break;
            case OP_NOT:
                push(BOOL_VAL(isFalsey(pop())));
                break;
            case OP_NEGATE:
                if (!IS_NUMBER(peek(0))) {
                    runtimeError(("Operand must be a number."));
                    return INTERPRET_RUNTIME_ERROR;
                }
                push(NUMBER_VAL(-AS_NUMBER(pop())));
                break;
            case OP_LESS:
                BINARY_OP(BOOL_VAL, <);
                break;
            case OP_GREATER:
                BINARY_OP(BOOL_VAL, >);
                break;
            case OP_PRINT:
                printValue(pop());
                printf("\n");
                break;
            case OP_CALL: {
                int argCount = READ_BYTE();
                // printValue(peek(argCount));
                // printf("\n");
                // if (isBuiltIn(peek(argCount))) argCount++;
                // printf("argc: %d\n", argCount);
                if (!callValue(peek(argCount), argCount)) return INTERPRET_RUNTIME_ERROR;
                frame = &vm.frameArray.frames[vm.frameArray.count - 1];
                break;
            }
            case OP_ARRAY_CALL: {
                int argCount = READ_BYTE();

                Value elementIndex = pop();
                Value element;

                if (!IS_NUMBER(elementIndex)) {
                    runtimeError("Array index expression must evaluate to positive integer.");
                    return INTERPRET_RUNTIME_ERROR;
                }

                if (!callValue(peek(argCount), argCount)) return INTERPRET_RUNTIME_ERROR;

                if (!IS_ARRAY(peek(0))) {
                    runtimeError("Return value is not an array.");
                    return INTERPRET_RUNTIME_ERROR;
                }

                if (!arrayGet(AS_ARRAY(pop()), AS_NUMBER(elementIndex), &element)){
                    runtimeError("Index out of bounds.");
                    return INTERPRET_RUNTIME_ERROR;
                }

                frame = &vm.frameArray.frames[vm.frameArray.count - 1];
                push(element);

                break;
            }
            case OP_GET_UPVALUE: {
                int index = READ_BYTE();
                push(*frame->closure->upvalues[index]->location);
                break;

            }
            case OP_SET_UPVALUE: {
                int index = READ_BYTE();
                *frame->closure->upvalues[index]->location = peek(0);
                break;
            }
            case OP_GET_ELEMENT_UPVALUE: {
                printf("Entering get\n");
                int index = READ_BYTE();
                Value elementIndex = pop();
                Value dataStruct = *frame->closure->upvalues[index]->location;
                push(dataStruct);

                if (!IS_STRING(elementIndex) && !IS_NUMBER(elementIndex)) {
                    runtimeError("Index expression must evaluate to positive integer or string for dictionaries");
                    return INTERPRET_RUNTIME_ERROR;
                }

                if (!IS_ARRAY(dataStruct)
                        && !IS_MAP(dataStruct)) {
                    runtimeError("Indexed element is not an array or dictionary");
                    return INTERPRET_RUNTIME_ERROR;
                }

                Value element;
                if (IS_ARRAY(dataStruct)) {
                    if (!arrayGet(AS_ARRAY(dataStruct), AS_NUMBER(elementIndex), &element)){
                        runtimeError("Index out of bounds.");
                        return INTERPRET_RUNTIME_ERROR;
                    }
                } else {
                    if (!tableGet(&AS_MAP(dataStruct)->map, AS_STRING(elementIndex), &element)) {
                        runtimeError("Key not found");
                        return INTERPRET_RUNTIME_ERROR;
                    }
                }

                pop();
                push(element);
                break;

            }
            case OP_SET_ELEMENT_UPVALUE: {
                printf("Entering set\n");
                int index = READ_BYTE();
                Value setValue = pop();
                Value elementIndex = pop();
                Value dataStruct = *frame->closure->upvalues[index]->location;
                push(dataStruct);

                if (!IS_STRING(elementIndex) && !IS_NUMBER(elementIndex)) {
                    runtimeError("Index expression must evaluate to positive integer or string for dictionaries");
                    return INTERPRET_RUNTIME_ERROR;
                }

                if (!IS_ARRAY(dataStruct)
                        && !IS_MAP(dataStruct)) {
                    runtimeError("Indexed element is not an array or dictionary");
                    return INTERPRET_RUNTIME_ERROR;
                }

                if (IS_ARRAY(dataStruct)) {
                    if (!arraySet(AS_ARRAY(*frame->closure->upvalues[index]->location), AS_NUMBER(elementIndex), setValue)) {
                        ObjString* type = valueTypeToString(AS_ARRAY(dataStruct)->type);
                        runtimeError("Error in setting element %g of array. Array type is %s", AS_NUMBER(elementIndex), type->chars);
                        return INTERPRET_RUNTIME_ERROR;
                    }
                } else {
                    if (!tableSet(&AS_MAP(dataStruct)->map, AS_STRING(elementIndex), setValue)) {
                        ObjString* type = valueTypeToString(AS_ARRAY(dataStruct)->type);
                        runtimeError("Error in setting entry %s, elem of type %s of map", AS_STRING(elementIndex), type->chars);
                        return INTERPRET_RUNTIME_ERROR;
                    }
                }

                pop();
                break;
            }
            case OP_GET_ELEMENT_FROM_TOP: {
                Value elem;
                Obj* dataStruct = AS_OBJ(peek(0));
                Value elemIndex = peek(1);

                //printValue(elemIndex);

                switch (dataStruct->type) {
                    case OBJ_ARRAY: {
                        if (elemIndex.type != VAL_NUMBER) {
                            runtimeError("Index must evaluate to positive integer for arrays");
                            return INTERPRET_RUNTIME_ERROR;
                        }
                        if (!arrayGet(AS_ARRAY((peek(0))), AS_NUMBER(elemIndex), &elem)) {
                            runtimeError("Error in getting element from array");
                            return INTERPRET_RUNTIME_ERROR;
                        }

                        break;
                    }
                    case OBJ_DICTIONARY: {
                        if (!IS_STRING(elemIndex)) {
                            runtimeError("Index must evaluate to string for dictionaries");
                            return INTERPRET_RUNTIME_ERROR;
                        }
                        if (!tableGet(&AS_MAP(peek(0))->map, AS_STRING(elemIndex), &elem)) {
                            runtimeError("Value not found");
                            return INTERPRET_RUNTIME_ERROR;
                        }

                        break;
                    }


                    default:
                    runtimeError("Value must be an addressable type");
                    return INTERPRET_COMPILE_ERROR;
                }

                pop();
                pop();
                push(elem);

                break;
            }
            case OP_SWAP: {
                int a = READ_BYTE();
                int b = READ_BYTE();
                Value* first = vm.stackTop - 1 - a;
                Value* second = vm.stackTop - 1 - b;
                Value temp = *first;

                *first = *second;
                *second = temp;
                break;
            }
            case OP_SAVE_VALUE: {
                push(peek(0));
                break;
            }
            case OP_PUSH: {
                int arg = READ_BYTE();
                push(frame->slots[arg]);
                break;
            }
            case OP_REVERSE_N: {
                int n = READ_BYTE();
                Value values[n];
                for (int i = 0; i < n; i++) {
                    values[i] = pop();
                }

                for (int i = 0; i < n; i++) {
                    push(values[i]);
                }

                break;
            }
            case OP_QUEUE: {
                if (!queue(&vm.queue[vm.nestingLevel])) {
                    runtimeError("Error in queueing value");
                    return INTERPRET_RUNTIME_ERROR;
                }
                break;
            }
            case OP_DEQUE: {
                // if (vm.lastIn[vm.nestingLevel] > vm.queue[vm.nestingLevel].count) {
                //     runtimeError("Last element of queue is outside bounds");
                //     return INTERPRET_RUNTIME_ERROR;
                // }

                // printValue(vm.queue[vm.nestingLevel].values[vm.lastIn[vm.nestingLevel]]);
                Value value;
                if (!deque(&vm.queue[vm.nestingLevel], &value)) {
                    runtimeError("Nesting level below zero");
                    return INTERPRET_RUNTIME_ERROR;
                }
                push(value);
                break;
            }
            case OP_QUEUE_REWIND: {
                vm.queue[vm.nestingLevel].count++;
                // printf("rewinded\n");
                break;
            }
            case OP_QUEUE_ADVANCE: {
                vm.queue[vm.nestingLevel].count--;
                break;
            }
            case OP_QUEUE_CLEAR: {
                // vm.lastIn[vm.nestingLevel] = 0;
                vm.queueCount[vm.nestingLevel] = 0;
                initValueArray(&vm.queue[vm.nestingLevel--]);

                if (vm.nestingLevel < 0) vm.nestingLevel++;
                break;
            }
            case OP_INCREMENT_NESTING_LVL: {
                if (vm.nestingLevel == MAX_NESTING_LVL) {
                    runtimeError("Max nesting level is 64");
                    return INTERPRET_RUNTIME_ERROR;
                }

                // printf("Incremented nesting lvl\n");
                initValueArray(&vm.queue[++vm.nestingLevel]);
                break;
            }
            case OP_DECREMENT_NESTING_LVL: {
                if (vm.nestingLevel <= 0) {
                    runtimeError("Error: nesting level below zero");
                    return INTERPRET_RUNTIME_ERROR;
                }
                initValueArray(&vm.queue[vm.nestingLevel--]);
                break;
            }
            case OP_INDIRECT_STORE: {
                Value setVal = pop();
                Value refObj = pop();
                Value refIndex = pop();

                if (!isIterable(refObj)) {
                    runtimeError("Value must be of indexeable type");
                    return INTERPRET_RUNTIME_ERROR;
                }

                switch (AS_OBJ(refObj)->type) {
                    case OBJ_ARRAY: {
                        if (!IS_NUMBER(refIndex)) {
                            runtimeError("Indexing expression must evaluate to positive integer for arrays");
                            return INTERPRET_RUNTIME_ERROR;
                        }

                        ObjArray* arr = AS_ARRAY(refObj);
                        int index = AS_NUMBER(refIndex);

                        if (!arraySet(arr, index, setVal)) {
                            ObjString* name = valueTypeToString(arr->type);
                            runtimeError("Error in setting element %g of array. Array type is %s", index, name->chars);
                            return INTERPRET_RUNTIME_ERROR;
                        }

                        break;
                    }
                    case OBJ_DICTIONARY: {
                        if (!IS_STRING(refIndex)) {
                            runtimeError("Indexing expression must evaluate to string for maps");
                            return INTERPRET_RUNTIME_ERROR;
                        }

                        ObjDictionary* dict = AS_MAP(refObj);
                        ObjString* key = AS_STRING(refIndex);

                        if (tableSet(&dict->map, key, setVal)) {
                            runtimeError("Key '%s' not found in dictionary\n", key->chars);
                            return INTERPRET_RUNTIME_ERROR;
                        }

                        break;
                    }

                }

                push(setVal);
                break;
            }
            case OP_CHECK_TYPE: {
                ValueType type = READ_BYTE();
                if (peek(0).type != type) {
                    ObjString* valueType = valueTypeToString(type);
                    runtimeError("Expected value of type '%s'", valueType->chars);
                    return INTERPRET_RUNTIME_ERROR;
                }

                break;
            }
            case OP_PUSH_FROM: {
                int slot = READ_BYTE();
                push(peek(slot));
                break;
            }
            case OP_RANGE: {
                double end = AS_NUMBER(pop());
                double start = AS_NUMBER(pop());
                ObjRange* range = newRange(start, end);

                push(OBJ_VAL(range));
                break;
            }
            case OP_CLOSE_UPVALUE: {
                closeUpvalues(vm.stackTop - 1);
                pop();
                break;
            }
            case OP_CLOSURE: {
                ObjFunction* function = AS_FUNCTION(READ_CONSTANT());
                ObjClosure* closure = newClosure(function);
                push(OBJ_VAL(closure));

                for (int i = 0; i < closure->function->upvalueCount; i++) {
                    bool isLocal = READ_BYTE();
                    int index = READ_BYTE();

                    if (isLocal) {
                        closure->upvalues[i] = captureUpvalue(frame->slots + index);
                    } else {
                        closure->upvalues[i] = frame->closure->upvalues[index];
                    }
                }
                break;
            }
            case OP_RETURN:
                Value rv = pop();
                closeUpvalues(frame->slots);

                vm.frameArray.count--;
                if (vm.frameArray.count == 0 ) {
                    pop();
                    return INTERPRET_OK;
                }

                vm.stackTop = frame->slots;
                push(rv);

                frame = &vm.frameArray.frames[vm.frameArray.count - 1];
                break;
            case OP_CLASS: {
                push(OBJ_VAL(newClass(READ_STRING())));
                break;
            }

            case OP_GET_PROPERTY: {
                ObjString* name = READ_STRING();

                if (IS_INSTANCE(peek(0))) {
                    ObjInstance* instance = AS_INSTANCE(peek(0));

                    if (!bindMethod(instance->klass, name)) {
                        return INTERPRET_RUNTIME_ERROR;
                    }

                    break;
                }

                ObjClass* nativeClass = NULL;
                if (isBuiltInAndSet(peek(0), &nativeClass)) {

                    if (!bindNativeMethod(nativeClass, name)) {
                        return INTERPRET_RUNTIME_ERROR;
                    }

                    break;
                }

                runtimeError("Only instances can have properties");
                return INTERPRET_RUNTIME_ERROR;

            }
            case OP_SET_PROPERTY: {
                if (!IS_INSTANCE(peek(1))) {
                    runtimeError("Only instances can have properties");
                    return INTERPRET_RUNTIME_ERROR;
                }

                ObjInstance* instance = AS_INSTANCE(peek(1));
                ObjString* fieldName = READ_STRING();
                Value constValue;

                if (tableGet(&instance->fields, fieldName, &constValue)) {
                    if (valuesEqual(constValue, NUMBER_VAL(-1))) {
                        runtimeError("Cannot modify const field");
                        return INTERPRET_RUNTIME_ERROR;
                    }
                }

                tableSet(&instance->fields, fieldName, peek(0));

                // we remove the instance from the stack
                // and leave only the property set value
                Value value = pop();
                pop();
                push(value);
                break;
            }
            case OP_METHOD:
                defineMethod(READ_STRING());
                break;
            case OP_DEFINE_PROPERTY:
                ObjString* name = READ_STRING();
                defineProperty(name, READ_BYTE());
                break;
        }
    }

    #undef READ_BYTE
    #undef READ_WORD
    #undef READ_CONSTANT
    #undef READ_STRING
    #undef BINARY_OP
}


InterpretResult interpret(const char* source) {

    ObjFunction* function = compile(source);
    if (function == NULL) return INTERPRET_COMPILE_ERROR;

    push(OBJ_VAL(function));
    ObjClosure* closure = newClosure(function);
    pop();
    push(OBJ_VAL(closure));
    callValue(OBJ_VAL(closure), 0);

    return run();
}



