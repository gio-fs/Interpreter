#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "common.h"
#include "object.h"
#include "chunk.h"
#include "memory.h"
#include "clox_debug.h"
#include "clox_compiler.h"
#include "clox_scanner.h"

Parser parser;

Compiler* current = NULL;

Chunk* compilingChunk;

static void grouping(bool canAssign);
static void unary(bool canAssign);
static void binary(bool canAssign);
static void ternary(bool canAssign);
static void number(bool canAssign);
static void literal(bool canAssign);
static void string(bool canAssign);
static void variable(bool canAssign);
static void interp(bool canAssign);
static void and_(bool canAssign);
static void or_(bool canAssign);
static void call(bool canAssign);
static void array(bool canAssign);
static void dict(bool canAssign);
static void lambda(bool canAssign);

ParseRule rules[] = {
    [TOKEN_LEFT_PAREN]          = {grouping,  call,       PREC_CALL},
    [TOKEN_RIGHT_PAREN]         = {NULL,      NULL,       PREC_NONE},
    [TOKEN_LEFT_BRACE]          = {dict,      NULL,       PREC_NONE},
    [TOKEN_RIGHT_BRACE]         = {NULL,      NULL,       PREC_NONE},
    [TOKEN_COMMA]               = {NULL,      NULL,       PREC_NONE},
    [TOKEN_DOT]                 = {NULL,      NULL,       PREC_NONE},
    [TOKEN_MINUS]               = {unary,     binary,     PREC_TERM},
    [TOKEN_PLUS]                = {NULL,      binary,     PREC_TERM},
    [TOKEN_SEMICOLON]           = {NULL,      NULL,       PREC_NONE},
    [TOKEN_SLASH]               = {NULL,      binary,     PREC_FACTOR},
    [TOKEN_STAR]                = {NULL,      binary,     PREC_FACTOR},
    [TOKEN_BANG]                = {unary,     NULL,       PREC_NONE},
    [TOKEN_BANG_EQUAL]          = {NULL,      binary,     PREC_EQUALITY},
    [TOKEN_EQUAL]               = {NULL,      NULL,       PREC_NONE},
    [TOKEN_EQUAL_EQUAL]         = {NULL,      binary,     PREC_EQUALITY},
    [TOKEN_GREATER]             = {NULL,      binary,     PREC_COMPARISON},
    [TOKEN_GREATER_EQUAL]       = {NULL,      binary,     PREC_COMPARISON},
    [TOKEN_LESS]                = {NULL,      binary,     PREC_COMPARISON},
    [TOKEN_LESS_EQUAL]          = {NULL,      binary,     PREC_COMPARISON},
    [TOKEN_IDENTIFIER]          = {variable,  NULL,       PREC_NONE},
    [TOKEN_STRING]              = {string,    NULL,       PREC_NONE},
    [TOKEN_STRING_WITH_INTERP]  = {interp,    NULL,       PREC_NONE},
    [TOKEN_STRING_INTERP_START] = {NULL,      NULL,       PREC_NONE},
    [TOKEN_NUMBER]              = {number,    NULL,       PREC_NONE},
    [TOKEN_AND]                 = {NULL,      and_,       PREC_AND},
    [TOKEN_CLASS]               = {NULL,      NULL,       PREC_NONE},
    [TOKEN_ELSE]                = {NULL,      NULL,       PREC_NONE},
    [TOKEN_FALSE]               = {literal,   NULL,       PREC_NONE},
    [TOKEN_FOR]                 = {NULL,      NULL,       PREC_NONE},
    [TOKEN_FUNC]                = {NULL,      NULL,       PREC_NONE},
    [TOKEN_IF]                  = {NULL,      NULL,       PREC_NONE},
    [TOKEN_NIL]                 = {literal,   NULL,       PREC_NONE},
    [TOKEN_OR]                  = {NULL,      or_,        PREC_OR},
    [TOKEN_PRINT]               = {NULL,      NULL,       PREC_NONE},
    [TOKEN_RETURN]              = {NULL,      NULL,       PREC_NONE},
    [TOKEN_SUPER]               = {NULL,      NULL,       PREC_NONE},
    [TOKEN_THIS]                = {NULL,      NULL,       PREC_NONE},
    [TOKEN_TRUE]                = {literal,   NULL,       PREC_NONE},
    [TOKEN_VAR]                 = {NULL,      NULL,       PREC_NONE},
    [TOKEN_WHILE]               = {NULL,      NULL,       PREC_NONE},
    [TOKEN_CONST]               = {NULL,      NULL,       PREC_NONE},
    [TOKEN_ERROR]               = {NULL,      NULL,       PREC_NONE},
    [TOKEN_EOF]                 = {NULL,      NULL,       PREC_NONE},
    [TOKEN_QUESTION]            = {NULL,      ternary,    PREC_TERNARY},
    [TOKEN_LEFT_SQUARE_BRACE]   = {array,     NULL,       PREC_NONE}, 
    [TOKEN_RIGHT_SQUARE_BRACE]  = {NULL,      NULL,       PREC_NONE},
    [TOKEN_LAMBDA]              = {lambda,    lambda,     PREC_NONE},
    [TOKEN_MINUS_EQUAL]         = {NULL,      NULL,       PREC_NONE},
    [TOKEN_PLUS_EQUAL]          = {NULL,      NULL,       PREC_NONE}

};


