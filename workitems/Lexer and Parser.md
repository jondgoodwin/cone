Free up `:` for other uses: type spec and ?

Fix () and [] when used as postfix operators
- Switch generics and macros to ()
- Add parameters to function/index calls (!)

Lexer:
- Raw string literals
- UTF8:
- UTF8 support for IsLetter
- Bad token will not correctly print bad utf8 character code
- Error message line pointer will not correctly handle multi-byte characters

Number literal inference
- Figure out the design for how it works in lexer & with method call selection
- Remove old literal conversion approach
- Fix lexer handling of number literals ‘1:i64’
- Number literal nodes can take an expected type (and default otherwise)
- Move number conversion logic to “into” node and alter gen logic accordingly
- Document up number coercion/conversion in Cone doc.
- Document future number capabilities in this TODO doc