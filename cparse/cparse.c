/* vi:set sw=4:

    cparse.c
  
    Copyright (c) 1999,2000 Minero Aoki <aamine@dp.u-netsurf.ne.jp>
  
    This library is free software.
    You can distribute/modify this program under the terms of
    the GNU Lesser General Public Lisence version 2 or later.

*/

#include <stdio.h>
#include "ruby.h"

#define RACC_VERSION "1.3.3"

#define DFLT_TOK -1
#define ERR_TOK   1
#define FINAL_TOK 0

#define vDFLT_TOK  INT2FIX(DFLT_TOK)
#define vERR_TOK   INT2FIX(ERR_TOK)
#define vFINAL_TOK INT2FIX(FINAL_TOK)

static VALUE RaccBug;

static ID id_yydebug;
static ID id_nexttoken;
static ID id_onerror;
static ID id_noreduce;
static ID id_catch;
static VALUE sym_raccjump;
static ID id_errstatus;

static ID id_d_shift;
static ID id_d_reduce;
static ID id_d_accept;
static ID id_d_read_token;
static ID id_d_next_state;
static ID id_d_e_pop;

#ifdef ID2SYM
# define id_to_value(i) ID2SYM(i)
#else
# define id_to_value(i) INT2FIX(i)
#endif

#ifdef SYM2ID
# define value_to_id(v) SYM2ID(v)
#else
# define value_to_id(v) (ID)FIX2LONG(v)
#endif

#ifdef DEBUG
# define D(code) if (v->in_debug) code
#else
# define D(code)
#endif


#define STACK_INIT_LEN 64
#define INIT_STACK(s) \
    s = rb_ary_new2(STACK_INIT_LEN)

#define AREF(s, idx) \
    ((idx >= RARRAY(s)->len) ? Qnil : RARRAY(s)->ptr[idx])

#define PUSH(s, i) \
    rb_ary_store(s, RARRAY(s)->len, i)

#define POP(s) \
    RARRAY(s)->len--

#define LAST_I(s) \
    RARRAY(s)->ptr[RARRAY(s)->len - 1]

#define GET_TAIL(s, leng) \
    rb_ary_new4(leng, RARRAY(s)->ptr + RARRAY(s)->len - leng)

#define CUT_TAIL(s, leng) \
   RARRAY(s)->len -= leng


struct cparse_params {
    VALUE parser;
    VALUE recv;
    ID    mid;

    VALUE action_table;
    VALUE action_check;
    VALUE action_default;
    VALUE action_pointer;
    VALUE goto_table;
    VALUE goto_check;
    VALUE goto_default;
    VALUE goto_pointer;
    long  nt_base;
    VALUE reduce_table;
    VALUE token_table;

    VALUE state;
    long curstate;
    VALUE vstack;
    VALUE tstack;
    VALUE t;
    long shift_n;
    long reduce_n;
    long ruleno;

    long errstatus;
    long nerr;

    int use_result_var;
    int iterator_p;

    VALUE retval;
    int fin;

    int debug;
    int in_debug;

    long i;
};


#define REDUCE(v, act) \
    v->ruleno = -act * 3;                            \
    tmp = rb_iterate(catch_iter, v->parser,          \
                     do_reduce, (VALUE)v);           \
    code = FIX2INT(tmp);                             \
    tmp = rb_ivar_get(v->parser, id_errstatus);      \
    v->errstatus = FIX2INT(tmp);                     \
    switch (code) {                                  \
    case 0: /* normal */                             \
        break;                                       \
    case 1: /* yyerror */                            \
        goto user_yyerror;                           \
    case 2: /* yyaccept */                           \
        goto accept;                                 \
    default:                                         \
        break;                                       \
    }

#define SHIFT(v, act, tok, val) \
    PUSH(v->vstack, val);                            \
    if (v->debug) {                                  \
        PUSH(v->tstack, tok);                        \
        rb_funcall(v->parser, id_d_shift,            \
                   3, tok, v->tstack, v->vstack);    \
    }                                                \
    v->curstate = act;                               \
    PUSH(v->state, INT2FIX(v->curstate));

#define ACCEPT(v) \
    if (v->debug) rb_funcall(v->parser, id_d_accept, 0); \
    v->retval = RARRAY(v->vstack)->ptr[0];               \
    v->fin = 1;                                          \
    return;


