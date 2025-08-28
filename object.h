#ifndef clox_object_h
#define clox_object_h
#include "common.h"
#include "value.h"



typedef enum {
    OBJ_STRING,
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

#define OBJ_TYPE(value)     ((AS_OBJ(value))->type)

#define IS_STRING(value)    isObjType(value, OBJ_STRING)

#define AS_STRING(value)    ((ObjString*)AS_OBJ(value)) //points to an objstring on heap
#define AS_CSTRING(value)   (((ObjString*)AS_OBJ(value))->chars) //points directly to the chars array of the string


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