Chunk* currentChunk() {
    return &current->function->chunk;
}

static void initCompiler(Compiler* compiler, FunctionType type) {
    compiler->enclosing = current;
    compiler->function = NULL;
    compiler->type = type;
    compiler->localCount = 0;
    compiler->scopeDepth = 0;
    compiler->function = newFunction();
    current = compiler;

    if (type != TYPE_SCRIPT) {
        current->function->name = copyString(parser.previous.start, parser.previous.length);
    }

    Local* local = &current->locals[current->localCount++];
    local->depth = 0;
    local->name.start = "";
    local->isCaptured = false;
    local->isConst = false;
    local->name.length = 0; 
}

static void errorAt(Token* token, const char* message) {

    if (parser.panicMode) return;
    parser.panicMode = true;

    fprintf(stderr, "[line %d] Error ", token->line);

    if (token->type == TOKEN_EOF) {
        fprintf(stderr, " at end");
    } else if (token->type == TOKEN_ERROR) {

    } else {
        fprintf(stderr, "at '%.*s'\t", token->length, token->start);
    }

    fprintf(stderr, ": %s\n", message);
    parser.hadError = true;
}

 static void error(const char* message) {
    errorAt(&parser.previous, message);
}

static void errorAtCurrent(const char* message) {
    errorAt(&parser.current, message);
}

static Token makeSyntheticToken(const char* src) {
    Token token;
    token.type = TOKEN_IDENTIFIER;
    token.start = src;
    token.length = strlen(src);
    token.line = 1;

    return token;
}

static void advance() {
    parser.previous = parser.current;

    for (;;) {
        parser.current = scanToken();
        if (parser.current.type != TOKEN_ERROR) break;

        errorAtCurrent(parser.current.start);
    }
}

static void consume(TokenType type, const char* message) {
    if (parser.current.type == type) {
        advance();
        return;
    }

    errorAtCurrent(message);
}

static bool checkType(TokenType type) {
    return parser.current.type == type;
}

static bool match(TokenType type) {
    if (!checkType(type)) return false;
    advance();
    return true;
}


uint32_t makeConstant(Value value) {
    uint32_t constant = addConstant(currentChunk(), value);
    return constant;
}

uint32_t identifierConstant(Token* name) {
    return makeConstant(OBJ_VAL(copyString(name->start, name->length)));
}

static bool identifiersEqual(Token* a, Token* b) {
    if (a->length != b->length) return false;
    return memcmp(a->start, b->start, b->length) == 0;
}

static int resolveLocal(Compiler* compiler, Token* name) {
    //Locals are resolved from the top
    for (int i = compiler->localCount - 1; i >= 0; i--) {
        Local* local = &compiler->locals[i];
        if (identifiersEqual(name, &local->name)) {
            if (local->depth == -1) {
                error("Can't read local variable in its own initializer.");
            }
            return i;
        }
    }

    return -1;
}

static void addLocal(Token name, bool isConst) {
    //we add the local to the current scope
    Local* local = &current->locals[current->localCount++];
    local->name = name;
    //marked as uninitialized with depth = -1. Variable is "ready for use" after its
    //initializer is compiled
    local->depth = -1;
    local->isConst = isConst;
    local->isCaptured = false;
}

static int addUpvalue(Compiler* compiler, uint8_t index, bool isLocal) {
    int upvalueCount = compiler->function->upvalueCount;

    for (int i = 0; i < upvalueCount; i++) {
        Upvalue* upvalue = &compiler->upvalues[i];
        if (upvalue->index == index && upvalue->isLocal == isLocal) {
            return i;
        }
    }

    if (upvalueCount == UINT8_COUNT) {
        error("too many upvalues in function");
        return 0;
    }

    compiler->upvalues[upvalueCount].isLocal = isLocal;
    compiler->upvalues[upvalueCount].index = index;
    return compiler->function->upvalueCount++;
}

static int resolveUpvalue(Compiler* compiler, Token* name) {
    if (compiler->enclosing == NULL) return -1;

    int local = resolveLocal(compiler->enclosing, name);
    if (local != -1) {
        compiler->enclosing->locals[local].isCaptured = true;
        return addUpvalue(compiler, (uint8_t)local, true);
    }

    // if upvalue isnt found in next enclosing function, recursion
    // occurs until the upvalue is found. Each function returns its upvalue index 
    // count and adds to its upvalues the index of the enclosing function. Recursion
    // stops if resolveLocal of the enclosing function returns index != -1. If top 
    // level is reached and resolveLocal returns -1, upvalue doesnt exist and 
    // the first calling function returns -1, leading to global/global long.
    int upvalue = resolveUpvalue(compiler->enclosing, name);
    if (upvalue != -1) {
        return addUpvalue(compiler, (uint8_t)upvalue, false);
    }

    return -1;
}

static void markInitialized() {
    if (current->scopeDepth ==  0) return;
    current->locals[current->localCount - 1].depth = current->scopeDepth;
}




static void emitByte(uint8_t byte) {
    writeChunk(currentChunk(), byte, parser.previous.line);
}

static void emitBytes(uint8_t byte1, uint8_t byte2) {
    emitByte(byte1);
    emitByte(byte2);
}