static void initvars _((VALUE, struct cparse_params*, VALUE, VALUE, ID));
static void wrap_yyparse _((struct cparse_params*));
static void parser_core _((struct cparse_params*, VALUE, VALUE, int));
static void check_nexttoken_value _((VALUE));
static VALUE catch_iter _((VALUE));
static VALUE do_reduce _((VALUE, VALUE, VALUE));


static VALUE
racc_cparse(parser, arg, indebug)
    VALUE parser, arg, indebug;
{
    struct cparse_params vv;
    struct cparse_params *v;

    v = &vv;
    v->in_debug = RTEST(indebug);
    D(puts("start C doparse"));
    initvars(parser, v, arg, Qnil, 0);
    v->iterator_p = 0;
    D(puts("params initialized"));
    parser_core(v, Qnil, Qnil, 0);

    return v->retval;
}

static VALUE
racc_yyparse(parser, recv, mid, arg, indebug)
    VALUE parser, recv, mid, arg, indebug;
{
    struct cparse_params vv;
    struct cparse_params *v;

    v = &vv;
    v->in_debug = RTEST(indebug);
    D(puts("start C yyparse"));
    initvars(parser, v, arg, recv, mid);
    v->iterator_p = 1;
    D(puts("params initialized"));
    parser_core(v, Qnil, Qnil, 0);
    wrap_yyparse(v);
    if (! v->fin) {
        rb_raise(rb_eArgError, "%s() is finished before '$end' found",
                 rb_id2name(v->mid));
    }

    return v->retval;
}

static VALUE
call_scaniter(data)
    VALUE data;
{
    struct cparse_params *v;

    v = (struct cparse_params*)data;
    rb_funcall(v->recv, v->mid, 0);

    return Qnil;
}

static VALUE
catch_scaniter(arr, data, self)
    VALUE arr, data, self;
{
    struct cparse_params *v;
    VALUE tok, val;

    v = (struct cparse_params*)data;
    check_nexttoken_value(arr);
    tok = AREF(arr, 0);
    val = AREF(arr, 1);
    parser_core(v, tok, val, 1);
    if (v->fin)
       /* rb_break */; 

    return Qnil;
}

static void
wrap_yyparse(v)
    struct cparse_params *v;
{
    rb_iterate(call_scaniter, (VALUE)v,
               catch_scaniter, (VALUE)v);
}

static void
initvars(parser, v, arg, recv, mid)
    VALUE parser, arg, recv, mid;
    struct cparse_params *v;
{
    VALUE act_tbl, act_chk, act_def, act_ptr,
          goto_tbl, goto_chk, goto_def, goto_ptr,
          ntbas, red_tbl, tok_tbl, shi_n, red_n;
    VALUE debugp;


    v->parser = parser;
    v->recv = recv;
    if (mid)
        v->mid = value_to_id(mid);

    debugp = rb_ivar_get(parser, id_yydebug);
    v->debug = RTEST(debugp);

    Check_Type(arg, T_ARRAY);
    if (!(RARRAY(arg)->len == 13 ||
          RARRAY(arg)->len == 14))
        rb_raise(RaccBug, "[Racc Bug] wrong arg.size %ld", RARRAY(arg)->len);
    act_tbl  = RARRAY(arg)->ptr[0];
    act_chk  = RARRAY(arg)->ptr[1];
    act_def  = RARRAY(arg)->ptr[2];
    act_ptr  = RARRAY(arg)->ptr[3];
    goto_tbl = RARRAY(arg)->ptr[4];
    goto_chk = RARRAY(arg)->ptr[5];
    goto_def = RARRAY(arg)->ptr[6];
    goto_ptr = RARRAY(arg)->ptr[7];
    ntbas    = RARRAY(arg)->ptr[8];
    red_tbl  = RARRAY(arg)->ptr[9];
    tok_tbl  = RARRAY(arg)->ptr[10];
    shi_n    = RARRAY(arg)->ptr[11];
    red_n    = RARRAY(arg)->ptr[12];
    if (RARRAY(arg)->len > 13) {
        VALUE useres;
        useres = RARRAY(arg)->ptr[13];
        v->use_result_var = RTEST(useres);
    }
    else {
        v->use_result_var = 1;
    }
    Check_Type(act_tbl,  T_ARRAY);
    Check_Type(act_chk,  T_ARRAY);
    Check_Type(act_def,  T_ARRAY);
    Check_Type(act_ptr,  T_ARRAY);
    Check_Type(goto_tbl, T_ARRAY);
    Check_Type(goto_chk, T_ARRAY);
    Check_Type(goto_def, T_ARRAY);
    Check_Type(goto_ptr, T_ARRAY);
    Check_Type(ntbas,    T_FIXNUM);
    Check_Type(red_tbl,  T_ARRAY);
    Check_Type(tok_tbl,  T_HASH);
    Check_Type(shi_n,    T_FIXNUM);
    Check_Type(red_n,    T_FIXNUM);
    v->action_table   = act_tbl;
    v->action_check   = act_chk;
    v->action_default = act_def;
    v->action_pointer = act_ptr;
    v->goto_table     = goto_tbl;
    v->goto_check     = goto_chk;
    v->goto_default   = goto_def;
    v->goto_pointer   = goto_ptr;
    v->nt_base        = FIX2LONG(ntbas);
    v->reduce_table   = red_tbl;
    v->token_table    = tok_tbl;
    v->shift_n        = FIX2LONG(shi_n);
    v->reduce_n       = FIX2LONG(red_n);

