#ifndef clox_compiler_h
#define clox_compiler_h
#include "vm.h"
#include "clox_scanner.h"

typedef struct {
    Token previous;
    Token current;
    bool hadError;
    bool panicMode;
} Parser;

 typedef enum {
    PREC_NONE,
    PREC_ASSIGNMENT,
    PREC_TERNARY,
    PREC_OR,
    PREC_AND,
    PREC_EQUALITY,
    PREC_COMPARISON,
    PREC_TERM,
    PREC_FACTOR,
    PREC_UNARY,
    PREC_CALL,
    PREC_PRIMARY
 } Precedence;

typedef void (*ParseFn)(bool canAssign);

typedef struct {
    ParseFn prefix;
    ParseFn infix;
    Precedence precedence;
} ParseRule;

typedef struct {
    Token name;
    int depth;
    bool isCaptured;
    bool isConst;
} Local;

typedef struct {
    uint8_t index;
    bool isLocal;
} Upvalue;
typedef enum {
    TYPE_FUNCTION,
    TYPE_SCRIPT,
    TYPE_LAMBDA
} FunctionType;
typedef struct Compiler {
    // linked list to keep track of the compiler struct of each function
    struct Compiler* enclosing;
    ObjFunction* function;
    FunctionType type;

    Local locals[UINT8_COUNT];
    int localCount;
    Upvalue upvalues[UINT8_COUNT];
    int upvaluesCount;
    int scopeDepth;
} Compiler;

typedef struct {
    int breakJumps[UINT8_MAX];
    int breakCount;
    int depth;
} BreakEntries;


ObjFunction* compile(const char* source);

#endif