static void emitThreeBytes(uint8_t byte1, uint8_t byte2, uint8_t byte3) {
    emitByte(byte1);
    emitByte(byte2);
    emitByte(byte3);
}

static void emitFourBytes(uint8_t byte1, uint8_t byte2, uint8_t byte3, uint8_t byte4) {
    emitByte(byte1);
    emitByte(byte2);
    emitByte(byte3);
    emitByte(byte4);
}

static void emitConstant(Value value) {

    if (currentChunk()->constants.count < UINT8_MAX) {
        uint8_t constant = makeConstant(value);
        emitBytes(OP_CONSTANT, constant);

    } else {
        uint32_t constantLong = makeConstant(value);
        emitFourBytes(OP_CONSTANT_LONG, (uint8_t)((constantLong & 0x000000ff)),
                                        (uint8_t)((constantLong & 0x0000ff00) >> 8),
                                        (uint8_t)((constantLong & 0x00ff0000) >> 16));

    }
}

static int emitJump(uint8_t instruction) {
    emitByte(instruction);
    emitByte(0xff);
    emitByte(0xff);

    return currentChunk()->count - 2;
}

static void patchJump(int offset) {
    //diff between the end of the then block and the byte before the OP_JUMP_IF_FALSE
    int jump = currentChunk()->count - offset - 2;

    if (jump > UINT16_MAX) {
        error("Jump is larger than 16 bits.");
    }

    //inserting the actual jump value as operand of OP_JUMP_IF_FALSE
    currentChunk()->code[offset] = (jump >> 8) & 0xff;
    currentChunk()->code[offset + 1] = jump & 0xff;
}
static void emitLoop(int loopStart) {
    emitByte(OP_LOOP);

    int offset = currentChunk()->count - loopStart + 2;
    if (offset > UINT16_MAX) error("Loop body is too large.");

    emitByte((offset >> 8) & 0xff);
    emitByte(offset & 0xff);
}

static void emitReturn(FunctionType type) {
    if (type == TYPE_LAMBDA) return;
    emitByte(OP_NIL);
    emitByte(OP_RETURN);
}

static ObjFunction* endCompiler(FunctionType type) {
    emitReturn(type);
    ObjFunction* function = current->function;

#ifdef DEBUG_PRINT_CODE
    if (!parser.hadError) {
        disassembleChunk(currentChunk(), function->name != NULL? function->name->chars : "<script>");
    }
#endif
    current = current->enclosing;
    return function;
}

static void beginScope() {
    current->scopeDepth++;
}

static void endScope() {
    current->scopeDepth--;
    while (current->localCount > 0 
            && current->locals[current->localCount - 1].depth > current->scopeDepth) {
        
        current->locals[current->localCount - 1].isCaptured?  
            emitByte(OP_CLOSE_UPVALUE) : emitByte(OP_POP);
      
        current->localCount--;
    }


}

static void popLocalsAbove(int depth) {
    int popped = 0;
    int i = current->localCount - 1;
    while (i >= 0 && current->locals[i].depth > depth) {
        emitByte(OP_POP);
        popped++;
        i--;
        fprintf(stderr, "[popLocalsAbove] popped %d locals\n", popped);
    }
}


static void expression();
 ParseRule* getRule(TokenType type);
 static void parsePrecedence(Precedence precedence);


static void grouping(bool canAssign) {
    expression();
    consume(TOKEN_RIGHT_PAREN, "Expect ') after expression.");
}

static void unary(bool canAssign) {
    TokenType operatorType = parser.previous.type;

    parsePrecedence(PREC_UNARY);

    switch(operatorType) {
        case TOKEN_BANG: emitByte(OP_NOT); break;
        case TOKEN_MINUS: emitByte(OP_NEGATE); break;

        default: return;
    }
}

static void binary(bool canAssign) {

 // op type
    TokenType operatorType = parser.previous.type;

 // compile the right op
    ParseRule* rule = getRule(operatorType);
    parsePrecedence((Precedence)(rule->precedence + 1));


 // emit the op instruction
    switch (operatorType) {
        case TOKEN_BANG_EQUAL:      emitBytes(OP_EQUAL, OP_NOT); break;
        case TOKEN_EQUAL_EQUAL:     emitByte(OP_EQUAL); break;
        case TOKEN_GREATER:         emitByte(OP_GREATER); break;
        case TOKEN_GREATER_EQUAL:   emitBytes(OP_LESS, OP_NOT); break;
        case TOKEN_LESS:            emitByte(OP_LESS); break;
        case TOKEN_LESS_EQUAL:      emitBytes(OP_GREATER, OP_NOT); break;
        case TOKEN_PLUS:            emitByte(OP_ADD); break;
        case TOKEN_MINUS:           emitByte(OP_SUBTRACT); break;
        case TOKEN_STAR:            emitByte(OP_MULTIPLY); break;
        case TOKEN_SLASH:           emitByte(OP_DIVIDE); break;

        default: return;
  }
}

static void ternary(bool canAssign) {

    int thenJump = emitJump(OP_JUMP_IF_FALSE);
    emitByte(OP_POP);

    parsePrecedence(PREC_TERNARY);
    consume(TOKEN_COLON, "Expect ':' after the then branch");
    
    int elseJump = emitJump(OP_JUMP);
    patchJump(thenJump);
    emitByte(OP_POP);

    parsePrecedence(PREC_TERNARY);
    patchJump(elseJump);
}