    if (v->debug) INIT_STACK(v->tstack);
    INIT_STACK(v->vstack);
    INIT_STACK(v->state);
    v->curstate = 0;
    PUSH(v->state, INT2FIX(0));
    v->t = INT2FIX(FINAL_TOK + 1); /* must not init to FINAL_TOK */
    v->nerr = 0;
    v->errstatus = 0;
    rb_ivar_set(parser, id_errstatus, INT2FIX(v->errstatus));

    v->retval = Qnil;
    v->fin = 0;

    v->iterator_p = 0;
}

static void
check_nexttoken_value(arr)
    VALUE arr;
{
    if (TYPE(arr) != T_ARRAY) {
        rb_raise(rb_eTypeError,
                 "next_token returned %s (must be Array[2])",
                 rb_class2name(CLASS_OF(arr)));
    }
    if (RARRAY(arr)->len != 2)
        rb_raise(rb_eArgError,
                 "an array from next_token is not size 2");
}

static void
parser_core(v, tok, val, resume)
    struct cparse_params *v;
    VALUE tok, val;
    int resume;
{
    long act;
    int read_next = 1;

    if (resume)
        goto resume;
    
    while (1) {
        long i;
        VALUE tmp;
        VALUE vact = 1;

        D(puts("enter new loop"));

        /* decide action */

        D(printf("(act) k1=%ld\n", v->curstate));
        tmp = AREF(v->action_pointer, v->curstate);
        if (! NIL_P(tmp)) {
            i = FIX2LONG(tmp);

            D(puts("(act) pointer[k1] true"));
            D(printf("read_next=%d\n", read_next));
            if (read_next) {
                if (v->t != vFINAL_TOK) {
    if (v->iterator_p) {
    /***** BUG? ******/
        D(puts("resuming..."));
        if (v->fin) {
            rb_raise(rb_eArgError,
                     "too many tokens given to racc parser");
        }
        v->i = i;
        return;
    }
                    tmp = rb_funcall(v->parser, id_nexttoken, 0);
                    check_nexttoken_value(tmp);
                    tok = AREF(tmp, 0);
                    val = AREF(tmp, 1);
    resume:
    if (v->iterator_p) {
        D(puts("resume"));
        i = v->i;
    }
                    tmp = rb_hash_aref(v->token_table, tok);
                    v->t = NIL_P(tmp) ? vERR_TOK : tmp;
                    D(printf("(act) t(k2)=%ld\n", FIX2LONG(v->t)));
                    if (v->debug) {
                        rb_funcall(v->parser, id_d_read_token,
                                   3, v->t, tok, val);
                    }
                }
                read_next = 0;
            }

            i += FIX2LONG(v->t);
            D(printf("(act) i=%ld\n", i));
            if (i >= 0) {
                vact = AREF(v->action_table, i);
                D(printf("(act) table[i]=%ld\n", FIX2LONG(vact)));
                if (! NIL_P(vact)) {
                    tmp = AREF(v->action_check, i);
                    D(printf("(act) check[i]=%ld\n", FIX2LONG(tmp)));
                    if (! NIL_P(tmp) && FIX2LONG(tmp) == v->curstate) {
                        D(puts("(act) found"));
                        goto act_found;
                    }
                }
            }
        }
        D(puts("(act) not found: use default"));
        vact = AREF(v->action_default, v->curstate);

    act_found:
        act = FIX2LONG(vact);
        D(printf("act=%ld\n", act));


        if (act > 0 && act < v->shift_n) {
            D(puts("shift"));

            if (v->errstatus > 0) {
                v->errstatus--;
                rb_ivar_set(v->parser, id_errstatus, INT2FIX(v->errstatus));
            }
            SHIFT(v, act, v->t, val);
            read_next = 1;
        }
        else if (act < 0 && act > -(v->reduce_n)) {
            int code;
            D(puts("reduce"));

            REDUCE(v, act);
        }
        else if (act == -(v->reduce_n)) {
            D(printf("error detected, status=%ld\n", v->errstatus));

            if (v->errstatus == 0) {
                v->nerr++;
                rb_funcall(v->parser, id_onerror,
                           3, v->t, val, v->vstack);
            }

    user_yyerror:

            if (v->errstatus == 3) {
                if (v->t == vFINAL_TOK) {
                    v->retval = Qfalse;
                    v->fin = 1;
                    return;
                }
                read_next = 1;
            }
            v->errstatus = 3;
            rb_ivar_set(v->parser, id_errstatus, INT2FIX(v->errstatus));

            /* check if We can shift/reduce error token */
            D(printf("(err) k1=%ld\n", v->curstate));
            D(printf("(err) k2=%d (error)\n", ERR_TOK));
            while (1) {
                tmp = AREF(v->action_pointer, v->curstate);
                if (! NIL_P(tmp)) {
                    D(puts("(err) pointer[k1] true"));
                    i = FIX2LONG(tmp) + ERR_TOK;
                    D(printf("(err) i=%ld\n", i));
                    if (i >= 0) {
                        vact = AREF(v->action_table, i);
                        if (! NIL_P(vact)) {
                            D(printf("(err) table[i]=%ld\n", FIX2LONG(vact)));
                            tmp = AREF(v->action_check, i);
                            if (! NIL_P(tmp) && FIX2LONG(tmp) == v->curstate) {
                                D(puts("(err) found: can handle error tok"));
                                break;
                            }
                            else {
                                D(puts("(err) check[i]!=k1 or nil"));
                            }
                        }
                        else {
                            D(puts("(err) table[i] == nil"));
                        }
                    }
                }
                D(puts("(err) not found: can't handle error tok: pop"));

                if (RARRAY(v->state)->len == 0) {
                    v->retval = Qnil;
                    v->fin = 1;
                    return;
                }
                POP(v->state);
                POP(v->vstack);
                tmp = LAST_I(v->state);
                v->curstate = FIX2LONG(tmp);
                if (v->debug) {
                    POP(v->tstack);
                    rb_funcall(v->parser, id_d_e_pop,
                               3, v->state, v->tstack, v->vstack);
                }
            }
            act = FIX2LONG(vact);

            /* shift|reduce error token */

            if (act > 0 && act < v->shift_n) {
                D(puts("e shift"));
                SHIFT(v, act, ERR_TOK, val);
            }
            else if (act < 0 && act > -(v->reduce_n)) {
                int code;

                D(puts("e reduce"));
                REDUCE(v, act);
            }
            else if (act == v->shift_n) {
                D(puts("e accept"));
                ACCEPT(v);
            }
            else {
                rb_raise(RaccBug, "[Racc Bug] unknown act value %ld", act);
            }
        }
        else if (act == v->shift_n) {
    accept:
            D(puts("accept"));
            ACCEPT(v);
        }
        else {
            rb_raise(RaccBug, "[Racc Bug] unknown act value %ld", act);
        }

        if (v->debug) {
            rb_funcall(v->parser, id_d_next_state,
                       2, INT2FIX(v->curstate), v->state);
        }
    }
}


