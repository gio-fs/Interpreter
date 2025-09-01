#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <string.h>
#include "common.h"
#include "memory.h"
#include "object.h"
#include "value.h"
#include "clox_compiler.h"
#include "clox_debug.h"
#include "vm.h"




VM vm;

static void resetStack() {
    vm.stackTop = vm.stack.values;
}

static void runtimeError(const char* format, ...) {

    va_list args; //pointer to the first argument
    va_start(args, format); //init args to format
    vfprintf(stderr, format, args);
    va_end(args); //frees args
    fputs("\n", stderr);

    size_t instruction = vm.ip - vm.chunk->code - 1;
    int line = getLine(vm.chunk, vm.chunk->lines[instruction].line);
    fprintf(stderr, "[line %d] ins script\n", line);


    resetStack();
}

static void growStack() {
    int oldCapacity = vm.stack.capacity;
    int size = (int)(vm.stackTop - vm.stack.values); // elementi reali

    vm.stack.capacity = GROW_CAPACITY(oldCapacity);
    vm.stack.values = GROW_ARRAY(Value, vm.stack.values, oldCapacity, vm.stack.capacity);

    vm.stackTop = vm.stack.values + size;
}


static void checkCapacity() {
    if (vm.stack.capacity <= (int)(vm.stackTop - vm.stack.values)) growStack();
}

void initVM() {

    vm.stack.count = 0;
    vm.stack.capacity = 256;
    vm.stack.values = NULL;
    vm.stack.values = GROW_ARRAY(Value, vm.stack.values, 0, vm.stack.capacity);
    vm.stackTop = vm.stack.values;
    vm.objects = NULL;

    initTable(&vm.strings);
    initTable(&vm.globals);
    initTable(&vm.constGlobals);
}

void freeVM() {

    FREE_ARRAY(Value, vm.stack.values, vm.stack.capacity);
    vm.stack.capacity = 0;
    vm.stackTop = NULL;


    freeTable(&vm.strings);
    freeTable(&vm.globals);
    freeTable(&vm.constGlobals);
    freeObjects();
}

static Value peek(int distance) {
    return vm.stackTop[-1 - distance];
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

    //temporary fallback for other obj types
    return copyString("<object>", 8);
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

    //newString is a temp value on heap:
    //the string object (result) itself is in the heap, and it's chars field points
    //to the same memory location of temp, thus taking the ownership of the memory.
    //Now temp doesn't have anymore the responsibility of freeing that memory
    //and will be handled by the GC when result will be freed. The actual array of characters
    //never changes place, only the ownership switches between temp and result

    ObjString* result = takeString(newString, length);
    push(OBJ_VAL(result));

}

static InterpretResult run() {

    #define READ_BYTE() (*vm.ip++)
    #define READ_WORD() (vm.ip += 2, (uint16_t)(vm.ip[-2] << 8| vm.ip[-1]))
    #define READ_CONSTANT() (vm.chunk->constants.values[READ_BYTE()])
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
            disassembleInstruction(vm.chunk, (int)(vm.ip - vm.chunk->code));
        #endif

        checkCapacity();
        uint8_t instruction = READ_BYTE();

        switch (instruction) {
            case OP_CONSTANT: {
                Value constant = READ_CONSTANT();
                push(constant);
                break;
            }
            case OP_CONSTANT_LONG: {
                uint32_t longIndex = (READ_BYTE() | READ_BYTE() << 8 | READ_BYTE() << 16); //longIndex is 3 bytes
                Value constantLong = vm.chunk->constants.values[longIndex];
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
                vm.ip += offset;
                break;
            }
            case OP_LOOP: {
                uint16_t offset = READ_WORD();
                vm.ip -= offset;
                break;
            }
            case OP_JUMP_IF_FALSE: {
                uint16_t offset = READ_WORD();
                //checking the if condtition on top of the stack
                if (isFalsey(peek(0))) vm.ip += offset;
                break;
            }
            case OP_GET_LOCAL: {
                //we need to push the local's value on top of the stack since
                //other bytecode instructions only look for stackTop - 1
                uint8_t slot = READ_BYTE();
                push(vm.stack.values[slot]);
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
                ObjString* name = AS_STRING(vm.chunk->constants.values[longIndex]);
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
                ObjString* name = AS_STRING(vm.chunk->constants.values[longIndex]);
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
                ObjString* name = AS_STRING(vm.chunk->constants.values[longIndex]);
                Value value = peek(0);
                if (!tableSet(&vm.constGlobals, name, value) || tableGet(&vm.globals, name, &value)) {
                    runtimeError("Variable '%s' is already defined.", name->chars);
                    return INTERPRET_RUNTIME_ERROR;
                }
                pop();
                break;
            }
            case OP_SET_LOCAL: {
                //we need to push the local's value on top of the stack since
                //other bytecode instructions only look for stackTop - 1
                uint8_t slot = READ_BYTE();
                vm.stack.values[slot] = peek(0);

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
                ObjString* name = AS_STRING(vm.chunk->constants.values[longIndex]);
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
            case OP_EQUAL: {
                Value b = pop();
                Value a = pop();
                push(BOOL_VAL(valuesEqual(a, b)));
                break;
            }
            case OP_ADD: {
                if ((IS_NUMBER(peek(0)) && IS_STRING(peek(1))) || (IS_STRING(peek(0)) && IS_NUMBER(peek(1))) || (IS_STRING(peek(0)) && IS_STRING(peek(1)))) {
                    concatenate();
                } else if (IS_NUMBER(peek(0)) && IS_NUMBER(peek(1))) {
                    double b = AS_NUMBER(pop());
                    double a = AS_NUMBER(pop());
                    push(NUMBER_VAL(a + b));
                } /*else {
                    runtimeError("Operands must be both numbers or strings.");
                    return INTERPRET_RUNTIME_ERROR;
                }*/
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

                return INTERPRET_OK;
        }
    }

    #undef READ_BYTE
    #undef READ_WORD
    #undef READ_CONSTANT
    #undef READ_STRING
    #undef BINARY_OP
}


InterpretResult interpret(const char* source) {

    Chunk chunk;
    initChunk(&chunk);

    if(!compile(source, &chunk)) {
        freeChunk(&chunk);
        return INTERPRET_COMPILE_ERROR;
    }

    vm.chunk = &chunk;
    vm.ip = vm.chunk->code;

    InterpretResult result = run();

    freeChunk(&chunk);
    return result;
}

void push(Value value) {
    *vm.stackTop = value;
    vm.stackTop++;
}

Value pop() {
    vm.stackTop--;
    return *vm.stackTop;
}