static void number(bool canAssign) {
    double value = strtod(parser.previous.start, NULL);
    emitConstant(NUMBER_VAL(value));
}

static void literal(bool canAssign) {
    switch (parser.previous.type) {
        case TOKEN_FALSE:   emitByte(OP_FALSE); break;
        case TOKEN_NIL:     emitByte(OP_NIL); break;
        case TOKEN_TRUE:    emitByte(OP_TRUE); break;

        default: return;
    }
}

static void string(bool canAssign) {

    emitConstant(OBJ_VAL(copyString(parser.previous.start + 1, parser.previous.length - 2)));
}

static void interp(bool canAssign) {

    emitConstant(OBJ_VAL(copyString(parser.previous.start + 1, parser.previous.length - 1)));

    do {
        consume(TOKEN_STRING_INTERP_START, "Expect '${'");
        expression();
        consume(TOKEN_SEMICOLON, "Expect '}'");

        emitByte(OP_ADD);

        if (match(TOKEN_STRING_WITH_INTERP)) {
            // if a string is in the middle of two interpolations, the length is just 
            // the number of the actual characters in the parser
            emitConstant(OBJ_VAL(copyString(parser.previous.start, parser.previous.length)));
            emitByte(OP_ADD);

        } else if (match(TOKEN_STRING)) {
            // this means no more interpolations and we can break from the cycle
            emitConstant(OBJ_VAL(copyString(parser.previous.start, parser.previous.length - 1)));
            emitByte(OP_ADD);
            break;

        } else {
            // 
            break;
        }

    } while (true);
}



static uint8_t argumentList() {
    uint8_t argCount = 0;

    if (!checkType(TOKEN_RIGHT_PAREN)) {
        do {
            expression();

            if (argCount == 255) {
                error("Max number of arguments is 255.");
            }

            argCount++;
        } while (match(TOKEN_COMMA));
    }

    consume(TOKEN_RIGHT_PAREN, "Expect ')' after function arguments");
    return argCount;
}

static void call(bool canAssign) {
    uint8_t argCount = argumentList();
    
    if (match(TOKEN_LEFT_SQUARE_BRACE)) {
        expression();
        consume(TOKEN_RIGHT_SQUARE_BRACE, "Expect ']' after indexing expression");
    
        emitBytes(OP_ARRAY_CALL, argCount);
    }

    emitBytes(OP_CALL, argCount);
}

static int namedVariable(Token name, bool canAssign) {

    int arg = resolveLocal(current, &name);
    bool isArray = false;

    OpCode getOp, setOp, setArrOp, getArrOp;
        
    if (arg != -1) {
        getOp = OP_GET_LOCAL;
        setOp = OP_SET_LOCAL;
        getArrOp = OP_GET_ARRAY;
        setArrOp = OP_SET_ARRAY;
    } else if ((arg = resolveUpvalue(current, &name)) != -1) {
        getOp = OP_GET_UPVALUE;
        setOp = OP_SET_UPVALUE;
        getArrOp = OP_GET_ARRAY_UPVALUE;
        setArrOp = OP_SET_ARRAY_UPVALUE;
    } else if ((arg = identifierConstant(&name)) <= UINT8_MAX) {
        getOp = OP_GET_GLOBAL;
        setOp = OP_SET_GLOBAL;
        getArrOp = OP_GET_ARRAY_GLOBAL;
        setArrOp = OP_SET_ARRAY_GLOBAL;
    } else {
        getOp = OP_GET_GLOBAL_LONG;
        setOp = OP_SET_GLOBAL_LONG;
        getArrOp = OP_GET_ARRAY_GLOBAL_LONG;
        setArrOp = OP_SET_ARRAY_GLOBAL_LONG;
    }

    if (match(TOKEN_LEFT_SQUARE_BRACE)) {
        expression();
        consume(TOKEN_RIGHT_SQUARE_BRACE, "Expect ']' after indexing expression");
        isArray = true;
    }

    bool compoundAssign = match(TOKEN_MINUS_EQUAL) || match(TOKEN_PLUS_EQUAL);
    TokenType compoundType;
    uint8_t _arg;
    if (compoundAssign) compoundType = parser.previous.type;

    if (isArray && compoundAssign) {
        Token slot = makeSyntheticToken("__index_slot");
        addLocal(slot, false);
        current->locals[current->localCount - 1].depth = 1;
        _arg = resolveLocal(current, &slot);
        emitBytes(OP_SET_LOCAL, _arg);
        emitByte(OP_SAVE_INDEX);
    }

    if (canAssign && (match(TOKEN_EQUAL) || match(TOKEN_PLUS_EQUAL) || match(TOKEN_MINUS_EQUAL))) {
        if (setOp == OP_SET_LOCAL && current->locals[arg].isConst) {
            error("Cannot assign to const variable.");
        }

        expression(); 

        if (arg <= UINT8_MAX) {
            if (isArray) emitBytes(setArrOp, (uint8_t)arg);
            else emitBytes(setOp, (uint8_t)arg);
            
        } else {
            if (isArray) emitFourBytes(setArrOp, (uint8_t)((arg & 0x000000ff)), 
                                                 (uint8_t)((arg & 0x0000ff00) >> 8),
                                                 (uint8_t)((arg & 0x00ff0000) >> 16));
            else emitFourBytes(setOp, (uint8_t)((arg & 0x000000ff)), 
                                      (uint8_t)((arg & 0x0000ff00) >> 8),
                                      (uint8_t)((arg & 0x00ff0000) >> 16));
        } 
    } else {
        if (arg <= UINT8_MAX) {
            if (isArray) emitBytes(getArrOp, (uint8_t)arg);
            else emitBytes(getOp, (uint8_t)arg);
            
        } else {
            if (isArray) emitFourBytes(getArrOp, (uint8_t)((arg & 0x000000ff)), 
                                                 (uint8_t)((arg & 0x0000ff00) >> 8),
                                                 (uint8_t)((arg & 0x00ff0000) >> 16));
            else emitFourBytes(getOp, (uint8_t)((arg & 0x000000ff)), 
                                      (uint8_t)((arg & 0x0000ff00) >> 8),
                                      (uint8_t)((arg & 0x00ff0000) >> 16));
        }
    }

    if (canAssign && compoundAssign) {
        if (isArray) emitBytes(OP_GET_LOCAL, _arg);

        if (arg <= UINT8_MAX) {
            if (isArray) emitBytes(setArrOp, (uint8_t)arg);
            else emitBytes(setOp, (uint8_t)arg);
            
        } else {
            if (isArray) emitFourBytes(setArrOp, (uint8_t)((arg & 0x000000ff)), 
                                                 (uint8_t)((arg & 0x0000ff00) >> 8),
                                                 (uint8_t)((arg & 0x00ff0000) >> 16));
            else emitFourBytes(setOp, (uint8_t)((arg & 0x000000ff)), 
                                      (uint8_t)((arg & 0x0000ff00) >> 8),
                                      (uint8_t)((arg & 0x00ff0000) >> 16));
        } 

        expression();
        compoundType == TOKEN_MINUS_EQUAL? emitByte(OP_SUBTRACT) : emitByte(OP_ADD);

        if (arg <= UINT8_MAX) isArray? emitBytes(setArrOp, (uint8_t)arg) : emitBytes(setOp, (uint8_t)arg);
        else {
            if (isArray) emitFourBytes(setArrOp, (uint8_t)((arg & 0x000000ff)), 
                                                 (uint8_t)((arg & 0x0000ff00) >> 8),
                                                 (uint8_t)((arg & 0x00ff0000) >> 16));
            else emitFourBytes(setOp, (uint8_t)((arg & 0x000000ff)), 
                                      (uint8_t)((arg & 0x0000ff00) >> 8),
                                      (uint8_t)((arg & 0x00ff0000) >> 16));
        }

    }

    return arg;
}
 

