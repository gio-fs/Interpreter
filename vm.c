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

static void defineNative(const char* name, NativeFn function) {
    push(OBJ_VAL(copyString(name, (int)strlen(name))));
    push(OBJ_VAL(newNative(function)));
    tableSet(&vm.globals, AS_STRING(vm.stack.values[0]), vm.stack.values[1]);
    pop();
    pop(); 
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
    }
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

static bool callLambda(ObjClosure* lambda, int argc) {
    if (argc != lambda->function->arity) {
        runtimeError("Expected %d captures but got %d" , lambda->function->arity, argc);
        return false;
    }

    growStack();

    CallFrame* frame = &vm.frameArray.frames[vm.frameArray.count++];
    frame->closure = lambda;
    frame->ip = lambda->function->chunk.code;
    frame->slots = vm.stackTop - 1;

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

    // temporary fallback for obj types
    return copyString("object", 6);
}

static void concatenate() {

    ObjString* b = valueToString(pop());
    ObjString* a = valueToString(pop());

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


static bool isIterable(Value value) {
    return AS_OBJ(value)->type == OBJ_ARRAY || AS_OBJ(value)->type == OBJ_DICTIONARY;
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

    vm.grayCount = 0;
    vm.grayCapacity = 0;
    vm.grayStack = NULL;

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
    #define READ_LONG() (frame->ip += 3, (uint32_t)(frame->ip[-3]) | frame->ip[-2] << 8 | frame->ip[-1] << 16)
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

    for (;;) {

        #ifdef DEBUG_TRACE_EXECUTION
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
                ValueType firstElementType = peek(length - 1).type; 
                ObjArray* arr = newArray(firstElementType);

                for (int i = length - 1; i >= 0; i--) {
                    bool hasAppended = appendArray(arr, peek(i));
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

                if (arr->values.count == 0) arr->type = VAL_NIL;

                push(OBJ_VAL(arr));
                break;
            }
            case OP_ARRAY_LONG: {
                int length = READ_LONG();
                // -1 because peek already returns last element so this way
                // distance becomes -1 -(length - 1),  thus length elements from top
                ValueType firstElementType = peek(length - 1).type; 
                ObjArray* arr = newArray(firstElementType);

                for (int i = length - 1; i >= 0; i--) {
                    bool hasAppended = appendArray(arr, peek(i));
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
                
                if (arr->values.count == 0) arr->type = VAL_NIL;

                push(OBJ_VAL(arr));
                break;
            }
            case OP_MAP: {
                int count = READ_BYTE();
                ObjDictionary* dict = newDictionary();


                for (int i = count - 1; i > 0; i -= 2) {
                    ObjString* key = AS_STRING(peek(i));    
                    Value elem = peek(i - 1);                       
        
                    Entry curr = {.key = key, .value = elem};
                    tableSet(&dict->map, key, elem);
                    writeEntryList(&dict->entries, curr);
                }

                for (int i = 0; i < count; i++) {
                    pop();
                }

                push(OBJ_VAL(dict));
                break;
            }
            case OP_MAP_LONG: {
                uint32_t count = READ_LONG();
                ObjDictionary* dict = newDictionary();

                for (int i = count - 1; i > 0; i -= 2) {
                    Value elem = peek(i - 1);
                    ObjString* key = AS_STRING(peek(i));
                    
                    Entry curr = {.key = key, .value = elem};
                    tableSet(&dict->map, key, elem);
                    writeEntryList(&dict->entries, curr);
                }

                for (int i = 0; i < count; i++) {
                    pop();
                }

                push(OBJ_VAL(dict));
                break;
            }
            case OP_CALL_LAMBDA: {
                int argCount = READ_BYTE();
                printf("argc: %d\n", argCount);
                if (!callLambda(AS_CLOSURE(peek(0)), argCount)) return INTERPRET_RUNTIME_ERROR;

                frame = &vm.frameArray.frames[vm.frameArray.count - 1];
                break;
            }
            case OP_RET_FROM_LAMBDA: {
                Value rv = pop();

                vm.frameArray.count--;
                if (vm.frameArray.count == 0 ) {
                    runtimeError("Lambda shouldn't return from top level");
                    return INTERPRET_OK;
                }

                vm.stackTop = frame->slots;
                push(rv);

                frame = &vm.frameArray.frames[vm.frameArray.count - 1];
                break;
            }
            /*case OP_GET_ARRAY: {
                int slot = READ_BYTE();
                Value elementIndex = pop();

                if (!IS_STRING(elementIndex) && !IS_NUMBER(elementIndex)) {
                    runtimeError("Index must evaluate to positive integer for an array or to a string for dictionaries.");
                    return INTERPRET_RUNTIME_ERROR;
                }

                if (IS_STRING(elementIndex)) {
                    if (!IS_MAP(frame->slots[slot])) {
                        runtimeError("Element must be a dictionary");
                        return INTERPRET_RUNTIME_ERROR;
                    }
                    ObjDictionary* dict = AS_MAP(frame->slots[slot]);
                    ObjString* key = AS_STRING(elementIndex);
                    Value value;

                    if (!tableGet(&dict->map, key, &value)) {
                        runtimeError("Key not found");
                        return INTERPRET_RUNTIME_ERROR;
                    }

                    push(value);
                    break;
                }

                if (!IS_ARRAY(frame->slots[slot])) {
                    runtimeError("Indexed element is not an array");
                    return INTERPRET_RUNTIME_ERROR;
                }

                ObjArray* cachedArr = AS_ARRAY(frame->slots[slot]);
                Value element;
                if (!getArray(cachedArr, AS_NUMBER(elementIndex), &element)){
                    runtimeError("Index out of bounds.");
                    return INTERPRET_RUNTIME_ERROR;
                }
                
                push(element);
                break;
            }*/
           case OP_GET_ARRAY: {
                int slot = READ_BYTE();
                Value elementIndex = pop();

                if (!IS_STRING(elementIndex) && !IS_NUMBER(elementIndex)) {
                    runtimeError("Index must evaluate to positive integer for an array or to a string for dictionaries.");
                    return INTERPRET_RUNTIME_ERROR;
                }

                if (IS_STRING(elementIndex)) {
                    if (!IS_MAP(frame->slots[slot])) {
                        runtimeError("Element must be a dictionary");
                        return INTERPRET_RUNTIME_ERROR;
                    }
                    
                    ObjDictionary* dict = AS_MAP(frame->slots[slot]);
                    ObjString* key = AS_STRING(elementIndex);
                    Value value;

                    if (!tableGet(&dict->map, key, &value)) {
                        printf("DEBUG: Key '%s' not found in dictionary\n", key->chars);
                        runtimeError("Key not found");
                        return INTERPRET_RUNTIME_ERROR;
                    }

                    push(value);
                    break;
                }

                if (!IS_ARRAY(frame->slots[slot])) {
                    runtimeError("Indexed element is not an array");
                    return INTERPRET_RUNTIME_ERROR;
                }

                ObjArray* cachedArr = AS_ARRAY(frame->slots[slot]);
                Value element;
                if (!getArray(cachedArr, AS_NUMBER(elementIndex), &element)){
                    runtimeError("Index out of bounds.");
                    return INTERPRET_RUNTIME_ERROR;
                }
                
                push(element);
                break;
            }   
            case OP_SET_ARRAY: {
                int slot = READ_BYTE();
                Value setValue = pop();
                Value elementIndex = peek(0);
                
                if (!IS_STRING(elementIndex) && !IS_NUMBER(elementIndex)) {
                    runtimeError("Index must evaluate to a positive integer for an array or to a string for dictionaries.");
                    return INTERPRET_RUNTIME_ERROR;
                }

                if (IS_STRING(elementIndex)) {
                    if (!IS_MAP(frame->slots[slot])) {
                        runtimeError("Element must be a dictionary");
                        return INTERPRET_RUNTIME_ERROR;
                    }
                    ObjDictionary* dict = AS_MAP(frame->slots[slot]);
                    ObjString* key = AS_STRING(elementIndex);

                    if (tableSet(&dict->map, key, setValue)) {
                        runtimeError("'%s' doesn't exist in this dictionary", key->chars);
                        return INTERPRET_RUNTIME_ERROR;
                    }

                    break;
                }

                if (!IS_ARRAY(frame->slots[slot])) {
                    runtimeError("Indexed variable is not an array");
                    return INTERPRET_RUNTIME_ERROR;
                }

                ObjArray* cachedArr = AS_ARRAY(frame->slots[slot]);

                if (!setArray(cachedArr, AS_NUMBER(elementIndex), setValue)) {
                    ObjString* name = valueTypeToString(cachedArr->type);
                    runtimeError("Error in setting element %g of array. Array type is %s", AS_NUMBER(elementIndex), name->chars);
                    return INTERPRET_RUNTIME_ERROR;
                }

                break;
            }
            
            case OP_GET_ARRAY_GLOBAL: {
                ObjString* name = READ_STRING();
                Value elementIndex = pop();
                Value arr;

                if ( !IS_STRING(elementIndex) && !IS_NUMBER(elementIndex)) {
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
                if (!getArray(cachedArr, AS_NUMBER(elementIndex), &element)){
                    runtimeError("Index out of bounds.");
                    return INTERPRET_RUNTIME_ERROR;
                }

                push(element);
                break;
            }
            case OP_SET_ARRAY_GLOBAL: {
                ObjString* name = READ_STRING();
                Value setValue = pop();
                Value elementIndex = peek(0);
                Value arr;
                printf("element index : ");
                printValue(elementIndex);
                printf("\n");

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

                if (!setArray(cachedArray, AS_NUMBER(elementIndex), setValue)) {
                    ObjString* name = valueTypeToString(cachedArray->type);
                    runtimeError("Error in setting element %g of array. Array type is %s", AS_NUMBER(elementIndex), name->chars);
                    return INTERPRET_RUNTIME_ERROR;
                }
                
                break;
                
            }
            case OP_FOR_EACH: {
                int arg = READ_BYTE();
                int itArg = READ_BYTE();
                Value iterable = frame->slots[itArg];
                Value item;
                int count = (int)AS_NUMBER(frame->slots[arg]);

                
                if (!isIterable(iterable)) {
                    runtimeError("Object is not iterable");
                    return INTERPRET_RUNTIME_ERROR;
                }

                switch (AS_OBJ(iterable)->type) {
                    case OBJ_ARRAY: { 
                        if (count >= AS_ARRAY(iterable)->values.count) break;

                        item = AS_ARRAY(iterable)->values.values[count];
                        frame->slots[arg - 1] = item;
                        frame->slots[arg] = NUMBER_VAL(count);

                        push(NUMBER_VAL(AS_ARRAY(iterable)->values.count));
                        break;
                    }
                    case OBJ_DICTIONARY: {
                        if (count >= AS_MAP(iterable)->map.count) break;

                        item = OBJ_VAL(AS_MAP(iterable)->entries.entries[count].key);
                        frame->slots[arg - 1] = item;
                        frame->slots[arg] = NUMBER_VAL(count);
                    
                        push(NUMBER_VAL(AS_MAP(iterable)->map.count));
                        break;
                    }

                    default:
                    // unreacheable
                    runtimeError("Fatal error: unreacheable branch");
                    return INTERPRET_RUNTIME_ERROR;
                }

                break;
            }
            case OP_FOR_EACH_GLOBAL: {
                int arg = READ_BYTE();
                ObjString* itName = AS_STRING(READ_CONSTANT());
                Value iterable;
                Value item;
                int count = (int)AS_NUMBER(frame->slots[arg]);

                if (!tableGet(&vm.globals, itName, &iterable) 
                        && !tableGet(&vm.constGlobals, itName, &iterable)) {

                    runtimeError("Global variable '%s' does not exist", itName->chars);
                    return INTERPRET_RUNTIME_ERROR;
                }

                if (!isIterable(iterable)) {
                    runtimeError("Object is not iterable");
                    return INTERPRET_RUNTIME_ERROR;
                }

                switch (AS_OBJ(iterable)->type) {
                    case OBJ_ARRAY: { 
                        if (count >= AS_ARRAY(iterable)->values.count) break;

                        item = AS_ARRAY(iterable)->values.values[count];
                        frame->slots[arg - 1] = item;
                        frame->slots[arg] = NUMBER_VAL(count);

                        push(NUMBER_VAL(AS_ARRAY(iterable)->values.count));
                        break;
                    }
                    case OBJ_DICTIONARY: {
                        if (count >= AS_MAP(iterable)->map.count) break;

                        item = OBJ_VAL(AS_MAP(iterable)->entries.entries[count].key);
                        frame->slots[arg - 1] = item;
                        frame->slots[arg] = NUMBER_VAL(count);

                        push(NUMBER_VAL(AS_MAP(iterable)->map.count));
                        
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
                printf("argc: %d\n", argCount);
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

                if (!getArray(AS_ARRAY(pop()), AS_NUMBER(elementIndex), &element)){
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
            case OP_GET_ARRAY_UPVALUE: {
                printf("Entering get\n");
                int index = READ_BYTE();
                Value elementIndex = pop();
            
                if (!IS_NUMBER(elementIndex)) {
                    runtimeError("Array index must evaluate to positive integer.");
                    return INTERPRET_RUNTIME_ERROR;
                }

                if (!IS_ARRAY(*frame->closure->upvalues[index]->location)) {
                    runtimeError("Indexed element is not an array");
                    return INTERPRET_RUNTIME_ERROR;
                }

                Value element;
                if (!getArray(AS_ARRAY(*frame->closure->upvalues[index]->location), AS_NUMBER(elementIndex), &element)){
                    runtimeError("Index out of bounds.");
                    return INTERPRET_RUNTIME_ERROR;
                }
                
                push(element);
                printf("Exit from get\n");
                break;

            }
            case OP_SET_ARRAY_UPVALUE: {
                printf("Entering set\n");
                int index = READ_BYTE();
                Value setValue = pop();
                Value elementIndex = pop();
                
                if (!IS_NUMBER(elementIndex)) {
                    runtimeError("Array index expression must evaluate to positive integer.");
                    return INTERPRET_RUNTIME_ERROR;
                }

                if (!IS_ARRAY(*frame->closure->upvalues[index]->location)) {
                    runtimeError("Indexed variable is not an array");
                    return INTERPRET_RUNTIME_ERROR;
                }

                if (!setArray(AS_ARRAY(*frame->closure->upvalues[index]->location), AS_NUMBER(elementIndex), setValue)) {
                    ObjString* name = valueTypeToString(AS_ARRAY(*frame->closure->upvalues[index]->location)->type);
                    runtimeError("Error in setting element %g of array. Array type is %s", AS_NUMBER(elementIndex), name->chars);
                    return INTERPRET_RUNTIME_ERROR;
                }
                printf("Exit from set\n");
                break;
            }
            case OP_SAVE_INDEX: {
                push(peek(0));
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



