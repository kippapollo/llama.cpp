#ifndef LLAMA_CPP_FORCED_GRAMMAR_H
#define LLAMA_CPP_FORCED_GRAMMAR_H

// Enforced regardless of user-provided grammar / json_schema.
#define LLAMA_CPP_FORCED_GRAMMAR R"GRAMMAR(
# GBNF Grammar for Coding Agent - Simplified
# Allows natural English responses and code generation
root ::= response
response ::= rejection-message | normal-response
# Exact rejection for non-coding requests
rejection-message ::= "I can only assist with coding-related requests."
# Normal response allows natural text flow
normal-response ::= (token | whitespace)+
token ::= en-char+
whitespace ::= [ \t\n\r]+
en-char ::= letter | digit | punctuation
letter ::= [a-zA-Z]
digit ::= [0-9]
punctuation ::= [!"#$%&'()*+,-./:;<=>?@[\\\]^_`{|}~]
)GRAMMAR"

#endif // LLAMA_CPP_FORCED_GRAMMAR_H