static void variable(bool canAssign) {
    namedVariable(parser.previous, canAssign);
}

static void declareVariable(bool isConst) {
    //globals are implicitly declared
    if (current->scopeDepth == 0) return;

    Token* name = &parser.previous;
    //if we don't find another variable with the same name in this scope,
    //we add the variable to the locals array
    for (int i = current->localCount - 1; i >= 0; i--) {
        Local* local = &current->locals[i];
        if (local->depth != -1 && local->depth < current->scopeDepth) {
            break;
        }

        if (identifiersEqual(name, &local->name)) {
            error("Another variable with this name is in this scope.");
        }
    }

    addLocal(*name, isConst);
}

static uint32_t parseVariable(const char* errorMessage, bool isConst) {
    consume(TOKEN_IDENTIFIER, errorMessage);

    declareVariable(isConst);
    if (current->scopeDepth > 0) return - 1;

    return identifierConstant(&parser.previous);
}

static void defineVariable(uint32_t global, bool isConst) {
    if (current->scopeDepth > 0) {
        markInitialized();
        return;
    }

    
    if (global <= UINT8_MAX) {
        isConst? emitBytes(OP_DEFINE_CONST_GLOBAL, (uint8_t)global) : emitBytes(OP_DEFINE_GLOBAL, (uint8_t)global);
    } else {
        if (isConst) emitFourBytes(OP_DEFINE_CONST_GLOBAL_LONG, (uint8_t)((global & 0x000000ff)),
                                                                (uint8_t)((global & 0x0000ff00) >> 8),
                                                                (uint8_t)((global & 0x00ff0000) >> 16));
        else emitFourBytes(OP_DEFINE_GLOBAL_LONG, (uint8_t)((global & 0x000000ff)),
                                                  (uint8_t)((global & 0x0000ff00) >> 8),
                                                  (uint8_t)((global & 0x00ff0000) >> 16));                                                    
    }
    
}

static void array(bool canAssign) {
    uint32_t elementsCount = 0;

    do {
        expression();
        elementsCount++;
    } while (match(TOKEN_COMMA));
 
        
    consume(TOKEN_RIGHT_SQUARE_BRACE, "Expect ']' after array initialization");

    if (elementsCount < 256) emitBytes(OP_ARRAY, (uint8_t)elementsCount);
    else emitFourBytes(OP_ARRAY_LONG, (uint8_t)((elementsCount & 0x000000ff)),
                                      (uint8_t)((elementsCount & 0x0000ff00) >> 8),
                                      (uint8_t)((elementsCount & 0x00ff0000) >> 16));
}

