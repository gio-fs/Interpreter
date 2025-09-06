#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <string.h>
#include <time.h>
#include "common.h"
#include "memory.h"
#include "object.h"
#include "value.h"
#include "clox_compiler.h"
#include "clox_debug.h"
#include "vm.h"




VM vm;

static Value clockNative(int argCount, Value* args) {
    return NUMBER_VAL((double)clock() / CLOCKS_PER_SEC);
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

void push(Value value) {
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
        ObjFunction* function = frame->function;

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

static void defineNative(const char* name, NativeFn function) {
    push(OBJ_VAL(copyString(name, (int)strlen(name))));
    push(OBJ_VAL(newNative(function)));
    tableSet(&vm.globals, AS_STRING(vm.stack.values[0]), vm.stack.values[1]);
    pop();
    pop();
}

static bool call(ObjFunction* function, int argc) {
    if (argc != function->arity) {
        runtimeError("Expected %d arguments but got %d" , function->arity, argc);
        return false;
    }

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
    }
    
    CallFrame* frame = &vm.frameArray.frames[vm.frameArray.count++];
    frame->function = function;
    frame->ip = function->chunk.code;

    // give the frame it's window from the stack: the "- 1" it's to skip the function obj
    frame->slots = vm.stackTop - argc - 1;
    return true;
}

static bool callValue(Value callee, int argCount) {
    if (IS_OBJ(callee)) {
        switch (OBJ_TYPE(callee)) {
            case OBJ_FUNCTION: return call(AS_FUNCTION(callee), argCount);
            case OBJ_NATIVE: {
                NativeFn native = AS_NATIVE(callee);
                Value result = native(argCount, vm.stackTop - argCount);
                vm.stackTop -= argCount + 1;
                push(result);
                return true;
            }
            default:
            // handling non callable objects
            break;
        } 
    }
    
    runtimeError("Callee must be a function or a class.");
    return false;
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

static ObjString* valueToString(Value value) {
    if (IS_STRING(value)) {
        return AS_STRING(value);

    } else if (IS_NUMBER(value)) {
        double num = AS_NUMBER(value);
        char buffer[40];

        // snprintf copies char to buffer with a specific format
        if (num == (int)num) {
            snprintf(buffer, sizeof(buffer), "%.0f", num);
        } else {
            snprintf(buffer, sizeof(buffer), "%g", num);
        }

        return copyString(buffer, strlen(buffer));

    } else if (IS_BOOL(value)) {
        const char* boolStr = AS_BOOL(value) ? "true" : "false";
        return copyString(boolStr, strlen(boolStr));

    } else if (IS_NIL(value)) {
        return copyString("nil", 3);
    }

    // temporary fallback for other obj types
    return copyString("object", 6);
}

static void concatenate() {

    ObjString* b = valueToString(pop());
    ObjString* a = valueToString(pop());

    if (b->chars[0] == ' ') b->length = 1;

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

}

void initVM() {
    
    vm.stack.count = 0;
    vm.stack.capacity = STACK_INIT_CAPACITY;
    vm.stack.values = ALLOCATE(Value, vm.stack.capacity);
    vm.stackTop = vm.stack.values;
    vm.objects = NULL;

    initCallFrameArray(&vm.frameArray);

    initTable(&vm.strings);
    initTable(&vm.globals);
    initTable(&vm.constGlobals);

    defineNative("clock", clockNative);
}

void freeVM() {
    
    freeCallFrameArray(&vm.frameArray);

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
    #define READ_CONSTANT() (frame->function->chunk.constants.values[READ_BYTE()])
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

    for (;;) {

        #ifdef DEBUG_TRACE_EXECUTION
            printf("\n");
            for (Value* slot = vm.stack.values; slot < vm.stackTop; slot++) {
                printf("[ ");
                printValue(*slot);
                printf(" ] ");
            }
            printf("\n");
            printf("\n");
            disassembleInstruction(&frame->function->chunk, (int)(frame->ip - frame->function->chunk.code));
        #endif

        uint8_t instruction = READ_BYTE();

        switch (instruction) {
            case OP_CONSTANT: {
                Value constant = READ_CONSTANT();
                push(constant);
                break;
            }
            case OP_CONSTANT_LONG: {
                uint32_t longIndex = (READ_BYTE() | READ_BYTE() << 8 | READ_BYTE() << 16); // longIndex is 3 bytes
                Value constantLong = frame->function->chunk.constants.values[longIndex];
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
                uint32_t longIndex = (READ_BYTE() | READ_BYTE() << 8 | READ_BYTE() << 16);
                ObjString* name = AS_STRING(frame->function->chunk.constants.values[longIndex]);
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
                uint32_t longIndex = (READ_BYTE() | READ_BYTE() << 8 | READ_BYTE() << 16);
                ObjString* name = AS_STRING(frame->function->chunk.constants.values[longIndex]);
                Value value = peek(0);
                if (!tableSet(&vm.globals, name, value) || tableGet(&vm.constGlobals, name, &value)) {
                    runtimeError("Variable '%s' is already defined.", name->chars);
                    return INTERPRET_RUNTIME_ERROR;
                }
                pop();
                break;
            }
            case OP_DEFINE_CONST_GLOBAL_LONG: {
                uint32_t longIndex = (READ_BYTE() | READ_BYTE() << 8 | READ_BYTE() << 16);
                ObjString* name = AS_STRING(frame->function->chunk.constants.values[longIndex]);
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
                uint32_t longIndex = (READ_BYTE() | READ_BYTE() << 8 | READ_BYTE() << 16);
                ObjString* name = AS_STRING(frame->function->chunk.constants.values[longIndex]);
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
            case OP_CALL: {
                int argCount = READ_BYTE();
                if (!callValue(peek(argCount), argCount)) return INTERPRET_RUNTIME_ERROR;

                frame = &vm.frameArray.frames[vm.frameArray.count - 1];
                break;
            }
            case OP_ARRAY: {
                int length = READ_BYTE();
                // -1 because peek already returns last element so this way
                // distance becomes -1 -(length - 1),  thus length elements from top
                ValueType firstElementType = peek(length - 1).type; 
                if (firstElementType == VAL_NIL) {
                    runtimeError("Nil arrays are not allowed");
                    return INTERPRET_RUNTIME_ERROR;
                }
                ObjArray* arr = newArray(firstElementType);

                for (int i = length - 1; i >= 0; i--) {
                    bool hasAppended = append(arr, peek(i));
                    if (!hasAppended) {
                        ObjString* errorType = valueTypeToString(peek(i).type);
                        ObjString* arrType = valueTypeToString(firstElementType);
                        runtimeError("Expected a value of type %s but tried to append %s", arrType->chars, errorType->chars);
                        return INTERPRET_RUNTIME_ERROR;
                    }
                }

                for (int i = length - 1; i >= 0; i--) {
                    pop();
                }

                push(OBJ_VAL(arr));
                break;
            }
            case OP_ARRAY_LONG: {
                int length = (READ_BYTE() | READ_BYTE() << 8 | READ_BYTE() << 16);
                // -1 because peek already returns last element so this way
                // distance becomes -1 -(length - 1),  thus length elements from top
                ValueType firstElementType = peek(length - 1).type; 
                ObjArray* arr = newArray(firstElementType);

                for (int i = length - 1; i >= 0; i--) {
                    bool hasAppended = append(arr, peek(i));
                    if (!hasAppended) {
                        ObjString* errorType = valueTypeToString(peek(i).type);
                        ObjString* arrType = valueTypeToString(firstElementType);
                        runtimeError("Expected a value of type %s but tried to append %s", arrType->chars, errorType->chars);
                        return INTERPRET_RUNTIME_ERROR;
                    }
                }

                for (int i = length - 1; i >= 0; i--) {
                    pop();
                }

                push(OBJ_VAL(arr));
                break;
            }
            case OP_GET_ARRAY_ELEMENT: {
                int arrIndex = READ_BYTE();
                Value elementIndex = pop();

                if (!IS_NUMBER(elementIndex)) {
                    runtimeError("Array index must evaluate to positive integer.");
                    return INTERPRET_RUNTIME_ERROR;
                }

                if (!IS_ARRAY(frame->slots[arrIndex])) {
                    runtimeError("Indexed element is not an array");
                    return INTERPRET_RUNTIME_ERROR;
                }

                ObjArray* cachedArr = AS_ARRAY(frame->slots[arrIndex]);

                // ObjString* type =  valueTypeToString(AS_ARRAY(frame->slots[arrIndex])->type);
                // printf("Array type: %s\n", type->chars);

                switch (cachedArr->type) {
                    case VAL_BOOL: 
                        push(BOOL_VAL((AS_BOOL(get(cachedArr, AS_NUMBER(elementIndex))))));
                        break;
                    case VAL_NIL: {
                        // this should be unreacheable since nil type checking is in array initialization
                        runtimeError("Nil arrays are not allowed");
                        return INTERPRET_RUNTIME_ERROR;
                    }
                    case VAL_NUMBER: 
                        push(NUMBER_VAL((AS_NUMBER(get(cachedArr, AS_NUMBER(elementIndex))))));
                        break;
                    case VAL_OBJ: 
                        push(OBJ_VAL((AS_OBJ(get(cachedArr, AS_NUMBER(elementIndex))))));
                        break;
                    
                    default: 
                        // unreacheable
                        runtimeError("Fatal error: array type is unknown");
                        return INTERPRET_RUNTIME_ERROR;
                }

                break;
            }
            case OP_SET_ARRAY_ELEMENT: {
                int arrIndex = READ_BYTE();
                Value setValue = pop();
                Value elementIndex = peek(0);
                
                if (!IS_NUMBER(elementIndex)) {
                    runtimeError("Array index expression must evaluate to positive integer.");
                    return INTERPRET_RUNTIME_ERROR;
                }

                if (!IS_ARRAY(frame->slots[arrIndex])) {
                    runtimeError("Indexed variable is not an array");
                    return INTERPRET_RUNTIME_ERROR;
                }

                ObjArray* cachedArr = AS_ARRAY(frame->slots[arrIndex]);

                if (!set(cachedArr, AS_NUMBER(elementIndex), setValue)) {
                    ObjString* name = valueTypeToString(cachedArr->type);
                    runtimeError("Error in setting element %g of array. Array type is %s", AS_NUMBER(elementIndex), name->chars);
                    return INTERPRET_RUNTIME_ERROR;
                }

                break;
            }
            case OP_GET_ARRAY_GLOBAL_ELEMENT: {
                ObjString* name = READ_STRING();
                Value elementIndex = pop();
                Value arr;

                if (!IS_NUMBER(elementIndex)) {
                    runtimeError("Array index must evaluate to positive integer.");
                    return INTERPRET_RUNTIME_ERROR;
                }

                // ObjString* type =  valueTypeToString(AS_ARRAY(frame->slots[arrIndex])->type);
                // printf("Array type: %s\n", type->chars);

                if(!tableGet(&vm.globals, name, &arr)) {
                    runtimeError("Undefined variable '%s' .", name->chars);
                    return INTERPRET_RUNTIME_ERROR;
                }
                
                if (!IS_ARRAY(arr)) {
                    runtimeError("Indexed variable is not an array");
                    return INTERPRET_RUNTIME_ERROR;
                }

                ObjArray* cachedArr = AS_ARRAY(arr);

                switch (cachedArr->type) {
                    case VAL_BOOL: {
                        Value result = BOOL_VAL((AS_BOOL(get(cachedArr, AS_NUMBER(elementIndex)))));
                        push(result);
                        break;
                    }
                        
                    case VAL_NIL: {
                        // this should be unreacheable since nil type checking is in array initialization
                        runtimeError("Nil arrays are not allowed");
                        return INTERPRET_RUNTIME_ERROR;
                    }
                    case VAL_NUMBER: {
                        Value result = NUMBER_VAL((AS_NUMBER(get(cachedArr, AS_NUMBER(elementIndex)))));
                        push(result);
                        break;
                    }
                        
                    case VAL_OBJ: {
                         Value result = OBJ_VAL((AS_OBJ(get(cachedArr, AS_NUMBER(elementIndex)))));
                        push(result);
                        break;
                    }
        
                    default: 
                        // unreacheable
                        runtimeError("Fatal error: array type is unknown");
                        return INTERPRET_RUNTIME_ERROR;
                }

                break;
            }
            case OP_SET_ARRAY_GLOBAL_ELEMENT: {
                ObjString* name = READ_STRING();
                Value setValue = pop();
                Value elementIndex = peek(0);
                Value arr;

                if (tableFindString(&vm.constGlobals, name->chars, name->length, name->hash)) {
                    runtimeError("Variable '%s' is const.", name->chars);
                    return INTERPRET_RUNTIME_ERROR;
                }

                if (!tableGet(&vm.globals, name, &arr)) {
                    runtimeError("Variable '%s' is not defined", name->chars);
                    return INTERPRET_RUNTIME_ERROR;
                }

                if (!IS_ARRAY(arr)) {
                    runtimeError("Indexed variable is not an array");
                    return INTERPRET_RUNTIME_ERROR;
                }

                ObjArray* cachedArray = AS_ARRAY(arr);

                if (!set(cachedArray, AS_NUMBER(elementIndex), setValue)) {
                    ObjString* name = valueTypeToString(cachedArray->type);
                    runtimeError("Error in setting element %g of array. Array type is %s", AS_NUMBER(elementIndex), name->chars);
                    return INTERPRET_RUNTIME_ERROR;
                }
                
                if (tableSet(&vm.globals, name, OBJ_VAL(cachedArray))) {
                    tableDelete(&vm.globals, name);
                    runtimeError("Undefined variable '%s'", name->chars);
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
            case OP_RETURN:
                Value rv = pop();
                
                vm.frameArray.count--;
                if (vm.frameArray.count == 0 ) {
                    pop();
                    return INTERPRET_OK;
                }

                vm.stackTop = frame->slots;
                push(rv);

                frame = &vm.frameArray.frames[vm.frameArray.count - 1];
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
    callValue(OBJ_VAL(function), 0);

    return run();
}