static VALUE
catch_iter(parser)
    VALUE parser;
{
    return rb_funcall(parser, id_catch, 1, sym_raccjump);
}


static VALUE
do_reduce(val, data, self)
    VALUE val, data, self;
{
    struct cparse_params *v;
    VALUE reduce_to, reduce_len, method_id;
    long len;
    ID mid;
    VALUE tmp, tmp_t, tmp_v;
    long i, k1, k2;
    VALUE ret;

    v = (struct cparse_params*)data;
    reduce_len = RARRAY(v->reduce_table)->ptr[v->ruleno];
    reduce_to  = RARRAY(v->reduce_table)->ptr[v->ruleno+1];
    method_id  = RARRAY(v->reduce_table)->ptr[v->ruleno+2];
    len = FIX2LONG(reduce_len);
    mid = value_to_id(method_id);

    if (len == 0) {
        tmp = Qnil;
        if (mid != id_noreduce)
            tmp_v = rb_ary_new();
        if (v->debug)
            tmp_t = rb_ary_new();
    }
    else {
        if (mid != id_noreduce) {
            tmp_v = GET_TAIL(v->vstack, len);
            tmp = RARRAY(tmp_v)->ptr[0];
        }
        else {
            tmp = RARRAY(v->vstack)->ptr[ RARRAY(v->vstack)->len - len ];
        }
        CUT_TAIL(v->vstack, len);
        if (v->debug) {
            tmp_t = GET_TAIL(v->tstack, len);
            CUT_TAIL(v->tstack, len);
        }
        CUT_TAIL(v->state, len);
    }

    /* method call must be done before tstack.push */
    if (mid != id_noreduce) {
        if (v->use_result_var) {
            tmp = rb_funcall(v->parser, mid,
                             3, tmp_v, v->vstack, tmp);
        }
        else {
            tmp = rb_funcall(v->parser, mid,
                             2, tmp_v, v->vstack);
        }
    }
    PUSH(v->vstack, tmp);
    if (v->debug) {
        PUSH(v->tstack, reduce_to);
        rb_funcall(v->parser, id_d_reduce,
                   4, tmp_t, reduce_to, v->tstack, v->vstack);
    }

    if (RARRAY(v->state)->len == 0) {
        rb_raise(RaccBug, "state stack unexpected empty");
    }
    tmp = LAST_I(v->state);
    k2 = FIX2LONG(tmp);
    k1 = FIX2LONG(reduce_to) - v->nt_base;
    D(printf("(goto) k1=%ld\n", k1));
    D(printf("(goto) k2=%ld\n", k2));

    tmp = AREF(v->goto_pointer, k1);
    if (! NIL_P(tmp)) {
        i = FIX2LONG(tmp) + k2;
        D(printf("(goto) i=%ld\n", i));
        if (i >= 0) {
            ret = AREF(v->goto_table, i);
            if (! NIL_P(ret)) {
                D(printf("(goto) table[i]=%ld (ret)\n", FIX2LONG(ret)));
                tmp = AREF(v->goto_check, i);
                if (!NIL_P(tmp) && tmp == INT2FIX(k1)) {
                    D(printf("(goto) check[i]=%ld\n", FIX2LONG(tmp)));
                    D(puts("(goto) found"));
                    goto doret;
                }
                else {
                    D(puts("(goto) check[i]!=table[i] or nil"));
                }
            }
            else {
                D(puts("(goto) table[i] == nil"));
            }
        }
    }
    D(puts("(goto) not found: use default"));
    ret = AREF(v->goto_default, k1);

doret:
    PUSH(v->state, ret);
    v->curstate = FIX2LONG(ret);

    return INT2FIX(0);
}


void
Init_cparse()
{
    VALUE Parser;

    Parser = rb_eval_string("::Racc::Parser");
    rb_define_private_method(Parser, "_racc_do_parse_c", racc_cparse, 2);
    rb_define_private_method(Parser, "_racc_yyparse_c", racc_yyparse, 4);
    rb_define_const(Parser, "Racc_c_parser_version", rb_str_new2(RACC_VERSION));

    RaccBug = rb_eRuntimeError;

    id_yydebug      = rb_intern("@yydebug");
    id_nexttoken    = rb_intern("next_token");
    id_onerror      = rb_intern("on_error");
    id_noreduce     = rb_intern("_reduce_none");
    id_catch        = rb_intern("catch");
    id_errstatus    = rb_intern("@racc_error_status");
    sym_raccjump    = id_to_value(rb_intern("racc_jump"));

    id_d_shift       = rb_intern("racc_shift");
    id_d_reduce      = rb_intern("racc_reduce");
    id_d_accept      = rb_intern("racc_accept");
    id_d_read_token  = rb_intern("racc_read_token");
    id_d_next_state  = rb_intern("racc_next_state");
    id_d_e_pop       = rb_intern("racc_e_pop");
}