static void dict(bool canAssign) {
    uint32_t totalCount = 0;

    do {
        expression();
        totalCount++;
        consume(TOKEN_COLON, "Expect ':' after key value");
        expression();
        totalCount++;
    } while (match(TOKEN_COMMA));

    consume(TOKEN_RIGHT_BRACE, "Expect '}' after dictionary initialization");

    if (totalCount < 256) emitBytes(OP_MAP, (uint8_t)totalCount);
    else emitFourBytes(OP_MAP_LONG, (uint8_t)((totalCount & 0x000000ff)),
                                    (uint8_t)((totalCount & 0x0000ff00) >> 8),
                                    (uint8_t)((totalCount & 0x00ff0000) >> 16));

}


static void varDeclaration(bool isConst) {
    uint32_t global = parseVariable("Expect a variable name.", isConst);

    if (match(TOKEN_EQUAL)) {
        expression();
    } else {
        emitByte(OP_NIL);
    }

    consume(TOKEN_SEMICOLON, "Expect ';' after variable declaration.");

    defineVariable(global, isConst);

}


static void function(FunctionType type);

static void funDeclaration() {
    // TOKEN_CONST makes sense only for variables, so we set the flag isConst to false 
    uint32_t global = parseVariable("Expect function name.", false);
    markInitialized();
    function(TYPE_FUNCTION);
    defineVariable(global, false);
}

static void and_(bool canAssign) {
    int endJump = emitJump(OP_JUMP_IF_FALSE);

    emitByte(OP_POP);
    parsePrecedence(PREC_AND);

    patchJump(endJump);
}

static void or_(bool canAssign) {
    int elseJump = emitJump(OP_JUMP_IF_FALSE);
    int endJump = emitJump(OP_JUMP);

    patchJump(elseJump);
    emitByte(OP_POP);

    parsePrecedence(PREC_OR);
    patchJump(endJump);

    //se l'operando di sinistra è vero -> endJump : return
    //else -> elseJump : parsePrecedence, return
}

ParseRule* getRule(TokenType type) {
    return &rules[type];
}

static void parsePrecedence(Precedence precedence) {
    advance();
    ParseFn prefixRule = getRule(parser.previous.type)->prefix;
    if (prefixRule == NULL) {
        error("Expect expression.");
        return;
    }

    //if parsePrecedence is called with precedence > than assignement
    //canAssign is false, skips the infix loop and correctly
    //communicates the error of invalid assignement
    bool canAssign = precedence <= PREC_ASSIGNMENT;
    prefixRule(canAssign);

    while (precedence <= getRule(parser.current.type)->precedence) {
        printf("Entering midfix loop\n");
        advance();
        ParseFn infixRule = getRule(parser.previous.type)->infix;
        infixRule(canAssign);
    }

    if (canAssign && match(TOKEN_EQUAL)) {
        error("Invalid assignement target.");
    }

    printf("Exiting parsePrecedence\n");

}


static void statement();
static void loopStatement(int loopStart, BreakEntries* breakEntries);

static void expressionStatement() {
    expression();
    printf(" Current token: %d\n", parser.current.type);
    consume(TOKEN_SEMICOLON, "Expect ';' after expression.");
    emitByte(OP_POP);
}

static void printStatement() {
    expression();
    consume(TOKEN_SEMICOLON, "Expect ';' after value.");
    emitByte(OP_PRINT);
}

static void ifStatement(bool insideLoop, int loopStart, BreakEntries* breakEntries) {

    consume(TOKEN_LEFT_PAREN, "Expect '(' before condition.");
    expression();
    consume(TOKEN_RIGHT_PAREN, "Expect ')' after condition.");


    int thenJump = emitJump(OP_JUMP_IF_FALSE);


    emitByte(OP_POP);


    if (insideLoop) {
        loopStatement(loopStart, breakEntries);
    } else {
        statement();
    }


    int elseJump = emitJump(OP_JUMP);


    patchJump(thenJump);
    emitByte(OP_POP);


    if (match(TOKEN_ELSE)) {
        if (insideLoop) {
            loopStatement(loopStart, breakEntries);
        } else {
            statement();
        }
    }


    patchJump(elseJump);
}


static void whileStatement() {

    BreakEntries breakEntries = {.breakCount = 0, .depth = 0};
    int loopStart = currentChunk()->count;
    breakEntries.depth = current->scopeDepth;

    consume(TOKEN_LEFT_PAREN, "Expect '(' before condition.");
    expression();
    consume(TOKEN_RIGHT_PAREN, "Expect ')' after condition.");

    int exitJump = emitJump(OP_JUMP_IF_FALSE);

    emitByte(OP_POP);


    loopStatement(loopStart, &breakEntries);

    emitLoop(loopStart);

    patchJump(exitJump);
    emitByte(OP_POP);

    for (int i = 0; i < breakEntries.breakCount; i++) {
        patchJump(breakEntries.breakJumps[i]);
    }
}




