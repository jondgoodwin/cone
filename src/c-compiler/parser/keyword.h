/** Keyword Initialiation
 * @file
 *
 * This source file is part of the Cone Programming Language C compiler
 * See Copyright Notice in conec.h
*/

#ifndef keyword_h
#define keyword_h

typedef struct AstNode AstNode;

// Primitive numeric types
AstNode *i8Type;
AstNode *i16Type;
AstNode *i32Type;
AstNode *i64Type;
AstNode *u8Type;
AstNode *u16Type;
AstNode *u32Type;
AstNode *u64Type;
AstNode *f32Type;
AstNode *f64Type;

void keywordInit();

#endif
