Here's my current implementation of the Lox language by the "Crafting Interpreters" book. 
Added arrays and dictionaries as built-in types wrapped with native classes in order to use factory
methods like get() add() and set() (looking forward to add multiple others). Added 'iterators' (for each loop) with support of lazy evaluation of ranges, added also compound assignement. Added 'match' expression/statement,
taking inspiration from Rust's syntax. Functions can be lambdas by using the keyword 'lambda' before the function declaration. Other minor features are string interpolation, ternary operator,
 possibility to declare class fields, break and continue inside loops, const fields/variabes, long instructions (for a maximum of 65536 constants per compiler/chunk), optimized line getter, dynamic stack. 