static void forEachStatement(BreakEntries* breakEntries) {
    beginScope();

    addLocal(parser.current, false);
    markInitialized();
    uint8_t arg = resolveLocal(current, &parser.current);

    emitConstant(NIL_VAL);
    emitBytes(OP_SET_LOCAL, arg);
    advance();

    consume(TOKEN_IN, "Expect keyword 'in' after identifier.");

    int itArg = resolveLocal(current, &parser.current);
    if (itArg == -1) itArg = resolveUpvalue(current, &parser.current);
    Token itName = parser.current;
    advance();

    Token count = makeSyntheticToken("__for_each_count");
    addLocal(count, false);
    markInitialized();
    uint8_t _arg = resolveLocal(current, &count);

    emitConstant(NUMBER_VAL(0));
    emitBytes(OP_SET_LOCAL, _arg);

    int loopStart = currentChunk()->count;
    int exitJump = -1;

    if (resolveLocal(current, &itName) != -1
            || resolveUpvalue(current, &itName) != -1) emitThreeBytes(OP_FOR_EACH, _arg, itArg);
    else {
        uint32_t global = identifierConstant(&itName);
        emitThreeBytes(OP_FOR_EACH_GLOBAL, _arg, global);
    }
    
    emitBytes(OP_GET_LOCAL, _arg);
    emitByte(OP_GREATER);

    exitJump = emitJump(OP_JUMP_IF_FALSE);
    emitByte(OP_POP);

    loopStatement(loopStart, breakEntries);

    emitBytes(OP_GET_LOCAL, _arg);
    emitConstant((NUMBER_VAL(1)));
    emitByte(OP_ADD);
    emitBytes(OP_SET_LOCAL, _arg);
    emitByte(OP_POP);
    
    emitLoop(loopStart);

    if (exitJump != -1) {
        patchJump(exitJump);
        emitByte(OP_POP);
    }

    // count popped by OP_GREATER
    current->localCount--;
    endScope();
}


static void forStatement() {
    BreakEntries breakEntries = {.breakCount = 0, .depth = 0};
    breakEntries.depth = current->scopeDepth;

    if (checkType(TOKEN_IDENTIFIER)) {
        forEachStatement(&breakEntries);
        return;
    }

    beginScope();
    consume(TOKEN_LEFT_PAREN, "Expect '(' after 'for'.");

    if (match(TOKEN_SEMICOLON)) {
        // nothing, skips to contition clause
    } else if (match(TOKEN_CONST)) {
        consume(TOKEN_VAR, "Expect 'var' after 'const'.");
        varDeclaration(true);
    } else if (match(TOKEN_VAR)) {
        varDeclaration(false);
    } else {
        expressionStatement();
    }


    int loopStart = currentChunk()->count;

    // Condition clause
    int exitJump = -1;
    if (!match(TOKEN_SEMICOLON)) {

        expression();
        consume(TOKEN_SEMICOLON, "Expect ';' after loop condition.");

        exitJump = emitJump(OP_JUMP_IF_FALSE);
        emitByte(OP_POP);
    }


    // Increment clause
    if (!match(TOKEN_RIGHT_PAREN)) {

        int bodyJump = emitJump(OP_JUMP);

        int incrementStart = currentChunk()->count;
        expression();
        emitByte(OP_POP);
        consume(TOKEN_RIGHT_PAREN, "Expect ')' after for clauses.");

        emitLoop(loopStart);
        loopStart = incrementStart;
        patchJump(bodyJump);
    }


    loopStatement(loopStart, &breakEntries);
    emitLoop(loopStart);


    if (exitJump != -1) {
        patchJump(exitJump);
        emitByte(OP_POP);

    }

    for (int i = 0; i < breakEntries.breakCount; i++) {
        patchJump(breakEntries.breakJumps[i]);
    }

    endScope();
}

static void returnStatement() {
    // if we're outside functions, it's an error to return from top-level
    if (current->type == TYPE_SCRIPT) {
        error("Can't return from top-level function.");
    }
    
    if (match(TOKEN_SEMICOLON)) {
        emitReturn(current->type);
    } else {
        expression();
        consume(TOKEN_SEMICOLON, "Expect ';' after return value");
        if (current->type != TYPE_LAMBDA) emitByte(OP_RETURN);
        else emitByte(OP_RET_FROM_LAMBDA);
    }
}

static void block();
static void loopBlock(int loopStart, BreakEntries* breakEntries);

static void statement() {

    if (match(TOKEN_PRINT)) {
        printStatement();
    } else if (match(TOKEN_IF)) {
        //we're not inside a loop, so insideLoop false and we don't have a loopStart and break entries
        ifStatement(false, -1, NULL);
    } else if (match(TOKEN_WHILE)) {
        whileStatement();
    } else if (match(TOKEN_FOR)) {
        forStatement();
    } else if (match(TOKEN_RETURN)) {
        returnStatement();
    } else if (match(TOKEN_LEFT_BRACE)) {
        beginScope();
        block();
        endScope();
    } else {
        expressionStatement();
    }
}

static void loopStatement(int loopStart, BreakEntries* breakEntries) {
    if (match(TOKEN_PRINT)) {
        printStatement();
    } else if (match(TOKEN_IF)) {
        ifStatement(true, loopStart, breakEntries);
    } else if (match(TOKEN_WHILE)) {
        whileStatement();
    } else if (match(TOKEN_FOR)) {
        forStatement();
    } else if (match(TOKEN_RETURN)) {
        returnStatement();
    } else if (match(TOKEN_CONTINUE)) {
        consume(TOKEN_SEMICOLON, "Expect ';' after statement.");
        popLocalsAbove(breakEntries->depth);
        emitLoop(loopStart);
    } else if (match(TOKEN_BREAK)) {
        consume(TOKEN_SEMICOLON, "Expect ';' after statement.");
        popLocalsAbove(breakEntries->depth);
        breakEntries->breakJumps[breakEntries->breakCount] = emitJump(OP_JUMP);
        breakEntries->breakCount++;
    } else if (match(TOKEN_LEFT_BRACE)) {
        beginScope();
        loopBlock(loopStart, breakEntries);
        endScope();
    } else {
        expressionStatement();
    }
}

