%option noyywrap
%{
/*****************声明和选项设置  begin*****************/
#include <stdio.h>
#include <stdlib.h>

#include "syntax_tree.h"
#include "syntax_analyzer.h"

int lines = 1;
int pos_start = 1;
int pos_end = 1;

void pass_node(char *text){
     yylval.node = new_syntax_tree_node(text);
}

/*****************声明和选项设置  end*****************/

%}


%%
 /* to do for students */
 /* two cases for you, pass_node will send flex's token to bison */
"else"    { pos_start = pos_end; pos_end += yyleng; pass_node(yytext); return ELSE; }
"if"      { pos_start = pos_end; pos_end += yyleng; pass_node(yytext); return IF; }
"int"     { pos_start = pos_end; pos_end += yyleng; pass_node(yytext); return INT; }
"return"  { pos_start = pos_end; pos_end += yyleng; pass_node(yytext); return RETURN; }
"void"    { pos_start = pos_end; pos_end += yyleng; pass_node(yytext); return VOID; }
"while"   { pos_start = pos_end; pos_end += yyleng; pass_node(yytext); return WHILE; }
"float"   { pos_start = pos_end; pos_end += yyleng; pass_node(yytext); return FLOAT; }

"+"     { pos_start = pos_end; pos_end += yyleng; pass_node(yytext); return ADD; }
"-"     { pos_start = pos_end; pos_end += yyleng; pass_node(yytext); return SUB; }
"*"     { pos_start = pos_end; pos_end += yyleng; pass_node(yytext); return MUL; }
"/"     { pos_start = pos_end; pos_end += yyleng; pass_node(yytext); return DIV; }
"<"     { pos_start = pos_end; pos_end += yyleng; pass_node(yytext); return LESSTHAN; }
"<="    { pos_start = pos_end; pos_end += yyleng; pass_node(yytext); return LEQ; }
">"     { pos_start = pos_end; pos_end += yyleng; pass_node(yytext); return GREATERTHAN; }
">="    { pos_start = pos_end; pos_end += yyleng; pass_node(yytext); return GEQ; }
"=="    { pos_start = pos_end; pos_end += yyleng; pass_node(yytext); return EQUALITY; }
"!="    { pos_start = pos_end; pos_end += yyleng; pass_node(yytext); return NOTEQUAL; }
"="     { pos_start = pos_end; pos_end += yyleng; pass_node(yytext); return EQUAL; }

";"     { pos_start = pos_end; pos_end += yyleng; pass_node(yytext); return SEMICOLON; }
","     { pos_start = pos_end; pos_end += yyleng; pass_node(yytext); return COMMA; }
"("     { pos_start = pos_end; pos_end += yyleng; pass_node(yytext); return LPAREN; }
")"     { pos_start = pos_end; pos_end += yyleng; pass_node(yytext); return RPAREN; }
"["     { pos_start = pos_end; pos_end += yyleng; pass_node(yytext); return LBRACKET; }
"]"     { pos_start = pos_end; pos_end += yyleng; pass_node(yytext); return RBRACKET; }
"{"     { pos_start = pos_end; pos_end += yyleng; pass_node(yytext); return LBRACE; }
"}"     { pos_start = pos_end; pos_end += yyleng; pass_node(yytext); return RBRACE; }

[a-zA-Z]+  {
    pos_start = pos_end;
    pos_end += yyleng;
    pass_node(yytext);
    return IDENTIFIER;
}

[0-9]+\.    {
    pos_start = pos_end;
    pos_end += yyleng;
    pass_node(yytext);
    return FLOATCONST;
}

[0-9]*\.[0-9]+  {
    pos_start = pos_end;
    pos_end += yyleng;
    pass_node(yytext);
    return FLOATCONST;
}

[0-9]+  {
    pos_start = pos_end;
    pos_end += yyleng;
    pass_node(yytext);
    return INTEGER;
}

[ \t]     {pos_end += 1;}
\n|\r\n   {pos_end = 1; lines += 1;}

"/*"([^*]|"*"*[^*/])*"*"+"/" { pos_start = pos_end; pos_end += yyleng; }

. { pos_start = pos_end; pos_end++; return ERROR; }

 /****请在此补全所有flex的模式与动作  end******/
%%