static void synchronize() {
    parser.panicMode = false;
    while (parser.current.type != TOKEN_EOF) {
        if (parser.previous.type  == TOKEN_SEMICOLON) return;

        switch (parser.current.type) {
            case TOKEN_CLASS:
            case TOKEN_FUNC:
            case TOKEN_VAR:
            case TOKEN_CONST:
            case TOKEN_FOR:
            case TOKEN_IF:
            case TOKEN_WHILE:
            case TOKEN_PRINT:
            case TOKEN_RETURN:
                return;

            default:
            //nothing. The parser continues to search for a statement
            break;
        }

        advance();
    }
}

static void declaration () {
    if (match(TOKEN_FUNC)) {
        funDeclaration();
    } else if (match(TOKEN_CONST)) {
        consume(TOKEN_VAR, "Expect variable after 'const'.");
        varDeclaration(true);
    } else if (match(TOKEN_VAR)) {
        varDeclaration(false);
    } else {
        statement();
    }

    //the synchronization point for the parser is the end of a statement
    //synchronization is needed because a syntax error could lead
    //to potential misinterpretations and cascade errors.
    if (parser.panicMode) synchronize();
}

static void loopDeclaration(int loopStart, BreakEntries* breakEntries) {
    if (match(TOKEN_FUNC)) {
        funDeclaration();  
    } else if (match(TOKEN_CONST)) {
        consume(TOKEN_VAR, "Expect variable after const.");
        varDeclaration(true);
    } else if (match(TOKEN_VAR)) {
        varDeclaration(false);
    } else {
        loopStatement(loopStart, breakEntries);
    }

    if (parser.panicMode) synchronize();
}

static void block() {
    while(!checkType(TOKEN_RIGHT_BRACE) && !checkType(TOKEN_EOF)) {
        declaration();
    }

    consume(TOKEN_RIGHT_BRACE, "Expect '}' after block.");
}

static void loopBlock(int loopStart, BreakEntries* breakEntries) {
    while(!checkType(TOKEN_RIGHT_BRACE) && !checkType(TOKEN_EOF)) {
        loopDeclaration(loopStart, breakEntries);
    }

    consume(TOKEN_RIGHT_BRACE, "Expect '}' after block.");
}

static void expression() {
    parsePrecedence(PREC_ASSIGNMENT);
}

static void function(FunctionType type) {
    Compiler compiler;
    initCompiler(&compiler, type);
    beginScope();

    // param list
    consume(TOKEN_LEFT_PAREN, "Expect '(' after function name");
    if (!checkType(TOKEN_RIGHT_PAREN)) {

        do {
            current->function->arity++;
            if (current->function->arity > 255) {
                    errorAtCurrent("Can't have more than 255 parameters.");
            }

            if (type == TYPE_LAMBDA) expression();
            else {
                bool isConst = match(TOKEN_CONST);
                uint8_t paramConstant = parseVariable("Expect parameter name.", isConst);
                defineVariable(paramConstant, isConst);
            }
            
        } while (match(TOKEN_COMMA));
        
    }

    consume(TOKEN_RIGHT_PAREN, "Expect ')' after function parameters");


    // body
    consume(TOKEN_LEFT_BRACE, "Expect '{' before function body");
    block();

    // creating function object
    ObjFunction* function = endCompiler(type);
    
    // if (compiler.upvaluesCount == 0)  makeConstant(OBJ_VAL(function));

    emitBytes(OP_CLOSURE, makeConstant(OBJ_VAL(function)));

    for (int i = 0; i < function->upvalueCount; i++) {
        emitByte(compiler.upvalues[i].isLocal ? 1 : 0);
        emitByte(compiler.upvalues[i].index);
        /* 
            printf("  upvalue %d: isLocal=%d, index=%d, name=%s\n",
            i,
            compiler.upvalues[i].isLocal,
            compiler.upvalues[i].index,
            compiler.enclosing->locals[compiler.upvalues[i].index].name);
            printf("Emitting closure for %s:\n");
        */
    }

    if (type == TYPE_LAMBDA) {
        emitBytes(OP_CALL_LAMBDA, function->arity);
    }
    
}

static void lambda(bool canAssign) {
    function(TYPE_LAMBDA);
}

void markCompilerRoots() {
    Compiler* compiler = current;
    while (compiler != NULL) {
        markObj((Obj*)compiler->function);
        compiler = compiler->enclosing;
    }
}


ObjFunction* compile(const char* source) {
    initScanner(source);
    Compiler compiler;
    initCompiler(&compiler, TYPE_SCRIPT);
    compilingChunk = currentChunk();

    parser.hadError = false;
    parser.panicMode = false;

    advance();

    while (!match(TOKEN_EOF)) {
        declaration();
    }

    ObjFunction* function = endCompiler(current->type);
    return parser.hadError ? NULL : function;
}
