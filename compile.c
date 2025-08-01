/**********************************************************************

  compile.c - ruby node tree -> VM instruction sequence

  $Author$
  created at: 04/01/01 03:42:15 JST

  Copyright (C) 2004-2007 Koichi Sasada

**********************************************************************/

#include "ruby/internal/config.h"
#include <math.h>

#ifdef HAVE_DLADDR
# include <dlfcn.h>
#endif

#include "encindex.h"
#include "id_table.h"
#include "internal.h"
#include "internal/array.h"
#include "internal/compile.h"
#include "internal/complex.h"
#include "internal/encoding.h"
#include "internal/error.h"
#include "internal/gc.h"
#include "internal/hash.h"
#include "internal/io.h"
#include "internal/numeric.h"
#include "internal/object.h"
#include "internal/rational.h"
#include "internal/re.h"
#include "internal/ruby_parser.h"
#include "internal/symbol.h"
#include "internal/thread.h"
#include "internal/variable.h"
#include "iseq.h"
#include "ruby/ractor.h"
#include "ruby/re.h"
#include "ruby/util.h"
#include "vm_core.h"
#include "vm_callinfo.h"
#include "vm_debug.h"
#include "yjit.h"

#include "builtin.h"
#include "insns.inc"
#include "insns_info.inc"

#define FIXNUM_INC(n, i) ((n)+(INT2FIX(i)&~FIXNUM_FLAG))

typedef struct iseq_link_element {
    enum {
        ISEQ_ELEMENT_ANCHOR,
        ISEQ_ELEMENT_LABEL,
        ISEQ_ELEMENT_INSN,
        ISEQ_ELEMENT_ADJUST,
        ISEQ_ELEMENT_TRACE,
    } type;
    struct iseq_link_element *next;
    struct iseq_link_element *prev;
} LINK_ELEMENT;

typedef struct iseq_link_anchor {
    LINK_ELEMENT anchor;
    LINK_ELEMENT *last;
} LINK_ANCHOR;

typedef enum {
    LABEL_RESCUE_NONE,
    LABEL_RESCUE_BEG,
    LABEL_RESCUE_END,
    LABEL_RESCUE_TYPE_MAX
} LABEL_RESCUE_TYPE;

typedef struct iseq_label_data {
    LINK_ELEMENT link;
    int label_no;
    int position;
    int sc_state;
    int sp;
    int refcnt;
    unsigned int set: 1;
    unsigned int rescued: 2;
    unsigned int unremovable: 1;
} LABEL;

typedef struct iseq_insn_data {
    LINK_ELEMENT link;
    enum ruby_vminsn_type insn_id;
    int operand_size;
    int sc_state;
    VALUE *operands;
    struct {
        int line_no;
        int node_id;
        rb_event_flag_t events;
    } insn_info;
} INSN;

typedef struct iseq_adjust_data {
    LINK_ELEMENT link;
    LABEL *label;
    int line_no;
} ADJUST;

typedef struct iseq_trace_data {
    LINK_ELEMENT link;
    rb_event_flag_t event;
    long data;
} TRACE;

struct ensure_range {
    LABEL *begin;
    LABEL *end;
    struct ensure_range *next;
};

struct iseq_compile_data_ensure_node_stack {
    const void *ensure_node;
    struct iseq_compile_data_ensure_node_stack *prev;
    struct ensure_range *erange;
};

const ID rb_iseq_shared_exc_local_tbl[] = {idERROR_INFO};

/**
 * debug function(macro) interface depend on CPDEBUG
 * if it is less than 0, runtime option is in effect.
 *
 * debug level:
 *  0: no debug output
 *  1: show node type
 *  2: show node important parameters
 *  ...
 *  5: show other parameters
 * 10: show every AST array
 */

#ifndef CPDEBUG
#define CPDEBUG 0
#endif

#if CPDEBUG >= 0
#define compile_debug CPDEBUG
#else
#define compile_debug ISEQ_COMPILE_DATA(iseq)->option->debug_level
#endif

#if CPDEBUG

#define compile_debug_print_indent(level) \
    ruby_debug_print_indent((level), compile_debug, gl_node_level * 2)

#define debugp(header, value) (void) \
  (compile_debug_print_indent(1) && \
   ruby_debug_print_value(1, compile_debug, (header), (value)))

#define debugi(header, id)  (void) \
  (compile_debug_print_indent(1) && \
   ruby_debug_print_id(1, compile_debug, (header), (id)))

#define debugp_param(header, value)  (void) \
  (compile_debug_print_indent(1) && \
   ruby_debug_print_value(1, compile_debug, (header), (value)))

#define debugp_verbose(header, value)  (void) \
  (compile_debug_print_indent(2) && \
   ruby_debug_print_value(2, compile_debug, (header), (value)))

#define debugp_verbose_node(header, value)  (void) \
  (compile_debug_print_indent(10) && \
   ruby_debug_print_value(10, compile_debug, (header), (value)))

#define debug_node_start(node)  ((void) \
  (compile_debug_print_indent(1) && \
   (ruby_debug_print_node(1, CPDEBUG, "", (const NODE *)(node)), gl_node_level)), \
   gl_node_level++)

#define debug_node_end()  gl_node_level --

#else

#define debugi(header, id)                 ((void)0)
#define debugp(header, value)              ((void)0)
#define debugp_verbose(header, value)      ((void)0)
#define debugp_verbose_node(header, value) ((void)0)
#define debugp_param(header, value)        ((void)0)
#define debug_node_start(node)             ((void)0)
#define debug_node_end()                   ((void)0)
#endif

#if CPDEBUG > 1 || CPDEBUG < 0
#undef printf
#define printf ruby_debug_printf
#define debugs if (compile_debug_print_indent(1)) ruby_debug_printf
#define debug_compile(msg, v) ((void)(compile_debug_print_indent(1) && fputs((msg), stderr)), (v))
#else
#define debugs                             if(0)printf
#define debug_compile(msg, v) (v)
#endif

#define LVAR_ERRINFO (1)

/* create new label */
#define NEW_LABEL(l) new_label_body(iseq, (l))
#define LABEL_FORMAT "<L%03d>"

#define NEW_ISEQ(node, name, type, line_no) \
  new_child_iseq(iseq, (node), rb_fstring(name), 0, (type), (line_no))

#define NEW_CHILD_ISEQ(node, name, type, line_no) \
  new_child_iseq(iseq, (node), rb_fstring(name), iseq, (type), (line_no))

#define NEW_CHILD_ISEQ_WITH_CALLBACK(callback_func, name, type, line_no) \
  new_child_iseq_with_callback(iseq, (callback_func), (name), iseq, (type), (line_no))

/* add instructions */
#define ADD_SEQ(seq1, seq2) \
  APPEND_LIST((seq1), (seq2))

/* add an instruction */
#define ADD_INSN(seq, line_node, insn) \
  ADD_ELEM((seq), (LINK_ELEMENT *) new_insn_body(iseq, nd_line(line_node), nd_node_id(line_node), BIN(insn), 0))

/* add an instruction with the given line number and node id */
#define ADD_SYNTHETIC_INSN(seq, line_no, node_id, insn) \
  ADD_ELEM((seq), (LINK_ELEMENT *) new_insn_body(iseq, (line_no), (node_id), BIN(insn), 0))

/* insert an instruction before next */
#define INSERT_BEFORE_INSN(next, line_no, node_id, insn) \
  ELEM_INSERT_PREV(&(next)->link, (LINK_ELEMENT *) new_insn_body(iseq, line_no, node_id, BIN(insn), 0))

/* insert an instruction after prev */
#define INSERT_AFTER_INSN(prev, line_no, node_id, insn) \
  ELEM_INSERT_NEXT(&(prev)->link, (LINK_ELEMENT *) new_insn_body(iseq, line_no, node_id, BIN(insn), 0))

/* add an instruction with some operands (1, 2, 3, 5) */
#define ADD_INSN1(seq, line_node, insn, op1) \
  ADD_ELEM((seq), (LINK_ELEMENT *) \
           new_insn_body(iseq, nd_line(line_node), nd_node_id(line_node), BIN(insn), 1, (VALUE)(op1)))

/* insert an instruction with some operands (1, 2, 3, 5) before next */
#define INSERT_BEFORE_INSN1(next, line_no, node_id, insn, op1) \
  ELEM_INSERT_PREV(&(next)->link, (LINK_ELEMENT *) \
           new_insn_body(iseq, line_no, node_id, BIN(insn), 1, (VALUE)(op1)))

/* insert an instruction with some operands (1, 2, 3, 5) after prev */
#define INSERT_AFTER_INSN1(prev, line_no, node_id, insn, op1) \
  ELEM_INSERT_NEXT(&(prev)->link, (LINK_ELEMENT *) \
           new_insn_body(iseq, line_no, node_id, BIN(insn), 1, (VALUE)(op1)))

#define LABEL_REF(label) ((label)->refcnt++)

/* add an instruction with label operand (alias of ADD_INSN1) */
#define ADD_INSNL(seq, line_node, insn, label) (ADD_INSN1(seq, line_node, insn, label), LABEL_REF(label))

#define ADD_INSN2(seq, line_node, insn, op1, op2) \
  ADD_ELEM((seq), (LINK_ELEMENT *) \
           new_insn_body(iseq, nd_line(line_node), nd_node_id(line_node), BIN(insn), 2, (VALUE)(op1), (VALUE)(op2)))

#define ADD_INSN3(seq, line_node, insn, op1, op2, op3) \
  ADD_ELEM((seq), (LINK_ELEMENT *) \
           new_insn_body(iseq, nd_line(line_node), nd_node_id(line_node), BIN(insn), 3, (VALUE)(op1), (VALUE)(op2), (VALUE)(op3)))

/* Specific Insn factory */
#define ADD_SEND(seq, line_node, id, argc) \
  ADD_SEND_R((seq), (line_node), (id), (argc), NULL, (VALUE)INT2FIX(0), NULL)

#define ADD_SEND_WITH_FLAG(seq, line_node, id, argc, flag) \
  ADD_SEND_R((seq), (line_node), (id), (argc), NULL, (VALUE)(flag), NULL)

#define ADD_SEND_WITH_BLOCK(seq, line_node, id, argc, block) \
  ADD_SEND_R((seq), (line_node), (id), (argc), (block), (VALUE)INT2FIX(0), NULL)

#define ADD_CALL_RECEIVER(seq, line_node) \
  ADD_INSN((seq), (line_node), putself)

#define ADD_CALL(seq, line_node, id, argc) \
  ADD_SEND_R((seq), (line_node), (id), (argc), NULL, (VALUE)INT2FIX(VM_CALL_FCALL), NULL)

#define ADD_CALL_WITH_BLOCK(seq, line_node, id, argc, block) \
  ADD_SEND_R((seq), (line_node), (id), (argc), (block), (VALUE)INT2FIX(VM_CALL_FCALL), NULL)

#define ADD_SEND_R(seq, line_node, id, argc, block, flag, keywords) \
  ADD_ELEM((seq), (LINK_ELEMENT *) new_insn_send(iseq, nd_line(line_node), nd_node_id(line_node), (id), (VALUE)(argc), (block), (VALUE)(flag), (keywords)))

#define ADD_TRACE(seq, event) \
  ADD_ELEM((seq), (LINK_ELEMENT *)new_trace_body(iseq, (event), 0))
#define ADD_TRACE_WITH_DATA(seq, event, data) \
  ADD_ELEM((seq), (LINK_ELEMENT *)new_trace_body(iseq, (event), (data)))

static void iseq_add_getlocal(rb_iseq_t *iseq, LINK_ANCHOR *const seq, const NODE *const line_node, int idx, int level);
static void iseq_add_setlocal(rb_iseq_t *iseq, LINK_ANCHOR *const seq, const NODE *const line_node, int idx, int level);

#define ADD_GETLOCAL(seq, line_node, idx, level) iseq_add_getlocal(iseq, (seq), (line_node), (idx), (level))
#define ADD_SETLOCAL(seq, line_node, idx, level) iseq_add_setlocal(iseq, (seq), (line_node), (idx), (level))

/* add label */
#define ADD_LABEL(seq, label) \
  ADD_ELEM((seq), (LINK_ELEMENT *) (label))

#define APPEND_LABEL(seq, before, label) \
  APPEND_ELEM((seq), (before), (LINK_ELEMENT *) (label))

#define ADD_ADJUST(seq, line_node, label) \
  ADD_ELEM((seq), (LINK_ELEMENT *) new_adjust_body(iseq, (label), nd_line(line_node)))

#define ADD_ADJUST_RESTORE(seq, label) \
  ADD_ELEM((seq), (LINK_ELEMENT *) new_adjust_body(iseq, (label), -1))

#define LABEL_UNREMOVABLE(label) \
    ((label) ? (LABEL_REF(label), (label)->unremovable=1) : 0)
#define ADD_CATCH_ENTRY(type, ls, le, iseqv, lc) do {				\
    VALUE _e = rb_ary_new3(5, (type),						\
                           (VALUE)(ls) | 1, (VALUE)(le) | 1,			\
                           (VALUE)(iseqv), (VALUE)(lc) | 1);			\
    LABEL_UNREMOVABLE(ls);							\
    LABEL_REF(le);								\
    LABEL_REF(lc);								\
    if (NIL_P(ISEQ_COMPILE_DATA(iseq)->catch_table_ary)) \
        RB_OBJ_WRITE(iseq, &ISEQ_COMPILE_DATA(iseq)->catch_table_ary, rb_ary_hidden_new(3)); \
    rb_ary_push(ISEQ_COMPILE_DATA(iseq)->catch_table_ary, freeze_hide_obj(_e));	\
} while (0)

/* compile node */
#define COMPILE(anchor, desc, node) \
  (debug_compile("== " desc "\n", \
                 iseq_compile_each(iseq, (anchor), (node), 0)))

/* compile node, this node's value will be popped */
#define COMPILE_POPPED(anchor, desc, node)    \
  (debug_compile("== " desc "\n", \
                 iseq_compile_each(iseq, (anchor), (node), 1)))

/* compile node, which is popped when 'popped' is true */
#define COMPILE_(anchor, desc, node, popped)  \
  (debug_compile("== " desc "\n", \
                 iseq_compile_each(iseq, (anchor), (node), (popped))))

#define COMPILE_RECV(anchor, desc, node, recv) \
    (private_recv_p(node) ? \
     (ADD_INSN(anchor, node, putself), VM_CALL_FCALL) : \
     COMPILE(anchor, desc, recv) ? 0 : -1)

#define OPERAND_AT(insn, idx) \
  (((INSN*)(insn))->operands[(idx)])

#define INSN_OF(insn) \
  (((INSN*)(insn))->insn_id)

#define IS_INSN(link) ((link)->type == ISEQ_ELEMENT_INSN)
#define IS_LABEL(link) ((link)->type == ISEQ_ELEMENT_LABEL)
#define IS_ADJUST(link) ((link)->type == ISEQ_ELEMENT_ADJUST)
#define IS_TRACE(link) ((link)->type == ISEQ_ELEMENT_TRACE)
#define IS_INSN_ID(iobj, insn) (INSN_OF(iobj) == BIN(insn))
#define IS_NEXT_INSN_ID(link, insn) \
    ((link)->next && IS_INSN((link)->next) && IS_INSN_ID((link)->next, insn))

/* error */
#if CPDEBUG > 0
RBIMPL_ATTR_NORETURN()
#endif
RBIMPL_ATTR_FORMAT(RBIMPL_PRINTF_FORMAT, 3, 4)
static void
append_compile_error(const rb_iseq_t *iseq, int line, const char *fmt, ...)
{
    VALUE err_info = ISEQ_COMPILE_DATA(iseq)->err_info;
    VALUE file = rb_iseq_path(iseq);
    VALUE err = err_info == Qtrue ? Qfalse : err_info;
    va_list args;

    va_start(args, fmt);
    err = rb_syntax_error_append(err, file, line, -1, NULL, fmt, args);
    va_end(args);
    if (NIL_P(err_info)) {
        RB_OBJ_WRITE(iseq, &ISEQ_COMPILE_DATA(iseq)->err_info, err);
        rb_set_errinfo(err);
    }
    else if (!err_info) {
        RB_OBJ_WRITE(iseq, &ISEQ_COMPILE_DATA(iseq)->err_info, Qtrue);
    }
    if (compile_debug) {
        if (SPECIAL_CONST_P(err)) err = rb_eSyntaxError;
        rb_exc_fatal(err);
    }
}

#if 0
static void
compile_bug(rb_iseq_t *iseq, int line, const char *fmt, ...)
{
    va_list args;
    va_start(args, fmt);
    rb_report_bug_valist(rb_iseq_path(iseq), line, fmt, args);
    va_end(args);
    abort();
}
#endif

#define COMPILE_ERROR append_compile_error

#define ERROR_ARGS_AT(n) iseq, nd_line(n),
#define ERROR_ARGS ERROR_ARGS_AT(node)

#define EXPECT_NODE(prefix, node, ndtype, errval) \
do { \
    const NODE *error_node = (node); \
    enum node_type error_type = nd_type(error_node); \
    if (error_type != (ndtype)) { \
        COMPILE_ERROR(ERROR_ARGS_AT(error_node) \
                      prefix ": " #ndtype " is expected, but %s", \
                      ruby_node_name(error_type)); \
        return errval; \
    } \
} while (0)

#define EXPECT_NODE_NONULL(prefix, parent, ndtype, errval) \
do { \
    COMPILE_ERROR(ERROR_ARGS_AT(parent) \
                  prefix ": must be " #ndtype ", but 0"); \
    return errval; \
} while (0)

#define UNKNOWN_NODE(prefix, node, errval) \
do { \
    const NODE *error_node = (node); \
    COMPILE_ERROR(ERROR_ARGS_AT(error_node) prefix ": unknown node (%s)", \
                  ruby_node_name(nd_type(error_node))); \
    return errval; \
} while (0)

#define COMPILE_OK 1
#define COMPILE_NG 0

#define CHECK(sub) if (!(sub)) {BEFORE_RETURN;return COMPILE_NG;}
#define NO_CHECK(sub) (void)(sub)
#define BEFORE_RETURN

#define DECL_ANCHOR(name) \
    LINK_ANCHOR name[1] = {{{ISEQ_ELEMENT_ANCHOR,},&name[0].anchor}}
#define INIT_ANCHOR(name) \
    ((name->last = &name->anchor)->next = NULL) /* re-initialize */

static inline VALUE
freeze_hide_obj(VALUE obj)
{
    OBJ_FREEZE(obj);
    RBASIC_CLEAR_CLASS(obj);
    return obj;
}

#include "optinsn.inc"
#if OPT_INSTRUCTIONS_UNIFICATION
#include "optunifs.inc"
#endif

/* for debug */
#if CPDEBUG < 0
#define ISEQ_ARG iseq,
#define ISEQ_ARG_DECLARE rb_iseq_t *iseq,
#else
#define ISEQ_ARG
#define ISEQ_ARG_DECLARE
#endif

#if CPDEBUG
#define gl_node_level ISEQ_COMPILE_DATA(iseq)->node_level
#endif

static void dump_disasm_list_with_cursor(const LINK_ELEMENT *link, const LINK_ELEMENT *curr, const LABEL *dest);
static void dump_disasm_list(const LINK_ELEMENT *elem);

static int insn_data_length(INSN *iobj);
static int calc_sp_depth(int depth, INSN *iobj);

static INSN *new_insn_body(rb_iseq_t *iseq, int line_no, int node_id, enum ruby_vminsn_type insn_id, int argc, ...);
static LABEL *new_label_body(rb_iseq_t *iseq, long line);
static ADJUST *new_adjust_body(rb_iseq_t *iseq, LABEL *label, int line);
static TRACE *new_trace_body(rb_iseq_t *iseq, rb_event_flag_t event, long data);


static int iseq_compile_each(rb_iseq_t *iseq, LINK_ANCHOR *anchor, const NODE *n, int);
static int iseq_setup(rb_iseq_t *iseq, LINK_ANCHOR *const anchor);
static int iseq_setup_insn(rb_iseq_t *iseq, LINK_ANCHOR *const anchor);
static int iseq_optimize(rb_iseq_t *iseq, LINK_ANCHOR *const anchor);
static int iseq_insns_unification(rb_iseq_t *iseq, LINK_ANCHOR *const anchor);

static int iseq_set_local_table(rb_iseq_t *iseq, const rb_ast_id_table_t *tbl, const NODE *const node_args);
static int iseq_set_exception_local_table(rb_iseq_t *iseq);
static int iseq_set_arguments(rb_iseq_t *iseq, LINK_ANCHOR *const anchor, const NODE *const node);

static int iseq_set_sequence(rb_iseq_t *iseq, LINK_ANCHOR *const anchor);
static int iseq_set_exception_table(rb_iseq_t *iseq);
static int iseq_set_optargs_table(rb_iseq_t *iseq);

static int compile_defined_expr(rb_iseq_t *iseq, LINK_ANCHOR *const ret, const NODE *const node, VALUE needstr, bool ignore);
static int compile_hash(rb_iseq_t *iseq, LINK_ANCHOR *const ret, const NODE *node, int method_call_keywords, int popped);

/*
 * To make Array to LinkedList, use link_anchor
 */

static void
verify_list(ISEQ_ARG_DECLARE const char *info, LINK_ANCHOR *const anchor)
{
#if CPDEBUG
    int flag = 0;
    LINK_ELEMENT *list, *plist;

    if (!compile_debug) return;

    list = anchor->anchor.next;
    plist = &anchor->anchor;
    while (list) {
        if (plist != list->prev) {
            flag += 1;
        }
        plist = list;
        list = list->next;
    }

    if (anchor->last != plist && anchor->last != 0) {
        flag |= 0x70000;
    }

    if (flag != 0) {
        rb_bug("list verify error: %08x (%s)", flag, info);
    }
#endif
}
#if CPDEBUG < 0
#define verify_list(info, anchor) verify_list(iseq, (info), (anchor))
#endif

static void
verify_call_cache(rb_iseq_t *iseq)
{
#if CPDEBUG
    VALUE *original = rb_iseq_original_iseq(iseq);
    size_t i = 0;
    while (i < ISEQ_BODY(iseq)->iseq_size) {
        VALUE insn = original[i];
        const char *types = insn_op_types(insn);

        for (int j=0; types[j]; j++) {
            if (types[j] == TS_CALLDATA) {
                struct rb_call_data *cd = (struct rb_call_data *)original[i+j+1];
                const struct rb_callinfo *ci = cd->ci;
                const struct rb_callcache *cc = cd->cc;
                if (cc != vm_cc_empty()) {
                    vm_ci_dump(ci);
                    rb_bug("call cache is not initialized by vm_cc_empty()");
                }
            }
        }
        i += insn_len(insn);
    }

    for (unsigned int i=0; i<ISEQ_BODY(iseq)->ci_size; i++) {
        struct rb_call_data *cd = &ISEQ_BODY(iseq)->call_data[i];
        const struct rb_callinfo *ci = cd->ci;
        const struct rb_callcache *cc = cd->cc;
        if (cc != NULL && cc != vm_cc_empty()) {
            vm_ci_dump(ci);
            rb_bug("call cache is not initialized by vm_cc_empty()");
        }
    }
#endif
}

/*
 * elem1, elem2 => elem1, elem2, elem
 */
static void
ADD_ELEM(ISEQ_ARG_DECLARE LINK_ANCHOR *const anchor, LINK_ELEMENT *elem)
{
    elem->prev = anchor->last;
    anchor->last->next = elem;
    anchor->last = elem;
    verify_list("add", anchor);
}

/*
 * elem1, before, elem2 => elem1, before, elem, elem2
 */
static void
APPEND_ELEM(ISEQ_ARG_DECLARE LINK_ANCHOR *const anchor, LINK_ELEMENT *before, LINK_ELEMENT *elem)
{
    elem->prev = before;
    elem->next = before->next;
    elem->next->prev = elem;
    before->next = elem;
    if (before == anchor->last) anchor->last = elem;
    verify_list("add", anchor);
}
#if CPDEBUG < 0
#define ADD_ELEM(anchor, elem) ADD_ELEM(iseq, (anchor), (elem))
#define APPEND_ELEM(anchor, before, elem) APPEND_ELEM(iseq, (anchor), (before), (elem))
#endif

static int
branch_coverage_valid_p(rb_iseq_t *iseq, int first_line)
{
    if (!ISEQ_COVERAGE(iseq)) return 0;
    if (!ISEQ_BRANCH_COVERAGE(iseq)) return 0;
    if (first_line <= 0) return 0;
    return 1;
}

#define PTR2NUM(x) (rb_int2inum((intptr_t)(void *)(x)))

static VALUE
setup_branch(const rb_code_location_t *loc, const char *type, VALUE structure, VALUE key)
{
    const int first_lineno = loc->beg_pos.lineno, first_column = loc->beg_pos.column;
    const int last_lineno = loc->end_pos.lineno, last_column = loc->end_pos.column;
    VALUE branch = rb_ary_hidden_new(6);

    rb_hash_aset(structure, key, branch);
    rb_ary_push(branch, ID2SYM(rb_intern(type)));
    rb_ary_push(branch, INT2FIX(first_lineno));
    rb_ary_push(branch, INT2FIX(first_column));
    rb_ary_push(branch, INT2FIX(last_lineno));
    rb_ary_push(branch, INT2FIX(last_column));
    return branch;
}

static VALUE
decl_branch_base(rb_iseq_t *iseq, VALUE key, const rb_code_location_t *loc, const char *type)
{
    if (!branch_coverage_valid_p(iseq, loc->beg_pos.lineno)) return Qundef;

    /*
     * if !structure[node]
     *   structure[node] = [type, first_lineno, first_column, last_lineno, last_column, branches = {}]
     * else
     *   branches = structure[node][5]
     * end
     */

    VALUE structure = RARRAY_AREF(ISEQ_BRANCH_COVERAGE(iseq), 0);
    VALUE branch_base = rb_hash_aref(structure, key);
    VALUE branches;

    if (NIL_P(branch_base)) {
        branch_base = setup_branch(loc, type, structure, key);
        branches = rb_hash_new();
        rb_obj_hide(branches);
        rb_ary_push(branch_base, branches);
    }
    else {
        branches = RARRAY_AREF(branch_base, 5);
    }

    return branches;
}

static NODE
generate_dummy_line_node(int lineno, int node_id)
{
    NODE dummy = { 0 };
    nd_set_line(&dummy, lineno);
    nd_set_node_id(&dummy, node_id);
    return dummy;
}

static void
add_trace_branch_coverage(rb_iseq_t *iseq, LINK_ANCHOR *const seq, const rb_code_location_t *loc, int node_id, int branch_id, const char *type, VALUE branches)
{
    if (!branch_coverage_valid_p(iseq, loc->beg_pos.lineno)) return;

    /*
     * if !branches[branch_id]
     *   branches[branch_id] = [type, first_lineno, first_column, last_lineno, last_column, counter_idx]
     * else
     *   counter_idx= branches[branch_id][5]
     * end
     */

    VALUE key = INT2FIX(branch_id);
    VALUE branch = rb_hash_aref(branches, key);
    long counter_idx;

    if (NIL_P(branch)) {
        branch = setup_branch(loc, type, branches, key);
        VALUE counters = RARRAY_AREF(ISEQ_BRANCH_COVERAGE(iseq), 1);
        counter_idx = RARRAY_LEN(counters);
        rb_ary_push(branch, LONG2FIX(counter_idx));
        rb_ary_push(counters, INT2FIX(0));
    }
    else {
        counter_idx = FIX2LONG(RARRAY_AREF(branch, 5));
    }

    ADD_TRACE_WITH_DATA(seq, RUBY_EVENT_COVERAGE_BRANCH, counter_idx);
    ADD_SYNTHETIC_INSN(seq, loc->end_pos.lineno, node_id, nop);
}

#define ISEQ_LAST_LINE(iseq) (ISEQ_COMPILE_DATA(iseq)->last_line)

static int
validate_label(st_data_t name, st_data_t label, st_data_t arg)
{
    rb_iseq_t *iseq = (rb_iseq_t *)arg;
    LABEL *lobj = (LABEL *)label;
    if (!lobj->link.next) {
        do {
            COMPILE_ERROR(iseq, lobj->position,
                          "%"PRIsVALUE": undefined label",
                          rb_sym2str((VALUE)name));
        } while (0);
    }
    return ST_CONTINUE;
}

static void
validate_labels(rb_iseq_t *iseq, st_table *labels_table)
{
    st_foreach(labels_table, validate_label, (st_data_t)iseq);
    st_free_table(labels_table);
}

static NODE *
get_nd_recv(const NODE *node)
{
    switch (nd_type(node)) {
      case NODE_CALL:
        return RNODE_CALL(node)->nd_recv;
      case NODE_OPCALL:
        return RNODE_OPCALL(node)->nd_recv;
      case NODE_FCALL:
        return 0;
      case NODE_QCALL:
        return RNODE_QCALL(node)->nd_recv;
      case NODE_VCALL:
        return 0;
      case NODE_ATTRASGN:
        return RNODE_ATTRASGN(node)->nd_recv;
      case NODE_OP_ASGN1:
        return RNODE_OP_ASGN1(node)->nd_recv;
      case NODE_OP_ASGN2:
        return RNODE_OP_ASGN2(node)->nd_recv;
      default:
        rb_bug("unexpected node: %s", ruby_node_name(nd_type(node)));
    }
}

static ID
get_node_call_nd_mid(const NODE *node)
{
    switch (nd_type(node)) {
      case NODE_CALL:
        return RNODE_CALL(node)->nd_mid;
      case NODE_OPCALL:
        return RNODE_OPCALL(node)->nd_mid;
      case NODE_FCALL:
        return RNODE_FCALL(node)->nd_mid;
      case NODE_QCALL:
        return RNODE_QCALL(node)->nd_mid;
      case NODE_VCALL:
        return RNODE_VCALL(node)->nd_mid;
      case NODE_ATTRASGN:
        return RNODE_ATTRASGN(node)->nd_mid;
      default:
        rb_bug("unexpected node: %s", ruby_node_name(nd_type(node)));
    }
}

static NODE *
get_nd_args(const NODE *node)
{
    switch (nd_type(node)) {
      case NODE_CALL:
        return RNODE_CALL(node)->nd_args;
      case NODE_OPCALL:
        return RNODE_OPCALL(node)->nd_args;
      case NODE_FCALL:
        return RNODE_FCALL(node)->nd_args;
      case NODE_QCALL:
        return RNODE_QCALL(node)->nd_args;
      case NODE_VCALL:
        return 0;
      case NODE_ATTRASGN:
        return RNODE_ATTRASGN(node)->nd_args;
      default:
        rb_bug("unexpected node: %s", ruby_node_name(nd_type(node)));
    }
}

static ID
get_node_colon_nd_mid(const NODE *node)
{
    switch (nd_type(node)) {
      case NODE_COLON2:
        return RNODE_COLON2(node)->nd_mid;
      case NODE_COLON3:
        return RNODE_COLON3(node)->nd_mid;
      default:
        rb_bug("unexpected node: %s", ruby_node_name(nd_type(node)));
    }
}

static ID
get_nd_vid(const NODE *node)
{
    switch (nd_type(node)) {
      case NODE_LASGN:
        return RNODE_LASGN(node)->nd_vid;
      case NODE_DASGN:
        return RNODE_DASGN(node)->nd_vid;
      case NODE_IASGN:
        return RNODE_IASGN(node)->nd_vid;
      case NODE_CVASGN:
        return RNODE_CVASGN(node)->nd_vid;
      default:
        rb_bug("unexpected node: %s", ruby_node_name(nd_type(node)));
    }
}

static NODE *
get_nd_value(const NODE *node)
{
    switch (nd_type(node)) {
      case NODE_LASGN:
        return RNODE_LASGN(node)->nd_value;
      case NODE_DASGN:
        return RNODE_DASGN(node)->nd_value;
      default:
        rb_bug("unexpected node: %s", ruby_node_name(nd_type(node)));
    }
}

static VALUE
get_string_value(const NODE *node)
{
    switch (nd_type(node)) {
      case NODE_STR:
        return rb_node_str_string_val(node);
      case NODE_FILE:
        return rb_node_file_path_val(node);
      default:
        rb_bug("unexpected node: %s", ruby_node_name(nd_type(node)));
    }
}

VALUE
rb_iseq_compile_callback(rb_iseq_t *iseq, const struct rb_iseq_new_with_callback_callback_func * ifunc)
{
    DECL_ANCHOR(ret);
    INIT_ANCHOR(ret);

    (*ifunc->func)(iseq, ret, ifunc->data);

    ADD_SYNTHETIC_INSN(ret, ISEQ_COMPILE_DATA(iseq)->last_line, -1, leave);

    CHECK(iseq_setup_insn(iseq, ret));
    return iseq_setup(iseq, ret);
}

static bool drop_unreachable_return(LINK_ANCHOR *ret);

VALUE
rb_iseq_compile_node(rb_iseq_t *iseq, const NODE *node)
{
    DECL_ANCHOR(ret);
    INIT_ANCHOR(ret);

    if (node == 0) {
        NO_CHECK(COMPILE(ret, "nil", node));
        iseq_set_local_table(iseq, 0, 0);
    }
    /* assume node is T_NODE */
    else if (nd_type_p(node, NODE_SCOPE)) {
        /* iseq type of top, method, class, block */
        iseq_set_local_table(iseq, RNODE_SCOPE(node)->nd_tbl, (NODE *)RNODE_SCOPE(node)->nd_args);
        iseq_set_arguments(iseq, ret, (NODE *)RNODE_SCOPE(node)->nd_args);

        switch (ISEQ_BODY(iseq)->type) {
          case ISEQ_TYPE_BLOCK:
            {
                LABEL *start = ISEQ_COMPILE_DATA(iseq)->start_label = NEW_LABEL(0);
                LABEL *end = ISEQ_COMPILE_DATA(iseq)->end_label = NEW_LABEL(0);

                start->rescued = LABEL_RESCUE_BEG;
                end->rescued = LABEL_RESCUE_END;

                ADD_TRACE(ret, RUBY_EVENT_B_CALL);
                ADD_SYNTHETIC_INSN(ret, ISEQ_BODY(iseq)->location.first_lineno, -1, nop);
                ADD_LABEL(ret, start);
                CHECK(COMPILE(ret, "block body", RNODE_SCOPE(node)->nd_body));
                ADD_LABEL(ret, end);
                ADD_TRACE(ret, RUBY_EVENT_B_RETURN);
                ISEQ_COMPILE_DATA(iseq)->last_line = ISEQ_BODY(iseq)->location.code_location.end_pos.lineno;

                /* wide range catch handler must put at last */
                ADD_CATCH_ENTRY(CATCH_TYPE_REDO, start, end, NULL, start);
                ADD_CATCH_ENTRY(CATCH_TYPE_NEXT, start, end, NULL, end);
                break;
            }
          case ISEQ_TYPE_CLASS:
            {
                ADD_TRACE(ret, RUBY_EVENT_CLASS);
                CHECK(COMPILE(ret, "scoped node", RNODE_SCOPE(node)->nd_body));
                ADD_TRACE(ret, RUBY_EVENT_END);
                ISEQ_COMPILE_DATA(iseq)->last_line = nd_line(node);
                break;
            }
          case ISEQ_TYPE_METHOD:
            {
                ISEQ_COMPILE_DATA(iseq)->root_node = RNODE_SCOPE(node)->nd_body;
                ADD_TRACE(ret, RUBY_EVENT_CALL);
                CHECK(COMPILE(ret, "scoped node", RNODE_SCOPE(node)->nd_body));
                ISEQ_COMPILE_DATA(iseq)->root_node = RNODE_SCOPE(node)->nd_body;
                ADD_TRACE(ret, RUBY_EVENT_RETURN);
                ISEQ_COMPILE_DATA(iseq)->last_line = nd_line(node);
                break;
            }
          default: {
            CHECK(COMPILE(ret, "scoped node", RNODE_SCOPE(node)->nd_body));
            break;
          }
        }
    }
    else {
        const char *m;
#define INVALID_ISEQ_TYPE(type) \
        ISEQ_TYPE_##type: m = #type; goto invalid_iseq_type
        switch (ISEQ_BODY(iseq)->type) {
          case INVALID_ISEQ_TYPE(METHOD);
          case INVALID_ISEQ_TYPE(CLASS);
          case INVALID_ISEQ_TYPE(BLOCK);
          case INVALID_ISEQ_TYPE(EVAL);
          case INVALID_ISEQ_TYPE(MAIN);
          case INVALID_ISEQ_TYPE(TOP);
#undef INVALID_ISEQ_TYPE /* invalid iseq types end */
          case ISEQ_TYPE_RESCUE:
            iseq_set_exception_local_table(iseq);
            CHECK(COMPILE(ret, "rescue", node));
            break;
          case ISEQ_TYPE_ENSURE:
            iseq_set_exception_local_table(iseq);
            CHECK(COMPILE_POPPED(ret, "ensure", node));
            break;
          case ISEQ_TYPE_PLAIN:
            CHECK(COMPILE(ret, "ensure", node));
            break;
          default:
            COMPILE_ERROR(ERROR_ARGS "unknown scope: %d", ISEQ_BODY(iseq)->type);
            return COMPILE_NG;
          invalid_iseq_type:
            COMPILE_ERROR(ERROR_ARGS "compile/ISEQ_TYPE_%s should not be reached", m);
            return COMPILE_NG;
        }
    }

    if (ISEQ_BODY(iseq)->type == ISEQ_TYPE_RESCUE || ISEQ_BODY(iseq)->type == ISEQ_TYPE_ENSURE) {
        NODE dummy_line_node = generate_dummy_line_node(0, -1);
        ADD_GETLOCAL(ret, &dummy_line_node, LVAR_ERRINFO, 0);
        ADD_INSN1(ret, &dummy_line_node, throw, INT2FIX(0) /* continue throw */ );
    }
    else if (!drop_unreachable_return(ret)) {
        ADD_SYNTHETIC_INSN(ret, ISEQ_COMPILE_DATA(iseq)->last_line, -1, leave);
    }

#if OPT_SUPPORT_JOKE
    if (ISEQ_COMPILE_DATA(iseq)->labels_table) {
        st_table *labels_table = ISEQ_COMPILE_DATA(iseq)->labels_table;
        ISEQ_COMPILE_DATA(iseq)->labels_table = 0;
        validate_labels(iseq, labels_table);
    }
#endif
    CHECK(iseq_setup_insn(iseq, ret));
    return iseq_setup(iseq, ret);
}

static int
rb_iseq_translate_threaded_code(rb_iseq_t *iseq)
{
#if OPT_DIRECT_THREADED_CODE || OPT_CALL_THREADED_CODE
    const void * const *table = rb_vm_get_insns_address_table();
    unsigned int i;
    VALUE *encoded = (VALUE *)ISEQ_BODY(iseq)->iseq_encoded;

    for (i = 0; i < ISEQ_BODY(iseq)->iseq_size; /* */ ) {
        int insn = (int)ISEQ_BODY(iseq)->iseq_encoded[i];
        int len = insn_len(insn);
        encoded[i] = (VALUE)table[insn];
        i += len;
    }
    FL_SET((VALUE)iseq, ISEQ_TRANSLATED);
#endif

#if USE_YJIT
    rb_yjit_live_iseq_count++;
    rb_yjit_iseq_alloc_count++;
#endif

    return COMPILE_OK;
}

VALUE *
rb_iseq_original_iseq(const rb_iseq_t *iseq) /* cold path */
{
    VALUE *original_code;

    if (ISEQ_ORIGINAL_ISEQ(iseq)) return ISEQ_ORIGINAL_ISEQ(iseq);
    original_code = ISEQ_ORIGINAL_ISEQ_ALLOC(iseq, ISEQ_BODY(iseq)->iseq_size);
    MEMCPY(original_code, ISEQ_BODY(iseq)->iseq_encoded, VALUE, ISEQ_BODY(iseq)->iseq_size);

#if OPT_DIRECT_THREADED_CODE || OPT_CALL_THREADED_CODE
    {
        unsigned int i;

        for (i = 0; i < ISEQ_BODY(iseq)->iseq_size; /* */ ) {
            const void *addr = (const void *)original_code[i];
            const int insn = rb_vm_insn_addr2insn(addr);

            original_code[i] = insn;
            i += insn_len(insn);
        }
    }
#endif
    return original_code;
}

/*********************************************/
/* definition of data structure for compiler */
/*********************************************/

/*
 * On 32-bit SPARC, GCC by default generates SPARC V7 code that may require
 * 8-byte word alignment. On the other hand, Oracle Solaris Studio seems to
 * generate SPARCV8PLUS code with unaligned memory access instructions.
 * That is why the STRICT_ALIGNMENT is defined only with GCC.
 */
#if defined(__sparc) && SIZEOF_VOIDP == 4 && defined(__GNUC__)
  #define STRICT_ALIGNMENT
#endif

/*
 * Some OpenBSD platforms (including sparc64) require strict alignment.
 */
#if defined(__OpenBSD__)
  #include <sys/endian.h>
  #ifdef __STRICT_ALIGNMENT
    #define STRICT_ALIGNMENT
  #endif
#endif

#ifdef STRICT_ALIGNMENT
  #if defined(HAVE_TRUE_LONG_LONG) && SIZEOF_LONG_LONG > SIZEOF_VALUE
    #define ALIGNMENT_SIZE SIZEOF_LONG_LONG
  #else
    #define ALIGNMENT_SIZE SIZEOF_VALUE
  #endif
  #define PADDING_SIZE_MAX    ((size_t)((ALIGNMENT_SIZE) - 1))
  #define ALIGNMENT_SIZE_MASK PADDING_SIZE_MAX
  /* Note: ALIGNMENT_SIZE == (2 ** N) is expected. */
#else
  #define PADDING_SIZE_MAX 0
#endif /* STRICT_ALIGNMENT */

#ifdef STRICT_ALIGNMENT
/* calculate padding size for aligned memory access */
static size_t
calc_padding(void *ptr, size_t size)
{
    size_t mis;
    size_t padding = 0;

    mis = (size_t)ptr & ALIGNMENT_SIZE_MASK;
    if (mis > 0) {
        padding = ALIGNMENT_SIZE - mis;
    }
/*
 * On 32-bit sparc or equivalents, when a single VALUE is requested
 * and padding == sizeof(VALUE), it is clear that no padding is needed.
 */
#if ALIGNMENT_SIZE > SIZEOF_VALUE
    if (size == sizeof(VALUE) && padding == sizeof(VALUE)) {
        padding = 0;
    }
#endif

    return padding;
}
#endif /* STRICT_ALIGNMENT */

static void *
compile_data_alloc_with_arena(struct iseq_compile_data_storage **arena, size_t size)
{
    void *ptr = 0;
    struct iseq_compile_data_storage *storage = *arena;
#ifdef STRICT_ALIGNMENT
    size_t padding = calc_padding((void *)&storage->buff[storage->pos], size);
#else
    const size_t padding = 0; /* expected to be optimized by compiler */
#endif /* STRICT_ALIGNMENT */

    if (size >= INT_MAX - padding) rb_memerror();
    if (storage->pos + size + padding > storage->size) {
        unsigned int alloc_size = storage->size;

        while (alloc_size < size + PADDING_SIZE_MAX) {
            if (alloc_size >= INT_MAX / 2) rb_memerror();
            alloc_size *= 2;
        }
        storage->next = (void *)ALLOC_N(char, alloc_size +
                                        offsetof(struct iseq_compile_data_storage, buff));
        storage = *arena = storage->next;
        storage->next = 0;
        storage->pos = 0;
        storage->size = alloc_size;
#ifdef STRICT_ALIGNMENT
        padding = calc_padding((void *)&storage->buff[storage->pos], size);
#endif /* STRICT_ALIGNMENT */
    }

#ifdef STRICT_ALIGNMENT
    storage->pos += (int)padding;
#endif /* STRICT_ALIGNMENT */

    ptr = (void *)&storage->buff[storage->pos];
    storage->pos += (int)size;
    return ptr;
}

static void *
compile_data_alloc(rb_iseq_t *iseq, size_t size)
{
    struct iseq_compile_data_storage ** arena = &ISEQ_COMPILE_DATA(iseq)->node.storage_current;
    return compile_data_alloc_with_arena(arena, size);
}

static inline void *
compile_data_alloc2(rb_iseq_t *iseq, size_t x, size_t y)
{
    size_t size = rb_size_mul_or_raise(x, y, rb_eRuntimeError);
    return compile_data_alloc(iseq, size);
}

static inline void *
compile_data_calloc2(rb_iseq_t *iseq, size_t x, size_t y)
{
    size_t size = rb_size_mul_or_raise(x, y, rb_eRuntimeError);
    void *p = compile_data_alloc(iseq, size);
    memset(p, 0, size);
    return p;
}

static INSN *
compile_data_alloc_insn(rb_iseq_t *iseq)
{
    struct iseq_compile_data_storage ** arena = &ISEQ_COMPILE_DATA(iseq)->insn.storage_current;
    return (INSN *)compile_data_alloc_with_arena(arena, sizeof(INSN));
}

static LABEL *
compile_data_alloc_label(rb_iseq_t *iseq)
{
    return (LABEL *)compile_data_alloc(iseq, sizeof(LABEL));
}

static ADJUST *
compile_data_alloc_adjust(rb_iseq_t *iseq)
{
    return (ADJUST *)compile_data_alloc(iseq, sizeof(ADJUST));
}

static TRACE *
compile_data_alloc_trace(rb_iseq_t *iseq)
{
    return (TRACE *)compile_data_alloc(iseq, sizeof(TRACE));
}

/*
 * elem1, elemX => elem1, elem2, elemX
 */
static void
ELEM_INSERT_NEXT(LINK_ELEMENT *elem1, LINK_ELEMENT *elem2)
{
    elem2->next = elem1->next;
    elem2->prev = elem1;
    elem1->next = elem2;
    if (elem2->next) {
        elem2->next->prev = elem2;
    }
}

/*
 * elem1, elemX => elemX, elem2, elem1
 */
static void
ELEM_INSERT_PREV(LINK_ELEMENT *elem1, LINK_ELEMENT *elem2)
{
    elem2->prev = elem1->prev;
    elem2->next = elem1;
    elem1->prev = elem2;
    if (elem2->prev) {
        elem2->prev->next = elem2;
    }
}

/*
 * elemX, elem1, elemY => elemX, elem2, elemY
 */
static void
ELEM_REPLACE(LINK_ELEMENT *elem1, LINK_ELEMENT *elem2)
{
    elem2->prev = elem1->prev;
    elem2->next = elem1->next;
    if (elem1->prev) {
        elem1->prev->next = elem2;
    }
    if (elem1->next) {
        elem1->next->prev = elem2;
    }
}

static void
ELEM_REMOVE(LINK_ELEMENT *elem)
{
    elem->prev->next = elem->next;
    if (elem->next) {
        elem->next->prev = elem->prev;
    }
}

static LINK_ELEMENT *
FIRST_ELEMENT(const LINK_ANCHOR *const anchor)
{
    return anchor->anchor.next;
}

static LINK_ELEMENT *
LAST_ELEMENT(LINK_ANCHOR *const anchor)
{
    return anchor->last;
}

static LINK_ELEMENT *
ELEM_FIRST_INSN(LINK_ELEMENT *elem)
{
    while (elem) {
        switch (elem->type) {
          case ISEQ_ELEMENT_INSN:
          case ISEQ_ELEMENT_ADJUST:
            return elem;
          default:
            elem = elem->next;
        }
    }
    return NULL;
}

static int
LIST_INSN_SIZE_ONE(const LINK_ANCHOR *const anchor)
{
    LINK_ELEMENT *first_insn = ELEM_FIRST_INSN(FIRST_ELEMENT(anchor));
    if (first_insn != NULL &&
        ELEM_FIRST_INSN(first_insn->next) == NULL) {
        return TRUE;
    }
    else {
        return FALSE;
    }
}

static int
LIST_INSN_SIZE_ZERO(const LINK_ANCHOR *const anchor)
{
    if (ELEM_FIRST_INSN(FIRST_ELEMENT(anchor)) == NULL) {
        return TRUE;
    }
    else {
        return FALSE;
    }
}

/*
 * anc1: e1, e2, e3
 * anc2: e4, e5
 *#=>
 * anc1: e1, e2, e3, e4, e5
 * anc2: e4, e5 (broken)
 */
static void
APPEND_LIST(ISEQ_ARG_DECLARE LINK_ANCHOR *const anc1, LINK_ANCHOR *const anc2)
{
    if (anc2->anchor.next) {
        /* LINK_ANCHOR must not loop */
        RUBY_ASSERT(anc2->last != &anc2->anchor);
        anc1->last->next = anc2->anchor.next;
        anc2->anchor.next->prev = anc1->last;
        anc1->last = anc2->last;
    }
    else {
        RUBY_ASSERT(anc2->last == &anc2->anchor);
    }
    verify_list("append", anc1);
}
#if CPDEBUG < 0
#define APPEND_LIST(anc1, anc2) APPEND_LIST(iseq, (anc1), (anc2))
#endif

#if CPDEBUG && 0
static void
debug_list(ISEQ_ARG_DECLARE LINK_ANCHOR *const anchor, LINK_ELEMENT *cur)
{
    LINK_ELEMENT *list = FIRST_ELEMENT(anchor);
    printf("----\n");
    printf("anch: %p, frst: %p, last: %p\n", (void *)&anchor->anchor,
           (void *)anchor->anchor.next, (void *)anchor->last);
    while (list) {
        printf("curr: %p, next: %p, prev: %p, type: %d\n", (void *)list, (void *)list->next,
               (void *)list->prev, (int)list->type);
        list = list->next;
    }
    printf("----\n");

    dump_disasm_list_with_cursor(anchor->anchor.next, cur, 0);
    verify_list("debug list", anchor);
}
#if CPDEBUG < 0
#define debug_list(anc, cur) debug_list(iseq, (anc), (cur))
#endif
#else
#define debug_list(anc, cur) ((void)0)
#endif

static TRACE *
new_trace_body(rb_iseq_t *iseq, rb_event_flag_t event, long data)
{
    TRACE *trace = compile_data_alloc_trace(iseq);

    trace->link.type = ISEQ_ELEMENT_TRACE;
    trace->link.next = NULL;
    trace->event = event;
    trace->data = data;

    return trace;
}

static LABEL *
new_label_body(rb_iseq_t *iseq, long line)
{
    LABEL *labelobj = compile_data_alloc_label(iseq);

    labelobj->link.type = ISEQ_ELEMENT_LABEL;
    labelobj->link.next = 0;

    labelobj->label_no = ISEQ_COMPILE_DATA(iseq)->label_no++;
    labelobj->sc_state = 0;
    labelobj->sp = -1;
    labelobj->refcnt = 0;
    labelobj->set = 0;
    labelobj->rescued = LABEL_RESCUE_NONE;
    labelobj->unremovable = 0;
    labelobj->position = -1;
    return labelobj;
}

static ADJUST *
new_adjust_body(rb_iseq_t *iseq, LABEL *label, int line)
{
    ADJUST *adjust = compile_data_alloc_adjust(iseq);
    adjust->link.type = ISEQ_ELEMENT_ADJUST;
    adjust->link.next = 0;
    adjust->label = label;
    adjust->line_no = line;
    LABEL_UNREMOVABLE(label);
    return adjust;
}

static void
iseq_insn_each_markable_object(INSN *insn, void (*func)(VALUE *, VALUE), VALUE data)
{
    const char *types = insn_op_types(insn->insn_id);
    for (int j = 0; types[j]; j++) {
        char type = types[j];
        switch (type) {
          case TS_CDHASH:
          case TS_ISEQ:
          case TS_VALUE:
          case TS_IC: // constant path array
          case TS_CALLDATA: // ci is stored.
            func(&OPERAND_AT(insn, j), data);
            break;
          default:
            break;
        }
    }
}

static void
iseq_insn_each_object_write_barrier(VALUE * obj, VALUE iseq)
{
    RB_OBJ_WRITTEN(iseq, Qundef, *obj);
}

static INSN *
new_insn_core(rb_iseq_t *iseq, int line_no, int node_id, int insn_id, int argc, VALUE *argv)
{
    INSN *iobj = compile_data_alloc_insn(iseq);

    /* printf("insn_id: %d, line: %d\n", insn_id, nd_line(line_node)); */

    iobj->link.type = ISEQ_ELEMENT_INSN;
    iobj->link.next = 0;
    iobj->insn_id = insn_id;
    iobj->insn_info.line_no = line_no;
    iobj->insn_info.node_id = node_id;
    iobj->insn_info.events = 0;
    iobj->operands = argv;
    iobj->operand_size = argc;
    iobj->sc_state = 0;

    iseq_insn_each_markable_object(iobj, iseq_insn_each_object_write_barrier, (VALUE)iseq);

    return iobj;
}

static INSN *
new_insn_body(rb_iseq_t *iseq, int line_no, int node_id, enum ruby_vminsn_type insn_id, int argc, ...)
{
    VALUE *operands = 0;
    va_list argv;
    if (argc > 0) {
        int i;
        va_start(argv, argc);
        operands = compile_data_alloc2(iseq, sizeof(VALUE), argc);
        for (i = 0; i < argc; i++) {
            VALUE v = va_arg(argv, VALUE);
            operands[i] = v;
        }
        va_end(argv);
    }
    return new_insn_core(iseq, line_no, node_id, insn_id, argc, operands);
}

static INSN *
insn_replace_with_operands(rb_iseq_t *iseq, INSN *iobj, enum ruby_vminsn_type insn_id, int argc, ...)
{
    VALUE *operands = 0;
    va_list argv;
    if (argc > 0) {
        int i;
        va_start(argv, argc);
        operands = compile_data_alloc2(iseq, sizeof(VALUE), argc);
        for (i = 0; i < argc; i++) {
            VALUE v = va_arg(argv, VALUE);
            operands[i] = v;
        }
        va_end(argv);
    }

    iobj->insn_id = insn_id;
    iobj->operand_size = argc;
    iobj->operands = operands;
    iseq_insn_each_markable_object(iobj, iseq_insn_each_object_write_barrier, (VALUE)iseq);

    return iobj;
}

static const struct rb_callinfo *
new_callinfo(rb_iseq_t *iseq, ID mid, int argc, unsigned int flag, struct rb_callinfo_kwarg *kw_arg, int has_blockiseq)
{
    VM_ASSERT(argc >= 0);

    if (kw_arg) {
        flag |= VM_CALL_KWARG;
        argc += kw_arg->keyword_len;
    }

    if (!(flag & (VM_CALL_ARGS_SPLAT | VM_CALL_ARGS_BLOCKARG | VM_CALL_KWARG | VM_CALL_KW_SPLAT | VM_CALL_FORWARDING))
        && !has_blockiseq) {
        flag |= VM_CALL_ARGS_SIMPLE;
    }

    ISEQ_BODY(iseq)->ci_size++;
    const struct rb_callinfo *ci = vm_ci_new(mid, flag, argc, kw_arg);
    RB_OBJ_WRITTEN(iseq, Qundef, ci);
    return ci;
}

static INSN *
new_insn_send(rb_iseq_t *iseq, int line_no, int node_id, ID id, VALUE argc, const rb_iseq_t *blockiseq, VALUE flag, struct rb_callinfo_kwarg *keywords)
{
    VALUE *operands = compile_data_calloc2(iseq, sizeof(VALUE), 2);
    VALUE ci = (VALUE)new_callinfo(iseq, id, FIX2INT(argc), FIX2INT(flag), keywords, blockiseq != NULL);
    operands[0] = ci;
    operands[1] = (VALUE)blockiseq;
    if (blockiseq) {
        RB_OBJ_WRITTEN(iseq, Qundef, blockiseq);
    }

    INSN *insn;

    if (vm_ci_flag((struct rb_callinfo *)ci) & VM_CALL_FORWARDING) {
        insn = new_insn_core(iseq, line_no, node_id, BIN(sendforward), 2, operands);
    }
    else {
        insn = new_insn_core(iseq, line_no, node_id, BIN(send), 2, operands);
    }

    RB_OBJ_WRITTEN(iseq, Qundef, ci);
    RB_GC_GUARD(ci);
    return insn;
}

static rb_iseq_t *
new_child_iseq(rb_iseq_t *iseq, const NODE *const node,
               VALUE name, const rb_iseq_t *parent, enum rb_iseq_type type, int line_no)
{
    rb_iseq_t *ret_iseq;
    VALUE ast_value = rb_ruby_ast_new(node);

    debugs("[new_child_iseq]> ---------------------------------------\n");
    int isolated_depth = ISEQ_COMPILE_DATA(iseq)->isolated_depth;
    ret_iseq = rb_iseq_new_with_opt(ast_value, name,
                                    rb_iseq_path(iseq), rb_iseq_realpath(iseq),
                                    line_no, parent,
                                    isolated_depth ? isolated_depth + 1 : 0,
                                    type, ISEQ_COMPILE_DATA(iseq)->option,
                                    ISEQ_BODY(iseq)->variable.script_lines);
    debugs("[new_child_iseq]< ---------------------------------------\n");
    return ret_iseq;
}

static rb_iseq_t *
new_child_iseq_with_callback(rb_iseq_t *iseq, const struct rb_iseq_new_with_callback_callback_func *ifunc,
                     VALUE name, const rb_iseq_t *parent, enum rb_iseq_type type, int line_no)
{
    rb_iseq_t *ret_iseq;

    debugs("[new_child_iseq_with_callback]> ---------------------------------------\n");
    ret_iseq = rb_iseq_new_with_callback(ifunc, name,
                                 rb_iseq_path(iseq), rb_iseq_realpath(iseq),
                                 line_no, parent, type, ISEQ_COMPILE_DATA(iseq)->option);
    debugs("[new_child_iseq_with_callback]< ---------------------------------------\n");
    return ret_iseq;
}

static void
set_catch_except_p(rb_iseq_t *iseq)
{
    RUBY_ASSERT(ISEQ_COMPILE_DATA(iseq));
    ISEQ_COMPILE_DATA(iseq)->catch_except_p = true;
    if (ISEQ_BODY(iseq)->parent_iseq != NULL) {
        if (ISEQ_COMPILE_DATA(ISEQ_BODY(iseq)->parent_iseq)) {
          set_catch_except_p((rb_iseq_t *) ISEQ_BODY(iseq)->parent_iseq);
        }
    }
}

/* Set body->catch_except_p to true if the ISeq may catch an exception. If it is false,
   JIT-ed code may be optimized.  If we are extremely conservative, we should set true
   if catch table exists.  But we want to optimize while loop, which always has catch
   table entries for break/next/redo.

   So this function sets true for limited ISeqs with break/next/redo catch table entries
   whose child ISeq would really raise an exception. */
static void
update_catch_except_flags(rb_iseq_t *iseq, struct rb_iseq_constant_body *body)
{
    unsigned int pos;
    size_t i;
    int insn;
    const struct iseq_catch_table *ct = body->catch_table;

    /* This assumes that a block has parent_iseq which may catch an exception from the block, and that
       BREAK/NEXT/REDO catch table entries are used only when `throw` insn is used in the block. */
    pos = 0;
    while (pos < body->iseq_size) {
        insn = rb_vm_insn_decode(body->iseq_encoded[pos]);
        if (insn == BIN(throw)) {
            set_catch_except_p(iseq);
            break;
        }
        pos += insn_len(insn);
    }

    if (ct == NULL)
        return;

    for (i = 0; i < ct->size; i++) {
        const struct iseq_catch_table_entry *entry =
            UNALIGNED_MEMBER_PTR(ct, entries[i]);
        if (entry->type != CATCH_TYPE_BREAK
            && entry->type != CATCH_TYPE_NEXT
            && entry->type != CATCH_TYPE_REDO) {
            RUBY_ASSERT(ISEQ_COMPILE_DATA(iseq));
            ISEQ_COMPILE_DATA(iseq)->catch_except_p = true;
            break;
        }
    }
}

static void
iseq_insert_nop_between_end_and_cont(rb_iseq_t *iseq)
{
    VALUE catch_table_ary = ISEQ_COMPILE_DATA(iseq)->catch_table_ary;
    if (NIL_P(catch_table_ary)) return;
    unsigned int i, tlen = (unsigned int)RARRAY_LEN(catch_table_ary);
    const VALUE *tptr = RARRAY_CONST_PTR(catch_table_ary);
    for (i = 0; i < tlen; i++) {
        const VALUE *ptr = RARRAY_CONST_PTR(tptr[i]);
        LINK_ELEMENT *end = (LINK_ELEMENT *)(ptr[2] & ~1);
        LINK_ELEMENT *cont = (LINK_ELEMENT *)(ptr[4] & ~1);
        LINK_ELEMENT *e;

        enum rb_catch_type ct = (enum rb_catch_type)(ptr[0] & 0xffff);

        if (ct != CATCH_TYPE_BREAK
            && ct != CATCH_TYPE_NEXT
            && ct != CATCH_TYPE_REDO) {

            for (e = end; e && (IS_LABEL(e) || IS_TRACE(e)); e = e->next) {
                if (e == cont) {
                    INSN *nop = new_insn_core(iseq, 0, -1, BIN(nop), 0, 0);
                    ELEM_INSERT_NEXT(end, &nop->link);
                    break;
                }
            }
        }
    }

    RB_GC_GUARD(catch_table_ary);
}

static int
iseq_setup_insn(rb_iseq_t *iseq, LINK_ANCHOR *const anchor)
{
    if (RTEST(ISEQ_COMPILE_DATA(iseq)->err_info))
        return COMPILE_NG;

    /* debugs("[compile step 2] (iseq_array_to_linkedlist)\n"); */

    if (compile_debug > 5)
        dump_disasm_list(FIRST_ELEMENT(anchor));

    debugs("[compile step 3.1 (iseq_optimize)]\n");
    iseq_optimize(iseq, anchor);

    if (compile_debug > 5)
        dump_disasm_list(FIRST_ELEMENT(anchor));

    if (ISEQ_COMPILE_DATA(iseq)->option->instructions_unification) {
        debugs("[compile step 3.2 (iseq_insns_unification)]\n");
        iseq_insns_unification(iseq, anchor);
        if (compile_debug > 5)
            dump_disasm_list(FIRST_ELEMENT(anchor));
    }

    debugs("[compile step 3.4 (iseq_insert_nop_between_end_and_cont)]\n");
    iseq_insert_nop_between_end_and_cont(iseq);
    if (compile_debug > 5)
        dump_disasm_list(FIRST_ELEMENT(anchor));

    return COMPILE_OK;
}

static int
iseq_setup(rb_iseq_t *iseq, LINK_ANCHOR *const anchor)
{
    if (RTEST(ISEQ_COMPILE_DATA(iseq)->err_info))
        return COMPILE_NG;

    debugs("[compile step 4.1 (iseq_set_sequence)]\n");
    if (!iseq_set_sequence(iseq, anchor)) return COMPILE_NG;
    if (compile_debug > 5)
        dump_disasm_list(FIRST_ELEMENT(anchor));

    debugs("[compile step 4.2 (iseq_set_exception_table)]\n");
    if (!iseq_set_exception_table(iseq)) return COMPILE_NG;

    debugs("[compile step 4.3 (set_optargs_table)] \n");
    if (!iseq_set_optargs_table(iseq)) return COMPILE_NG;

    debugs("[compile step 5 (iseq_translate_threaded_code)] \n");
    if (!rb_iseq_translate_threaded_code(iseq)) return COMPILE_NG;

    debugs("[compile step 6 (update_catch_except_flags)] \n");
    RUBY_ASSERT(ISEQ_COMPILE_DATA(iseq));
    update_catch_except_flags(iseq, ISEQ_BODY(iseq));

    debugs("[compile step 6.1 (remove unused catch tables)] \n");
    RUBY_ASSERT(ISEQ_COMPILE_DATA(iseq));
    if (!ISEQ_COMPILE_DATA(iseq)->catch_except_p && ISEQ_BODY(iseq)->catch_table) {
        xfree(ISEQ_BODY(iseq)->catch_table);
        ISEQ_BODY(iseq)->catch_table = NULL;
    }

#if VM_INSN_INFO_TABLE_IMPL == 2
    if (ISEQ_BODY(iseq)->insns_info.succ_index_table == NULL) {
        debugs("[compile step 7 (rb_iseq_insns_info_encode_positions)] \n");
        rb_iseq_insns_info_encode_positions(iseq);
    }
#endif

    if (compile_debug > 1) {
        VALUE str = rb_iseq_disasm(iseq);
        printf("%s\n", StringValueCStr(str));
    }
    verify_call_cache(iseq);
    debugs("[compile step: finish]\n");

    return COMPILE_OK;
}

static int
iseq_set_exception_local_table(rb_iseq_t *iseq)
{
    ISEQ_BODY(iseq)->local_table_size = numberof(rb_iseq_shared_exc_local_tbl);
    ISEQ_BODY(iseq)->local_table = rb_iseq_shared_exc_local_tbl;
    return COMPILE_OK;
}

static int
get_lvar_level(const rb_iseq_t *iseq)
{
    int lev = 0;
    while (iseq != ISEQ_BODY(iseq)->local_iseq) {
        lev++;
        iseq = ISEQ_BODY(iseq)->parent_iseq;
    }
    return lev;
}

static int
get_dyna_var_idx_at_raw(const rb_iseq_t *iseq, ID id)
{
    unsigned int i;

    for (i = 0; i < ISEQ_BODY(iseq)->local_table_size; i++) {
        if (ISEQ_BODY(iseq)->local_table[i] == id) {
            return (int)i;
        }
    }
    return -1;
}

static int
get_local_var_idx(const rb_iseq_t *iseq, ID id)
{
    int idx = get_dyna_var_idx_at_raw(ISEQ_BODY(iseq)->local_iseq, id);

    if (idx < 0) {
        COMPILE_ERROR(iseq, ISEQ_LAST_LINE(iseq),
                      "get_local_var_idx: %d", idx);
    }

    return idx;
}

static int
get_dyna_var_idx(const rb_iseq_t *iseq, ID id, int *level, int *ls)
{
    int lv = 0, idx = -1;
    const rb_iseq_t *const topmost_iseq = iseq;

    while (iseq) {
        idx = get_dyna_var_idx_at_raw(iseq, id);
        if (idx >= 0) {
            break;
        }
        iseq = ISEQ_BODY(iseq)->parent_iseq;
        lv++;
    }

    if (idx < 0) {
        COMPILE_ERROR(topmost_iseq, ISEQ_LAST_LINE(topmost_iseq),
                      "get_dyna_var_idx: -1");
    }

    *level = lv;
    *ls = ISEQ_BODY(iseq)->local_table_size;
    return idx;
}

static int
iseq_local_block_param_p(const rb_iseq_t *iseq, unsigned int idx, unsigned int level)
{
    const struct rb_iseq_constant_body *body;
    while (level > 0) {
        iseq = ISEQ_BODY(iseq)->parent_iseq;
        level--;
    }
    body = ISEQ_BODY(iseq);
    if (body->local_iseq == iseq && /* local variables */
        body->param.flags.has_block &&
        body->local_table_size - body->param.block_start == idx) {
        return TRUE;
    }
    else {
        return FALSE;
    }
}

static int
iseq_block_param_id_p(const rb_iseq_t *iseq, ID id, int *pidx, int *plevel)
{
    int level, ls;
    int idx = get_dyna_var_idx(iseq, id, &level, &ls);
    if (iseq_local_block_param_p(iseq, ls - idx, level)) {
        *pidx = ls - idx;
        *plevel = level;
        return TRUE;
    }
    else {
        return FALSE;
    }
}

static void
access_outer_variables(const rb_iseq_t *iseq, int level, ID id, bool write)
{
    int isolated_depth = ISEQ_COMPILE_DATA(iseq)->isolated_depth;

    if (isolated_depth && level >= isolated_depth) {
        if (id == rb_intern("yield")) {
            COMPILE_ERROR(iseq, ISEQ_LAST_LINE(iseq), "can not yield from isolated Proc");
        }
        else {
            COMPILE_ERROR(iseq, ISEQ_LAST_LINE(iseq), "can not access variable '%s' from isolated Proc", rb_id2name(id));
        }
    }

    for (int i=0; i<level; i++) {
        VALUE val;
        struct rb_id_table *ovs = ISEQ_BODY(iseq)->outer_variables;

        if (!ovs) {
            ovs = ISEQ_BODY(iseq)->outer_variables = rb_id_table_create(8);
        }

        if (rb_id_table_lookup(ISEQ_BODY(iseq)->outer_variables, id, &val)) {
            if (write && !val) {
                rb_id_table_insert(ISEQ_BODY(iseq)->outer_variables, id, Qtrue);
            }
        }
        else {
            rb_id_table_insert(ISEQ_BODY(iseq)->outer_variables, id, RBOOL(write));
        }

        iseq = ISEQ_BODY(iseq)->parent_iseq;
    }
}

static ID
iseq_lvar_id(const rb_iseq_t *iseq, int idx, int level)
{
    for (int i=0; i<level; i++) {
        iseq = ISEQ_BODY(iseq)->parent_iseq;
    }

    ID id = ISEQ_BODY(iseq)->local_table[ISEQ_BODY(iseq)->local_table_size - idx];
    // fprintf(stderr, "idx:%d level:%d ID:%s\n", idx, level, rb_id2name(id));
    return id;
}

static void
iseq_add_getlocal(rb_iseq_t *iseq, LINK_ANCHOR *const seq, const NODE *const line_node, int idx, int level)
{
    if (iseq_local_block_param_p(iseq, idx, level)) {
        ADD_INSN2(seq, line_node, getblockparam, INT2FIX((idx) + VM_ENV_DATA_SIZE - 1), INT2FIX(level));
    }
    else {
        ADD_INSN2(seq, line_node, getlocal, INT2FIX((idx) + VM_ENV_DATA_SIZE - 1), INT2FIX(level));
    }
    if (level > 0) access_outer_variables(iseq, level, iseq_lvar_id(iseq, idx, level), Qfalse);
}

static void
iseq_add_setlocal(rb_iseq_t *iseq, LINK_ANCHOR *const seq, const NODE *const line_node, int idx, int level)
{
    if (iseq_local_block_param_p(iseq, idx, level)) {
        ADD_INSN2(seq, line_node, setblockparam, INT2FIX((idx) + VM_ENV_DATA_SIZE - 1), INT2FIX(level));
    }
    else {
        ADD_INSN2(seq, line_node, setlocal, INT2FIX((idx) + VM_ENV_DATA_SIZE - 1), INT2FIX(level));
    }
    if (level > 0) access_outer_variables(iseq, level, iseq_lvar_id(iseq, idx, level), Qtrue);
}



static void
iseq_calc_param_size(rb_iseq_t *iseq)
{
    struct rb_iseq_constant_body *const body = ISEQ_BODY(iseq);
    if (body->param.flags.has_opt ||
        body->param.flags.has_post ||
        body->param.flags.has_rest ||
        body->param.flags.has_block ||
        body->param.flags.has_kw ||
        body->param.flags.has_kwrest) {

        if (body->param.flags.has_block) {
            body->param.size = body->param.block_start + 1;
        }
        else if (body->param.flags.has_kwrest) {
            body->param.size = body->param.keyword->rest_start + 1;
        }
        else if (body->param.flags.has_kw) {
            body->param.size = body->param.keyword->bits_start + 1;
        }
        else if (body->param.flags.has_post) {
            body->param.size = body->param.post_start + body->param.post_num;
        }
        else if (body->param.flags.has_rest) {
            body->param.size = body->param.rest_start + 1;
        }
        else if (body->param.flags.has_opt) {
            body->param.size = body->param.lead_num + body->param.opt_num;
        }
        else {
            UNREACHABLE;
        }
    }
    else {
        body->param.size = body->param.lead_num;
    }
}

static int
iseq_set_arguments_keywords(rb_iseq_t *iseq, LINK_ANCHOR *const optargs,
                            const struct rb_args_info *args, int arg_size)
{
    const rb_node_kw_arg_t *node = args->kw_args;
    struct rb_iseq_constant_body *const body = ISEQ_BODY(iseq);
    struct rb_iseq_param_keyword *keyword;
    const VALUE default_values = rb_ary_hidden_new(1);
    const VALUE complex_mark = rb_str_tmp_new(0);
    int kw = 0, rkw = 0, di = 0, i;

    body->param.flags.has_kw = TRUE;
    body->param.keyword = keyword = ZALLOC_N(struct rb_iseq_param_keyword, 1);

    while (node) {
        kw++;
        node = node->nd_next;
    }
    arg_size += kw;
    keyword->bits_start = arg_size++;

    node = args->kw_args;
    while (node) {
        const NODE *val_node = get_nd_value(node->nd_body);
        VALUE dv;

        if (val_node == NODE_SPECIAL_REQUIRED_KEYWORD) {
            ++rkw;
        }
        else {
            switch (nd_type(val_node)) {
              case NODE_SYM:
                dv = rb_node_sym_string_val(val_node);
                break;
              case NODE_REGX:
                dv = rb_node_regx_string_val(val_node);
                break;
              case NODE_LINE:
                dv = rb_node_line_lineno_val(val_node);
                break;
              case NODE_INTEGER:
                dv = rb_node_integer_literal_val(val_node);
                break;
              case NODE_FLOAT:
                dv = rb_node_float_literal_val(val_node);
                break;
              case NODE_RATIONAL:
                dv = rb_node_rational_literal_val(val_node);
                break;
              case NODE_IMAGINARY:
                dv = rb_node_imaginary_literal_val(val_node);
                break;
              case NODE_ENCODING:
                dv = rb_node_encoding_val(val_node);
                break;
              case NODE_NIL:
                dv = Qnil;
                break;
              case NODE_TRUE:
                dv = Qtrue;
                break;
              case NODE_FALSE:
                dv = Qfalse;
                break;
              default:
                NO_CHECK(COMPILE_POPPED(optargs, "kwarg", RNODE(node))); /* nd_type_p(node, NODE_KW_ARG) */
                dv = complex_mark;
            }

            keyword->num = ++di;
            rb_ary_push(default_values, dv);
        }

        node = node->nd_next;
    }

    keyword->num = kw;

    if (RNODE_DVAR(args->kw_rest_arg)->nd_vid != 0) {
        ID kw_id = ISEQ_BODY(iseq)->local_table[arg_size];
        keyword->rest_start = arg_size++;
        body->param.flags.has_kwrest = TRUE;

        if (kw_id == idPow) body->param.flags.anon_kwrest = TRUE;
    }
    keyword->required_num = rkw;
    keyword->table = &body->local_table[keyword->bits_start - keyword->num];

    if (RARRAY_LEN(default_values)) {
        VALUE *dvs = ALLOC_N(VALUE, RARRAY_LEN(default_values));

        for (i = 0; i < RARRAY_LEN(default_values); i++) {
            VALUE dv = RARRAY_AREF(default_values, i);
            if (dv == complex_mark) dv = Qundef;
            RB_OBJ_WRITE(iseq, &dvs[i], dv);
        }

        keyword->default_values = dvs;
    }
    return arg_size;
}

static void
iseq_set_use_block(rb_iseq_t *iseq)
{
    struct rb_iseq_constant_body *const body = ISEQ_BODY(iseq);
    if (!body->param.flags.use_block) {
        body->param.flags.use_block = 1;

        rb_vm_t *vm = GET_VM();

        if (!rb_warning_category_enabled_p(RB_WARN_CATEGORY_STRICT_UNUSED_BLOCK)) {
            st_data_t key = (st_data_t)rb_intern_str(body->location.label); // String -> ID
            set_insert(vm->unused_block_warning_table, key);
        }
    }
}

static int
iseq_set_arguments(rb_iseq_t *iseq, LINK_ANCHOR *const optargs, const NODE *const node_args)
{
    debugs("iseq_set_arguments: %s\n", node_args ? "" : "0");

    if (node_args) {
        struct rb_iseq_constant_body *const body = ISEQ_BODY(iseq);
        struct rb_args_info *args = &RNODE_ARGS(node_args)->nd_ainfo;
        ID rest_id = 0;
        int last_comma = 0;
        ID block_id = 0;
        int arg_size;

        EXPECT_NODE("iseq_set_arguments", node_args, NODE_ARGS, COMPILE_NG);

        body->param.flags.ruby2_keywords = args->ruby2_keywords;
        body->param.lead_num = arg_size = (int)args->pre_args_num;
        if (body->param.lead_num > 0) body->param.flags.has_lead = TRUE;
        debugs("  - argc: %d\n", body->param.lead_num);

        rest_id = args->rest_arg;
        if (rest_id == NODE_SPECIAL_EXCESSIVE_COMMA) {
            last_comma = 1;
            rest_id = 0;
        }
        block_id = args->block_arg;

        bool optimized_forward = (args->forwarding && args->pre_args_num == 0 && !args->opt_args);

        if (optimized_forward) {
            rest_id = 0;
            block_id = 0;
        }

        if (args->opt_args) {
            const rb_node_opt_arg_t *node = args->opt_args;
            LABEL *label;
            VALUE labels = rb_ary_hidden_new(1);
            VALUE *opt_table;
            int i = 0, j;

            while (node) {
                label = NEW_LABEL(nd_line(RNODE(node)));
                rb_ary_push(labels, (VALUE)label | 1);
                ADD_LABEL(optargs, label);
                NO_CHECK(COMPILE_POPPED(optargs, "optarg", node->nd_body));
                node = node->nd_next;
                i += 1;
            }

            /* last label */
            label = NEW_LABEL(nd_line(node_args));
            rb_ary_push(labels, (VALUE)label | 1);
            ADD_LABEL(optargs, label);

            opt_table = ALLOC_N(VALUE, i+1);

            MEMCPY(opt_table, RARRAY_CONST_PTR(labels), VALUE, i+1);
            for (j = 0; j < i+1; j++) {
                opt_table[j] &= ~1;
            }
            rb_ary_clear(labels);

            body->param.flags.has_opt = TRUE;
            body->param.opt_num = i;
            body->param.opt_table = opt_table;
            arg_size += i;
        }

        if (rest_id) {
            body->param.rest_start = arg_size++;
            body->param.flags.has_rest = TRUE;
            if (rest_id == '*') body->param.flags.anon_rest = TRUE;
            RUBY_ASSERT(body->param.rest_start != -1);
        }

        if (args->first_post_arg) {
            body->param.post_start = arg_size;
            body->param.post_num = args->post_args_num;
            body->param.flags.has_post = TRUE;
            arg_size += args->post_args_num;

            if (body->param.flags.has_rest) { /* TODO: why that? */
                body->param.post_start = body->param.rest_start + 1;
            }
        }

        if (args->kw_args) {
            arg_size = iseq_set_arguments_keywords(iseq, optargs, args, arg_size);
        }
        else if (args->kw_rest_arg && !optimized_forward) {
            ID kw_id = ISEQ_BODY(iseq)->local_table[arg_size];
            struct rb_iseq_param_keyword *keyword = ZALLOC_N(struct rb_iseq_param_keyword, 1);
            keyword->rest_start = arg_size++;
            body->param.keyword = keyword;
            body->param.flags.has_kwrest = TRUE;

            static ID anon_kwrest = 0;
            if (!anon_kwrest) anon_kwrest = rb_intern("**");
            if (kw_id == anon_kwrest) body->param.flags.anon_kwrest = TRUE;
        }
        else if (args->no_kwarg) {
            body->param.flags.accepts_no_kwarg = TRUE;
        }

        if (block_id) {
            body->param.block_start = arg_size++;
            body->param.flags.has_block = TRUE;
            iseq_set_use_block(iseq);
        }

        // Only optimize specifically methods like this: `foo(...)`
        if (optimized_forward) {
            body->param.flags.use_block = 1;
            body->param.flags.forwardable = TRUE;
            arg_size = 1;
        }

        iseq_calc_param_size(iseq);
        body->param.size = arg_size;

        if (args->pre_init) { /* m_init */
            NO_CHECK(COMPILE_POPPED(optargs, "init arguments (m)", args->pre_init));
        }
        if (args->post_init) { /* p_init */
            NO_CHECK(COMPILE_POPPED(optargs, "init arguments (p)", args->post_init));
        }

        if (body->type == ISEQ_TYPE_BLOCK) {
            if (body->param.flags.has_opt    == FALSE &&
                body->param.flags.has_post   == FALSE &&
                body->param.flags.has_rest   == FALSE &&
                body->param.flags.has_kw     == FALSE &&
                body->param.flags.has_kwrest == FALSE) {

                if (body->param.lead_num == 1 && last_comma == 0) {
                    /* {|a|} */
                    body->param.flags.ambiguous_param0 = TRUE;
                }
            }
        }
    }

    return COMPILE_OK;
}

static int
iseq_set_local_table(rb_iseq_t *iseq, const rb_ast_id_table_t *tbl, const NODE *const node_args)
{
    unsigned int size = tbl ? tbl->size : 0;
    unsigned int offset = 0;

    if (node_args) {
        struct rb_args_info *args = &RNODE_ARGS(node_args)->nd_ainfo;

        // If we have a function that only has `...` as the parameter,
        // then its local table should only be `...`
        // FIXME: I think this should be fixed in the AST rather than special case here.
        if (args->forwarding && args->pre_args_num == 0 && !args->opt_args) {
            CHECK(size >= 3);
            size -= 3;
            offset += 3;
        }
    }

    if (size > 0) {
        ID *ids = ALLOC_N(ID, size);
        MEMCPY(ids, tbl->ids + offset, ID, size);
        ISEQ_BODY(iseq)->local_table = ids;
    }
    ISEQ_BODY(iseq)->local_table_size = size;

    debugs("iseq_set_local_table: %u\n", ISEQ_BODY(iseq)->local_table_size);
    return COMPILE_OK;
}

int
rb_iseq_cdhash_cmp(VALUE val, VALUE lit)
{
    int tval, tlit;

    if (val == lit) {
        return 0;
    }
    else if ((tlit = OBJ_BUILTIN_TYPE(lit)) == -1) {
        return val != lit;
    }
    else if ((tval = OBJ_BUILTIN_TYPE(val)) == -1) {
        return -1;
    }
    else if (tlit != tval) {
        return -1;
    }
    else if (tlit == T_SYMBOL) {
        return val != lit;
    }
    else if (tlit == T_STRING) {
        return rb_str_hash_cmp(lit, val);
    }
    else if (tlit == T_BIGNUM) {
        long x = FIX2LONG(rb_big_cmp(lit, val));

        /* Given lit and val are both Bignum, x must be -1, 0, 1.
         * There is no need to call rb_fix2int here. */
        RUBY_ASSERT((x == 1) || (x == 0) || (x == -1));
        return (int)x;
    }
    else if (tlit == T_FLOAT) {
        return rb_float_cmp(lit, val);
    }
    else if (tlit == T_RATIONAL) {
        const struct RRational *rat1 = RRATIONAL(val);
        const struct RRational *rat2 = RRATIONAL(lit);
        return rb_iseq_cdhash_cmp(rat1->num, rat2->num) || rb_iseq_cdhash_cmp(rat1->den, rat2->den);
    }
    else if (tlit == T_COMPLEX) {
        const struct RComplex *comp1 = RCOMPLEX(val);
        const struct RComplex *comp2 = RCOMPLEX(lit);
        return rb_iseq_cdhash_cmp(comp1->real, comp2->real) || rb_iseq_cdhash_cmp(comp1->imag, comp2->imag);
    }
    else if (tlit == T_REGEXP) {
        return rb_reg_equal(val, lit) ? 0 : -1;
    }
    else {
        UNREACHABLE_RETURN(-1);
    }
}

st_index_t
rb_iseq_cdhash_hash(VALUE a)
{
    switch (OBJ_BUILTIN_TYPE(a)) {
      case -1:
      case T_SYMBOL:
        return (st_index_t)a;
      case T_STRING:
        return rb_str_hash(a);
      case T_BIGNUM:
        return FIX2LONG(rb_big_hash(a));
      case T_FLOAT:
        return rb_dbl_long_hash(RFLOAT_VALUE(a));
      case T_RATIONAL:
        return rb_rational_hash(a);
      case T_COMPLEX:
        return rb_complex_hash(a);
      case T_REGEXP:
        return NUM2LONG(rb_reg_hash(a));
      default:
        UNREACHABLE_RETURN(0);
    }
}

static const struct st_hash_type cdhash_type = {
    rb_iseq_cdhash_cmp,
    rb_iseq_cdhash_hash,
};

struct cdhash_set_label_struct {
    VALUE hash;
    int pos;
    int len;
};

static int
cdhash_set_label_i(VALUE key, VALUE val, VALUE ptr)
{
    struct cdhash_set_label_struct *data = (struct cdhash_set_label_struct *)ptr;
    LABEL *lobj = (LABEL *)(val & ~1);
    rb_hash_aset(data->hash, key, INT2FIX(lobj->position - (data->pos+data->len)));
    return ST_CONTINUE;
}


static inline VALUE
get_ivar_ic_value(rb_iseq_t *iseq,ID id)
{
    return INT2FIX(ISEQ_BODY(iseq)->ivc_size++);
}

static inline VALUE
get_cvar_ic_value(rb_iseq_t *iseq,ID id)
{
    VALUE val;
    struct rb_id_table *tbl = ISEQ_COMPILE_DATA(iseq)->ivar_cache_table;
    if (tbl) {
        if (rb_id_table_lookup(tbl,id,&val)) {
            return val;
        }
    }
    else {
        tbl = rb_id_table_create(1);
        ISEQ_COMPILE_DATA(iseq)->ivar_cache_table = tbl;
    }
    val = INT2FIX(ISEQ_BODY(iseq)->icvarc_size++);
    rb_id_table_insert(tbl,id,val);
    return val;
}

#define BADINSN_DUMP(anchor, list, dest) \
    dump_disasm_list_with_cursor(FIRST_ELEMENT(anchor), list, dest)

#define BADINSN_ERROR \
    (xfree(generated_iseq), \
     xfree(insns_info), \
     BADINSN_DUMP(anchor, list, NULL), \
     COMPILE_ERROR)

static int
fix_sp_depth(rb_iseq_t *iseq, LINK_ANCHOR *const anchor)
{
    int stack_max = 0, sp = 0, line = 0;
    LINK_ELEMENT *list;

    for (list = FIRST_ELEMENT(anchor); list; list = list->next) {
        if (IS_LABEL(list)) {
            LABEL *lobj = (LABEL *)list;
            lobj->set = TRUE;
        }
    }

    for (list = FIRST_ELEMENT(anchor); list; list = list->next) {
        switch (list->type) {
          case ISEQ_ELEMENT_INSN:
            {
                int j, len, insn;
                const char *types;
                VALUE *operands;
                INSN *iobj = (INSN *)list;

                /* update sp */
                sp = calc_sp_depth(sp, iobj);
                if (sp < 0) {
                    BADINSN_DUMP(anchor, list, NULL);
                    COMPILE_ERROR(iseq, iobj->insn_info.line_no,
                                  "argument stack underflow (%d)", sp);
                    return -1;
                }
                if (sp > stack_max) {
                    stack_max = sp;
                }

                line = iobj->insn_info.line_no;
                /* fprintf(stderr, "insn: %-16s, sp: %d\n", insn_name(iobj->insn_id), sp); */
                operands = iobj->operands;
                insn = iobj->insn_id;
                types = insn_op_types(insn);
                len = insn_len(insn);

                /* operand check */
                if (iobj->operand_size != len - 1) {
                    /* printf("operand size miss! (%d, %d)\n", iobj->operand_size, len); */
                    BADINSN_DUMP(anchor, list, NULL);
                    COMPILE_ERROR(iseq, iobj->insn_info.line_no,
                                  "operand size miss! (%d for %d)",
                                  iobj->operand_size, len - 1);
                    return -1;
                }

                for (j = 0; types[j]; j++) {
                    if (types[j] == TS_OFFSET) {
                        /* label(destination position) */
                        LABEL *lobj = (LABEL *)operands[j];
                        if (!lobj->set) {
                            BADINSN_DUMP(anchor, list, NULL);
                            COMPILE_ERROR(iseq, iobj->insn_info.line_no,
                                          "unknown label: "LABEL_FORMAT, lobj->label_no);
                            return -1;
                        }
                        if (lobj->sp == -1) {
                            lobj->sp = sp;
                        }
                        else if (lobj->sp != sp) {
                            debugs("%s:%d: sp inconsistency found but ignored (" LABEL_FORMAT " sp: %d, calculated sp: %d)\n",
                                   RSTRING_PTR(rb_iseq_path(iseq)), line,
                                   lobj->label_no, lobj->sp, sp);
                        }
                    }
                }
                break;
            }
          case ISEQ_ELEMENT_LABEL:
            {
                LABEL *lobj = (LABEL *)list;
                if (lobj->sp == -1) {
                    lobj->sp = sp;
                }
                else {
                    if (lobj->sp != sp) {
                        debugs("%s:%d: sp inconsistency found but ignored (" LABEL_FORMAT " sp: %d, calculated sp: %d)\n",
                                RSTRING_PTR(rb_iseq_path(iseq)), line,
                                lobj->label_no, lobj->sp, sp);
                    }
                    sp = lobj->sp;
                }
                break;
            }
          case ISEQ_ELEMENT_TRACE:
            {
                /* ignore */
                break;
            }
          case ISEQ_ELEMENT_ADJUST:
            {
                ADJUST *adjust = (ADJUST *)list;
                int orig_sp = sp;

                sp = adjust->label ? adjust->label->sp : 0;
                if (adjust->line_no != -1 && orig_sp - sp < 0) {
                    BADINSN_DUMP(anchor, list, NULL);
                    COMPILE_ERROR(iseq, adjust->line_no,
                                  "iseq_set_sequence: adjust bug %d < %d",
                                  orig_sp, sp);
                    return -1;
                }
                break;
            }
          default:
            BADINSN_DUMP(anchor, list, NULL);
            COMPILE_ERROR(iseq, line, "unknown list type: %d", list->type);
            return -1;
        }
    }
    return stack_max;
}

static int
add_insn_info(struct iseq_insn_info_entry *insns_info, unsigned int *positions,
              int insns_info_index, int code_index, const INSN *iobj)
{
    if (insns_info_index == 0 ||
        insns_info[insns_info_index-1].line_no != iobj->insn_info.line_no ||
#ifdef USE_ISEQ_NODE_ID
        insns_info[insns_info_index-1].node_id != iobj->insn_info.node_id ||
#endif
        insns_info[insns_info_index-1].events  != iobj->insn_info.events) {
        insns_info[insns_info_index].line_no    = iobj->insn_info.line_no;
#ifdef USE_ISEQ_NODE_ID
        insns_info[insns_info_index].node_id    = iobj->insn_info.node_id;
#endif
        insns_info[insns_info_index].events     = iobj->insn_info.events;
        positions[insns_info_index]             = code_index;
        return TRUE;
    }
    return FALSE;
}

static int
add_adjust_info(struct iseq_insn_info_entry *insns_info, unsigned int *positions,
                int insns_info_index, int code_index, const ADJUST *adjust)
{
    insns_info[insns_info_index].line_no    = adjust->line_no;
    insns_info[insns_info_index].node_id    = -1;
    insns_info[insns_info_index].events     = 0;
    positions[insns_info_index]             = code_index;
    return TRUE;
}

static ID *
array_to_idlist(VALUE arr)
{
    RUBY_ASSERT(RB_TYPE_P(arr, T_ARRAY));
    long size = RARRAY_LEN(arr);
    ID *ids = (ID *)ALLOC_N(ID, size + 1);
    for (long i = 0; i < size; i++) {
        VALUE sym = RARRAY_AREF(arr, i);
        ids[i] = SYM2ID(sym);
    }
    ids[size] = 0;
    return ids;
}

static VALUE
idlist_to_array(const ID *ids)
{
    VALUE arr = rb_ary_new();
    while (*ids) {
        rb_ary_push(arr, ID2SYM(*ids++));
    }
    return arr;
}

/**
  ruby insn object list -> raw instruction sequence
 */
static int
iseq_set_sequence(rb_iseq_t *iseq, LINK_ANCHOR *const anchor)
{
    struct iseq_insn_info_entry *insns_info;
    struct rb_iseq_constant_body *const body = ISEQ_BODY(iseq);
    unsigned int *positions;
    LINK_ELEMENT *list;
    VALUE *generated_iseq;
    rb_event_flag_t events = 0;
    long data = 0;

    int insn_num, code_index, insns_info_index, sp = 0;
    int stack_max = fix_sp_depth(iseq, anchor);

    if (stack_max < 0) return COMPILE_NG;

    /* fix label position */
    insn_num = code_index = 0;
    for (list = FIRST_ELEMENT(anchor); list; list = list->next) {
        switch (list->type) {
          case ISEQ_ELEMENT_INSN:
            {
                INSN *iobj = (INSN *)list;
                /* update sp */
                sp = calc_sp_depth(sp, iobj);
                insn_num++;
                events = iobj->insn_info.events |= events;
                if (ISEQ_COVERAGE(iseq)) {
                    if (ISEQ_LINE_COVERAGE(iseq) && (events & RUBY_EVENT_COVERAGE_LINE) &&
                        !(rb_get_coverage_mode() & COVERAGE_TARGET_ONESHOT_LINES)) {
                        int line = iobj->insn_info.line_no - 1;
                        if (line >= 0 && line < RARRAY_LEN(ISEQ_LINE_COVERAGE(iseq))) {
                            RARRAY_ASET(ISEQ_LINE_COVERAGE(iseq), line, INT2FIX(0));
                        }
                    }
                    if (ISEQ_BRANCH_COVERAGE(iseq) && (events & RUBY_EVENT_COVERAGE_BRANCH)) {
                        while (RARRAY_LEN(ISEQ_PC2BRANCHINDEX(iseq)) <= code_index) {
                            rb_ary_push(ISEQ_PC2BRANCHINDEX(iseq), Qnil);
                        }
                        RARRAY_ASET(ISEQ_PC2BRANCHINDEX(iseq), code_index, INT2FIX(data));
                    }
                }
                code_index += insn_data_length(iobj);
                events = 0;
                data = 0;
                break;
            }
          case ISEQ_ELEMENT_LABEL:
            {
                LABEL *lobj = (LABEL *)list;
                lobj->position = code_index;
                if (lobj->sp != sp) {
                    debugs("%s: sp inconsistency found but ignored (" LABEL_FORMAT " sp: %d, calculated sp: %d)\n",
                           RSTRING_PTR(rb_iseq_path(iseq)),
                           lobj->label_no, lobj->sp, sp);
                }
                sp = lobj->sp;
                break;
            }
          case ISEQ_ELEMENT_TRACE:
            {
                TRACE *trace = (TRACE *)list;
                events |= trace->event;
                if (trace->event & RUBY_EVENT_COVERAGE_BRANCH) data = trace->data;
                break;
            }
          case ISEQ_ELEMENT_ADJUST:
            {
                ADJUST *adjust = (ADJUST *)list;
                if (adjust->line_no != -1) {
                    int orig_sp = sp;
                    sp = adjust->label ? adjust->label->sp : 0;
                    if (orig_sp - sp > 0) {
                        if (orig_sp - sp > 1) code_index++; /* 1 operand */
                        code_index++; /* insn */
                        insn_num++;
                    }
                }
                break;
            }
          default: break;
        }
    }

    /* make instruction sequence */
    generated_iseq = ALLOC_N(VALUE, code_index);
    insns_info = ALLOC_N(struct iseq_insn_info_entry, insn_num);
    positions = ALLOC_N(unsigned int, insn_num);
    if (ISEQ_IS_SIZE(body)) {
        body->is_entries = ZALLOC_N(union iseq_inline_storage_entry, ISEQ_IS_SIZE(body));
    }
    else {
        body->is_entries = NULL;
    }

    if (body->ci_size) {
        body->call_data = ZALLOC_N(struct rb_call_data, body->ci_size);
    }
    else {
        body->call_data = NULL;
    }
    ISEQ_COMPILE_DATA(iseq)->ci_index = 0;

    // Calculate the bitmask buffer size.
    // Round the generated_iseq size up to the nearest multiple
    // of the number of bits in an unsigned long.

    // Allocate enough room for the bitmask list
    iseq_bits_t * mark_offset_bits;
    int code_size = code_index;

    bool needs_bitmap = false;

    if (ISEQ_MBITS_BUFLEN(code_index) == 1) {
        mark_offset_bits = &ISEQ_COMPILE_DATA(iseq)->mark_bits.single;
        ISEQ_COMPILE_DATA(iseq)->is_single_mark_bit = true;
    }
    else {
        mark_offset_bits = ZALLOC_N(iseq_bits_t, ISEQ_MBITS_BUFLEN(code_index));
        ISEQ_COMPILE_DATA(iseq)->mark_bits.list = mark_offset_bits;
        ISEQ_COMPILE_DATA(iseq)->is_single_mark_bit = false;
    }

    ISEQ_COMPILE_DATA(iseq)->iseq_encoded = (void *)generated_iseq;
    ISEQ_COMPILE_DATA(iseq)->iseq_size = code_index;

    list = FIRST_ELEMENT(anchor);
    insns_info_index = code_index = sp = 0;

    while (list) {
        switch (list->type) {
          case ISEQ_ELEMENT_INSN:
            {
                int j, len, insn;
                const char *types;
                VALUE *operands;
                INSN *iobj = (INSN *)list;

                /* update sp */
                sp = calc_sp_depth(sp, iobj);
                /* fprintf(stderr, "insn: %-16s, sp: %d\n", insn_name(iobj->insn_id), sp); */
                operands = iobj->operands;
                insn = iobj->insn_id;
                generated_iseq[code_index] = insn;
                types = insn_op_types(insn);
                len = insn_len(insn);

                for (j = 0; types[j]; j++) {
                    char type = types[j];

                    /* printf("--> [%c - (%d-%d)]\n", type, k, j); */
                    switch (type) {
                      case TS_OFFSET:
                        {
                            /* label(destination position) */
                            LABEL *lobj = (LABEL *)operands[j];
                            generated_iseq[code_index + 1 + j] = lobj->position - (code_index + len);
                            break;
                        }
                      case TS_CDHASH:
                        {
                            VALUE map = operands[j];
                            struct cdhash_set_label_struct data;
                            data.hash = map;
                            data.pos = code_index;
                            data.len = len;
                            rb_hash_foreach(map, cdhash_set_label_i, (VALUE)&data);

                            rb_hash_rehash(map);
                            freeze_hide_obj(map);
                            generated_iseq[code_index + 1 + j] = map;
                            ISEQ_MBITS_SET(mark_offset_bits, code_index + 1 + j);
                            RB_OBJ_WRITTEN(iseq, Qundef, map);
                            needs_bitmap = true;
                            break;
                        }
                      case TS_LINDEX:
                      case TS_NUM:	/* ulong */
                        generated_iseq[code_index + 1 + j] = FIX2INT(operands[j]);
                        break;
                      case TS_ISEQ:	/* iseq */
                      case TS_VALUE:	/* VALUE */
                        {
                            VALUE v = operands[j];
                            generated_iseq[code_index + 1 + j] = v;
                            /* to mark ruby object */
                            if (!SPECIAL_CONST_P(v)) {
                                RB_OBJ_WRITTEN(iseq, Qundef, v);
                                ISEQ_MBITS_SET(mark_offset_bits, code_index + 1 + j);
                                needs_bitmap = true;
                            }
                            break;
                        }
                      /* [ TS_IVC | TS_ICVARC | TS_ISE | TS_IC ] */
                      case TS_IC: /* inline cache: constants */
                        {
                            unsigned int ic_index = ISEQ_COMPILE_DATA(iseq)->ic_index++;
                            IC ic = &ISEQ_IS_ENTRY_START(body, type)[ic_index].ic_cache;
                            if (UNLIKELY(ic_index >= body->ic_size)) {
                                BADINSN_DUMP(anchor, &iobj->link, 0);
                                COMPILE_ERROR(iseq, iobj->insn_info.line_no,
                                              "iseq_set_sequence: ic_index overflow: index: %d, size: %d",
                                              ic_index, ISEQ_IS_SIZE(body));
                            }

                            ic->segments = array_to_idlist(operands[j]);

                            generated_iseq[code_index + 1 + j] = (VALUE)ic;
                        }
                        break;
                      case TS_IVC: /* inline ivar cache */
                        {
                            unsigned int ic_index = FIX2UINT(operands[j]);

                            IVC cache = ((IVC)&body->is_entries[ic_index]);

                            if (insn == BIN(setinstancevariable)) {
                                cache->iv_set_name = SYM2ID(operands[j - 1]);
                            }
                            else {
                                cache->iv_set_name = 0;
                            }

                            vm_ic_attr_index_initialize(cache, INVALID_SHAPE_ID);
                        }
                      case TS_ISE: /* inline storage entry: `once` insn */
                      case TS_ICVARC: /* inline cvar cache */
                        {
                            unsigned int ic_index = FIX2UINT(operands[j]);
                            IC ic = &ISEQ_IS_ENTRY_START(body, type)[ic_index].ic_cache;
                            if (UNLIKELY(ic_index >= ISEQ_IS_SIZE(body))) {
                                BADINSN_DUMP(anchor, &iobj->link, 0);
                                COMPILE_ERROR(iseq, iobj->insn_info.line_no,
                                              "iseq_set_sequence: ic_index overflow: index: %d, size: %d",
                                              ic_index, ISEQ_IS_SIZE(body));
                            }
                            generated_iseq[code_index + 1 + j] = (VALUE)ic;

                            break;
                        }
                      case TS_CALLDATA:
                        {
                            const struct rb_callinfo *source_ci = (const struct rb_callinfo *)operands[j];
                            RUBY_ASSERT(ISEQ_COMPILE_DATA(iseq)->ci_index <= body->ci_size);
                            struct rb_call_data *cd = &body->call_data[ISEQ_COMPILE_DATA(iseq)->ci_index++];
                            cd->ci = source_ci;
                            cd->cc = vm_cc_empty();
                            generated_iseq[code_index + 1 + j] = (VALUE)cd;
                            break;
                        }
                      case TS_ID: /* ID */
                        generated_iseq[code_index + 1 + j] = SYM2ID(operands[j]);
                        break;
                      case TS_FUNCPTR:
                        generated_iseq[code_index + 1 + j] = operands[j];
                        break;
                      case TS_BUILTIN:
                        generated_iseq[code_index + 1 + j] = operands[j];
                        break;
                      default:
                        BADINSN_ERROR(iseq, iobj->insn_info.line_no,
                                      "unknown operand type: %c", type);
                        return COMPILE_NG;
                    }
                }
                if (add_insn_info(insns_info, positions, insns_info_index, code_index, iobj)) insns_info_index++;
                code_index += len;
                break;
            }
          case ISEQ_ELEMENT_LABEL:
            {
                LABEL *lobj = (LABEL *)list;
                if (lobj->sp != sp) {
                    debugs("%s: sp inconsistency found but ignored (" LABEL_FORMAT " sp: %d, calculated sp: %d)\n",
                           RSTRING_PTR(rb_iseq_path(iseq)),
                           lobj->label_no, lobj->sp, sp);
                }
                sp = lobj->sp;
                break;
            }
          case ISEQ_ELEMENT_ADJUST:
            {
                ADJUST *adjust = (ADJUST *)list;
                int orig_sp = sp;

                if (adjust->label) {
                    sp = adjust->label->sp;
                }
                else {
                    sp = 0;
                }

                if (adjust->line_no != -1) {
                    const int diff = orig_sp - sp;
                    if (diff > 0) {
                        if (insns_info_index == 0) {
                            COMPILE_ERROR(iseq, adjust->line_no,
                                          "iseq_set_sequence: adjust bug (ISEQ_ELEMENT_ADJUST must not be the first in iseq)");
                        }
                        if (add_adjust_info(insns_info, positions, insns_info_index, code_index, adjust)) insns_info_index++;
                    }
                    if (diff > 1) {
                        generated_iseq[code_index++] = BIN(adjuststack);
                        generated_iseq[code_index++] = orig_sp - sp;
                    }
                    else if (diff == 1) {
                        generated_iseq[code_index++] = BIN(pop);
                    }
                    else if (diff < 0) {
                        int label_no = adjust->label ? adjust->label->label_no : -1;
                        xfree(generated_iseq);
                        xfree(insns_info);
                        xfree(positions);
                        if (ISEQ_MBITS_BUFLEN(code_size) > 1) {
                            xfree(mark_offset_bits);
                        }
                        debug_list(anchor, list);
                        COMPILE_ERROR(iseq, adjust->line_no,
                                      "iseq_set_sequence: adjust bug to %d %d < %d",
                                      label_no, orig_sp, sp);
                        return COMPILE_NG;
                    }
                }
                break;
            }
          default:
            /* ignore */
            break;
        }
        list = list->next;
    }

    body->iseq_encoded = (void *)generated_iseq;
    body->iseq_size = code_index;
    body->stack_max = stack_max;

    if (ISEQ_COMPILE_DATA(iseq)->is_single_mark_bit) {
        body->mark_bits.single = ISEQ_COMPILE_DATA(iseq)->mark_bits.single;
    }
    else {
        if (needs_bitmap) {
            body->mark_bits.list = mark_offset_bits;
        }
        else {
            body->mark_bits.list = NULL;
            ISEQ_COMPILE_DATA(iseq)->mark_bits.list = NULL;
            ruby_xfree(mark_offset_bits);
        }
    }

    /* get rid of memory leak when REALLOC failed */
    body->insns_info.body = insns_info;
    body->insns_info.positions = positions;

    REALLOC_N(insns_info, struct iseq_insn_info_entry, insns_info_index);
    body->insns_info.body = insns_info;
    REALLOC_N(positions, unsigned int, insns_info_index);
    body->insns_info.positions = positions;
    body->insns_info.size = insns_info_index;

    return COMPILE_OK;
}

static int
label_get_position(LABEL *lobj)
{
    return lobj->position;
}

static int
label_get_sp(LABEL *lobj)
{
    return lobj->sp;
}

static int
iseq_set_exception_table(rb_iseq_t *iseq)
{
    const VALUE *tptr, *ptr;
    unsigned int tlen, i;
    struct iseq_catch_table_entry *entry;

    ISEQ_BODY(iseq)->catch_table = NULL;

    VALUE catch_table_ary = ISEQ_COMPILE_DATA(iseq)->catch_table_ary;
    if (NIL_P(catch_table_ary)) return COMPILE_OK;
    tlen = (int)RARRAY_LEN(catch_table_ary);
    tptr = RARRAY_CONST_PTR(catch_table_ary);

    if (tlen > 0) {
        struct iseq_catch_table *table = xmalloc(iseq_catch_table_bytes(tlen));
        table->size = tlen;

        for (i = 0; i < table->size; i++) {
            int pos;
            ptr = RARRAY_CONST_PTR(tptr[i]);
            entry = UNALIGNED_MEMBER_PTR(table, entries[i]);
            entry->type = (enum rb_catch_type)(ptr[0] & 0xffff);
            pos = label_get_position((LABEL *)(ptr[1] & ~1));
            RUBY_ASSERT(pos >= 0);
            entry->start = (unsigned int)pos;
            pos = label_get_position((LABEL *)(ptr[2] & ~1));
            RUBY_ASSERT(pos >= 0);
            entry->end = (unsigned int)pos;
            entry->iseq = (rb_iseq_t *)ptr[3];
            RB_OBJ_WRITTEN(iseq, Qundef, entry->iseq);

            /* stack depth */
            if (ptr[4]) {
                LABEL *lobj = (LABEL *)(ptr[4] & ~1);
                entry->cont = label_get_position(lobj);
                entry->sp = label_get_sp(lobj);

                /* TODO: Dirty Hack!  Fix me */
                if (entry->type == CATCH_TYPE_RESCUE ||
                    entry->type == CATCH_TYPE_BREAK ||
                    entry->type == CATCH_TYPE_NEXT) {
                    RUBY_ASSERT(entry->sp > 0);
                    entry->sp--;
                }
            }
            else {
                entry->cont = 0;
            }
        }
        ISEQ_BODY(iseq)->catch_table = table;
        RB_OBJ_WRITE(iseq, &ISEQ_COMPILE_DATA(iseq)->catch_table_ary, 0); /* free */
    }

    RB_GC_GUARD(catch_table_ary);

    return COMPILE_OK;
}

/*
 * set optional argument table
 *   def foo(a, b=expr1, c=expr2)
 *   =>
 *    b:
 *      expr1
 *    c:
 *      expr2
 */
static int
iseq_set_optargs_table(rb_iseq_t *iseq)
{
    int i;
    VALUE *opt_table = (VALUE *)ISEQ_BODY(iseq)->param.opt_table;

    if (ISEQ_BODY(iseq)->param.flags.has_opt) {
        for (i = 0; i < ISEQ_BODY(iseq)->param.opt_num + 1; i++) {
            opt_table[i] = label_get_position((LABEL *)opt_table[i]);
        }
    }
    return COMPILE_OK;
}

static LINK_ELEMENT *
get_destination_insn(INSN *iobj)
{
    LABEL *lobj = (LABEL *)OPERAND_AT(iobj, 0);
    LINK_ELEMENT *list;
    rb_event_flag_t events = 0;

    list = lobj->link.next;
    while (list) {
        switch (list->type) {
          case ISEQ_ELEMENT_INSN:
          case ISEQ_ELEMENT_ADJUST:
            goto found;
          case ISEQ_ELEMENT_LABEL:
            /* ignore */
            break;
          case ISEQ_ELEMENT_TRACE:
            {
                TRACE *trace = (TRACE *)list;
                events |= trace->event;
            }
            break;
          default: break;
        }
        list = list->next;
    }
  found:
    if (list && IS_INSN(list)) {
        INSN *iobj = (INSN *)list;
        iobj->insn_info.events |= events;
    }
    return list;
}

static LINK_ELEMENT *
get_next_insn(INSN *iobj)
{
    LINK_ELEMENT *list = iobj->link.next;

    while (list) {
        if (IS_INSN(list) || IS_ADJUST(list)) {
            return list;
        }
        list = list->next;
    }
    return 0;
}

static LINK_ELEMENT *
get_prev_insn(INSN *iobj)
{
    LINK_ELEMENT *list = iobj->link.prev;

    while (list) {
        if (IS_INSN(list) || IS_ADJUST(list)) {
            return list;
        }
        list = list->prev;
    }
    return 0;
}

static void
unref_destination(INSN *iobj, int pos)
{
    LABEL *lobj = (LABEL *)OPERAND_AT(iobj, pos);
    --lobj->refcnt;
    if (!lobj->refcnt) ELEM_REMOVE(&lobj->link);
}

static bool
replace_destination(INSN *dobj, INSN *nobj)
{
    VALUE n = OPERAND_AT(nobj, 0);
    LABEL *dl = (LABEL *)OPERAND_AT(dobj, 0);
    LABEL *nl = (LABEL *)n;
    if (dl == nl) return false;
    --dl->refcnt;
    ++nl->refcnt;
    OPERAND_AT(dobj, 0) = n;
    if (!dl->refcnt) ELEM_REMOVE(&dl->link);
    return true;
}

static LABEL*
find_destination(INSN *i)
{
    int pos, len = insn_len(i->insn_id);
    for (pos = 0; pos < len; ++pos) {
        if (insn_op_types(i->insn_id)[pos] == TS_OFFSET) {
            return (LABEL *)OPERAND_AT(i, pos);
        }
    }
    return 0;
}

static int
remove_unreachable_chunk(rb_iseq_t *iseq, LINK_ELEMENT *i)
{
    LINK_ELEMENT *first = i, *end;
    int *unref_counts = 0, nlabels = ISEQ_COMPILE_DATA(iseq)->label_no;

    if (!i) return 0;
    unref_counts = ALLOCA_N(int, nlabels);
    MEMZERO(unref_counts, int, nlabels);
    end = i;
    do {
        LABEL *lab;
        if (IS_INSN(i)) {
            if (IS_INSN_ID(i, leave)) {
                end = i;
                break;
            }
            else if ((lab = find_destination((INSN *)i)) != 0) {
                unref_counts[lab->label_no]++;
            }
        }
        else if (IS_LABEL(i)) {
            lab = (LABEL *)i;
            if (lab->unremovable) return 0;
            if (lab->refcnt > unref_counts[lab->label_no]) {
                if (i == first) return 0;
                break;
            }
            continue;
        }
        else if (IS_TRACE(i)) {
            /* do nothing */
        }
        else if (IS_ADJUST(i)) {
            return 0;
        }
        end = i;
    } while ((i = i->next) != 0);
    i = first;
    do {
        if (IS_INSN(i)) {
            struct rb_iseq_constant_body *body = ISEQ_BODY(iseq);
            VALUE insn = INSN_OF(i);
            int pos, len = insn_len(insn);
            for (pos = 0; pos < len; ++pos) {
                switch (insn_op_types(insn)[pos]) {
                  case TS_OFFSET:
                    unref_destination((INSN *)i, pos);
                    break;
                  case TS_CALLDATA:
                    --(body->ci_size);
                    break;
                }
            }
        }
        ELEM_REMOVE(i);
    } while ((i != end) && (i = i->next) != 0);
    return 1;
}

static int
iseq_pop_newarray(rb_iseq_t *iseq, INSN *iobj)
{
    switch (OPERAND_AT(iobj, 0)) {
      case INT2FIX(0): /* empty array */
        ELEM_REMOVE(&iobj->link);
        return TRUE;
      case INT2FIX(1): /* single element array */
        ELEM_REMOVE(&iobj->link);
        return FALSE;
      default:
        iobj->insn_id = BIN(adjuststack);
        return TRUE;
    }
}

static int
is_frozen_putstring(INSN *insn, VALUE *op)
{
    if (IS_INSN_ID(insn, putstring) || IS_INSN_ID(insn, putchilledstring)) {
        *op = OPERAND_AT(insn, 0);
        return 1;
    }
    else if (IS_INSN_ID(insn, putobject)) { /* frozen_string_literal */
        *op = OPERAND_AT(insn, 0);
        return RB_TYPE_P(*op, T_STRING);
    }
    return 0;
}

static int
optimize_checktype(rb_iseq_t *iseq, INSN *iobj)
{
    /*
     *   putobject obj
     *   dup
     *   checktype T_XXX
     *   branchif l1
     * l2:
     *   ...
     * l1:
     *
     * => obj is a T_XXX
     *
     *   putobject obj (T_XXX)
     *   jump L1
     * L1:
     *
     * => obj is not a T_XXX
     *
     *   putobject obj (T_XXX)
     *   jump L2
     * L2:
     */
    int line, node_id;
    INSN *niobj, *ciobj, *dup = 0;
    LABEL *dest = 0;
    VALUE type;

    switch (INSN_OF(iobj)) {
      case BIN(putstring):
      case BIN(putchilledstring):
        type = INT2FIX(T_STRING);
        break;
      case BIN(putnil):
        type = INT2FIX(T_NIL);
        break;
      case BIN(putobject):
        type = INT2FIX(TYPE(OPERAND_AT(iobj, 0)));
        break;
      default: return FALSE;
    }

    ciobj = (INSN *)get_next_insn(iobj);
    if (IS_INSN_ID(ciobj, jump)) {
        ciobj = (INSN *)get_next_insn((INSN*)OPERAND_AT(ciobj, 0));
    }
    if (IS_INSN_ID(ciobj, dup)) {
        ciobj = (INSN *)get_next_insn(dup = ciobj);
    }
    if (!ciobj || !IS_INSN_ID(ciobj, checktype)) return FALSE;
    niobj = (INSN *)get_next_insn(ciobj);
    if (!niobj) {
        /* TODO: putobject true/false */
        return FALSE;
    }
    switch (INSN_OF(niobj)) {
      case BIN(branchif):
        if (OPERAND_AT(ciobj, 0) == type) {
            dest = (LABEL *)OPERAND_AT(niobj, 0);
        }
        break;
      case BIN(branchunless):
        if (OPERAND_AT(ciobj, 0) != type) {
            dest = (LABEL *)OPERAND_AT(niobj, 0);
        }
        break;
      default:
        return FALSE;
    }
    line = ciobj->insn_info.line_no;
    node_id = ciobj->insn_info.node_id;
    if (!dest) {
        if (niobj->link.next && IS_LABEL(niobj->link.next)) {
            dest = (LABEL *)niobj->link.next; /* reuse label */
        }
        else {
            dest = NEW_LABEL(line);
            ELEM_INSERT_NEXT(&niobj->link, &dest->link);
        }
    }
    INSERT_AFTER_INSN1(iobj, line, node_id, jump, dest);
    LABEL_REF(dest);
    if (!dup) INSERT_AFTER_INSN(iobj, line, node_id, pop);
    return TRUE;
}

static const struct rb_callinfo *
ci_flag_set(const rb_iseq_t *iseq, const struct rb_callinfo *ci, unsigned int add)
{
    const struct rb_callinfo *nci = vm_ci_new(vm_ci_mid(ci),
                                             vm_ci_flag(ci) | add,
                                             vm_ci_argc(ci),
                                             vm_ci_kwarg(ci));
    RB_OBJ_WRITTEN(iseq, ci, nci);
    return nci;
}

static const struct rb_callinfo *
ci_argc_set(const rb_iseq_t *iseq, const struct rb_callinfo *ci, int argc)
{
    const struct rb_callinfo *nci = vm_ci_new(vm_ci_mid(ci),
                                              vm_ci_flag(ci),
                                              argc,
                                              vm_ci_kwarg(ci));
    RB_OBJ_WRITTEN(iseq, ci, nci);
    return nci;
}

#define vm_ci_simple(ci) (vm_ci_flag(ci) & VM_CALL_ARGS_SIMPLE)

static int
iseq_peephole_optimize(rb_iseq_t *iseq, LINK_ELEMENT *list, const int do_tailcallopt)
{
    INSN *const iobj = (INSN *)list;

  again:
    optimize_checktype(iseq, iobj);

    if (IS_INSN_ID(iobj, jump)) {
        INSN *niobj, *diobj, *piobj;
        diobj = (INSN *)get_destination_insn(iobj);
        niobj = (INSN *)get_next_insn(iobj);

        if (diobj == niobj) {
            /*
             *   jump LABEL
             *  LABEL:
             * =>
             *   LABEL:
             */
            unref_destination(iobj, 0);
            ELEM_REMOVE(&iobj->link);
            return COMPILE_OK;
        }
        else if (iobj != diobj && IS_INSN(&diobj->link) &&
                 IS_INSN_ID(diobj, jump) &&
                 OPERAND_AT(iobj, 0) != OPERAND_AT(diobj, 0) &&
                 diobj->insn_info.events == 0) {
            /*
             *  useless jump elimination:
             *     jump LABEL1
             *     ...
             *   LABEL1:
             *     jump LABEL2
             *
             *   => in this case, first jump instruction should jump to
             *      LABEL2 directly
             */
            if (replace_destination(iobj, diobj)) {
                remove_unreachable_chunk(iseq, iobj->link.next);
                goto again;
            }
        }
        else if (IS_INSN_ID(diobj, leave)) {
            /*
             *  jump LABEL
             *  ...
             * LABEL:
             *  leave
             * =>
             *  leave
             *  ...
             * LABEL:
             *  leave
             */
            /* replace */
            unref_destination(iobj, 0);
            iobj->insn_id = BIN(leave);
            iobj->operand_size = 0;
            iobj->insn_info = diobj->insn_info;
            goto again;
        }
        else if (IS_INSN(iobj->link.prev) &&
                 (piobj = (INSN *)iobj->link.prev) &&
                 (IS_INSN_ID(piobj, branchif) ||
                  IS_INSN_ID(piobj, branchunless))) {
            INSN *pdiobj = (INSN *)get_destination_insn(piobj);
            if (niobj == pdiobj) {
                int refcnt = IS_LABEL(piobj->link.next) ?
                    ((LABEL *)piobj->link.next)->refcnt : 0;
                /*
                 * useless jump elimination (if/unless destination):
                 *   if   L1
                 *   jump L2
                 * L1:
                 *   ...
                 * L2:
                 *
                 * ==>
                 *   unless L2
                 * L1:
                 *   ...
                 * L2:
                 */
                piobj->insn_id = (IS_INSN_ID(piobj, branchif))
                  ? BIN(branchunless) : BIN(branchif);
                if (replace_destination(piobj, iobj) && refcnt <= 1) {
                    ELEM_REMOVE(&iobj->link);
                }
                else {
                    /* TODO: replace other branch destinations too */
                }
                return COMPILE_OK;
            }
            else if (diobj == pdiobj) {
                /*
                 * useless jump elimination (if/unless before jump):
                 * L1:
                 *   ...
                 *   if   L1
                 *   jump L1
                 *
                 * ==>
                 * L1:
                 *   ...
                 *   pop
                 *   jump L1
                 */
                INSN *popiobj = new_insn_core(iseq, iobj->insn_info.line_no, iobj->insn_info.node_id, BIN(pop), 0, 0);
                ELEM_REPLACE(&piobj->link, &popiobj->link);
            }
        }
        if (remove_unreachable_chunk(iseq, iobj->link.next)) {
            goto again;
        }
    }

    /*
     * putstring "beg"
     * putstring "end"
     * newrange excl
     *
     * ==>
     *
     * putobject "beg".."end"
     */
    if (IS_INSN_ID(iobj, newrange)) {
        INSN *const range = iobj;
        INSN *beg, *end;
        VALUE str_beg, str_end;

        if ((end = (INSN *)get_prev_insn(range)) != 0 &&
                is_frozen_putstring(end, &str_end) &&
                (beg = (INSN *)get_prev_insn(end)) != 0 &&
                is_frozen_putstring(beg, &str_beg)) {
            int excl = FIX2INT(OPERAND_AT(range, 0));
            VALUE lit_range = rb_range_new(str_beg, str_end, excl);

            ELEM_REMOVE(&beg->link);
            ELEM_REMOVE(&end->link);
            range->insn_id = BIN(putobject);
            OPERAND_AT(range, 0) = lit_range;
            RB_OBJ_WRITTEN(iseq, Qundef, lit_range);
        }
    }

    if (IS_INSN_ID(iobj, leave)) {
        remove_unreachable_chunk(iseq, iobj->link.next);
    }

    /*
     *  ...
     *  duparray [...]
     *  concatarray | concattoarray
     * =>
     *  ...
     *  putobject [...]
     *  concatarray | concattoarray
     */
    if (IS_INSN_ID(iobj, duparray)) {
        LINK_ELEMENT *next = iobj->link.next;
        if (IS_INSN(next) && (IS_INSN_ID(next, concatarray) || IS_INSN_ID(next, concattoarray))) {
            iobj->insn_id = BIN(putobject);
        }
    }

    /*
     * duparray [...]
     * send     <calldata!mid:freeze, argc:0, ARGS_SIMPLE>, nil
     * =>
     * opt_ary_freeze [...], <calldata!mid:freeze, argc:0, ARGS_SIMPLE>
     */
    if (IS_INSN_ID(iobj, duparray)) {
        LINK_ELEMENT *next = iobj->link.next;
        if (IS_INSN(next) && (IS_INSN_ID(next, send))) {
            const struct rb_callinfo *ci = (struct rb_callinfo *)OPERAND_AT(next, 0);
            const rb_iseq_t *blockiseq = (rb_iseq_t *)OPERAND_AT(next, 1);

            if (vm_ci_simple(ci) && vm_ci_argc(ci) == 0 && blockiseq == NULL && vm_ci_mid(ci) == idFreeze) {
                VALUE ary = iobj->operands[0];
                rb_obj_reveal(ary, rb_cArray);

                insn_replace_with_operands(iseq, iobj, BIN(opt_ary_freeze), 2, ary, (VALUE)ci);
                ELEM_REMOVE(next);
            }
        }
    }

    /*
     * duphash {...}
     * send     <calldata!mid:freeze, argc:0, ARGS_SIMPLE>, nil
     * =>
     * opt_hash_freeze {...}, <calldata!mid:freeze, argc:0, ARGS_SIMPLE>
     */
    if (IS_INSN_ID(iobj, duphash)) {
        LINK_ELEMENT *next = iobj->link.next;
        if (IS_INSN(next) && (IS_INSN_ID(next, send))) {
            const struct rb_callinfo *ci = (struct rb_callinfo *)OPERAND_AT(next, 0);
            const rb_iseq_t *blockiseq = (rb_iseq_t *)OPERAND_AT(next, 1);

            if (vm_ci_simple(ci) && vm_ci_argc(ci) == 0 && blockiseq == NULL && vm_ci_mid(ci) == idFreeze) {
                VALUE hash = iobj->operands[0];
                rb_obj_reveal(hash, rb_cHash);

                insn_replace_with_operands(iseq, iobj, BIN(opt_hash_freeze), 2, hash, (VALUE)ci);
                ELEM_REMOVE(next);
            }
        }
    }

    /*
     * newarray 0
     * send     <calldata!mid:freeze, argc:0, ARGS_SIMPLE>, nil
     * =>
     * opt_ary_freeze [], <calldata!mid:freeze, argc:0, ARGS_SIMPLE>
     */
    if (IS_INSN_ID(iobj, newarray) && iobj->operands[0] == INT2FIX(0)) {
        LINK_ELEMENT *next = iobj->link.next;
        if (IS_INSN(next) && (IS_INSN_ID(next, send))) {
            const struct rb_callinfo *ci = (struct rb_callinfo *)OPERAND_AT(next, 0);
            const rb_iseq_t *blockiseq = (rb_iseq_t *)OPERAND_AT(next, 1);

            if (vm_ci_simple(ci) && vm_ci_argc(ci) == 0 && blockiseq == NULL && vm_ci_mid(ci) == idFreeze) {
                insn_replace_with_operands(iseq, iobj, BIN(opt_ary_freeze), 2, rb_cArray_empty_frozen, (VALUE)ci);
                ELEM_REMOVE(next);
            }
        }
    }

    /*
     * newhash 0
     * send     <calldata!mid:freeze, argc:0, ARGS_SIMPLE>, nil
     * =>
     * opt_hash_freeze {}, <calldata!mid:freeze, argc:0, ARGS_SIMPLE>
     */
    if (IS_INSN_ID(iobj, newhash) && iobj->operands[0] == INT2FIX(0)) {
        LINK_ELEMENT *next = iobj->link.next;
        if (IS_INSN(next) && (IS_INSN_ID(next, send))) {
            const struct rb_callinfo *ci = (struct rb_callinfo *)OPERAND_AT(next, 0);
            const rb_iseq_t *blockiseq = (rb_iseq_t *)OPERAND_AT(next, 1);

            if (vm_ci_simple(ci) && vm_ci_argc(ci) == 0 && blockiseq == NULL && vm_ci_mid(ci) == idFreeze) {
                insn_replace_with_operands(iseq, iobj, BIN(opt_hash_freeze), 2, rb_cHash_empty_frozen, (VALUE)ci);
                ELEM_REMOVE(next);
            }
        }
    }

    if (IS_INSN_ID(iobj, branchif) ||
        IS_INSN_ID(iobj, branchnil) ||
        IS_INSN_ID(iobj, branchunless)) {
        /*
         *   if L1
         *   ...
         * L1:
         *   jump L2
         * =>
         *   if L2
         */
        INSN *nobj = (INSN *)get_destination_insn(iobj);

        /* This is super nasty hack!!!
         *
         * This jump-jump optimization may ignore event flags of the jump
         * instruction being skipped.  Actually, Line 2 TracePoint event
         * is never fired in the following code:
         *
         *   1: raise if 1 == 2
         *   2: while true
         *   3:   break
         *   4: end
         *
         * This is critical for coverage measurement.  [Bug #15980]
         *
         * This is a stopgap measure: stop the jump-jump optimization if
         * coverage measurement is enabled and if the skipped instruction
         * has any event flag.
         *
         * Note that, still, TracePoint Line event does not occur on Line 2.
         * This should be fixed in future.
         */
        int stop_optimization =
            ISEQ_COVERAGE(iseq) && ISEQ_LINE_COVERAGE(iseq) &&
            nobj->link.type == ISEQ_ELEMENT_INSN &&
            nobj->insn_info.events;
        if (!stop_optimization) {
            INSN *pobj = (INSN *)iobj->link.prev;
            int prev_dup = 0;
            if (pobj) {
                if (!IS_INSN(&pobj->link))
                    pobj = 0;
                else if (IS_INSN_ID(pobj, dup))
                    prev_dup = 1;
            }

            for (;;) {
                if (IS_INSN(&nobj->link) && IS_INSN_ID(nobj, jump)) {
                    if (!replace_destination(iobj, nobj)) break;
                }
                else if (prev_dup && IS_INSN_ID(nobj, dup) &&
                         !!(nobj = (INSN *)nobj->link.next) &&
                         /* basic blocks, with no labels in the middle */
                         nobj->insn_id == iobj->insn_id) {
                    /*
                     *   dup
                     *   if L1
                     *   ...
                     * L1:
                     *   dup
                     *   if L2
                     * =>
                     *   dup
                     *   if L2
                     *   ...
                     * L1:
                     *   dup
                     *   if L2
                     */
                    if (!replace_destination(iobj, nobj)) break;
                }
                else if (pobj) {
                    /*
                     *   putnil
                     *   if L1
                     * =>
                     *   # nothing
                     *
                     *   putobject true
                     *   if L1
                     * =>
                     *   jump L1
                     *
                     *   putstring ".."
                     *   if L1
                     * =>
                     *   jump L1
                     *
                     *   putstring ".."
                     *   dup
                     *   if L1
                     * =>
                     *   putstring ".."
                     *   jump L1
                     *
                     */
                    int cond;
                    if (prev_dup && IS_INSN(pobj->link.prev)) {
                        pobj = (INSN *)pobj->link.prev;
                    }
                    if (IS_INSN_ID(pobj, putobject)) {
                        cond = (IS_INSN_ID(iobj, branchif) ?
                                OPERAND_AT(pobj, 0) != Qfalse :
                                IS_INSN_ID(iobj, branchunless) ?
                                OPERAND_AT(pobj, 0) == Qfalse :
                                FALSE);
                    }
                    else if (IS_INSN_ID(pobj, putstring) ||
                             IS_INSN_ID(pobj, duparray) ||
                             IS_INSN_ID(pobj, newarray)) {
                        cond = IS_INSN_ID(iobj, branchif);
                    }
                    else if (IS_INSN_ID(pobj, putnil)) {
                        cond = !IS_INSN_ID(iobj, branchif);
                    }
                    else break;
                    if (prev_dup || !IS_INSN_ID(pobj, newarray)) {
                        ELEM_REMOVE(iobj->link.prev);
                    }
                    else if (!iseq_pop_newarray(iseq, pobj)) {
                        pobj = new_insn_core(iseq, pobj->insn_info.line_no, pobj->insn_info.node_id, BIN(pop), 0, NULL);
                        ELEM_INSERT_PREV(&iobj->link, &pobj->link);
                    }
                    if (cond) {
                        if (prev_dup) {
                            pobj = new_insn_core(iseq, pobj->insn_info.line_no, pobj->insn_info.node_id, BIN(putnil), 0, NULL);
                            ELEM_INSERT_NEXT(&iobj->link, &pobj->link);
                        }
                        iobj->insn_id = BIN(jump);
                        goto again;
                    }
                    else {
                        unref_destination(iobj, 0);
                        ELEM_REMOVE(&iobj->link);
                    }
                    break;
                }
                else break;
                nobj = (INSN *)get_destination_insn(nobj);
            }
        }
    }

    if (IS_INSN_ID(iobj, pop)) {
        /*
         *  putself / putnil / putobject obj / putstring "..."
         *  pop
         * =>
         *  # do nothing
         */
        LINK_ELEMENT *prev = iobj->link.prev;
        if (IS_INSN(prev)) {
            enum ruby_vminsn_type previ = ((INSN *)prev)->insn_id;
            if (previ == BIN(putobject) || previ == BIN(putnil) ||
                previ == BIN(putself) || previ == BIN(putstring) ||
                previ == BIN(putchilledstring) ||
                previ == BIN(dup) ||
                previ == BIN(getlocal) ||
                previ == BIN(getblockparam) ||
                previ == BIN(getblockparamproxy) ||
                previ == BIN(getinstancevariable) ||
                previ == BIN(duparray)) {
                /* just push operand or static value and pop soon, no
                 * side effects */
                ELEM_REMOVE(prev);
                ELEM_REMOVE(&iobj->link);
            }
            else if (previ == BIN(newarray) && iseq_pop_newarray(iseq, (INSN*)prev)) {
                ELEM_REMOVE(&iobj->link);
            }
            else if (previ == BIN(concatarray)) {
                INSN *piobj = (INSN *)prev;
                INSERT_BEFORE_INSN1(piobj, piobj->insn_info.line_no, piobj->insn_info.node_id, splatarray, Qfalse);
                INSN_OF(piobj) = BIN(pop);
            }
            else if (previ == BIN(concatstrings)) {
                if (OPERAND_AT(prev, 0) == INT2FIX(1)) {
                    ELEM_REMOVE(prev);
                }
                else {
                    ELEM_REMOVE(&iobj->link);
                    INSN_OF(prev) = BIN(adjuststack);
                }
            }
        }
    }

    if (IS_INSN_ID(iobj, newarray) ||
        IS_INSN_ID(iobj, duparray) ||
        IS_INSN_ID(iobj, concatarray) ||
        IS_INSN_ID(iobj, splatarray) ||
        0) {
        /*
         *  newarray N
         *  splatarray
         * =>
         *  newarray N
         * newarray always puts an array
         */
        LINK_ELEMENT *next = iobj->link.next;
        if (IS_INSN(next) && IS_INSN_ID(next, splatarray)) {
            /* remove splatarray following always-array insn */
            ELEM_REMOVE(next);
        }
    }

    if (IS_INSN_ID(iobj, newarray)) {
        LINK_ELEMENT *next = iobj->link.next;
        if (IS_INSN(next) && IS_INSN_ID(next, expandarray) &&
            OPERAND_AT(next, 1) == INT2FIX(0)) {
            VALUE op1, op2;
            op1 = OPERAND_AT(iobj, 0);
            op2 = OPERAND_AT(next, 0);
            ELEM_REMOVE(next);

            if (op1 == op2) {
                /*
                 *  newarray 2
                 *  expandarray 2, 0
                 * =>
                 *  swap
                 */
                if (op1 == INT2FIX(2)) {
                    INSN_OF(iobj) = BIN(swap);
                    iobj->operand_size = 0;
                }
                /*
                 *  newarray X
                 *  expandarray X, 0
                 * =>
                 *  opt_reverse X
                 */
                else {
                    INSN_OF(iobj) = BIN(opt_reverse);
                }
            }
            else {
                long diff = FIX2LONG(op1) - FIX2LONG(op2);
                INSN_OF(iobj) = BIN(opt_reverse);
                OPERAND_AT(iobj, 0) = OPERAND_AT(next, 0);

                if (op1 > op2) {
                    /* X > Y
                     *  newarray X
                     *  expandarray Y, 0
                     * =>
                     *  pop * (Y-X)
                     *  opt_reverse Y
                     */
                    for (; diff > 0; diff--) {
                        INSERT_BEFORE_INSN(iobj, iobj->insn_info.line_no, iobj->insn_info.node_id, pop);
                    }
                }
                else { /* (op1 < op2) */
                    /* X < Y
                     *  newarray X
                     *  expandarray Y, 0
                     * =>
                     *  putnil * (Y-X)
                     *  opt_reverse Y
                     */
                    for (; diff < 0; diff++) {
                        INSERT_BEFORE_INSN(iobj, iobj->insn_info.line_no, iobj->insn_info.node_id, putnil);
                    }
                }
            }
        }
    }

    if (IS_INSN_ID(iobj, duparray)) {
        LINK_ELEMENT *next = iobj->link.next;
        /*
         *  duparray obj
         *  expandarray X, 0
         * =>
         *  putobject obj
         *  expandarray X, 0
         */
        if (IS_INSN(next) && IS_INSN_ID(next, expandarray)) {
            INSN_OF(iobj) = BIN(putobject);
        }
    }

    if (IS_INSN_ID(iobj, anytostring)) {
        LINK_ELEMENT *next = iobj->link.next;
        /*
         *  anytostring
         *  concatstrings 1
         * =>
         *  anytostring
         */
        if (IS_INSN(next) && IS_INSN_ID(next, concatstrings) &&
            OPERAND_AT(next, 0) == INT2FIX(1)) {
            ELEM_REMOVE(next);
        }
    }

    if (IS_INSN_ID(iobj, putstring) || IS_INSN_ID(iobj, putchilledstring) ||
        (IS_INSN_ID(iobj, putobject) && RB_TYPE_P(OPERAND_AT(iobj, 0), T_STRING))) {
        /*
         *  putstring ""
         *  concatstrings N
         * =>
         *  concatstrings N-1
         */
        if (IS_NEXT_INSN_ID(&iobj->link, concatstrings) &&
            RSTRING_LEN(OPERAND_AT(iobj, 0)) == 0) {
            INSN *next = (INSN *)iobj->link.next;
            if ((OPERAND_AT(next, 0) = FIXNUM_INC(OPERAND_AT(next, 0), -1)) == INT2FIX(1)) {
                ELEM_REMOVE(&next->link);
            }
            ELEM_REMOVE(&iobj->link);
        }
        if (IS_NEXT_INSN_ID(&iobj->link, toregexp)) {
            INSN *next = (INSN *)iobj->link.next;
            if (OPERAND_AT(next, 1) == INT2FIX(1)) {
                VALUE src = OPERAND_AT(iobj, 0);
                int opt = (int)FIX2LONG(OPERAND_AT(next, 0));
                VALUE path = rb_iseq_path(iseq);
                int line = iobj->insn_info.line_no;
                VALUE errinfo = rb_errinfo();
                VALUE re = rb_reg_compile(src, opt, RSTRING_PTR(path), line);
                if (NIL_P(re)) {
                    VALUE message = rb_attr_get(rb_errinfo(), idMesg);
                    rb_set_errinfo(errinfo);
                    COMPILE_ERROR(iseq, line, "%" PRIsVALUE, message);
                }
                RB_OBJ_WRITE(iseq, &OPERAND_AT(iobj, 0), re);
                ELEM_REMOVE(iobj->link.next);
            }
        }
    }

    if (IS_INSN_ID(iobj, concatstrings)) {
        /*
         *  concatstrings N
         *  concatstrings M
         * =>
         *  concatstrings N+M-1
         */
        LINK_ELEMENT *next = iobj->link.next;
        INSN *jump = 0;
        if (IS_INSN(next) && IS_INSN_ID(next, jump))
            next = get_destination_insn(jump = (INSN *)next);
        if (IS_INSN(next) && IS_INSN_ID(next, concatstrings)) {
            int n = FIX2INT(OPERAND_AT(iobj, 0)) + FIX2INT(OPERAND_AT(next, 0)) - 1;
            OPERAND_AT(iobj, 0) = INT2FIX(n);
            if (jump) {
                LABEL *label = ((LABEL *)OPERAND_AT(jump, 0));
                if (!--label->refcnt) {
                    ELEM_REMOVE(&label->link);
                }
                else {
                    label = NEW_LABEL(0);
                    OPERAND_AT(jump, 0) = (VALUE)label;
                }
                label->refcnt++;
                ELEM_INSERT_NEXT(next, &label->link);
                CHECK(iseq_peephole_optimize(iseq, get_next_insn(jump), do_tailcallopt));
            }
            else {
                ELEM_REMOVE(next);
            }
        }
    }

    if (do_tailcallopt &&
        (IS_INSN_ID(iobj, send) ||
         IS_INSN_ID(iobj, opt_aref_with) ||
         IS_INSN_ID(iobj, opt_aset_with) ||
         IS_INSN_ID(iobj, invokesuper))) {
        /*
         *  send ...
         *  leave
         * =>
         *  send ..., ... | VM_CALL_TAILCALL, ...
         *  leave # unreachable
         */
        INSN *piobj = NULL;
        if (iobj->link.next) {
            LINK_ELEMENT *next = iobj->link.next;
            do {
                if (!IS_INSN(next)) {
                    next = next->next;
                    continue;
                }
                switch (INSN_OF(next)) {
                  case BIN(nop):
                    next = next->next;
                    break;
                  case BIN(jump):
                    /* if cond
                     *   return tailcall
                     * end
                     */
                    next = get_destination_insn((INSN *)next);
                    break;
                  case BIN(leave):
                    piobj = iobj;
                    /* fall through */
                  default:
                    next = NULL;
                    break;
                }
            } while (next);
        }

        if (piobj) {
            const struct rb_callinfo *ci = (struct rb_callinfo *)OPERAND_AT(piobj, 0);
            if (IS_INSN_ID(piobj, send) ||
                IS_INSN_ID(piobj, invokesuper)) {
                if (OPERAND_AT(piobj, 1) == 0) { /* no blockiseq */
                    ci = ci_flag_set(iseq, ci, VM_CALL_TAILCALL);
                    OPERAND_AT(piobj, 0) = (VALUE)ci;
                    RB_OBJ_WRITTEN(iseq, Qundef, ci);
                }
            }
            else {
                ci = ci_flag_set(iseq, ci, VM_CALL_TAILCALL);
                OPERAND_AT(piobj, 0) = (VALUE)ci;
                RB_OBJ_WRITTEN(iseq, Qundef, ci);
            }
        }
    }

    if (IS_INSN_ID(iobj, dup)) {
        if (IS_NEXT_INSN_ID(&iobj->link, setlocal)) {
            LINK_ELEMENT *set1 = iobj->link.next, *set2 = NULL;

            /*
            *  dup
            *  setlocal x, y
            *  setlocal x, y
            * =>
            *  dup
            *  setlocal x, y
            */
            if (IS_NEXT_INSN_ID(set1, setlocal)) {
                set2 = set1->next;
                if (OPERAND_AT(set1, 0) == OPERAND_AT(set2, 0) &&
                    OPERAND_AT(set1, 1) == OPERAND_AT(set2, 1)) {
                    ELEM_REMOVE(set1);
                    ELEM_REMOVE(&iobj->link);
                }
            }

            /*
            *  dup
            *  setlocal x, y
            *  dup
            *  setlocal x, y
            * =>
            *  dup
            *  setlocal x, y
            */
            else if (IS_NEXT_INSN_ID(set1, dup) &&
                     IS_NEXT_INSN_ID(set1->next, setlocal)) {
                set2 = set1->next->next;
                if (OPERAND_AT(set1, 0) == OPERAND_AT(set2, 0) &&
                    OPERAND_AT(set1, 1) == OPERAND_AT(set2, 1)) {
                    ELEM_REMOVE(set1->next);
                    ELEM_REMOVE(set2);
                }
            }
        }
    }

    /*
    *  getlocal x, y
    *  dup
    *  setlocal x, y
    * =>
    *  dup
    */
    if (IS_INSN_ID(iobj, getlocal)) {
        LINK_ELEMENT *niobj = &iobj->link;
        if (IS_NEXT_INSN_ID(niobj, dup)) {
            niobj = niobj->next;
        }
        if (IS_NEXT_INSN_ID(niobj, setlocal)) {
            LINK_ELEMENT *set1 = niobj->next;
            if (OPERAND_AT(iobj, 0) == OPERAND_AT(set1, 0) &&
                OPERAND_AT(iobj, 1) == OPERAND_AT(set1, 1)) {
                ELEM_REMOVE(set1);
                ELEM_REMOVE(niobj);
            }
        }
    }

    /*
    *  opt_invokebuiltin_delegate
    *  trace
    *  leave
    * =>
    *  opt_invokebuiltin_delegate_leave
    *  trace
    *  leave
    */
    if (IS_INSN_ID(iobj, opt_invokebuiltin_delegate)) {
        if (IS_TRACE(iobj->link.next)) {
            if (IS_NEXT_INSN_ID(iobj->link.next, leave)) {
                iobj->insn_id = BIN(opt_invokebuiltin_delegate_leave);
                const struct rb_builtin_function *bf = (const struct rb_builtin_function *)iobj->operands[0];
                if (iobj == (INSN *)list && bf->argc == 0 && (ISEQ_BODY(iseq)->builtin_attrs & BUILTIN_ATTR_LEAF)) {
                    ISEQ_BODY(iseq)->builtin_attrs |= BUILTIN_ATTR_SINGLE_NOARG_LEAF;
                }
            }
        }
    }

    /*
    *  getblockparam
    *  branchif / branchunless
    * =>
    *  getblockparamproxy
    *  branchif / branchunless
    */
    if (IS_INSN_ID(iobj, getblockparam)) {
        if (IS_NEXT_INSN_ID(&iobj->link, branchif) || IS_NEXT_INSN_ID(&iobj->link, branchunless)) {
            iobj->insn_id = BIN(getblockparamproxy);
        }
    }

    if (IS_INSN_ID(iobj, splatarray) && OPERAND_AT(iobj, 0) == false) {
        LINK_ELEMENT *niobj = &iobj->link;
        if (IS_NEXT_INSN_ID(niobj, duphash)) {
            niobj = niobj->next;
            LINK_ELEMENT *siobj;
            unsigned int set_flags = 0, unset_flags = 0;

            /*
            * Eliminate hash allocation for f(*a, kw: 1)
            *
            *  splatarray false
            *  duphash
            *  send ARGS_SPLAT|KW_SPLAT|KW_SPLAT_MUT and not ARGS_BLOCKARG
            * =>
            *  splatarray false
            *  putobject
            *  send ARGS_SPLAT|KW_SPLAT
            */
            if (IS_NEXT_INSN_ID(niobj, send)) {
                siobj = niobj->next;
                set_flags = VM_CALL_ARGS_SPLAT|VM_CALL_KW_SPLAT|VM_CALL_KW_SPLAT_MUT;
                unset_flags = VM_CALL_ARGS_BLOCKARG;
            }
            /*
            * Eliminate hash allocation for f(*a, kw: 1, &{arg,lvar,@iv})
            *
            *  splatarray false
            *  duphash
            *  getlocal / getinstancevariable / getblockparamproxy
            *  send ARGS_SPLAT|KW_SPLAT|KW_SPLAT_MUT|ARGS_BLOCKARG
            * =>
            *  splatarray false
            *  putobject
            *  getlocal / getinstancevariable / getblockparamproxy
            *  send ARGS_SPLAT|KW_SPLAT|ARGS_BLOCKARG
            */
            else if ((IS_NEXT_INSN_ID(niobj, getlocal) || IS_NEXT_INSN_ID(niobj, getinstancevariable) ||
                        IS_NEXT_INSN_ID(niobj, getblockparamproxy)) && (IS_NEXT_INSN_ID(niobj->next, send))) {
                siobj = niobj->next->next;
                set_flags = VM_CALL_ARGS_SPLAT|VM_CALL_KW_SPLAT|VM_CALL_KW_SPLAT_MUT|VM_CALL_ARGS_BLOCKARG;
            }

            if (set_flags) {
                const struct rb_callinfo *ci = (const struct rb_callinfo *)OPERAND_AT(siobj, 0);
                unsigned int flags = vm_ci_flag(ci);
                if ((flags & set_flags) == set_flags && !(flags & unset_flags)) {
                    ((INSN*)niobj)->insn_id = BIN(putobject);
                    RB_OBJ_WRITE(iseq, &OPERAND_AT(niobj, 0), rb_hash_freeze(rb_hash_resurrect(OPERAND_AT(niobj, 0))));

                    const struct rb_callinfo *nci = vm_ci_new(vm_ci_mid(ci),
                        flags & ~VM_CALL_KW_SPLAT_MUT, vm_ci_argc(ci), vm_ci_kwarg(ci));
                    RB_OBJ_WRITTEN(iseq, ci, nci);
                    OPERAND_AT(siobj, 0) = (VALUE)nci;
                }
            }
        }
    }

    return COMPILE_OK;
}

static int
insn_set_specialized_instruction(rb_iseq_t *iseq, INSN *iobj, int insn_id)
{
    if (insn_id == BIN(opt_neq)) {
        VALUE original_ci = iobj->operands[0];
        VALUE new_ci = (VALUE)new_callinfo(iseq, idEq, 1, 0, NULL, FALSE);
        insn_replace_with_operands(iseq, iobj, insn_id, 2, new_ci, original_ci);
    }
    else {
        iobj->insn_id = insn_id;
        iobj->operand_size = insn_len(insn_id) - 1;
    }
    iobj->insn_info.events |= RUBY_EVENT_C_CALL | RUBY_EVENT_C_RETURN;

    return COMPILE_OK;
}

static int
iseq_specialized_instruction(rb_iseq_t *iseq, INSN *iobj)
{
    if (IS_INSN_ID(iobj, newarray) && iobj->link.next &&
        IS_INSN(iobj->link.next)) {
        /*
         *   [a, b, ...].max/min -> a, b, c, opt_newarray_send max/min
         */
        INSN *niobj = (INSN *)iobj->link.next;
        if (IS_INSN_ID(niobj, send)) {
            const struct rb_callinfo *ci = (struct rb_callinfo *)OPERAND_AT(niobj, 0);
            if (vm_ci_simple(ci) && vm_ci_argc(ci) == 0) {
                VALUE method = INT2FIX(0);
                switch (vm_ci_mid(ci)) {
                  case idMax:
                      method = INT2FIX(VM_OPT_NEWARRAY_SEND_MAX);
                      break;
                  case idMin:
                      method = INT2FIX(VM_OPT_NEWARRAY_SEND_MIN);
                      break;
                  case idHash:
                      method = INT2FIX(VM_OPT_NEWARRAY_SEND_HASH);
                      break;
                }

                if (method != INT2FIX(0)) {
                    VALUE num = iobj->operands[0];
                    insn_replace_with_operands(iseq, iobj, BIN(opt_newarray_send), 2, num, method);
                    ELEM_REMOVE(&niobj->link);
                    return COMPILE_OK;
                }
            }
        }
        else if ((IS_INSN_ID(niobj, putstring) || IS_INSN_ID(niobj, putchilledstring) ||
                  (IS_INSN_ID(niobj, putobject) && RB_TYPE_P(OPERAND_AT(niobj, 0), T_STRING))) &&
                 IS_NEXT_INSN_ID(&niobj->link, send)) {
            const struct rb_callinfo *ci = (struct rb_callinfo *)OPERAND_AT((INSN *)niobj->link.next, 0);
            if (vm_ci_simple(ci) && vm_ci_argc(ci) == 1 && vm_ci_mid(ci) == idPack) {
                VALUE num = iobj->operands[0];
                insn_replace_with_operands(iseq, iobj, BIN(opt_newarray_send), 2, FIXNUM_INC(num, 1), INT2FIX(VM_OPT_NEWARRAY_SEND_PACK));
                ELEM_REMOVE(&iobj->link);
                ELEM_REMOVE(niobj->link.next);
                ELEM_INSERT_NEXT(&niobj->link, &iobj->link);
                return COMPILE_OK;
            }
        }
        // newarray n, putchilledstring "E", getlocal b, send :pack with {buffer: b}
        // -> putchilledstring "E", getlocal b, opt_newarray_send n+2, :pack, :buffer
        else if ((IS_INSN_ID(niobj, putstring) || IS_INSN_ID(niobj, putchilledstring) ||
                  (IS_INSN_ID(niobj, putobject) && RB_TYPE_P(OPERAND_AT(niobj, 0), T_STRING))) &&
                 IS_NEXT_INSN_ID(&niobj->link, getlocal) &&
                 (niobj->link.next && IS_NEXT_INSN_ID(niobj->link.next, send))) {
            const struct rb_callinfo *ci = (struct rb_callinfo *)OPERAND_AT((INSN *)(niobj->link.next)->next, 0);
            const struct rb_callinfo_kwarg *kwarg = vm_ci_kwarg(ci);
            if (vm_ci_mid(ci) == idPack && vm_ci_argc(ci) == 2 &&
                    (kwarg && kwarg->keyword_len == 1 && kwarg->keywords[0] == rb_id2sym(idBuffer))) {
                VALUE num = iobj->operands[0];
                insn_replace_with_operands(iseq, iobj, BIN(opt_newarray_send), 2, FIXNUM_INC(num, 2), INT2FIX(VM_OPT_NEWARRAY_SEND_PACK_BUFFER));
                // Remove the "send" insn.
                ELEM_REMOVE((niobj->link.next)->next);
                // Remove the modified insn from its original "newarray" position...
                ELEM_REMOVE(&iobj->link);
                // and insert it after the buffer insn.
                ELEM_INSERT_NEXT(niobj->link.next, &iobj->link);
                return COMPILE_OK;
            }
        }

        // Break the "else if" chain since some prior checks abort after sub-ifs.
        // We already found "newarray".  To match `[...].include?(arg)` we look for
        // the instruction(s) representing the argument followed by a "send".
        if ((IS_INSN_ID(niobj, putstring) || IS_INSN_ID(niobj, putchilledstring) ||
                  IS_INSN_ID(niobj, putobject) ||
                  IS_INSN_ID(niobj, putself) ||
                  IS_INSN_ID(niobj, getlocal) ||
                  IS_INSN_ID(niobj, getinstancevariable)) &&
                 IS_NEXT_INSN_ID(&niobj->link, send)) {

            LINK_ELEMENT *sendobj = &(niobj->link); // Below we call ->next;
            const struct rb_callinfo *ci;
            // Allow any number (0 or more) of simple method calls on the argument
            // (as in `[...].include?(arg.method1.method2)`.
            do {
                sendobj = sendobj->next;
                ci = (struct rb_callinfo *)OPERAND_AT(sendobj, 0);
            } while (vm_ci_simple(ci) && vm_ci_argc(ci) == 0 && IS_NEXT_INSN_ID(sendobj, send));

            // If this send is for .include? with one arg we can do our opt.
            if (vm_ci_simple(ci) && vm_ci_argc(ci) == 1 && vm_ci_mid(ci) == idIncludeP) {
                VALUE num = iobj->operands[0];
                INSN *sendins = (INSN *)sendobj;
                insn_replace_with_operands(iseq, sendins, BIN(opt_newarray_send), 2, FIXNUM_INC(num, 1), INT2FIX(VM_OPT_NEWARRAY_SEND_INCLUDE_P));
                // Remove the original "newarray" insn.
                ELEM_REMOVE(&iobj->link);
                return COMPILE_OK;
            }
        }
    }

    /*
     * duparray [...]
     * some insn for the arg...
     * send     <calldata!mid:include?, argc:1, ARGS_SIMPLE>, nil
     * =>
     * arg insn...
     * opt_duparray_send [...], :include?, 1
     */
    if (IS_INSN_ID(iobj, duparray) && iobj->link.next && IS_INSN(iobj->link.next)) {
        INSN *niobj = (INSN *)iobj->link.next;
        if ((IS_INSN_ID(niobj, getlocal) ||
             IS_INSN_ID(niobj, getinstancevariable) ||
             IS_INSN_ID(niobj, putself)) &&
            IS_NEXT_INSN_ID(&niobj->link, send)) {

            LINK_ELEMENT *sendobj = &(niobj->link); // Below we call ->next;
            const struct rb_callinfo *ci;
            // Allow any number (0 or more) of simple method calls on the argument
            // (as in `[...].include?(arg.method1.method2)`.
            do {
                sendobj = sendobj->next;
                ci = (struct rb_callinfo *)OPERAND_AT(sendobj, 0);
            } while (vm_ci_simple(ci) && vm_ci_argc(ci) == 0 && IS_NEXT_INSN_ID(sendobj, send));

            if (vm_ci_simple(ci) && vm_ci_argc(ci) == 1 && vm_ci_mid(ci) == idIncludeP) {
                // Move the array arg from duparray to opt_duparray_send.
                VALUE ary = iobj->operands[0];
                rb_obj_reveal(ary, rb_cArray);

                INSN *sendins = (INSN *)sendobj;
                insn_replace_with_operands(iseq, sendins, BIN(opt_duparray_send), 3, ary, rb_id2sym(idIncludeP), INT2FIX(1));

                // Remove the duparray insn.
                ELEM_REMOVE(&iobj->link);
                return COMPILE_OK;
            }
        }
    }


    if (IS_INSN_ID(iobj, send)) {
        const struct rb_callinfo *ci = (struct rb_callinfo *)OPERAND_AT(iobj, 0);
        const rb_iseq_t *blockiseq = (rb_iseq_t *)OPERAND_AT(iobj, 1);

#define SP_INSN(opt) insn_set_specialized_instruction(iseq, iobj, BIN(opt_##opt))
        if (vm_ci_simple(ci)) {
            switch (vm_ci_argc(ci)) {
              case 0:
                switch (vm_ci_mid(ci)) {
                  case idLength: SP_INSN(length); return COMPILE_OK;
                  case idSize:	 SP_INSN(size);	  return COMPILE_OK;
                  case idEmptyP: SP_INSN(empty_p);return COMPILE_OK;
                  case idNilP:   SP_INSN(nil_p);  return COMPILE_OK;
                  case idSucc:	 SP_INSN(succ);	  return COMPILE_OK;
                  case idNot:	 SP_INSN(not);	  return COMPILE_OK;
                }
                break;
              case 1:
                switch (vm_ci_mid(ci)) {
                  case idPLUS:	 SP_INSN(plus);	  return COMPILE_OK;
                  case idMINUS:	 SP_INSN(minus);  return COMPILE_OK;
                  case idMULT:	 SP_INSN(mult);	  return COMPILE_OK;
                  case idDIV:	 SP_INSN(div);	  return COMPILE_OK;
                  case idMOD:	 SP_INSN(mod);	  return COMPILE_OK;
                  case idEq:	 SP_INSN(eq);	  return COMPILE_OK;
                  case idNeq:	 SP_INSN(neq);	  return COMPILE_OK;
                  case idEqTilde:SP_INSN(regexpmatch2);return COMPILE_OK;
                  case idLT:	 SP_INSN(lt);	  return COMPILE_OK;
                  case idLE:	 SP_INSN(le);	  return COMPILE_OK;
                  case idGT:	 SP_INSN(gt);	  return COMPILE_OK;
                  case idGE:	 SP_INSN(ge);	  return COMPILE_OK;
                  case idLTLT:	 SP_INSN(ltlt);	  return COMPILE_OK;
                  case idAREF:	 SP_INSN(aref);	  return COMPILE_OK;
                  case idAnd:    SP_INSN(and);    return COMPILE_OK;
                  case idOr:     SP_INSN(or);    return COMPILE_OK;
                }
                break;
              case 2:
                switch (vm_ci_mid(ci)) {
                  case idASET:	 SP_INSN(aset);	  return COMPILE_OK;
                }
                break;
            }
        }

        if ((vm_ci_flag(ci) & (VM_CALL_ARGS_BLOCKARG | VM_CALL_FORWARDING)) == 0 && blockiseq == NULL) {
            iobj->insn_id = BIN(opt_send_without_block);
            iobj->operand_size = insn_len(iobj->insn_id) - 1;
        }
    }
#undef SP_INSN

    return COMPILE_OK;
}

static inline int
tailcallable_p(rb_iseq_t *iseq)
{
    switch (ISEQ_BODY(iseq)->type) {
      case ISEQ_TYPE_TOP:
      case ISEQ_TYPE_EVAL:
      case ISEQ_TYPE_MAIN:
        /* not tail callable because cfp will be over popped */
      case ISEQ_TYPE_RESCUE:
      case ISEQ_TYPE_ENSURE:
        /* rescue block can't tail call because of errinfo */
        return FALSE;
      default:
        return TRUE;
    }
}

static int
iseq_optimize(rb_iseq_t *iseq, LINK_ANCHOR *const anchor)
{
    LINK_ELEMENT *list;
    const int do_peepholeopt = ISEQ_COMPILE_DATA(iseq)->option->peephole_optimization;
    const int do_tailcallopt = tailcallable_p(iseq) &&
        ISEQ_COMPILE_DATA(iseq)->option->tailcall_optimization;
    const int do_si = ISEQ_COMPILE_DATA(iseq)->option->specialized_instruction;
    const int do_ou = ISEQ_COMPILE_DATA(iseq)->option->operands_unification;
    int rescue_level = 0;
    int tailcallopt = do_tailcallopt;

    list = FIRST_ELEMENT(anchor);

    int do_block_optimization = 0;
    LABEL * block_loop_label = NULL;

    // If we're optimizing a block
    if (ISEQ_BODY(iseq)->type == ISEQ_TYPE_BLOCK) {
        do_block_optimization = 1;

        // If the block starts with a nop and a label,
        // record the label so we can detect if it's a jump target
        LINK_ELEMENT * le = FIRST_ELEMENT(anchor)->next;
        if (IS_INSN(le) && IS_INSN_ID((INSN *)le, nop) && IS_LABEL(le->next)) {
            block_loop_label = (LABEL *)le->next;
        }
    }

    while (list) {
        if (IS_INSN(list)) {
            if (do_peepholeopt) {
                iseq_peephole_optimize(iseq, list, tailcallopt);
            }
            if (do_si) {
                iseq_specialized_instruction(iseq, (INSN *)list);
            }
            if (do_ou) {
                insn_operands_unification((INSN *)list);
            }

            if (do_block_optimization) {
                INSN * item = (INSN *)list;
                // Give up if there is a throw
                if (IS_INSN_ID(item, throw)) {
                    do_block_optimization = 0;
                }
                else {
                    // If the instruction has a jump target, check if the
                    // jump target is the block loop label
                    const char *types = insn_op_types(item->insn_id);
                    for (int j = 0; types[j]; j++) {
                        if (types[j] == TS_OFFSET) {
                            // If the jump target is equal to the block loop
                            // label, then we can't do the optimization because
                            // the leading `nop` instruction fires the block
                            // entry tracepoint
                            LABEL * target = (LABEL *)OPERAND_AT(item, j);
                            if (target == block_loop_label) {
                                do_block_optimization = 0;
                            }
                        }
                    }
                }
            }
        }
        if (IS_LABEL(list)) {
            switch (((LABEL *)list)->rescued) {
              case LABEL_RESCUE_BEG:
                rescue_level++;
                tailcallopt = FALSE;
                break;
              case LABEL_RESCUE_END:
                if (!--rescue_level) tailcallopt = do_tailcallopt;
                break;
            }
        }
        list = list->next;
    }

    if (do_block_optimization) {
        LINK_ELEMENT * le = FIRST_ELEMENT(anchor)->next;
        if (IS_INSN(le) && IS_INSN_ID((INSN *)le, nop)) {
            ELEM_REMOVE(le);
        }
    }
    return COMPILE_OK;
}

#if OPT_INSTRUCTIONS_UNIFICATION
static INSN *
new_unified_insn(rb_iseq_t *iseq,
                 int insn_id, int size, LINK_ELEMENT *seq_list)
{
    INSN *iobj = 0;
    LINK_ELEMENT *list = seq_list;
    int i, argc = 0;
    VALUE *operands = 0, *ptr = 0;


    /* count argc */
    for (i = 0; i < size; i++) {
        iobj = (INSN *)list;
        argc += iobj->operand_size;
        list = list->next;
    }

    if (argc > 0) {
        ptr = operands = compile_data_alloc2(iseq, sizeof(VALUE), argc);
    }

    /* copy operands */
    list = seq_list;
    for (i = 0; i < size; i++) {
        iobj = (INSN *)list;
        MEMCPY(ptr, iobj->operands, VALUE, iobj->operand_size);
        ptr += iobj->operand_size;
        list = list->next;
    }

    return new_insn_core(iseq, iobj->insn_info.line_no, iobj->insn_info.node_id, insn_id, argc, operands);
}
#endif

/*
 * This scheme can get more performance if do this optimize with
 * label address resolving.
 * It's future work (if compile time was bottle neck).
 */
static int
iseq_insns_unification(rb_iseq_t *iseq, LINK_ANCHOR *const anchor)
{
#if OPT_INSTRUCTIONS_UNIFICATION
    LINK_ELEMENT *list;
    INSN *iobj, *niobj;
    int id, k;
    intptr_t j;

    list = FIRST_ELEMENT(anchor);
    while (list) {
        if (IS_INSN(list)) {
            iobj = (INSN *)list;
            id = iobj->insn_id;
            if (unified_insns_data[id] != 0) {
                const int *const *entry = unified_insns_data[id];
                for (j = 1; j < (intptr_t)entry[0]; j++) {
                    const int *unified = entry[j];
                    LINK_ELEMENT *li = list->next;
                    for (k = 2; k < unified[1]; k++) {
                        if (!IS_INSN(li) ||
                            ((INSN *)li)->insn_id != unified[k]) {
                            goto miss;
                        }
                        li = li->next;
                    }
                    /* matched */
                    niobj =
                        new_unified_insn(iseq, unified[0], unified[1] - 1,
                                         list);

                    /* insert to list */
                    niobj->link.prev = (LINK_ELEMENT *)iobj->link.prev;
                    niobj->link.next = li;
                    if (li) {
                        li->prev = (LINK_ELEMENT *)niobj;
                    }

                    list->prev->next = (LINK_ELEMENT *)niobj;
                    list = (LINK_ELEMENT *)niobj;
                    break;
                  miss:;
                }
            }
        }
        list = list->next;
    }
#endif
    return COMPILE_OK;
}

static int
all_string_result_p(const NODE *node)
{
    if (!node) return FALSE;
    switch (nd_type(node)) {
      case NODE_STR: case NODE_DSTR: case NODE_FILE:
        return TRUE;
      case NODE_IF: case NODE_UNLESS:
        if (!RNODE_IF(node)->nd_body || !RNODE_IF(node)->nd_else) return FALSE;
        if (all_string_result_p(RNODE_IF(node)->nd_body))
            return all_string_result_p(RNODE_IF(node)->nd_else);
        return FALSE;
      case NODE_AND: case NODE_OR:
        if (!RNODE_AND(node)->nd_2nd)
            return all_string_result_p(RNODE_AND(node)->nd_1st);
        if (!all_string_result_p(RNODE_AND(node)->nd_1st))
            return FALSE;
        return all_string_result_p(RNODE_AND(node)->nd_2nd);
      default:
        return FALSE;
    }
}

struct dstr_ctxt {
    rb_iseq_t *const iseq;
    LINK_ANCHOR *const ret;
    VALUE lit;
    const NODE *lit_node;
    int cnt;
    int dregx;
};

static int
append_dstr_fragment(struct dstr_ctxt *args, const NODE *const node, rb_parser_string_t *str)
{
    VALUE s = rb_str_new_mutable_parser_string(str);
    if (args->dregx) {
        VALUE error = rb_reg_check_preprocess(s);
        if (!NIL_P(error)) {
            COMPILE_ERROR(args->iseq, nd_line(node), "%" PRIsVALUE, error);
            return COMPILE_NG;
        }
    }
    if (NIL_P(args->lit)) {
        args->lit = s;
        args->lit_node = node;
    }
    else {
        rb_str_buf_append(args->lit, s);
    }
    return COMPILE_OK;
}

static void
flush_dstr_fragment(struct dstr_ctxt *args)
{
    if (!NIL_P(args->lit)) {
        rb_iseq_t *iseq = args->iseq;
        VALUE lit = args->lit;
        args->lit = Qnil;
        lit = rb_fstring(lit);
        ADD_INSN1(args->ret, args->lit_node, putobject, lit);
        RB_OBJ_WRITTEN(args->iseq, Qundef, lit);
        args->cnt++;
    }
}

static int
compile_dstr_fragments_0(struct dstr_ctxt *args, const NODE *const node)
{
    const struct RNode_LIST *list = RNODE_DSTR(node)->nd_next;
    rb_parser_string_t *str = RNODE_DSTR(node)->string;

    if (str) {
        CHECK(append_dstr_fragment(args, node, str));
    }

    while (list) {
        const NODE *const head = list->nd_head;
        if (nd_type_p(head, NODE_STR)) {
            CHECK(append_dstr_fragment(args, node, RNODE_STR(head)->string));
        }
        else if (nd_type_p(head, NODE_DSTR)) {
            CHECK(compile_dstr_fragments_0(args, head));
        }
        else {
            flush_dstr_fragment(args);
            rb_iseq_t *iseq = args->iseq;
            CHECK(COMPILE(args->ret, "each string", head));
            args->cnt++;
        }
        list = (struct RNode_LIST *)list->nd_next;
    }
    return COMPILE_OK;
}

static int
compile_dstr_fragments(rb_iseq_t *iseq, LINK_ANCHOR *const ret, const NODE *const node, int *cntp, int dregx)
{
    struct dstr_ctxt args = {
        .iseq = iseq, .ret = ret,
        .lit = Qnil, .lit_node = NULL,
        .cnt = 0, .dregx = dregx,
    };
    CHECK(compile_dstr_fragments_0(&args, node));
    flush_dstr_fragment(&args);

    *cntp = args.cnt;

    return COMPILE_OK;
}

static int
compile_block(rb_iseq_t *iseq, LINK_ANCHOR *const ret, const NODE *node, int popped)
{
    while (node && nd_type_p(node, NODE_BLOCK)) {
        CHECK(COMPILE_(ret, "BLOCK body", RNODE_BLOCK(node)->nd_head,
                       (RNODE_BLOCK(node)->nd_next ? 1 : popped)));
        node = RNODE_BLOCK(node)->nd_next;
    }
    if (node) {
        CHECK(COMPILE_(ret, "BLOCK next", RNODE_BLOCK(node)->nd_next, popped));
    }
    return COMPILE_OK;
}

static int
compile_dstr(rb_iseq_t *iseq, LINK_ANCHOR *const ret, const NODE *const node)
{
    int cnt;
    if (!RNODE_DSTR(node)->nd_next) {
        VALUE lit = rb_node_dstr_string_val(node);
        ADD_INSN1(ret, node, putstring, lit);
        RB_OBJ_WRITTEN(iseq, Qundef, lit);
    }
    else {
        CHECK(compile_dstr_fragments(iseq, ret, node, &cnt, FALSE));
        ADD_INSN1(ret, node, concatstrings, INT2FIX(cnt));
    }
    return COMPILE_OK;
}

static int
compile_dregx(rb_iseq_t *iseq, LINK_ANCHOR *const ret, const NODE *const node, int popped)
{
    int cnt;
    int cflag = (int)RNODE_DREGX(node)->as.nd_cflag;

    if (!RNODE_DREGX(node)->nd_next) {
        if (!popped) {
            VALUE src = rb_node_dregx_string_val(node);
            VALUE match = rb_reg_compile(src, cflag, NULL, 0);
            ADD_INSN1(ret, node, putobject, match);
            RB_OBJ_WRITTEN(iseq, Qundef, match);
        }
        return COMPILE_OK;
    }

    CHECK(compile_dstr_fragments(iseq, ret, node, &cnt, TRUE));
    ADD_INSN2(ret, node, toregexp, INT2FIX(cflag), INT2FIX(cnt));

    if (popped) {
        ADD_INSN(ret, node, pop);
    }

    return COMPILE_OK;
}

static int
compile_flip_flop(rb_iseq_t *iseq, LINK_ANCHOR *const ret, const NODE *const node, int again,
                  LABEL *then_label, LABEL *else_label)
{
    const int line = nd_line(node);
    LABEL *lend = NEW_LABEL(line);
    rb_num_t cnt = ISEQ_FLIP_CNT_INCREMENT(ISEQ_BODY(iseq)->local_iseq)
        + VM_SVAR_FLIPFLOP_START;
    VALUE key = INT2FIX(cnt);

    ADD_INSN2(ret, node, getspecial, key, INT2FIX(0));
    ADD_INSNL(ret, node, branchif, lend);

    /* *flip == 0 */
    CHECK(COMPILE(ret, "flip2 beg", RNODE_FLIP2(node)->nd_beg));
    ADD_INSNL(ret, node, branchunless, else_label);
    ADD_INSN1(ret, node, putobject, Qtrue);
    ADD_INSN1(ret, node, setspecial, key);
    if (!again) {
        ADD_INSNL(ret, node, jump, then_label);
    }

    /* *flip == 1 */
    ADD_LABEL(ret, lend);
    CHECK(COMPILE(ret, "flip2 end", RNODE_FLIP2(node)->nd_end));
    ADD_INSNL(ret, node, branchunless, then_label);
    ADD_INSN1(ret, node, putobject, Qfalse);
    ADD_INSN1(ret, node, setspecial, key);
    ADD_INSNL(ret, node, jump, then_label);

    return COMPILE_OK;
}

static int
compile_branch_condition(rb_iseq_t *iseq, LINK_ANCHOR *ret, const NODE *cond,
                         LABEL *then_label, LABEL *else_label);

#define COMPILE_SINGLE 2
static int
compile_logical(rb_iseq_t *iseq, LINK_ANCHOR *const ret, const NODE *cond,
                LABEL *then_label, LABEL *else_label)
{
    DECL_ANCHOR(seq);
    INIT_ANCHOR(seq);
    LABEL *label = NEW_LABEL(nd_line(cond));
    if (!then_label) then_label = label;
    else if (!else_label) else_label = label;

    CHECK(compile_branch_condition(iseq, seq, cond, then_label, else_label));

    if (LIST_INSN_SIZE_ONE(seq)) {
        INSN *insn = (INSN *)ELEM_FIRST_INSN(FIRST_ELEMENT(seq));
        if (insn->insn_id == BIN(jump) && (LABEL *)(insn->operands[0]) == label)
            return COMPILE_OK;
    }
    if (!label->refcnt) {
        return COMPILE_SINGLE;
    }
    ADD_LABEL(seq, label);
    ADD_SEQ(ret, seq);
    return COMPILE_OK;
}

static int
compile_branch_condition(rb_iseq_t *iseq, LINK_ANCHOR *ret, const NODE *cond,
                         LABEL *then_label, LABEL *else_label)
{
    int ok;
    DECL_ANCHOR(ignore);

  again:
    switch (nd_type(cond)) {
      case NODE_AND:
        CHECK(ok = compile_logical(iseq, ret, RNODE_AND(cond)->nd_1st, NULL, else_label));
        cond = RNODE_AND(cond)->nd_2nd;
        if (ok == COMPILE_SINGLE) {
            INIT_ANCHOR(ignore);
            ret = ignore;
            then_label = NEW_LABEL(nd_line(cond));
        }
        goto again;
      case NODE_OR:
        CHECK(ok = compile_logical(iseq, ret, RNODE_OR(cond)->nd_1st, then_label, NULL));
        cond = RNODE_OR(cond)->nd_2nd;
        if (ok == COMPILE_SINGLE) {
            INIT_ANCHOR(ignore);
            ret = ignore;
            else_label = NEW_LABEL(nd_line(cond));
        }
        goto again;
      case NODE_SYM:
      case NODE_LINE:
      case NODE_FILE:
      case NODE_ENCODING:
      case NODE_INTEGER:    /* NODE_INTEGER is always true */
      case NODE_FLOAT:      /* NODE_FLOAT is always true */
      case NODE_RATIONAL:   /* NODE_RATIONAL is always true */
      case NODE_IMAGINARY:  /* NODE_IMAGINARY is always true */
      case NODE_TRUE:
      case NODE_STR:
      case NODE_REGX:
      case NODE_ZLIST:
      case NODE_LAMBDA:
        /* printf("useless condition eliminate (%s)\n",  ruby_node_name(nd_type(cond))); */
        ADD_INSNL(ret, cond, jump, then_label);
        return COMPILE_OK;
      case NODE_FALSE:
      case NODE_NIL:
        /* printf("useless condition eliminate (%s)\n", ruby_node_name(nd_type(cond))); */
        ADD_INSNL(ret, cond, jump, else_label);
        return COMPILE_OK;
      case NODE_LIST:
      case NODE_ARGSCAT:
      case NODE_DREGX:
      case NODE_DSTR:
        CHECK(COMPILE_POPPED(ret, "branch condition", cond));
        ADD_INSNL(ret, cond, jump, then_label);
        return COMPILE_OK;
      case NODE_FLIP2:
        CHECK(compile_flip_flop(iseq, ret, cond, TRUE, then_label, else_label));
        return COMPILE_OK;
      case NODE_FLIP3:
        CHECK(compile_flip_flop(iseq, ret, cond, FALSE, then_label, else_label));
        return COMPILE_OK;
      case NODE_DEFINED:
        CHECK(compile_defined_expr(iseq, ret, cond, Qfalse, ret == ignore));
        break;
      default:
        {
            DECL_ANCHOR(cond_seq);
            INIT_ANCHOR(cond_seq);

            CHECK(COMPILE(cond_seq, "branch condition", cond));

            if (LIST_INSN_SIZE_ONE(cond_seq)) {
                INSN *insn = (INSN *)ELEM_FIRST_INSN(FIRST_ELEMENT(cond_seq));
                if (insn->insn_id == BIN(putobject)) {
                    if (RTEST(insn->operands[0])) {
                        ADD_INSNL(ret, cond, jump, then_label);
                        // maybe unreachable
                        return COMPILE_OK;
                    }
                    else {
                        ADD_INSNL(ret, cond, jump, else_label);
                        return COMPILE_OK;
                    }
                }
            }
            ADD_SEQ(ret, cond_seq);
        }
        break;
    }

    ADD_INSNL(ret, cond, branchunless, else_label);
    ADD_INSNL(ret, cond, jump, then_label);
    return COMPILE_OK;
}

#define HASH_BRACE 1

static int
keyword_node_p(const NODE *const node)
{
    return nd_type_p(node, NODE_HASH) && (RNODE_HASH(node)->nd_brace & HASH_BRACE) != HASH_BRACE;
}

static VALUE
get_symbol_value(rb_iseq_t *iseq, const NODE *node)
{
    switch (nd_type(node)) {
      case NODE_SYM:
        return rb_node_sym_string_val(node);
      default:
        UNKNOWN_NODE("get_symbol_value", node, Qnil);
    }
}

static VALUE
node_hash_unique_key_index(rb_iseq_t *iseq, rb_node_hash_t *node_hash, int *count_ptr)
{
    NODE *node = node_hash->nd_head;
    VALUE hash = rb_hash_new();
    VALUE ary = rb_ary_new();

    for (int i = 0; node != NULL; i++, node = RNODE_LIST(RNODE_LIST(node)->nd_next)->nd_next) {
        VALUE key = get_symbol_value(iseq, RNODE_LIST(node)->nd_head);
        VALUE idx = rb_hash_aref(hash, key);
        if (!NIL_P(idx)) {
            rb_ary_store(ary, FIX2INT(idx), Qfalse);
            (*count_ptr)--;
        }
        rb_hash_aset(hash, key, INT2FIX(i));
        rb_ary_store(ary, i, Qtrue);
        (*count_ptr)++;
    }

    return ary;
}

static int
compile_keyword_arg(rb_iseq_t *iseq, LINK_ANCHOR *const ret,
                    const NODE *const root_node,
                    struct rb_callinfo_kwarg **const kw_arg_ptr,
                    unsigned int *flag)
{
    RUBY_ASSERT(nd_type_p(root_node, NODE_HASH));
    RUBY_ASSERT(kw_arg_ptr != NULL);
    RUBY_ASSERT(flag != NULL);

    if (RNODE_HASH(root_node)->nd_head && nd_type_p(RNODE_HASH(root_node)->nd_head, NODE_LIST)) {
        const NODE *node = RNODE_HASH(root_node)->nd_head;
        int seen_nodes = 0;

        while (node) {
            const NODE *key_node = RNODE_LIST(node)->nd_head;
            seen_nodes++;

            RUBY_ASSERT(nd_type_p(node, NODE_LIST));
            if (key_node && nd_type_p(key_node, NODE_SYM)) {
                /* can be keywords */
            }
            else {
                if (flag) {
                    *flag |= VM_CALL_KW_SPLAT;
                    if (seen_nodes > 1 || RNODE_LIST(RNODE_LIST(node)->nd_next)->nd_next) {
                        /* A new hash will be created for the keyword arguments
                         * in this case, so mark the method as passing mutable
                         * keyword splat.
                         */
                        *flag |= VM_CALL_KW_SPLAT_MUT;
                    }
                }
                return FALSE;
            }
            node = RNODE_LIST(node)->nd_next; /* skip value node */
            node = RNODE_LIST(node)->nd_next;
        }

        /* may be keywords */
        node = RNODE_HASH(root_node)->nd_head;
        {
            int len = 0;
            VALUE key_index = node_hash_unique_key_index(iseq, RNODE_HASH(root_node), &len);
            struct rb_callinfo_kwarg *kw_arg =
                rb_xmalloc_mul_add(len, sizeof(VALUE), sizeof(struct rb_callinfo_kwarg));
            VALUE *keywords = kw_arg->keywords;
            int i = 0;
            int j = 0;
            kw_arg->references = 0;
            kw_arg->keyword_len = len;

            *kw_arg_ptr = kw_arg;

            for (i=0; node != NULL; i++, node = RNODE_LIST(RNODE_LIST(node)->nd_next)->nd_next) {
                const NODE *key_node = RNODE_LIST(node)->nd_head;
                const NODE *val_node = RNODE_LIST(RNODE_LIST(node)->nd_next)->nd_head;
                int popped = TRUE;
                if (rb_ary_entry(key_index, i)) {
                    keywords[j] = get_symbol_value(iseq, key_node);
                    j++;
                    popped = FALSE;
                }
                NO_CHECK(COMPILE_(ret, "keyword values", val_node, popped));
            }
            RUBY_ASSERT(j == len);
            return TRUE;
        }
    }
    return FALSE;
}

static int
compile_args(rb_iseq_t *iseq, LINK_ANCHOR *const ret, const NODE *node, NODE **kwnode_ptr)
{
    int len = 0;

    for (; node; len++, node = RNODE_LIST(node)->nd_next) {
        if (CPDEBUG > 0) {
            EXPECT_NODE("compile_args", node, NODE_LIST, -1);
        }

        if (RNODE_LIST(node)->nd_next == NULL && keyword_node_p(RNODE_LIST(node)->nd_head)) { /* last node is kwnode */
            *kwnode_ptr = RNODE_LIST(node)->nd_head;
        }
        else {
            RUBY_ASSERT(!keyword_node_p(RNODE_LIST(node)->nd_head));
            NO_CHECK(COMPILE_(ret, "array element", RNODE_LIST(node)->nd_head, FALSE));
        }
    }

    return len;
}

static inline bool
frozen_string_literal_p(const rb_iseq_t *iseq)
{
    return ISEQ_COMPILE_DATA(iseq)->option->frozen_string_literal > 0;
}

static inline bool
static_literal_node_p(const NODE *node, const rb_iseq_t *iseq, bool hash_key)
{
    switch (nd_type(node)) {
      case NODE_SYM:
      case NODE_REGX:
      case NODE_LINE:
      case NODE_ENCODING:
      case NODE_INTEGER:
      case NODE_FLOAT:
      case NODE_RATIONAL:
      case NODE_IMAGINARY:
      case NODE_NIL:
      case NODE_TRUE:
      case NODE_FALSE:
        return TRUE;
      case NODE_STR:
      case NODE_FILE:
        return hash_key || frozen_string_literal_p(iseq);
      default:
        return FALSE;
    }
}

static inline VALUE
static_literal_value(const NODE *node, rb_iseq_t *iseq)
{
    switch (nd_type(node)) {
      case NODE_INTEGER:
        return rb_node_integer_literal_val(node);
      case NODE_FLOAT:
        return rb_node_float_literal_val(node);
      case NODE_RATIONAL:
        return rb_node_rational_literal_val(node);
      case NODE_IMAGINARY:
        return rb_node_imaginary_literal_val(node);
      case NODE_NIL:
        return Qnil;
      case NODE_TRUE:
        return Qtrue;
      case NODE_FALSE:
        return Qfalse;
      case NODE_SYM:
        return rb_node_sym_string_val(node);
      case NODE_REGX:
        return rb_node_regx_string_val(node);
      case NODE_LINE:
        return rb_node_line_lineno_val(node);
      case NODE_ENCODING:
        return rb_node_encoding_val(node);
      case NODE_FILE:
      case NODE_STR:
        if (ISEQ_COMPILE_DATA(iseq)->option->debug_frozen_string_literal || RTEST(ruby_debug)) {
            VALUE lit = get_string_value(node);
            return rb_str_with_debug_created_info(lit, rb_iseq_path(iseq), (int)nd_line(node));
        }
        else {
            return get_string_value(node);
        }
      default:
        rb_bug("unexpected node: %s", ruby_node_name(nd_type(node)));
    }
}

static int
compile_array(rb_iseq_t *iseq, LINK_ANCHOR *const ret, const NODE *node, int popped, bool first_chunk)
{
    const NODE *line_node = node;

    if (nd_type_p(node, NODE_ZLIST)) {
        if (!popped) {
            ADD_INSN1(ret, line_node, newarray, INT2FIX(0));
        }
        return 0;
    }

    EXPECT_NODE("compile_array", node, NODE_LIST, -1);

    if (popped) {
        for (; node; node = RNODE_LIST(node)->nd_next) {
            NO_CHECK(COMPILE_(ret, "array element", RNODE_LIST(node)->nd_head, popped));
        }
        return 1;
    }

    /* Compilation of an array literal.
     * The following code is essentially the same as:
     *
     *     for (int count = 0; node; count++; node->nd_next) {
     *         compile(node->nd_head);
     *     }
     *     ADD_INSN(newarray, count);
     *
     * However, there are three points.
     *
     * - The code above causes stack overflow for a big string literal.
     *   The following limits the stack length up to max_stack_len.
     *
     *   [x1,x2,...,x10000] =>
     *     push x1  ; push x2  ; ...; push x256; newarray 256;
     *     push x257; push x258; ...; push x512; pushtoarray 256;
     *     push x513; push x514; ...; push x768; pushtoarray 256;
     *     ...
     *
     * - Long subarray can be optimized by pre-allocating a hidden array.
     *
     *   [1,2,3,...,100] =>
     *     duparray [1,2,3,...,100]
     *
     *   [x, 1,2,3,...,100, z] =>
     *     push x; newarray 1;
     *     putobject [1,2,3,...,100] (<- hidden array); concattoarray;
     *     push z; pushtoarray 1;
     *
     * - If the last element is a keyword, pushtoarraykwsplat should be emitted
     *   to only push it onto the array if it is not empty
     *   (Note: a keyword is NODE_HASH which is not static_literal_node_p.)
     *
     *   [1,2,3,**kw] =>
     *     putobject 1; putobject 2; putobject 3; newarray 3; ...; pushtoarraykwsplat kw
     */

    const int max_stack_len = 0x100;
    const int min_tmp_ary_len = 0x40;
    int stack_len = 0;

    /* Either create a new array, or push to the existing array */
#define FLUSH_CHUNK \
    if (stack_len) {                                            \
        if (first_chunk) ADD_INSN1(ret, line_node, newarray, INT2FIX(stack_len)); \
        else ADD_INSN1(ret, line_node, pushtoarray, INT2FIX(stack_len));     \
        first_chunk = FALSE; \
        stack_len = 0;                            \
    }

    while (node) {
        int count = 1;

        /* pre-allocation check (this branch can be omittable) */
        if (static_literal_node_p(RNODE_LIST(node)->nd_head, iseq, false)) {
            /* count the elements that are optimizable */
            const NODE *node_tmp = RNODE_LIST(node)->nd_next;
            for (; node_tmp && static_literal_node_p(RNODE_LIST(node_tmp)->nd_head, iseq, false); node_tmp = RNODE_LIST(node_tmp)->nd_next)
                count++;

            if ((first_chunk && stack_len == 0 && !node_tmp) || count >= min_tmp_ary_len) {
                /* The literal contains only optimizable elements, or the subarray is long enough */
                VALUE ary = rb_ary_hidden_new(count);

                /* Create a hidden array */
                for (; count; count--, node = RNODE_LIST(node)->nd_next)
                    rb_ary_push(ary, static_literal_value(RNODE_LIST(node)->nd_head, iseq));
                OBJ_FREEZE(ary);

                /* Emit optimized code */
                FLUSH_CHUNK;
                if (first_chunk) {
                    ADD_INSN1(ret, line_node, duparray, ary);
                    first_chunk = FALSE;
                }
                else {
                    ADD_INSN1(ret, line_node, putobject, ary);
                    ADD_INSN(ret, line_node, concattoarray);
                }
                RB_OBJ_WRITTEN(iseq, Qundef, ary);
            }
        }

        /* Base case: Compile "count" elements */
        for (; count; count--, node = RNODE_LIST(node)->nd_next) {
            if (CPDEBUG > 0) {
                EXPECT_NODE("compile_array", node, NODE_LIST, -1);
            }

            if (!RNODE_LIST(node)->nd_next && keyword_node_p(RNODE_LIST(node)->nd_head)) {
                /* Create array or push existing non-keyword elements onto array */
                if (stack_len == 0 && first_chunk) {
                    ADD_INSN1(ret, line_node, newarray, INT2FIX(0));
                }
                else {
                    FLUSH_CHUNK;
                }
                NO_CHECK(COMPILE_(ret, "array element", RNODE_LIST(node)->nd_head, 0));
                ADD_INSN(ret, line_node, pushtoarraykwsplat);
                return 1;
            }
            else {
                NO_CHECK(COMPILE_(ret, "array element", RNODE_LIST(node)->nd_head, 0));
                stack_len++;
            }

            /* If there are many pushed elements, flush them to avoid stack overflow */
            if (stack_len >= max_stack_len) FLUSH_CHUNK;
        }
    }

    FLUSH_CHUNK;
#undef FLUSH_CHUNK
    return 1;
}

static inline int
static_literal_node_pair_p(const NODE *node, const rb_iseq_t *iseq)
{
    return RNODE_LIST(node)->nd_head && static_literal_node_p(RNODE_LIST(node)->nd_head, iseq, true) && static_literal_node_p(RNODE_LIST(RNODE_LIST(node)->nd_next)->nd_head, iseq, false);
}

static int
compile_hash(rb_iseq_t *iseq, LINK_ANCHOR *const ret, const NODE *node, int method_call_keywords, int popped)
{
    const NODE *line_node = node;

    node = RNODE_HASH(node)->nd_head;

    if (!node || nd_type_p(node, NODE_ZLIST)) {
        if (!popped) {
            ADD_INSN1(ret, line_node, newhash, INT2FIX(0));
        }
        return 0;
    }

    EXPECT_NODE("compile_hash", node, NODE_LIST, -1);

    if (popped) {
        for (; node; node = RNODE_LIST(node)->nd_next) {
            NO_CHECK(COMPILE_(ret, "hash element", RNODE_LIST(node)->nd_head, popped));
        }
        return 1;
    }

    /* Compilation of a hash literal (or keyword arguments).
     * This is very similar to compile_array, but there are some differences:
     *
     * - It contains key-value pairs.  So we need to take every two elements.
     *   We can assume that the length is always even.
     *
     * - Merging is done by a method call (id_core_hash_merge_ptr).
     *   Sometimes we need to insert the receiver, so "anchor" is needed.
     *   In addition, a method call is much slower than concatarray.
     *   So it pays only when the subsequence is really long.
     *   (min_tmp_hash_len must be much larger than min_tmp_ary_len.)
     *
     * - We need to handle keyword splat: **kw.
     *   For **kw, the key part (node->nd_head) is NULL, and the value part
     *   (node->nd_next->nd_head) is "kw".
     *   The code is a bit difficult to avoid hash allocation for **{}.
     */

    const int max_stack_len = 0x100;
    const int min_tmp_hash_len = 0x800;
    int stack_len = 0;
    int first_chunk = 1;
    DECL_ANCHOR(anchor);
    INIT_ANCHOR(anchor);

    /* Convert pushed elements to a hash, and merge if needed */
#define FLUSH_CHUNK()                                                                   \
    if (stack_len) {                                                                    \
        if (first_chunk) {                                                              \
            APPEND_LIST(ret, anchor);                                                   \
            ADD_INSN1(ret, line_node, newhash, INT2FIX(stack_len));                          \
        }                                                                               \
        else {                                                                          \
            ADD_INSN1(ret, line_node, putspecialobject, INT2FIX(VM_SPECIAL_OBJECT_VMCORE));  \
            ADD_INSN(ret, line_node, swap);                                                  \
            APPEND_LIST(ret, anchor);                                                   \
            ADD_SEND(ret, line_node, id_core_hash_merge_ptr, INT2FIX(stack_len + 1));        \
        }                                                                               \
        INIT_ANCHOR(anchor);                                                            \
        first_chunk = stack_len = 0;                                                    \
    }

    while (node) {
        int count = 1;

        /* pre-allocation check (this branch can be omittable) */
        if (static_literal_node_pair_p(node, iseq)) {
            /* count the elements that are optimizable */
            const NODE *node_tmp = RNODE_LIST(RNODE_LIST(node)->nd_next)->nd_next;
            for (; node_tmp && static_literal_node_pair_p(node_tmp, iseq); node_tmp = RNODE_LIST(RNODE_LIST(node_tmp)->nd_next)->nd_next)
                count++;

            if ((first_chunk && stack_len == 0 && !node_tmp) || count >= min_tmp_hash_len) {
                /* The literal contains only optimizable elements, or the subsequence is long enough */
                VALUE ary = rb_ary_hidden_new(count);

                /* Create a hidden hash */
                for (; count; count--, node = RNODE_LIST(RNODE_LIST(node)->nd_next)->nd_next) {
                    VALUE elem[2];
                    elem[0] = static_literal_value(RNODE_LIST(node)->nd_head, iseq);
                    elem[1] = static_literal_value(RNODE_LIST(RNODE_LIST(node)->nd_next)->nd_head, iseq);
                    rb_ary_cat(ary, elem, 2);
                }
                VALUE hash = rb_hash_new_with_size(RARRAY_LEN(ary) / 2);
                rb_hash_bulk_insert(RARRAY_LEN(ary), RARRAY_CONST_PTR(ary), hash);
                hash = rb_obj_hide(hash);
                OBJ_FREEZE(hash);

                /* Emit optimized code */
                FLUSH_CHUNK();
                if (first_chunk) {
                    ADD_INSN1(ret, line_node, duphash, hash);
                    first_chunk = 0;
                }
                else {
                    ADD_INSN1(ret, line_node, putspecialobject, INT2FIX(VM_SPECIAL_OBJECT_VMCORE));
                    ADD_INSN(ret, line_node, swap);

                    ADD_INSN1(ret, line_node, putobject, hash);

                    ADD_SEND(ret, line_node, id_core_hash_merge_kwd, INT2FIX(2));
                }
                RB_OBJ_WRITTEN(iseq, Qundef, hash);
            }
        }

        /* Base case: Compile "count" elements */
        for (; count; count--, node = RNODE_LIST(RNODE_LIST(node)->nd_next)->nd_next) {

            if (CPDEBUG > 0) {
                EXPECT_NODE("compile_hash", node, NODE_LIST, -1);
            }

            if (RNODE_LIST(node)->nd_head) {
                /* Normal key-value pair */
                NO_CHECK(COMPILE_(anchor, "hash key element", RNODE_LIST(node)->nd_head, 0));
                NO_CHECK(COMPILE_(anchor, "hash value element", RNODE_LIST(RNODE_LIST(node)->nd_next)->nd_head, 0));
                stack_len += 2;

                /* If there are many pushed elements, flush them to avoid stack overflow */
                if (stack_len >= max_stack_len) FLUSH_CHUNK();
            }
            else {
                /* kwsplat case: foo(..., **kw, ...) */
                FLUSH_CHUNK();

                const NODE *kw = RNODE_LIST(RNODE_LIST(node)->nd_next)->nd_head;
                int empty_kw = nd_type_p(kw, NODE_HASH) && (!RNODE_HASH(kw)->nd_head); /* foo(  ..., **{}, ...) */
                int first_kw = first_chunk && stack_len == 0; /* foo(1,2,3, **kw, ...) */
                int last_kw = !RNODE_LIST(RNODE_LIST(node)->nd_next)->nd_next; /* foo(  ..., **kw) */
                int only_kw = last_kw && first_kw;            /* foo(1,2,3, **kw) */

                empty_kw = empty_kw || nd_type_p(kw, NODE_NIL); /* foo(  ..., **nil, ...) */
                if (empty_kw) {
                    if (only_kw && method_call_keywords) {
                        /* **{} appears at the only keyword argument in method call,
                         * so it won't be modified.
                         * kw is a special NODE_LIT that contains a special empty hash,
                         * so this emits: putobject {}.
                         * This is only done for method calls and not for literal hashes,
                         * because literal hashes should always result in a new hash.
                         */
                        NO_CHECK(COMPILE(ret, "keyword splat", kw));
                    }
                    else if (first_kw) {
                        /* **{} appears as the first keyword argument, so it may be modified.
                         * We need to create a fresh hash object.
                         */
                        ADD_INSN1(ret, line_node, newhash, INT2FIX(0));
                    }
                    /* Any empty keyword splats that are not the first can be ignored.
                     * since merging an empty hash into the existing hash is the same
                     * as not merging it. */
                }
                else {
                    if (only_kw && method_call_keywords) {
                        /* **kw is only keyword argument in method call.
                         * Use directly.  This will be not be flagged as mutable.
                         * This is only done for method calls and not for literal hashes,
                         * because literal hashes should always result in a new hash.
                         */
                        NO_CHECK(COMPILE(ret, "keyword splat", kw));
                    }
                    else {
                        /* There is more than one keyword argument, or this is not a method
                         * call.  In that case, we need to add an empty hash (if first keyword),
                         * or merge the hash to the accumulated hash (if not the first keyword).
                         */
                        ADD_INSN1(ret, line_node, putspecialobject, INT2FIX(VM_SPECIAL_OBJECT_VMCORE));
                        if (first_kw) ADD_INSN1(ret, line_node, newhash, INT2FIX(0));
                        else ADD_INSN(ret, line_node, swap);

                        NO_CHECK(COMPILE(ret, "keyword splat", kw));

                        ADD_SEND(ret, line_node, id_core_hash_merge_kwd, INT2FIX(2));
                    }
                }

                first_chunk = 0;
            }
        }
    }

    FLUSH_CHUNK();
#undef FLUSH_CHUNK
    return 1;
}

VALUE
rb_node_case_when_optimizable_literal(const NODE *const node)
{
    switch (nd_type(node)) {
      case NODE_INTEGER:
        return rb_node_integer_literal_val(node);
      case NODE_FLOAT: {
        VALUE v = rb_node_float_literal_val(node);
        double ival;

        if (modf(RFLOAT_VALUE(v), &ival) == 0.0) {
            return FIXABLE(ival) ? LONG2FIX((long)ival) : rb_dbl2big(ival);
        }
        return v;
      }
      case NODE_RATIONAL:
      case NODE_IMAGINARY:
        return Qundef;
      case NODE_NIL:
        return Qnil;
      case NODE_TRUE:
        return Qtrue;
      case NODE_FALSE:
        return Qfalse;
      case NODE_SYM:
        return rb_node_sym_string_val(node);
      case NODE_LINE:
        return rb_node_line_lineno_val(node);
      case NODE_STR:
        return rb_node_str_string_val(node);
      case NODE_FILE:
        return rb_node_file_path_val(node);
    }
    return Qundef;
}

static int
when_vals(rb_iseq_t *iseq, LINK_ANCHOR *const cond_seq, const NODE *vals,
          LABEL *l1, int only_special_literals, VALUE literals)
{
    while (vals) {
        const NODE *val = RNODE_LIST(vals)->nd_head;
        VALUE lit = rb_node_case_when_optimizable_literal(val);

        if (UNDEF_P(lit)) {
            only_special_literals = 0;
        }
        else if (NIL_P(rb_hash_lookup(literals, lit))) {
            rb_hash_aset(literals, lit, (VALUE)(l1) | 1);
        }

        if (nd_type_p(val, NODE_STR) || nd_type_p(val, NODE_FILE)) {
            debugp_param("nd_lit", get_string_value(val));
            lit = get_string_value(val);
            ADD_INSN1(cond_seq, val, putobject, lit);
            RB_OBJ_WRITTEN(iseq, Qundef, lit);
        }
        else {
            if (!COMPILE(cond_seq, "when cond", val)) return -1;
        }

        // Emit pattern === target
        ADD_INSN1(cond_seq, vals, topn, INT2FIX(1));
        ADD_CALL(cond_seq, vals, idEqq, INT2FIX(1));
        ADD_INSNL(cond_seq, val, branchif, l1);
        vals = RNODE_LIST(vals)->nd_next;
    }
    return only_special_literals;
}

static int
when_splat_vals(rb_iseq_t *iseq, LINK_ANCHOR *const cond_seq, const NODE *vals,
                LABEL *l1, int only_special_literals, VALUE literals)
{
    const NODE *line_node = vals;

    switch (nd_type(vals)) {
      case NODE_LIST:
        if (when_vals(iseq, cond_seq, vals, l1, only_special_literals, literals) < 0)
            return COMPILE_NG;
        break;
      case NODE_SPLAT:
        ADD_INSN (cond_seq, line_node, dup);
        CHECK(COMPILE(cond_seq, "when splat", RNODE_SPLAT(vals)->nd_head));
        ADD_INSN1(cond_seq, line_node, splatarray, Qfalse);
        ADD_INSN1(cond_seq, line_node, checkmatch, INT2FIX(VM_CHECKMATCH_TYPE_CASE | VM_CHECKMATCH_ARRAY));
        ADD_INSNL(cond_seq, line_node, branchif, l1);
        break;
      case NODE_ARGSCAT:
        CHECK(when_splat_vals(iseq, cond_seq, RNODE_ARGSCAT(vals)->nd_head, l1, only_special_literals, literals));
        CHECK(when_splat_vals(iseq, cond_seq, RNODE_ARGSCAT(vals)->nd_body, l1, only_special_literals, literals));
        break;
      case NODE_ARGSPUSH:
        CHECK(when_splat_vals(iseq, cond_seq, RNODE_ARGSPUSH(vals)->nd_head, l1, only_special_literals, literals));
        ADD_INSN (cond_seq, line_node, dup);
        CHECK(COMPILE(cond_seq, "when argspush body", RNODE_ARGSPUSH(vals)->nd_body));
        ADD_INSN1(cond_seq, line_node, checkmatch, INT2FIX(VM_CHECKMATCH_TYPE_CASE));
        ADD_INSNL(cond_seq, line_node, branchif, l1);
        break;
      default:
        ADD_INSN (cond_seq, line_node, dup);
        CHECK(COMPILE(cond_seq, "when val", vals));
        ADD_INSN1(cond_seq, line_node, splatarray, Qfalse);
        ADD_INSN1(cond_seq, line_node, checkmatch, INT2FIX(VM_CHECKMATCH_TYPE_CASE | VM_CHECKMATCH_ARRAY));
        ADD_INSNL(cond_seq, line_node, branchif, l1);
        break;
    }
    return COMPILE_OK;
}

/* Multiple Assignment Handling
 *
 * In order to handle evaluation of multiple assignment such that the left hand side
 * is evaluated before the right hand side, we need to process the left hand side
 * and see if there are any attributes that need to be assigned, or constants set
 * on explicit objects.  If so, we add instructions to evaluate the receiver of
 * any assigned attributes or constants before we process the right hand side.
 *
 * For a multiple assignment such as:
 *
 *   l1.m1, l2[0] = r3, r4
 *
 * We start off evaluating l1 and l2, then we evaluate r3 and r4, then we
 * assign the result of r3 to l1.m1, and then the result of r4 to l2.m2.
 * On the VM stack, this looks like:
 *
 *     self                               # putself
 *     l1                                 # send
 *     l1, self                           # putself
 *     l1, l2                             # send
 *     l1, l2, 0                          # putobject 0
 *     l1, l2, 0, [r3, r4]                # after evaluation of RHS
 *     l1, l2, 0, [r3, r4], r4, r3        # expandarray
 *     l1, l2, 0, [r3, r4], r4, r3, l1    # topn 5
 *     l1, l2, 0, [r3, r4], r4, l1, r3    # swap
 *     l1, l2, 0, [r3, r4], r4, m1=       # send
 *     l1, l2, 0, [r3, r4], r4            # pop
 *     l1, l2, 0, [r3, r4], r4, l2        # topn 3
 *     l1, l2, 0, [r3, r4], r4, l2, 0     # topn 3
 *     l1, l2, 0, [r3, r4], r4, l2, 0, r4 # topn 2
 *     l1, l2, 0, [r3, r4], r4, []=       # send
 *     l1, l2, 0, [r3, r4], r4            # pop
 *     l1, l2, 0, [r3, r4]                # pop
 *     [r3, r4], l2, 0, [r3, r4]          # setn 3
 *     [r3, r4], l2, 0                    # pop
 *     [r3, r4], l2                       # pop
 *     [r3, r4]                           # pop
 *
 * This is made more complex when you have to handle splats, post args,
 * and arbitrary levels of nesting.  You need to keep track of the total
 * number of attributes to set, and for each attribute, how many entries
 * are on the stack before the final attribute, in order to correctly
 * calculate the topn value to use to get the receiver of the attribute
 * setter method.
 *
 * A brief description of the VM stack for simple multiple assignment
 * with no splat (rhs_array will not be present if the return value of
 * the multiple assignment is not needed):
 *
 *     lhs_attr1, lhs_attr2, ..., rhs_array, ..., rhs_arg2, rhs_arg1
 *
 * For multiple assignment with splats, while processing the part before
 * the splat (splat+post here is an array of the splat and the post arguments):
 *
 *     lhs_attr1, lhs_attr2, ..., rhs_array, splat+post, ..., rhs_arg2, rhs_arg1
 *
 * When processing the splat and post arguments:
 *
 *     lhs_attr1, lhs_attr2, ..., rhs_array, ..., post_arg2, post_arg1, splat
 *
 * When processing nested multiple assignment, existing values on the stack
 * are kept.  So for:
 *
 *     (l1.m1, l2.m2), l3.m3, l4* = [r1, r2], r3, r4
 *
 * The stack layout would be the following before processing the nested
 * multiple assignment:
 *
 *     l1, l2, [[r1, r2], r3, r4], [r4], r3, [r1, r2]
 *
 * In order to handle this correctly, we need to keep track of the nesting
 * level for each attribute assignment, as well as the attribute number
 * (left hand side attributes are processed left to right) and number of
 * arguments to pass to the setter method. struct masgn_lhs_node tracks
 * this information.
 *
 * We also need to track information for the entire multiple assignment, such
 * as the total number of arguments, and the current nesting level, to
 * handle both nested multiple assignment as well as cases where the
 * rhs is not needed.  We also need to keep track of all attribute
 * assignments in this, which we do using a linked listed. struct masgn_state
 * tracks this information.
 */

struct masgn_lhs_node {
  INSN *before_insn;
  struct masgn_lhs_node *next;
  const NODE *line_node;
  int argn;
  int num_args;
  int lhs_pos;
};

struct masgn_state {
    struct masgn_lhs_node *first_memo;
    struct masgn_lhs_node *last_memo;
    int lhs_level;
    int num_args;
    bool nested;
};

static int
add_masgn_lhs_node(struct masgn_state *state, int lhs_pos, const NODE *line_node, int argc, INSN *before_insn)
{
    if (!state) {
        rb_bug("no masgn_state");
    }

    struct masgn_lhs_node *memo;
    memo = malloc(sizeof(struct masgn_lhs_node));
    if (!memo) {
        return COMPILE_NG;
    }

    memo->before_insn = before_insn;
    memo->line_node = line_node;
    memo->argn = state->num_args + 1;
    memo->num_args = argc;
    state->num_args += argc;
    memo->lhs_pos = lhs_pos;
    memo->next = NULL;
    if (!state->first_memo) {
        state->first_memo = memo;
    }
    else {
        state->last_memo->next = memo;
    }
    state->last_memo = memo;

    return COMPILE_OK;
}

static int compile_massign0(rb_iseq_t *iseq, LINK_ANCHOR *const pre, LINK_ANCHOR *const rhs, LINK_ANCHOR *const lhs, LINK_ANCHOR *const post, const NODE *const node, struct masgn_state *state, int popped);

static int
compile_massign_lhs(rb_iseq_t *iseq, LINK_ANCHOR *const pre, LINK_ANCHOR *const rhs, LINK_ANCHOR *const lhs, LINK_ANCHOR *const post, const NODE *const node, struct masgn_state *state, int lhs_pos)
{
    switch (nd_type(node)) {
      case NODE_ATTRASGN: {
        INSN *iobj;
        const NODE *line_node = node;

        CHECK(COMPILE_POPPED(pre, "masgn lhs (NODE_ATTRASGN)", node));

        bool safenav_call = false;
        LINK_ELEMENT *insn_element = LAST_ELEMENT(pre);
        iobj = (INSN *)get_prev_insn((INSN *)insn_element); /* send insn */
        ASSUME(iobj);
        ELEM_REMOVE(insn_element);
        if (!IS_INSN_ID(iobj, send)) {
            safenav_call = true;
            iobj = (INSN *)get_prev_insn(iobj);
            ELEM_INSERT_NEXT(&iobj->link, insn_element);
        }
        (pre->last = iobj->link.prev)->next = 0;

        const struct rb_callinfo *ci = (struct rb_callinfo *)OPERAND_AT(iobj, 0);
        int argc = vm_ci_argc(ci) + 1;
        ci = ci_argc_set(iseq, ci, argc);
        OPERAND_AT(iobj, 0) = (VALUE)ci;
        RB_OBJ_WRITTEN(iseq, Qundef, ci);

        if (argc == 1) {
            ADD_INSN(lhs, line_node, swap);
        }
        else {
            ADD_INSN1(lhs, line_node, topn, INT2FIX(argc));
        }

        if (!add_masgn_lhs_node(state, lhs_pos, line_node, argc, (INSN *)LAST_ELEMENT(lhs))) {
            return COMPILE_NG;
        }

        iobj->link.prev = lhs->last;
        lhs->last->next = &iobj->link;
        for (lhs->last = &iobj->link; lhs->last->next; lhs->last = lhs->last->next);
        if (vm_ci_flag(ci) & VM_CALL_ARGS_SPLAT) {
            int argc = vm_ci_argc(ci);
            bool dupsplat = false;
            ci = ci_argc_set(iseq, ci, argc - 1);
            if (!(vm_ci_flag(ci) & VM_CALL_ARGS_SPLAT_MUT)) {
                /* Given h[*a], _ = ary
                 * setup_args sets VM_CALL_ARGS_SPLAT and not VM_CALL_ARGS_SPLAT_MUT
                 * `a` must be dupped, because it will be appended with ary[0]
                 * Since you are dupping `a`, you can set VM_CALL_ARGS_SPLAT_MUT
                 */
                dupsplat = true;
                ci = ci_flag_set(iseq, ci, VM_CALL_ARGS_SPLAT_MUT);
            }
            OPERAND_AT(iobj, 0) = (VALUE)ci;
            RB_OBJ_WRITTEN(iseq, Qundef, iobj);

            /* Given: h[*a], h[*b, 1] = ary
             *  h[*a] uses splatarray false and does not set VM_CALL_ARGS_SPLAT_MUT,
             *    so this uses splatarray true on a to dup it before using pushtoarray
             *  h[*b, 1] uses splatarray true and sets VM_CALL_ARGS_SPLAT_MUT,
             *    so you can use pushtoarray directly
             */
            int line_no = nd_line(line_node);
            int node_id = nd_node_id(line_node);

            if (dupsplat) {
                INSERT_BEFORE_INSN(iobj, line_no, node_id, swap);
                INSERT_BEFORE_INSN1(iobj, line_no, node_id, splatarray, Qtrue);
                INSERT_BEFORE_INSN(iobj, line_no, node_id, swap);
            }
            INSERT_BEFORE_INSN1(iobj, line_no, node_id, pushtoarray, INT2FIX(1));
        }
        if (!safenav_call) {
            ADD_INSN(lhs, line_node, pop);
            if (argc != 1) {
                ADD_INSN(lhs, line_node, pop);
            }
        }
        for (int i=0; i < argc; i++) {
            ADD_INSN(post, line_node, pop);
        }
        break;
      }
      case NODE_MASGN: {
        DECL_ANCHOR(nest_rhs);
        INIT_ANCHOR(nest_rhs);
        DECL_ANCHOR(nest_lhs);
        INIT_ANCHOR(nest_lhs);

        int prev_level = state->lhs_level;
        bool prev_nested = state->nested;
        state->nested = 1;
        state->lhs_level = lhs_pos - 1;
        CHECK(compile_massign0(iseq, pre, nest_rhs, nest_lhs, post, node, state, 1));
        state->lhs_level = prev_level;
        state->nested = prev_nested;

        ADD_SEQ(lhs, nest_rhs);
        ADD_SEQ(lhs, nest_lhs);
        break;
      }
      case NODE_CDECL:
        if (!RNODE_CDECL(node)->nd_vid) {
            /* Special handling only needed for expr::C, not for C */
            INSN *iobj;

            CHECK(COMPILE_POPPED(pre, "masgn lhs (NODE_CDECL)", node));

            LINK_ELEMENT *insn_element = LAST_ELEMENT(pre);
            iobj = (INSN *)insn_element; /* setconstant insn */
            ELEM_REMOVE((LINK_ELEMENT *)get_prev_insn((INSN *)get_prev_insn(iobj)));
            ELEM_REMOVE((LINK_ELEMENT *)get_prev_insn(iobj));
            ELEM_REMOVE(insn_element);
            pre->last = iobj->link.prev;
            ADD_ELEM(lhs, (LINK_ELEMENT *)iobj);

            if (!add_masgn_lhs_node(state, lhs_pos, node, 1, (INSN *)LAST_ELEMENT(lhs))) {
                return COMPILE_NG;
            }

            ADD_INSN(post, node, pop);
            break;
        }
        /* Fallthrough */
      default: {
        DECL_ANCHOR(anchor);
        INIT_ANCHOR(anchor);
        CHECK(COMPILE_POPPED(anchor, "masgn lhs", node));
        ELEM_REMOVE(FIRST_ELEMENT(anchor));
        ADD_SEQ(lhs, anchor);
      }
    }

    return COMPILE_OK;
}

static int
compile_massign_opt_lhs(rb_iseq_t *iseq, LINK_ANCHOR *const ret, const NODE *lhsn)
{
    if (lhsn) {
        CHECK(compile_massign_opt_lhs(iseq, ret, RNODE_LIST(lhsn)->nd_next));
        CHECK(compile_massign_lhs(iseq, ret, ret, ret, ret, RNODE_LIST(lhsn)->nd_head, NULL, 0));
    }
    return COMPILE_OK;
}

static int
compile_massign_opt(rb_iseq_t *iseq, LINK_ANCHOR *const ret,
                    const NODE *rhsn, const NODE *orig_lhsn)
{
    VALUE mem[64];
    const int memsize = numberof(mem);
    int memindex = 0;
    int llen = 0, rlen = 0;
    int i;
    const NODE *lhsn = orig_lhsn;

#define MEMORY(v) { \
    int i; \
    if (memindex == memsize) return 0; \
    for (i=0; i<memindex; i++) { \
        if (mem[i] == (v)) return 0; \
    } \
    mem[memindex++] = (v); \
}

    if (rhsn == 0 || !nd_type_p(rhsn, NODE_LIST)) {
        return 0;
    }

    while (lhsn) {
        const NODE *ln = RNODE_LIST(lhsn)->nd_head;
        switch (nd_type(ln)) {
          case NODE_LASGN:
          case NODE_DASGN:
          case NODE_IASGN:
          case NODE_CVASGN:
            MEMORY(get_nd_vid(ln));
            break;
          default:
            return 0;
        }
        lhsn = RNODE_LIST(lhsn)->nd_next;
        llen++;
    }

    while (rhsn) {
        if (llen <= rlen) {
            NO_CHECK(COMPILE_POPPED(ret, "masgn val (popped)", RNODE_LIST(rhsn)->nd_head));
        }
        else {
            NO_CHECK(COMPILE(ret, "masgn val", RNODE_LIST(rhsn)->nd_head));
        }
        rhsn = RNODE_LIST(rhsn)->nd_next;
        rlen++;
    }

    if (llen > rlen) {
        for (i=0; i<llen-rlen; i++) {
            ADD_INSN(ret, orig_lhsn, putnil);
        }
    }

    compile_massign_opt_lhs(iseq, ret, orig_lhsn);
    return 1;
}

static int
compile_massign0(rb_iseq_t *iseq, LINK_ANCHOR *const pre, LINK_ANCHOR *const rhs, LINK_ANCHOR *const lhs, LINK_ANCHOR *const post, const NODE *const node, struct masgn_state *state, int popped)
{
    const NODE *rhsn = RNODE_MASGN(node)->nd_value;
    const NODE *splatn = RNODE_MASGN(node)->nd_args;
    const NODE *lhsn = RNODE_MASGN(node)->nd_head;
    const NODE *lhsn_count = lhsn;
    int lhs_splat = (splatn && NODE_NAMED_REST_P(splatn)) ? 1 : 0;

    int llen = 0;
    int lpos = 0;

    while (lhsn_count) {
        llen++;
        lhsn_count = RNODE_LIST(lhsn_count)->nd_next;
    }
    while (lhsn) {
        CHECK(compile_massign_lhs(iseq, pre, rhs, lhs, post, RNODE_LIST(lhsn)->nd_head, state, (llen - lpos) + lhs_splat + state->lhs_level));
        lpos++;
        lhsn = RNODE_LIST(lhsn)->nd_next;
    }

    if (lhs_splat) {
        if (nd_type_p(splatn, NODE_POSTARG)) {
            /*a, b, *r, p1, p2 */
            const NODE *postn = RNODE_POSTARG(splatn)->nd_2nd;
            const NODE *restn = RNODE_POSTARG(splatn)->nd_1st;
            int plen = (int)RNODE_LIST(postn)->as.nd_alen;
            int ppos = 0;
            int flag = 0x02 | (NODE_NAMED_REST_P(restn) ? 0x01 : 0x00);

            ADD_INSN2(lhs, splatn, expandarray, INT2FIX(plen), INT2FIX(flag));

            if (NODE_NAMED_REST_P(restn)) {
                CHECK(compile_massign_lhs(iseq, pre, rhs, lhs, post, restn, state, 1 + plen + state->lhs_level));
            }
            while (postn) {
                CHECK(compile_massign_lhs(iseq, pre, rhs, lhs, post, RNODE_LIST(postn)->nd_head, state, (plen - ppos) + state->lhs_level));
                ppos++;
                postn = RNODE_LIST(postn)->nd_next;
            }
        }
        else {
            /* a, b, *r */
            CHECK(compile_massign_lhs(iseq, pre, rhs, lhs, post, splatn, state, 1 + state->lhs_level));
        }
    }

    if (!state->nested) {
        NO_CHECK(COMPILE(rhs, "normal masgn rhs", rhsn));
    }

    if (!popped) {
        ADD_INSN(rhs, node, dup);
    }
    ADD_INSN2(rhs, node, expandarray, INT2FIX(llen), INT2FIX(lhs_splat));
    return COMPILE_OK;
}

static int
compile_massign(rb_iseq_t *iseq, LINK_ANCHOR *const ret, const NODE *const node, int popped)
{
    if (!popped || RNODE_MASGN(node)->nd_args || !compile_massign_opt(iseq, ret, RNODE_MASGN(node)->nd_value, RNODE_MASGN(node)->nd_head)) {
        struct masgn_state state;
        state.lhs_level = popped ? 0 : 1;
        state.nested = 0;
        state.num_args = 0;
        state.first_memo = NULL;
        state.last_memo = NULL;

        DECL_ANCHOR(pre);
        INIT_ANCHOR(pre);
        DECL_ANCHOR(rhs);
        INIT_ANCHOR(rhs);
        DECL_ANCHOR(lhs);
        INIT_ANCHOR(lhs);
        DECL_ANCHOR(post);
        INIT_ANCHOR(post);
        int ok = compile_massign0(iseq, pre, rhs, lhs, post, node, &state, popped);

        struct masgn_lhs_node *memo = state.first_memo, *tmp_memo;
        while (memo) {
            VALUE topn_arg = INT2FIX((state.num_args - memo->argn) + memo->lhs_pos);
            for (int i = 0; i < memo->num_args; i++) {
                INSERT_BEFORE_INSN1(memo->before_insn, nd_line(memo->line_node), nd_node_id(memo->line_node), topn, topn_arg);
            }
            tmp_memo = memo->next;
            free(memo);
            memo = tmp_memo;
        }
        CHECK(ok);

        ADD_SEQ(ret, pre);
        ADD_SEQ(ret, rhs);
        ADD_SEQ(ret, lhs);
        if (!popped && state.num_args >= 1) {
            /* make sure rhs array is returned before popping */
            ADD_INSN1(ret, node, setn, INT2FIX(state.num_args));
        }
        ADD_SEQ(ret, post);
    }
    return COMPILE_OK;
}

static VALUE
collect_const_segments(rb_iseq_t *iseq, const NODE *node)
{
    VALUE arr = rb_ary_new();
    for (;;) {
        switch (nd_type(node)) {
          case NODE_CONST:
            rb_ary_unshift(arr, ID2SYM(RNODE_CONST(node)->nd_vid));
            return arr;
          case NODE_COLON3:
            rb_ary_unshift(arr, ID2SYM(RNODE_COLON3(node)->nd_mid));
            rb_ary_unshift(arr, ID2SYM(idNULL));
            return arr;
          case NODE_COLON2:
            rb_ary_unshift(arr, ID2SYM(RNODE_COLON2(node)->nd_mid));
            node = RNODE_COLON2(node)->nd_head;
            break;
          default:
            return Qfalse;
        }
    }
}

static int
compile_const_prefix(rb_iseq_t *iseq, const NODE *const node,
                     LINK_ANCHOR *const pref, LINK_ANCHOR *const body)
{
    switch (nd_type(node)) {
      case NODE_CONST:
        debugi("compile_const_prefix - colon", RNODE_CONST(node)->nd_vid);
        ADD_INSN1(body, node, putobject, Qtrue);
        ADD_INSN1(body, node, getconstant, ID2SYM(RNODE_CONST(node)->nd_vid));
        break;
      case NODE_COLON3:
        debugi("compile_const_prefix - colon3", RNODE_COLON3(node)->nd_mid);
        ADD_INSN(body, node, pop);
        ADD_INSN1(body, node, putobject, rb_cObject);
        ADD_INSN1(body, node, putobject, Qtrue);
        ADD_INSN1(body, node, getconstant, ID2SYM(RNODE_COLON3(node)->nd_mid));
        break;
      case NODE_COLON2:
        CHECK(compile_const_prefix(iseq, RNODE_COLON2(node)->nd_head, pref, body));
        debugi("compile_const_prefix - colon2", RNODE_COLON2(node)->nd_mid);
        ADD_INSN1(body, node, putobject, Qfalse);
        ADD_INSN1(body, node, getconstant, ID2SYM(RNODE_COLON2(node)->nd_mid));
        break;
      default:
        CHECK(COMPILE(pref, "const colon2 prefix", node));
        break;
    }
    return COMPILE_OK;
}

static int
compile_cpath(LINK_ANCHOR *const ret, rb_iseq_t *iseq, const NODE *cpath)
{
    if (nd_type_p(cpath, NODE_COLON3)) {
        /* toplevel class ::Foo */
        ADD_INSN1(ret, cpath, putobject, rb_cObject);
        return VM_DEFINECLASS_FLAG_SCOPED;
    }
    else if (nd_type_p(cpath, NODE_COLON2) && RNODE_COLON2(cpath)->nd_head) {
        /* Bar::Foo */
        NO_CHECK(COMPILE(ret, "nd_else->nd_head", RNODE_COLON2(cpath)->nd_head));
        return VM_DEFINECLASS_FLAG_SCOPED;
    }
    else {
        /* class at cbase Foo */
        ADD_INSN1(ret, cpath, putspecialobject,
                  INT2FIX(VM_SPECIAL_OBJECT_CONST_BASE));
        return 0;
    }
}

static inline int
private_recv_p(const NODE *node)
{
    NODE *recv = get_nd_recv(node);
    if (recv && nd_type_p(recv, NODE_SELF)) {
        return RNODE_SELF(recv)->nd_state != 0;
    }
    return 0;
}

static void
defined_expr(rb_iseq_t *iseq, LINK_ANCHOR *const ret,
             const NODE *const node, LABEL **lfinish, VALUE needstr, bool ignore);

static int
compile_call(rb_iseq_t *iseq, LINK_ANCHOR *const ret, const NODE *const node, const enum node_type type, const NODE *const line_node, int popped, bool assume_receiver);

static void
defined_expr0(rb_iseq_t *iseq, LINK_ANCHOR *const ret,
              const NODE *const node, LABEL **lfinish, VALUE needstr,
              bool keep_result)
{
    enum defined_type expr_type = DEFINED_NOT_DEFINED;
    enum node_type type;
    const int line = nd_line(node);
    const NODE *line_node = node;

    switch (type = nd_type(node)) {

        /* easy literals */
      case NODE_NIL:
        expr_type = DEFINED_NIL;
        break;
      case NODE_SELF:
        expr_type = DEFINED_SELF;
        break;
      case NODE_TRUE:
        expr_type = DEFINED_TRUE;
        break;
      case NODE_FALSE:
        expr_type = DEFINED_FALSE;
        break;

      case NODE_HASH:
      case NODE_LIST:{
        const NODE *vals = (nd_type(node) == NODE_HASH) ? RNODE_HASH(node)->nd_head : node;

        if (vals) {
            do {
                if (RNODE_LIST(vals)->nd_head) {
                    defined_expr0(iseq, ret, RNODE_LIST(vals)->nd_head, lfinish, Qfalse, false);

                    if (!lfinish[1]) {
                        lfinish[1] = NEW_LABEL(line);
                    }
                    ADD_INSNL(ret, line_node, branchunless, lfinish[1]);
                }
            } while ((vals = RNODE_LIST(vals)->nd_next) != NULL);
        }
      }
        /* fall through */
      case NODE_STR:
      case NODE_SYM:
      case NODE_REGX:
      case NODE_LINE:
      case NODE_FILE:
      case NODE_ENCODING:
      case NODE_INTEGER:
      case NODE_FLOAT:
      case NODE_RATIONAL:
      case NODE_IMAGINARY:
      case NODE_ZLIST:
      case NODE_AND:
      case NODE_OR:
      default:
        expr_type = DEFINED_EXPR;
        break;

      case NODE_SPLAT:
        defined_expr0(iseq, ret, RNODE_LIST(node)->nd_head, lfinish, Qfalse, false);
        if (!lfinish[1]) {
            lfinish[1] = NEW_LABEL(line);
        }
        ADD_INSNL(ret, line_node, branchunless, lfinish[1]);
        expr_type = DEFINED_EXPR;
        break;

        /* variables */
      case NODE_LVAR:
      case NODE_DVAR:
        expr_type = DEFINED_LVAR;
        break;

#define PUSH_VAL(type) (needstr == Qfalse ? Qtrue : rb_iseq_defined_string(type))
      case NODE_IVAR:
        ADD_INSN3(ret, line_node, definedivar,
                  ID2SYM(RNODE_IVAR(node)->nd_vid), get_ivar_ic_value(iseq,RNODE_IVAR(node)->nd_vid), PUSH_VAL(DEFINED_IVAR));
        return;

      case NODE_GVAR:
        ADD_INSN(ret, line_node, putnil);
        ADD_INSN3(ret, line_node, defined, INT2FIX(DEFINED_GVAR),
                  ID2SYM(RNODE_GVAR(node)->nd_vid), PUSH_VAL(DEFINED_GVAR));
        return;

      case NODE_CVAR:
        ADD_INSN(ret, line_node, putnil);
        ADD_INSN3(ret, line_node, defined, INT2FIX(DEFINED_CVAR),
                  ID2SYM(RNODE_CVAR(node)->nd_vid), PUSH_VAL(DEFINED_CVAR));
        return;

      case NODE_CONST:
        ADD_INSN(ret, line_node, putnil);
        ADD_INSN3(ret, line_node, defined, INT2FIX(DEFINED_CONST),
                  ID2SYM(RNODE_CONST(node)->nd_vid), PUSH_VAL(DEFINED_CONST));
        return;
      case NODE_COLON2:
        if (!lfinish[1]) {
            lfinish[1] = NEW_LABEL(line);
        }
        defined_expr0(iseq, ret, RNODE_COLON2(node)->nd_head, lfinish, Qfalse, false);
        ADD_INSNL(ret, line_node, branchunless, lfinish[1]);
        NO_CHECK(COMPILE(ret, "defined/colon2#nd_head", RNODE_COLON2(node)->nd_head));

        if (rb_is_const_id(RNODE_COLON2(node)->nd_mid)) {
            ADD_INSN3(ret, line_node, defined, INT2FIX(DEFINED_CONST_FROM),
                    ID2SYM(RNODE_COLON2(node)->nd_mid), PUSH_VAL(DEFINED_CONST));
        }
        else {
            ADD_INSN3(ret, line_node, defined, INT2FIX(DEFINED_METHOD),
                    ID2SYM(RNODE_COLON2(node)->nd_mid), PUSH_VAL(DEFINED_METHOD));
        }
        return;
      case NODE_COLON3:
        ADD_INSN1(ret, line_node, putobject, rb_cObject);
        ADD_INSN3(ret, line_node, defined,
                  INT2FIX(DEFINED_CONST_FROM), ID2SYM(RNODE_COLON3(node)->nd_mid), PUSH_VAL(DEFINED_CONST));
        return;

        /* method dispatch */
      case NODE_CALL:
      case NODE_OPCALL:
      case NODE_VCALL:
      case NODE_FCALL:
      case NODE_ATTRASGN:{
        const int explicit_receiver =
            (type == NODE_CALL || type == NODE_OPCALL ||
             (type == NODE_ATTRASGN && !private_recv_p(node)));

        if (get_nd_args(node) || explicit_receiver) {
            if (!lfinish[1]) {
                lfinish[1] = NEW_LABEL(line);
            }
            if (!lfinish[2]) {
                lfinish[2] = NEW_LABEL(line);
            }
        }
        if (get_nd_args(node)) {
            defined_expr0(iseq, ret, get_nd_args(node), lfinish, Qfalse, false);
            ADD_INSNL(ret, line_node, branchunless, lfinish[1]);
        }
        if (explicit_receiver) {
            defined_expr0(iseq, ret, get_nd_recv(node), lfinish, Qfalse, true);
            switch (nd_type(get_nd_recv(node))) {
              case NODE_CALL:
              case NODE_OPCALL:
              case NODE_VCALL:
              case NODE_FCALL:
              case NODE_ATTRASGN:
                ADD_INSNL(ret, line_node, branchunless, lfinish[2]);
                compile_call(iseq, ret, get_nd_recv(node), nd_type(get_nd_recv(node)), line_node, 0, true);
                break;
              default:
                ADD_INSNL(ret, line_node, branchunless, lfinish[1]);
                NO_CHECK(COMPILE(ret, "defined/recv", get_nd_recv(node)));
                break;
            }
            if (keep_result) {
                ADD_INSN(ret, line_node, dup);
            }
            ADD_INSN3(ret, line_node, defined, INT2FIX(DEFINED_METHOD),
                      ID2SYM(get_node_call_nd_mid(node)), PUSH_VAL(DEFINED_METHOD));
        }
        else {
            ADD_INSN(ret, line_node, putself);
            if (keep_result) {
                ADD_INSN(ret, line_node, dup);
            }
            ADD_INSN3(ret, line_node, defined, INT2FIX(DEFINED_FUNC),
                      ID2SYM(get_node_call_nd_mid(node)), PUSH_VAL(DEFINED_METHOD));
        }
        return;
      }

      case NODE_YIELD:
        ADD_INSN(ret, line_node, putnil);
        ADD_INSN3(ret, line_node, defined, INT2FIX(DEFINED_YIELD), 0,
                  PUSH_VAL(DEFINED_YIELD));
        iseq_set_use_block(ISEQ_BODY(iseq)->local_iseq);
        return;

      case NODE_BACK_REF:
      case NODE_NTH_REF:
        ADD_INSN(ret, line_node, putnil);
        ADD_INSN3(ret, line_node, defined, INT2FIX(DEFINED_REF),
                  INT2FIX((RNODE_BACK_REF(node)->nd_nth << 1) | (type == NODE_BACK_REF)),
                  PUSH_VAL(DEFINED_GVAR));
        return;

      case NODE_SUPER:
      case NODE_ZSUPER:
        ADD_INSN(ret, line_node, putnil);
        ADD_INSN3(ret, line_node, defined, INT2FIX(DEFINED_ZSUPER), 0,
                  PUSH_VAL(DEFINED_ZSUPER));
        return;

#undef PUSH_VAL
      case NODE_OP_ASGN1:
      case NODE_OP_ASGN2:
      case NODE_OP_ASGN_OR:
      case NODE_OP_ASGN_AND:
      case NODE_MASGN:
      case NODE_LASGN:
      case NODE_DASGN:
      case NODE_GASGN:
      case NODE_IASGN:
      case NODE_CDECL:
      case NODE_CVASGN:
      case NODE_OP_CDECL:
        expr_type = DEFINED_ASGN;
        break;
    }

    RUBY_ASSERT(expr_type != DEFINED_NOT_DEFINED);

    if (needstr != Qfalse) {
        VALUE str = rb_iseq_defined_string(expr_type);
        ADD_INSN1(ret, line_node, putobject, str);
    }
    else {
        ADD_INSN1(ret, line_node, putobject, Qtrue);
    }
}

static void
build_defined_rescue_iseq(rb_iseq_t *iseq, LINK_ANCHOR *const ret, const void *unused)
{
    ADD_SYNTHETIC_INSN(ret, 0, -1, putnil);
    iseq_set_exception_local_table(iseq);
}

static void
defined_expr(rb_iseq_t *iseq, LINK_ANCHOR *const ret,
             const NODE *const node, LABEL **lfinish, VALUE needstr, bool ignore)
{
    LINK_ELEMENT *lcur = ret->last;
    defined_expr0(iseq, ret, node, lfinish, needstr, false);
    if (lfinish[1]) {
        int line = nd_line(node);
        LABEL *lstart = NEW_LABEL(line);
        LABEL *lend = NEW_LABEL(line);
        const rb_iseq_t *rescue;
        struct rb_iseq_new_with_callback_callback_func *ifunc =
            rb_iseq_new_with_callback_new_callback(build_defined_rescue_iseq, NULL);
        rescue = NEW_CHILD_ISEQ_WITH_CALLBACK(ifunc,
                                              rb_str_concat(rb_str_new2("defined guard in "),
                                                            ISEQ_BODY(iseq)->location.label),
                                              ISEQ_TYPE_RESCUE, 0);
        lstart->rescued = LABEL_RESCUE_BEG;
        lend->rescued = LABEL_RESCUE_END;
        APPEND_LABEL(ret, lcur, lstart);
        ADD_LABEL(ret, lend);
        if (!ignore) {
            ADD_CATCH_ENTRY(CATCH_TYPE_RESCUE, lstart, lend, rescue, lfinish[1]);
        }
    }
}

static int
compile_defined_expr(rb_iseq_t *iseq, LINK_ANCHOR *const ret, const NODE *const node, VALUE needstr, bool ignore)
{
    const int line = nd_line(node);
    const NODE *line_node = node;
    if (!RNODE_DEFINED(node)->nd_head) {
        VALUE str = rb_iseq_defined_string(DEFINED_NIL);
        ADD_INSN1(ret, line_node, putobject, str);
    }
    else {
        LABEL *lfinish[3];
        LINK_ELEMENT *last = ret->last;
        lfinish[0] = NEW_LABEL(line);
        lfinish[1] = 0;
        lfinish[2] = 0;
        defined_expr(iseq, ret, RNODE_DEFINED(node)->nd_head, lfinish, needstr, ignore);
        if (lfinish[1]) {
            ELEM_INSERT_NEXT(last, &new_insn_body(iseq, nd_line(line_node), nd_node_id(line_node), BIN(putnil), 0)->link);
            ADD_INSN(ret, line_node, swap);
            if (lfinish[2]) {
                ADD_LABEL(ret, lfinish[2]);
            }
            ADD_INSN(ret, line_node, pop);
            ADD_LABEL(ret, lfinish[1]);
        }
        ADD_LABEL(ret, lfinish[0]);
    }
    return COMPILE_OK;
}

static VALUE
make_name_for_block(const rb_iseq_t *orig_iseq)
{
    int level = 1;
    const rb_iseq_t *iseq = orig_iseq;

    if (ISEQ_BODY(orig_iseq)->parent_iseq != 0) {
        while (ISEQ_BODY(orig_iseq)->local_iseq != iseq) {
            if (ISEQ_BODY(iseq)->type == ISEQ_TYPE_BLOCK) {
                level++;
            }
            iseq = ISEQ_BODY(iseq)->parent_iseq;
        }
    }

    if (level == 1) {
        return rb_sprintf("block in %"PRIsVALUE, ISEQ_BODY(iseq)->location.label);
    }
    else {
        return rb_sprintf("block (%d levels) in %"PRIsVALUE, level, ISEQ_BODY(iseq)->location.label);
    }
}

static void
push_ensure_entry(rb_iseq_t *iseq,
                  struct iseq_compile_data_ensure_node_stack *enl,
                  struct ensure_range *er, const void *const node)
{
    enl->ensure_node = node;
    enl->prev = ISEQ_COMPILE_DATA(iseq)->ensure_node_stack;	/* prev */
    enl->erange = er;
    ISEQ_COMPILE_DATA(iseq)->ensure_node_stack = enl;
}

static void
add_ensure_range(rb_iseq_t *iseq, struct ensure_range *erange,
                 LABEL *lstart, LABEL *lend)
{
    struct ensure_range *ne =
        compile_data_alloc(iseq, sizeof(struct ensure_range));

    while (erange->next != 0) {
        erange = erange->next;
    }
    ne->next = 0;
    ne->begin = lend;
    ne->end = erange->end;
    erange->end = lstart;

    erange->next = ne;
}

static bool
can_add_ensure_iseq(const rb_iseq_t *iseq)
{
    struct iseq_compile_data_ensure_node_stack *e;
    if (ISEQ_COMPILE_DATA(iseq)->in_rescue && (e = ISEQ_COMPILE_DATA(iseq)->ensure_node_stack) != NULL) {
        while (e) {
            if (e->ensure_node) return false;
            e = e->prev;
        }
    }
    return true;
}

static void
add_ensure_iseq(LINK_ANCHOR *const ret, rb_iseq_t *iseq, int is_return)
{
    RUBY_ASSERT(can_add_ensure_iseq(iseq));

    struct iseq_compile_data_ensure_node_stack *enlp =
        ISEQ_COMPILE_DATA(iseq)->ensure_node_stack;
    struct iseq_compile_data_ensure_node_stack *prev_enlp = enlp;
    DECL_ANCHOR(ensure);

    INIT_ANCHOR(ensure);
    while (enlp) {
        if (enlp->erange != NULL) {
            DECL_ANCHOR(ensure_part);
            LABEL *lstart = NEW_LABEL(0);
            LABEL *lend = NEW_LABEL(0);
            INIT_ANCHOR(ensure_part);

            add_ensure_range(iseq, enlp->erange, lstart, lend);

            ISEQ_COMPILE_DATA(iseq)->ensure_node_stack = enlp->prev;
            ADD_LABEL(ensure_part, lstart);
            NO_CHECK(COMPILE_POPPED(ensure_part, "ensure part", enlp->ensure_node));
            ADD_LABEL(ensure_part, lend);
            ADD_SEQ(ensure, ensure_part);
        }
        else {
            if (!is_return) {
                break;
            }
        }
        enlp = enlp->prev;
    }
    ISEQ_COMPILE_DATA(iseq)->ensure_node_stack = prev_enlp;
    ADD_SEQ(ret, ensure);
}

#if RUBY_DEBUG
static int
check_keyword(const NODE *node)
{
    /* This check is essentially a code clone of compile_keyword_arg. */

    if (nd_type_p(node, NODE_LIST)) {
        while (RNODE_LIST(node)->nd_next) {
            node = RNODE_LIST(node)->nd_next;
        }
        node = RNODE_LIST(node)->nd_head;
    }

    return keyword_node_p(node);
}
#endif

static bool
keyword_node_single_splat_p(NODE *kwnode)
{
    RUBY_ASSERT(keyword_node_p(kwnode));

    NODE *node = RNODE_HASH(kwnode)->nd_head;
    return RNODE_LIST(node)->nd_head == NULL &&
           RNODE_LIST(RNODE_LIST(node)->nd_next)->nd_next == NULL;
}

static void
compile_single_keyword_splat_mutable(rb_iseq_t *iseq, LINK_ANCHOR *const args, const NODE *argn,
                                     NODE *kwnode, unsigned int *flag_ptr)
{
    *flag_ptr |= VM_CALL_KW_SPLAT_MUT;
    ADD_INSN1(args, argn, putspecialobject, INT2FIX(VM_SPECIAL_OBJECT_VMCORE));
    ADD_INSN1(args, argn, newhash, INT2FIX(0));
    compile_hash(iseq, args, kwnode, TRUE, FALSE);
    ADD_SEND(args, argn, id_core_hash_merge_kwd, INT2FIX(2));
}

#define SPLATARRAY_FALSE 0
#define SPLATARRAY_TRUE 1
#define DUP_SINGLE_KW_SPLAT 2

static int
setup_args_core(rb_iseq_t *iseq, LINK_ANCHOR *const args, const NODE *argn,
                unsigned int *dup_rest, unsigned int *flag_ptr, struct rb_callinfo_kwarg **kwarg_ptr)
{
    if (!argn) return 0;

    NODE *kwnode = NULL;

    switch (nd_type(argn)) {
      case NODE_LIST: {
        // f(x, y, z)
        int len = compile_args(iseq, args, argn, &kwnode);
        RUBY_ASSERT(flag_ptr == NULL || (*flag_ptr & VM_CALL_ARGS_SPLAT) == 0);

        if (kwnode) {
            if (compile_keyword_arg(iseq, args, kwnode, kwarg_ptr, flag_ptr)) {
                len -= 1;
            }
            else {
                if (keyword_node_single_splat_p(kwnode) && (*dup_rest & DUP_SINGLE_KW_SPLAT)) {
                    compile_single_keyword_splat_mutable(iseq, args, argn, kwnode, flag_ptr);
                }
                else {
                    compile_hash(iseq, args, kwnode, TRUE, FALSE);
                }
            }
        }

        return len;
      }
      case NODE_SPLAT: {
        // f(*a)
        NO_CHECK(COMPILE(args, "args (splat)", RNODE_SPLAT(argn)->nd_head));
        ADD_INSN1(args, argn, splatarray, RBOOL(*dup_rest & SPLATARRAY_TRUE));
        if (*dup_rest & SPLATARRAY_TRUE) *dup_rest &= ~SPLATARRAY_TRUE;
        if (flag_ptr) *flag_ptr |= VM_CALL_ARGS_SPLAT;
        RUBY_ASSERT(flag_ptr == NULL || (*flag_ptr & VM_CALL_KW_SPLAT) == 0);
        return 1;
      }
      case NODE_ARGSCAT: {
        if (flag_ptr) *flag_ptr |= VM_CALL_ARGS_SPLAT;
        int argc = setup_args_core(iseq, args, RNODE_ARGSCAT(argn)->nd_head, dup_rest, NULL, NULL);
        bool args_pushed = false;

        if (nd_type_p(RNODE_ARGSCAT(argn)->nd_body, NODE_LIST)) {
            int rest_len = compile_args(iseq, args, RNODE_ARGSCAT(argn)->nd_body, &kwnode);
            if (kwnode) rest_len--;
            ADD_INSN1(args, argn, pushtoarray, INT2FIX(rest_len));
            args_pushed = true;
        }
        else {
            RUBY_ASSERT(!check_keyword(RNODE_ARGSCAT(argn)->nd_body));
            NO_CHECK(COMPILE(args, "args (cat: splat)", RNODE_ARGSCAT(argn)->nd_body));
        }

        if (nd_type_p(RNODE_ARGSCAT(argn)->nd_head, NODE_LIST)) {
            ADD_INSN1(args, argn, splatarray, RBOOL(*dup_rest & SPLATARRAY_TRUE));
            if (*dup_rest & SPLATARRAY_TRUE) *dup_rest &= ~SPLATARRAY_TRUE;
            argc += 1;
        }
        else if (!args_pushed) {
            ADD_INSN(args, argn, concattoarray);
        }

        // f(..., *a, ..., k1:1, ...) #=> f(..., *[*a, ...], **{k1:1, ...})
        if (kwnode) {
            // kwsplat
            *flag_ptr |= VM_CALL_KW_SPLAT;
            compile_hash(iseq, args, kwnode, TRUE, FALSE);
            argc += 1;
        }

        return argc;
      }
      case NODE_ARGSPUSH: {
        if (flag_ptr) *flag_ptr |= VM_CALL_ARGS_SPLAT;
        int argc = setup_args_core(iseq, args, RNODE_ARGSPUSH(argn)->nd_head, dup_rest, NULL, NULL);

        if (nd_type_p(RNODE_ARGSPUSH(argn)->nd_body, NODE_LIST)) {
            int rest_len = compile_args(iseq, args, RNODE_ARGSPUSH(argn)->nd_body, &kwnode);
            if (kwnode) rest_len--;
            ADD_INSN1(args, argn, newarray, INT2FIX(rest_len));
            ADD_INSN1(args, argn, pushtoarray, INT2FIX(1));
        }
        else {
            if (keyword_node_p(RNODE_ARGSPUSH(argn)->nd_body)) {
                kwnode = RNODE_ARGSPUSH(argn)->nd_body;
            }
            else {
                NO_CHECK(COMPILE(args, "args (cat: splat)", RNODE_ARGSPUSH(argn)->nd_body));
                ADD_INSN1(args, argn, pushtoarray, INT2FIX(1));
            }
        }

        if (kwnode) {
            // f(*a, k:1)
            *flag_ptr |= VM_CALL_KW_SPLAT;
            if (!keyword_node_single_splat_p(kwnode)) {
                *flag_ptr |= VM_CALL_KW_SPLAT_MUT;
                compile_hash(iseq, args, kwnode, TRUE, FALSE);
            }
            else if (*dup_rest & DUP_SINGLE_KW_SPLAT) {
                compile_single_keyword_splat_mutable(iseq, args, argn, kwnode, flag_ptr);
            }
            else {
                compile_hash(iseq, args, kwnode, TRUE, FALSE);
            }
            argc += 1;
        }

        return argc;
      }
      default: {
        UNKNOWN_NODE("setup_arg", argn, Qnil);
      }
    }
}

static void
setup_args_splat_mut(unsigned int *flag, int dup_rest, int initial_dup_rest)
{
    if ((*flag & VM_CALL_ARGS_SPLAT) && dup_rest != initial_dup_rest) {
        *flag |= VM_CALL_ARGS_SPLAT_MUT;
    }
}

static bool
setup_args_dup_rest_p(const NODE *argn)
{
    switch(nd_type(argn)) {
      case NODE_LVAR:
      case NODE_DVAR:
      case NODE_GVAR:
      case NODE_IVAR:
      case NODE_CVAR:
      case NODE_CONST:
      case NODE_COLON3:
      case NODE_INTEGER:
      case NODE_FLOAT:
      case NODE_RATIONAL:
      case NODE_IMAGINARY:
      case NODE_STR:
      case NODE_SYM:
      case NODE_REGX:
      case NODE_SELF:
      case NODE_NIL:
      case NODE_TRUE:
      case NODE_FALSE:
      case NODE_LAMBDA:
      case NODE_NTH_REF:
      case NODE_BACK_REF:
        return false;
      case NODE_COLON2:
        return setup_args_dup_rest_p(RNODE_COLON2(argn)->nd_head);
      case NODE_LIST:
        while (argn) {
            if (setup_args_dup_rest_p(RNODE_LIST(argn)->nd_head)) {
                return true;
            }
            argn = RNODE_LIST(argn)->nd_next;
        }
        return false;
      default:
        return true;
    }
}

static VALUE
setup_args(rb_iseq_t *iseq, LINK_ANCHOR *const args, const NODE *argn,
           unsigned int *flag, struct rb_callinfo_kwarg **keywords)
{
    VALUE ret;
    unsigned int dup_rest = SPLATARRAY_TRUE, initial_dup_rest;

    if (argn) {
        const NODE *check_arg = nd_type_p(argn, NODE_BLOCK_PASS) ?
            RNODE_BLOCK_PASS(argn)->nd_head : argn;

        if (check_arg) {
            switch(nd_type(check_arg)) {
              case(NODE_SPLAT):
                // avoid caller side array allocation for f(*arg)
                dup_rest = SPLATARRAY_FALSE;
                break;
              case(NODE_ARGSCAT):
                // avoid caller side array allocation for f(1, *arg)
                dup_rest = !nd_type_p(RNODE_ARGSCAT(check_arg)->nd_head, NODE_LIST);
                break;
              case(NODE_ARGSPUSH):
                // avoid caller side array allocation for f(*arg, **hash) and f(1, *arg, **hash)
                dup_rest = !((nd_type_p(RNODE_ARGSPUSH(check_arg)->nd_head, NODE_SPLAT) ||
                    (nd_type_p(RNODE_ARGSPUSH(check_arg)->nd_head, NODE_ARGSCAT) &&
                     nd_type_p(RNODE_ARGSCAT(RNODE_ARGSPUSH(check_arg)->nd_head)->nd_head, NODE_LIST))) &&
                    nd_type_p(RNODE_ARGSPUSH(check_arg)->nd_body, NODE_HASH) &&
                    !RNODE_HASH(RNODE_ARGSPUSH(check_arg)->nd_body)->nd_brace);

                if (dup_rest == SPLATARRAY_FALSE) {
                    // require allocation for keyword key/value/splat that may modify splatted argument
                    NODE *node = RNODE_HASH(RNODE_ARGSPUSH(check_arg)->nd_body)->nd_head;
                    while (node) {
                        NODE *key_node = RNODE_LIST(node)->nd_head;
                        if (key_node && setup_args_dup_rest_p(key_node)) {
                            dup_rest = SPLATARRAY_TRUE;
                            break;
                        }

                        node = RNODE_LIST(node)->nd_next;
                        NODE *value_node = RNODE_LIST(node)->nd_head;
                        if (setup_args_dup_rest_p(value_node)) {
                            dup_rest = SPLATARRAY_TRUE;
                            break;
                        }

                        node = RNODE_LIST(node)->nd_next;
                    }
                }
                break;
              default:
                break;
            }
        }

        if (check_arg != argn && setup_args_dup_rest_p(RNODE_BLOCK_PASS(argn)->nd_body)) {
            // for block pass that may modify splatted argument, dup rest and kwrest if given
            dup_rest = SPLATARRAY_TRUE | DUP_SINGLE_KW_SPLAT;
        }
    }
    initial_dup_rest = dup_rest;

    if (argn && nd_type_p(argn, NODE_BLOCK_PASS)) {
        DECL_ANCHOR(arg_block);
        INIT_ANCHOR(arg_block);

        if (RNODE_BLOCK_PASS(argn)->forwarding && ISEQ_BODY(ISEQ_BODY(iseq)->local_iseq)->param.flags.forwardable) {
            int idx = ISEQ_BODY(ISEQ_BODY(iseq)->local_iseq)->local_table_size;// - get_local_var_idx(iseq, idDot3);

            RUBY_ASSERT(nd_type_p(RNODE_BLOCK_PASS(argn)->nd_head, NODE_ARGSPUSH));
            const NODE * arg_node =
                RNODE_ARGSPUSH(RNODE_BLOCK_PASS(argn)->nd_head)->nd_head;

            int argc = 0;

            // Only compile leading args:
            //   foo(x, y, ...)
            //       ^^^^
            if (nd_type_p(arg_node, NODE_ARGSCAT)) {
                argc += setup_args_core(iseq, args, RNODE_ARGSCAT(arg_node)->nd_head, &dup_rest, flag, keywords);
            }

            *flag |= VM_CALL_FORWARDING;

            ADD_GETLOCAL(args, argn, idx, get_lvar_level(iseq));
            setup_args_splat_mut(flag, dup_rest, initial_dup_rest);
            return INT2FIX(argc);
        }
        else {
            *flag |= VM_CALL_ARGS_BLOCKARG;

            NO_CHECK(COMPILE(arg_block, "block", RNODE_BLOCK_PASS(argn)->nd_body));
        }

        if (LIST_INSN_SIZE_ONE(arg_block)) {
            LINK_ELEMENT *elem = FIRST_ELEMENT(arg_block);
            if (IS_INSN(elem)) {
                INSN *iobj = (INSN *)elem;
                if (iobj->insn_id == BIN(getblockparam)) {
                    iobj->insn_id = BIN(getblockparamproxy);
                }
            }
        }
        ret = INT2FIX(setup_args_core(iseq, args, RNODE_BLOCK_PASS(argn)->nd_head, &dup_rest, flag, keywords));
        ADD_SEQ(args, arg_block);
    }
    else {
        ret = INT2FIX(setup_args_core(iseq, args, argn, &dup_rest, flag, keywords));
    }
    setup_args_splat_mut(flag, dup_rest, initial_dup_rest);
    return ret;
}

static void
build_postexe_iseq(rb_iseq_t *iseq, LINK_ANCHOR *ret, const void *ptr)
{
    const NODE *body = ptr;
    int line = nd_line(body);
    VALUE argc = INT2FIX(0);
    const rb_iseq_t *block = NEW_CHILD_ISEQ(body, make_name_for_block(ISEQ_BODY(iseq)->parent_iseq), ISEQ_TYPE_BLOCK, line);

    ADD_INSN1(ret, body, putspecialobject, INT2FIX(VM_SPECIAL_OBJECT_VMCORE));
    ADD_CALL_WITH_BLOCK(ret, body, id_core_set_postexe, argc, block);
    RB_OBJ_WRITTEN(iseq, Qundef, (VALUE)block);
    iseq_set_local_table(iseq, 0, 0);
}

static void
compile_named_capture_assign(rb_iseq_t *iseq, LINK_ANCHOR *const ret, const NODE *const node)
{
    const NODE *vars;
    LINK_ELEMENT *last;
    int line = nd_line(node);
    const NODE *line_node = node;
    LABEL *fail_label = NEW_LABEL(line), *end_label = NEW_LABEL(line);

#if !(defined(NAMED_CAPTURE_BY_SVAR) && NAMED_CAPTURE_BY_SVAR-0)
    ADD_INSN1(ret, line_node, getglobal, ID2SYM(idBACKREF));
#else
    ADD_INSN2(ret, line_node, getspecial, INT2FIX(1) /* '~' */, INT2FIX(0));
#endif
    ADD_INSN(ret, line_node, dup);
    ADD_INSNL(ret, line_node, branchunless, fail_label);

    for (vars = node; vars; vars = RNODE_BLOCK(vars)->nd_next) {
        INSN *cap;
        if (RNODE_BLOCK(vars)->nd_next) {
            ADD_INSN(ret, line_node, dup);
        }
        last = ret->last;
        NO_CHECK(COMPILE_POPPED(ret, "capture", RNODE_BLOCK(vars)->nd_head));
        last = last->next; /* putobject :var */
        cap = new_insn_send(iseq, nd_line(line_node), nd_node_id(line_node), idAREF, INT2FIX(1),
                            NULL, INT2FIX(0), NULL);
        ELEM_INSERT_PREV(last->next, (LINK_ELEMENT *)cap);
#if !defined(NAMED_CAPTURE_SINGLE_OPT) || NAMED_CAPTURE_SINGLE_OPT-0
        if (!RNODE_BLOCK(vars)->nd_next && vars == node) {
            /* only one name */
            DECL_ANCHOR(nom);

            INIT_ANCHOR(nom);
            ADD_INSNL(nom, line_node, jump, end_label);
            ADD_LABEL(nom, fail_label);
# if 0				/* $~ must be MatchData or nil */
            ADD_INSN(nom, line_node, pop);
            ADD_INSN(nom, line_node, putnil);
# endif
            ADD_LABEL(nom, end_label);
            (nom->last->next = cap->link.next)->prev = nom->last;
            (cap->link.next = nom->anchor.next)->prev = &cap->link;
            return;
        }
#endif
    }
    ADD_INSNL(ret, line_node, jump, end_label);
    ADD_LABEL(ret, fail_label);
    ADD_INSN(ret, line_node, pop);
    for (vars = node; vars; vars = RNODE_BLOCK(vars)->nd_next) {
        last = ret->last;
        NO_CHECK(COMPILE_POPPED(ret, "capture", RNODE_BLOCK(vars)->nd_head));
        last = last->next; /* putobject :var */
        ((INSN*)last)->insn_id = BIN(putnil);
        ((INSN*)last)->operand_size = 0;
    }
    ADD_LABEL(ret, end_label);
}

static int
optimizable_range_item_p(const NODE *n)
{
    if (!n) return FALSE;
    switch (nd_type(n)) {
      case NODE_LINE:
        return TRUE;
      case NODE_INTEGER:
        return TRUE;
      case NODE_NIL:
        return TRUE;
      default:
        return FALSE;
    }
}

static VALUE
optimized_range_item(const NODE *n)
{
    switch (nd_type(n)) {
      case NODE_LINE:
        return rb_node_line_lineno_val(n);
      case NODE_INTEGER:
        return rb_node_integer_literal_val(n);
      case NODE_FLOAT:
        return rb_node_float_literal_val(n);
      case NODE_RATIONAL:
        return rb_node_rational_literal_val(n);
      case NODE_IMAGINARY:
        return rb_node_imaginary_literal_val(n);
      case NODE_NIL:
        return Qnil;
      default:
        rb_bug("unexpected node: %s", ruby_node_name(nd_type(n)));
    }
}

static int
compile_if(rb_iseq_t *iseq, LINK_ANCHOR *const ret, const NODE *const node, int popped, const enum node_type type)
{
    const NODE *const node_body = type == NODE_IF ? RNODE_IF(node)->nd_body : RNODE_UNLESS(node)->nd_else;
    const NODE *const node_else = type == NODE_IF ? RNODE_IF(node)->nd_else : RNODE_UNLESS(node)->nd_body;

    const int line = nd_line(node);
    const NODE *line_node = node;
    DECL_ANCHOR(cond_seq);
    LABEL *then_label, *else_label, *end_label;
    VALUE branches = Qfalse;

    INIT_ANCHOR(cond_seq);
    then_label = NEW_LABEL(line);
    else_label = NEW_LABEL(line);
    end_label = 0;

    NODE *cond = RNODE_IF(node)->nd_cond;
    if (nd_type(cond) == NODE_BLOCK) {
        cond = RNODE_BLOCK(cond)->nd_head;
    }

    CHECK(compile_branch_condition(iseq, cond_seq, cond, then_label, else_label));
    ADD_SEQ(ret, cond_seq);

    if (then_label->refcnt && else_label->refcnt) {
        branches = decl_branch_base(iseq, PTR2NUM(node), nd_code_loc(node), type == NODE_IF ? "if" : "unless");
    }

    if (then_label->refcnt) {
        ADD_LABEL(ret, then_label);

        DECL_ANCHOR(then_seq);
        INIT_ANCHOR(then_seq);
        CHECK(COMPILE_(then_seq, "then", node_body, popped));

        if (else_label->refcnt) {
            const NODE *const coverage_node = node_body ? node_body : node;
            add_trace_branch_coverage(
                iseq,
                ret,
                nd_code_loc(coverage_node),
                nd_node_id(coverage_node),
                0,
                type == NODE_IF ? "then" : "else",
                branches);
            end_label = NEW_LABEL(line);
            ADD_INSNL(then_seq, line_node, jump, end_label);
            if (!popped) {
                ADD_INSN(then_seq, line_node, pop);
            }
        }
        ADD_SEQ(ret, then_seq);
    }

    if (else_label->refcnt) {
        ADD_LABEL(ret, else_label);

        DECL_ANCHOR(else_seq);
        INIT_ANCHOR(else_seq);
        CHECK(COMPILE_(else_seq, "else", node_else, popped));

        if (then_label->refcnt) {
            const NODE *const coverage_node = node_else ? node_else : node;
            add_trace_branch_coverage(
                iseq,
                ret,
                nd_code_loc(coverage_node),
                nd_node_id(coverage_node),
                1,
                type == NODE_IF ? "else" : "then",
                branches);
        }
        ADD_SEQ(ret, else_seq);
    }

    if (end_label) {
        ADD_LABEL(ret, end_label);
    }

    return COMPILE_OK;
}

static int
compile_case(rb_iseq_t *iseq, LINK_ANCHOR *const ret, const NODE *const orig_node, int popped)
{
    const NODE *vals;
    const NODE *node = orig_node;
    LABEL *endlabel, *elselabel;
    DECL_ANCHOR(head);
    DECL_ANCHOR(body_seq);
    DECL_ANCHOR(cond_seq);
    int only_special_literals = 1;
    VALUE literals = rb_hash_new();
    int line;
    enum node_type type;
    const NODE *line_node;
    VALUE branches = Qfalse;
    int branch_id = 0;

    INIT_ANCHOR(head);
    INIT_ANCHOR(body_seq);
    INIT_ANCHOR(cond_seq);

    RHASH_TBL_RAW(literals)->type = &cdhash_type;

    CHECK(COMPILE(head, "case base", RNODE_CASE(node)->nd_head));

    branches = decl_branch_base(iseq, PTR2NUM(node), nd_code_loc(node), "case");

    node = RNODE_CASE(node)->nd_body;
    EXPECT_NODE("NODE_CASE", node, NODE_WHEN, COMPILE_NG);
    type = nd_type(node);
    line = nd_line(node);
    line_node = node;

    endlabel = NEW_LABEL(line);
    elselabel = NEW_LABEL(line);

    ADD_SEQ(ret, head);	/* case VAL */

    while (type == NODE_WHEN) {
        LABEL *l1;

        l1 = NEW_LABEL(line);
        ADD_LABEL(body_seq, l1);
        ADD_INSN(body_seq, line_node, pop);

        const NODE *const coverage_node = RNODE_WHEN(node)->nd_body ? RNODE_WHEN(node)->nd_body : node;
        add_trace_branch_coverage(
                iseq,
                body_seq,
                nd_code_loc(coverage_node),
                nd_node_id(coverage_node),
                branch_id++,
                "when",
                branches);

        CHECK(COMPILE_(body_seq, "when body", RNODE_WHEN(node)->nd_body, popped));
        ADD_INSNL(body_seq, line_node, jump, endlabel);

        vals = RNODE_WHEN(node)->nd_head;
        if (vals) {
            switch (nd_type(vals)) {
              case NODE_LIST:
                only_special_literals = when_vals(iseq, cond_seq, vals, l1, only_special_literals, literals);
                if (only_special_literals < 0) return COMPILE_NG;
                break;
              case NODE_SPLAT:
              case NODE_ARGSCAT:
              case NODE_ARGSPUSH:
                only_special_literals = 0;
                CHECK(when_splat_vals(iseq, cond_seq, vals, l1, only_special_literals, literals));
                break;
              default:
                UNKNOWN_NODE("NODE_CASE", vals, COMPILE_NG);
            }
        }
        else {
            EXPECT_NODE_NONULL("NODE_CASE", node, NODE_LIST, COMPILE_NG);
        }

        node = RNODE_WHEN(node)->nd_next;
        if (!node) {
            break;
        }
        type = nd_type(node);
        line = nd_line(node);
        line_node = node;
    }
    /* else */
    if (node) {
        ADD_LABEL(cond_seq, elselabel);
        ADD_INSN(cond_seq, line_node, pop);
        add_trace_branch_coverage(iseq, cond_seq, nd_code_loc(node), nd_node_id(node), branch_id, "else", branches);
        CHECK(COMPILE_(cond_seq, "else", node, popped));
        ADD_INSNL(cond_seq, line_node, jump, endlabel);
    }
    else {
        debugs("== else (implicit)\n");
        ADD_LABEL(cond_seq, elselabel);
        ADD_INSN(cond_seq, orig_node, pop);
        add_trace_branch_coverage(iseq, cond_seq, nd_code_loc(orig_node), nd_node_id(orig_node), branch_id, "else", branches);
        if (!popped) {
            ADD_INSN(cond_seq, orig_node, putnil);
        }
        ADD_INSNL(cond_seq, orig_node, jump, endlabel);
    }

    if (only_special_literals && ISEQ_COMPILE_DATA(iseq)->option->specialized_instruction) {
        ADD_INSN(ret, orig_node, dup);
        ADD_INSN2(ret, orig_node, opt_case_dispatch, literals, elselabel);
        RB_OBJ_WRITTEN(iseq, Qundef, literals);
        LABEL_REF(elselabel);
    }

    ADD_SEQ(ret, cond_seq);
    ADD_SEQ(ret, body_seq);
    ADD_LABEL(ret, endlabel);
    return COMPILE_OK;
}

static int
compile_case2(rb_iseq_t *iseq, LINK_ANCHOR *const ret, const NODE *const orig_node, int popped)
{
    const NODE *vals;
    const NODE *val;
    const NODE *node = RNODE_CASE2(orig_node)->nd_body;
    LABEL *endlabel;
    DECL_ANCHOR(body_seq);
    VALUE branches = Qfalse;
    int branch_id = 0;

    branches = decl_branch_base(iseq, PTR2NUM(orig_node), nd_code_loc(orig_node), "case");

    INIT_ANCHOR(body_seq);
    endlabel = NEW_LABEL(nd_line(node));

    while (node && nd_type_p(node, NODE_WHEN)) {
        const int line = nd_line(node);
        LABEL *l1 = NEW_LABEL(line);
        ADD_LABEL(body_seq, l1);

        const NODE *const coverage_node = RNODE_WHEN(node)->nd_body ? RNODE_WHEN(node)->nd_body : node;
        add_trace_branch_coverage(
                iseq,
                body_seq,
                nd_code_loc(coverage_node),
                nd_node_id(coverage_node),
                branch_id++,
                "when",
                branches);

        CHECK(COMPILE_(body_seq, "when", RNODE_WHEN(node)->nd_body, popped));
        ADD_INSNL(body_seq, node, jump, endlabel);

        vals = RNODE_WHEN(node)->nd_head;
        if (!vals) {
            EXPECT_NODE_NONULL("NODE_WHEN", node, NODE_LIST, COMPILE_NG);
        }
        switch (nd_type(vals)) {
          case NODE_LIST:
            while (vals) {
                LABEL *lnext;
                val = RNODE_LIST(vals)->nd_head;
                lnext = NEW_LABEL(nd_line(val));
                debug_compile("== when2\n", (void)0);
                CHECK(compile_branch_condition(iseq, ret, val, l1, lnext));
                ADD_LABEL(ret, lnext);
                vals = RNODE_LIST(vals)->nd_next;
            }
            break;
          case NODE_SPLAT:
          case NODE_ARGSCAT:
          case NODE_ARGSPUSH:
            ADD_INSN(ret, vals, putnil);
            CHECK(COMPILE(ret, "when2/cond splat", vals));
            ADD_INSN1(ret, vals, checkmatch, INT2FIX(VM_CHECKMATCH_TYPE_WHEN | VM_CHECKMATCH_ARRAY));
            ADD_INSNL(ret, vals, branchif, l1);
            break;
          default:
            UNKNOWN_NODE("NODE_WHEN", vals, COMPILE_NG);
        }
        node = RNODE_WHEN(node)->nd_next;
    }
    /* else */
    const NODE *const coverage_node = node ? node : orig_node;
    add_trace_branch_coverage(
        iseq,
        ret,
        nd_code_loc(coverage_node),
        nd_node_id(coverage_node),
        branch_id,
        "else",
        branches);
    CHECK(COMPILE_(ret, "else", node, popped));
    ADD_INSNL(ret, orig_node, jump, endlabel);

    ADD_SEQ(ret, body_seq);
    ADD_LABEL(ret, endlabel);
    return COMPILE_OK;
}

static int iseq_compile_pattern_match(rb_iseq_t *iseq, LINK_ANCHOR *const ret, const NODE *const node, LABEL *unmatched, bool in_single_pattern, bool in_alt_pattern, int base_index, bool use_deconstructed_cache);

static int iseq_compile_pattern_constant(rb_iseq_t *iseq, LINK_ANCHOR *const ret, const NODE *const node, LABEL *match_failed, bool in_single_pattern, int base_index);
static int iseq_compile_array_deconstruct(rb_iseq_t *iseq, LINK_ANCHOR *const ret, const NODE *const node, LABEL *deconstruct, LABEL *deconstructed, LABEL *match_failed, LABEL *type_error, bool in_single_pattern, int base_index, bool use_deconstructed_cache);
static int iseq_compile_pattern_set_general_errmsg(rb_iseq_t *iseq, LINK_ANCHOR *const ret, const NODE *const node, VALUE errmsg, int base_index);
static int iseq_compile_pattern_set_length_errmsg(rb_iseq_t *iseq, LINK_ANCHOR *const ret, const NODE *const node, VALUE errmsg, VALUE pattern_length, int base_index);
static int iseq_compile_pattern_set_eqq_errmsg(rb_iseq_t *iseq, LINK_ANCHOR *const ret, const NODE *const node, int base_index);

#define CASE3_BI_OFFSET_DECONSTRUCTED_CACHE 0
#define CASE3_BI_OFFSET_ERROR_STRING        1
#define CASE3_BI_OFFSET_KEY_ERROR_P         2
#define CASE3_BI_OFFSET_KEY_ERROR_MATCHEE   3
#define CASE3_BI_OFFSET_KEY_ERROR_KEY       4

static int
iseq_compile_pattern_each(rb_iseq_t *iseq, LINK_ANCHOR *const ret, const NODE *const node, LABEL *matched, LABEL *unmatched, bool in_single_pattern, bool in_alt_pattern, int base_index, bool use_deconstructed_cache)
{
    const int line = nd_line(node);
    const NODE *line_node = node;

    switch (nd_type(node)) {
      case NODE_ARYPTN: {
        /*
         *   if pattern.use_rest_num?
         *     rest_num = 0
         *   end
         *   if pattern.has_constant_node?
         *     unless pattern.constant === obj
         *       goto match_failed
         *     end
         *   end
         *   unless obj.respond_to?(:deconstruct)
         *     goto match_failed
         *   end
         *   d = obj.deconstruct
         *   unless Array === d
         *     goto type_error
         *   end
         *   min_argc = pattern.pre_args_num + pattern.post_args_num
         *   if pattern.has_rest_arg?
         *     unless d.length >= min_argc
         *       goto match_failed
         *     end
         *   else
         *     unless d.length == min_argc
         *       goto match_failed
         *     end
         *   end
         *   pattern.pre_args_num.each do |i|
         *     unless pattern.pre_args[i].match?(d[i])
         *       goto match_failed
         *     end
         *   end
         *   if pattern.use_rest_num?
         *     rest_num = d.length - min_argc
         *     if pattern.has_rest_arg? && pattern.has_rest_arg_id # not `*`, but `*rest`
         *       unless pattern.rest_arg.match?(d[pattern.pre_args_num, rest_num])
         *         goto match_failed
         *       end
         *     end
         *   end
         *   pattern.post_args_num.each do |i|
         *     j = pattern.pre_args_num + i
         *     j += rest_num
         *     unless pattern.post_args[i].match?(d[j])
         *       goto match_failed
         *     end
         *   end
         *   goto matched
         * type_error:
         *   FrozenCore.raise TypeError
         * match_failed:
         *   goto unmatched
         */
        const NODE *args = RNODE_ARYPTN(node)->pre_args;
        const int pre_args_num = RNODE_ARYPTN(node)->pre_args ? rb_long2int(RNODE_LIST(RNODE_ARYPTN(node)->pre_args)->as.nd_alen) : 0;
        const int post_args_num = RNODE_ARYPTN(node)->post_args ? rb_long2int(RNODE_LIST(RNODE_ARYPTN(node)->post_args)->as.nd_alen) : 0;

        const int min_argc = pre_args_num + post_args_num;
        const int use_rest_num = RNODE_ARYPTN(node)->rest_arg && (NODE_NAMED_REST_P(RNODE_ARYPTN(node)->rest_arg) ||
                                                      (!NODE_NAMED_REST_P(RNODE_ARYPTN(node)->rest_arg) && post_args_num > 0));

        LABEL *match_failed, *type_error, *deconstruct, *deconstructed;
        int i;
        match_failed = NEW_LABEL(line);
        type_error = NEW_LABEL(line);
        deconstruct = NEW_LABEL(line);
        deconstructed = NEW_LABEL(line);

        if (use_rest_num) {
            ADD_INSN1(ret, line_node, putobject, INT2FIX(0)); /* allocate stack for rest_num */
            ADD_INSN(ret, line_node, swap);
            if (base_index) {
                base_index++;
            }
        }

        CHECK(iseq_compile_pattern_constant(iseq, ret, node, match_failed, in_single_pattern, base_index));

        CHECK(iseq_compile_array_deconstruct(iseq, ret, node, deconstruct, deconstructed, match_failed, type_error, in_single_pattern, base_index, use_deconstructed_cache));

        ADD_INSN(ret, line_node, dup);
        ADD_SEND(ret, line_node, idLength, INT2FIX(0));
        ADD_INSN1(ret, line_node, putobject, INT2FIX(min_argc));
        ADD_SEND(ret, line_node, RNODE_ARYPTN(node)->rest_arg ? idGE : idEq, INT2FIX(1)); // (1)
        if (in_single_pattern) {
            CHECK(iseq_compile_pattern_set_length_errmsg(iseq, ret, node,
                                                         RNODE_ARYPTN(node)->rest_arg ? rb_fstring_lit("%p length mismatch (given %p, expected %p+)") :
                                                                            rb_fstring_lit("%p length mismatch (given %p, expected %p)"),
                                                         INT2FIX(min_argc), base_index + 1 /* (1) */));
        }
        ADD_INSNL(ret, line_node, branchunless, match_failed);

        for (i = 0; i < pre_args_num; i++) {
            ADD_INSN(ret, line_node, dup);
            ADD_INSN1(ret, line_node, putobject, INT2FIX(i));
            ADD_SEND(ret, line_node, idAREF, INT2FIX(1)); // (2)
            CHECK(iseq_compile_pattern_match(iseq, ret, RNODE_LIST(args)->nd_head, match_failed, in_single_pattern, in_alt_pattern, base_index + 1 /* (2) */, false));
            args = RNODE_LIST(args)->nd_next;
        }

        if (RNODE_ARYPTN(node)->rest_arg) {
            if (NODE_NAMED_REST_P(RNODE_ARYPTN(node)->rest_arg)) {
                ADD_INSN(ret, line_node, dup);
                ADD_INSN1(ret, line_node, putobject, INT2FIX(pre_args_num));
                ADD_INSN1(ret, line_node, topn, INT2FIX(1));
                ADD_SEND(ret, line_node, idLength, INT2FIX(0));
                ADD_INSN1(ret, line_node, putobject, INT2FIX(min_argc));
                ADD_SEND(ret, line_node, idMINUS, INT2FIX(1));
                ADD_INSN1(ret, line_node, setn, INT2FIX(4));
                ADD_SEND(ret, line_node, idAREF, INT2FIX(2)); // (3)

                CHECK(iseq_compile_pattern_match(iseq, ret, RNODE_ARYPTN(node)->rest_arg, match_failed, in_single_pattern, in_alt_pattern, base_index + 1 /* (3) */, false));
            }
            else {
                if (post_args_num > 0) {
                    ADD_INSN(ret, line_node, dup);
                    ADD_SEND(ret, line_node, idLength, INT2FIX(0));
                    ADD_INSN1(ret, line_node, putobject, INT2FIX(min_argc));
                    ADD_SEND(ret, line_node, idMINUS, INT2FIX(1));
                    ADD_INSN1(ret, line_node, setn, INT2FIX(2));
                    ADD_INSN(ret, line_node, pop);
                }
            }
        }

        args = RNODE_ARYPTN(node)->post_args;
        for (i = 0; i < post_args_num; i++) {
            ADD_INSN(ret, line_node, dup);

            ADD_INSN1(ret, line_node, putobject, INT2FIX(pre_args_num + i));
            ADD_INSN1(ret, line_node, topn, INT2FIX(3));
            ADD_SEND(ret, line_node, idPLUS, INT2FIX(1));

            ADD_SEND(ret, line_node, idAREF, INT2FIX(1)); // (4)
            CHECK(iseq_compile_pattern_match(iseq, ret, RNODE_LIST(args)->nd_head, match_failed, in_single_pattern, in_alt_pattern, base_index + 1 /* (4) */, false));
            args = RNODE_LIST(args)->nd_next;
        }

        ADD_INSN(ret, line_node, pop);
        if (use_rest_num) {
            ADD_INSN(ret, line_node, pop);
        }
        ADD_INSNL(ret, line_node, jump, matched);
        ADD_INSN(ret, line_node, putnil);
        if (use_rest_num) {
            ADD_INSN(ret, line_node, putnil);
        }

        ADD_LABEL(ret, type_error);
        ADD_INSN1(ret, line_node, putspecialobject, INT2FIX(VM_SPECIAL_OBJECT_VMCORE));
        ADD_INSN1(ret, line_node, putobject, rb_eTypeError);
        ADD_INSN1(ret, line_node, putobject, rb_fstring_lit("deconstruct must return Array"));
        ADD_SEND(ret, line_node, id_core_raise, INT2FIX(2));
        ADD_INSN(ret, line_node, pop);

        ADD_LABEL(ret, match_failed);
        ADD_INSN(ret, line_node, pop);
        if (use_rest_num) {
            ADD_INSN(ret, line_node, pop);
        }
        ADD_INSNL(ret, line_node, jump, unmatched);

        break;
      }
      case NODE_FNDPTN: {
        /*
         *   if pattern.has_constant_node?
         *     unless pattern.constant === obj
         *       goto match_failed
         *     end
         *   end
         *   unless obj.respond_to?(:deconstruct)
         *     goto match_failed
         *   end
         *   d = obj.deconstruct
         *   unless Array === d
         *     goto type_error
         *   end
         *   unless d.length >= pattern.args_num
         *     goto match_failed
         *   end
         *
         *   begin
         *     len = d.length
         *     limit = d.length - pattern.args_num
         *     i = 0
         *     while i <= limit
         *       if pattern.args_num.times.all? {|j| pattern.args[j].match?(d[i+j]) }
         *         if pattern.has_pre_rest_arg_id
         *           unless pattern.pre_rest_arg.match?(d[0, i])
         *             goto find_failed
         *           end
         *         end
         *         if pattern.has_post_rest_arg_id
         *           unless pattern.post_rest_arg.match?(d[i+pattern.args_num, len])
         *             goto find_failed
         *           end
         *         end
         *         goto find_succeeded
         *       end
         *       i+=1
         *     end
         *   find_failed:
         *     goto match_failed
         *   find_succeeded:
         *   end
         *
         *   goto matched
         * type_error:
         *   FrozenCore.raise TypeError
         * match_failed:
         *   goto unmatched
         */
        const NODE *args = RNODE_FNDPTN(node)->args;
        const int args_num = RNODE_FNDPTN(node)->args ? rb_long2int(RNODE_LIST(RNODE_FNDPTN(node)->args)->as.nd_alen) : 0;

        LABEL *match_failed, *type_error, *deconstruct, *deconstructed;
        match_failed = NEW_LABEL(line);
        type_error = NEW_LABEL(line);
        deconstruct = NEW_LABEL(line);
        deconstructed = NEW_LABEL(line);

        CHECK(iseq_compile_pattern_constant(iseq, ret, node, match_failed, in_single_pattern, base_index));

        CHECK(iseq_compile_array_deconstruct(iseq, ret, node, deconstruct, deconstructed, match_failed, type_error, in_single_pattern, base_index, use_deconstructed_cache));

        ADD_INSN(ret, line_node, dup);
        ADD_SEND(ret, line_node, idLength, INT2FIX(0));
        ADD_INSN1(ret, line_node, putobject, INT2FIX(args_num));
        ADD_SEND(ret, line_node, idGE, INT2FIX(1)); // (1)
        if (in_single_pattern) {
            CHECK(iseq_compile_pattern_set_length_errmsg(iseq, ret, node, rb_fstring_lit("%p length mismatch (given %p, expected %p+)"), INT2FIX(args_num), base_index + 1 /* (1) */));
        }
        ADD_INSNL(ret, line_node, branchunless, match_failed);

        {
            LABEL *while_begin = NEW_LABEL(nd_line(node));
            LABEL *next_loop = NEW_LABEL(nd_line(node));
            LABEL *find_succeeded = NEW_LABEL(line);
            LABEL *find_failed = NEW_LABEL(nd_line(node));
            int j;

            ADD_INSN(ret, line_node, dup); /* allocate stack for len */
            ADD_SEND(ret, line_node, idLength, INT2FIX(0)); // (2)

            ADD_INSN(ret, line_node, dup); /* allocate stack for limit */
            ADD_INSN1(ret, line_node, putobject, INT2FIX(args_num));
            ADD_SEND(ret, line_node, idMINUS, INT2FIX(1)); // (3)

            ADD_INSN1(ret, line_node, putobject, INT2FIX(0)); /* allocate stack for i */ // (4)

            ADD_LABEL(ret, while_begin);

            ADD_INSN(ret, line_node, dup);
            ADD_INSN1(ret, line_node, topn, INT2FIX(2));
            ADD_SEND(ret, line_node, idLE, INT2FIX(1));
            ADD_INSNL(ret, line_node, branchunless, find_failed);

            for (j = 0; j < args_num; j++) {
                ADD_INSN1(ret, line_node, topn, INT2FIX(3));
                ADD_INSN1(ret, line_node, topn, INT2FIX(1));
                if (j != 0) {
                    ADD_INSN1(ret, line_node, putobject, INT2FIX(j));
                    ADD_SEND(ret, line_node, idPLUS, INT2FIX(1));
                }
                ADD_SEND(ret, line_node, idAREF, INT2FIX(1)); // (5)

                CHECK(iseq_compile_pattern_match(iseq, ret, RNODE_LIST(args)->nd_head, next_loop, in_single_pattern, in_alt_pattern, base_index + 4 /* (2), (3), (4), (5) */, false));
                args = RNODE_LIST(args)->nd_next;
            }

            if (NODE_NAMED_REST_P(RNODE_FNDPTN(node)->pre_rest_arg)) {
                ADD_INSN1(ret, line_node, topn, INT2FIX(3));
                ADD_INSN1(ret, line_node, putobject, INT2FIX(0));
                ADD_INSN1(ret, line_node, topn, INT2FIX(2));
                ADD_SEND(ret, line_node, idAREF, INT2FIX(2)); // (6)
                CHECK(iseq_compile_pattern_match(iseq, ret, RNODE_FNDPTN(node)->pre_rest_arg, find_failed, in_single_pattern, in_alt_pattern, base_index + 4 /* (2), (3), (4), (6) */, false));
            }
            if (NODE_NAMED_REST_P(RNODE_FNDPTN(node)->post_rest_arg)) {
                ADD_INSN1(ret, line_node, topn, INT2FIX(3));
                ADD_INSN1(ret, line_node, topn, INT2FIX(1));
                ADD_INSN1(ret, line_node, putobject, INT2FIX(args_num));
                ADD_SEND(ret, line_node, idPLUS, INT2FIX(1));
                ADD_INSN1(ret, line_node, topn, INT2FIX(3));
                ADD_SEND(ret, line_node, idAREF, INT2FIX(2)); // (7)
                CHECK(iseq_compile_pattern_match(iseq, ret, RNODE_FNDPTN(node)->post_rest_arg, find_failed, in_single_pattern, in_alt_pattern, base_index + 4 /* (2), (3),(4), (7) */, false));
            }
            ADD_INSNL(ret, line_node, jump, find_succeeded);

            ADD_LABEL(ret, next_loop);
            ADD_INSN1(ret, line_node, putobject, INT2FIX(1));
            ADD_SEND(ret, line_node, idPLUS, INT2FIX(1));
            ADD_INSNL(ret, line_node, jump, while_begin);

            ADD_LABEL(ret, find_failed);
            ADD_INSN1(ret, line_node, adjuststack, INT2FIX(3));
            if (in_single_pattern) {
                ADD_INSN1(ret, line_node, putspecialobject, INT2FIX(VM_SPECIAL_OBJECT_VMCORE));
                ADD_INSN1(ret, line_node, putobject, rb_fstring_lit("%p does not match to find pattern"));
                ADD_INSN1(ret, line_node, topn, INT2FIX(2));
                ADD_SEND(ret, line_node, id_core_sprintf, INT2FIX(2)); // (8)
                ADD_INSN1(ret, line_node, setn, INT2FIX(base_index + CASE3_BI_OFFSET_ERROR_STRING + 1 /* (8) */)); // (9)

                ADD_INSN1(ret, line_node, putobject, Qfalse);
                ADD_INSN1(ret, line_node, setn, INT2FIX(base_index + CASE3_BI_OFFSET_KEY_ERROR_P + 2 /* (8), (9) */));

                ADD_INSN(ret, line_node, pop);
                ADD_INSN(ret, line_node, pop);
            }
            ADD_INSNL(ret, line_node, jump, match_failed);
            ADD_INSN1(ret, line_node, dupn, INT2FIX(3));

            ADD_LABEL(ret, find_succeeded);
            ADD_INSN1(ret, line_node, adjuststack, INT2FIX(3));
        }

        ADD_INSN(ret, line_node, pop);
        ADD_INSNL(ret, line_node, jump, matched);
        ADD_INSN(ret, line_node, putnil);

        ADD_LABEL(ret, type_error);
        ADD_INSN1(ret, line_node, putspecialobject, INT2FIX(VM_SPECIAL_OBJECT_VMCORE));
        ADD_INSN1(ret, line_node, putobject, rb_eTypeError);
        ADD_INSN1(ret, line_node, putobject, rb_fstring_lit("deconstruct must return Array"));
        ADD_SEND(ret, line_node, id_core_raise, INT2FIX(2));
        ADD_INSN(ret, line_node, pop);

        ADD_LABEL(ret, match_failed);
        ADD_INSN(ret, line_node, pop);
        ADD_INSNL(ret, line_node, jump, unmatched);

        break;
      }
      case NODE_HSHPTN: {
        /*
         *   keys = nil
         *   if pattern.has_kw_args_node? && !pattern.has_kw_rest_arg_node?
         *     keys = pattern.kw_args_node.keys
         *   end
         *   if pattern.has_constant_node?
         *     unless pattern.constant === obj
         *       goto match_failed
         *     end
         *   end
         *   unless obj.respond_to?(:deconstruct_keys)
         *     goto match_failed
         *   end
         *   d = obj.deconstruct_keys(keys)
         *   unless Hash === d
         *     goto type_error
         *   end
         *   if pattern.has_kw_rest_arg_node?
         *     d = d.dup
         *   end
         *   if pattern.has_kw_args_node?
         *     pattern.kw_args_node.each |k,|
         *       unless d.key?(k)
         *         goto match_failed
         *       end
         *     end
         *     pattern.kw_args_node.each |k, pat|
         *       if pattern.has_kw_rest_arg_node?
         *         unless pat.match?(d.delete(k))
         *           goto match_failed
         *         end
         *       else
         *         unless pat.match?(d[k])
         *           goto match_failed
         *         end
         *       end
         *     end
         *   else
         *     unless d.empty?
         *       goto match_failed
         *     end
         *   end
         *   if pattern.has_kw_rest_arg_node?
         *     if pattern.no_rest_keyword?
         *       unless d.empty?
         *         goto match_failed
         *       end
         *     else
         *       unless pattern.kw_rest_arg_node.match?(d)
         *         goto match_failed
         *       end
         *     end
         *   end
         *   goto matched
         * type_error:
         *   FrozenCore.raise TypeError
         * match_failed:
         *   goto unmatched
         */
        LABEL *match_failed, *type_error;
        VALUE keys = Qnil;

        match_failed = NEW_LABEL(line);
        type_error = NEW_LABEL(line);

        if (RNODE_HSHPTN(node)->nd_pkwargs && !RNODE_HSHPTN(node)->nd_pkwrestarg) {
            const NODE *kw_args = RNODE_HASH(RNODE_HSHPTN(node)->nd_pkwargs)->nd_head;
            keys = rb_ary_new_capa(kw_args ? RNODE_LIST(kw_args)->as.nd_alen/2 : 0);
            while (kw_args) {
                rb_ary_push(keys, get_symbol_value(iseq, RNODE_LIST(kw_args)->nd_head));
                kw_args = RNODE_LIST(RNODE_LIST(kw_args)->nd_next)->nd_next;
            }
        }

        CHECK(iseq_compile_pattern_constant(iseq, ret, node, match_failed, in_single_pattern, base_index));

        ADD_INSN(ret, line_node, dup);
        ADD_INSN1(ret, line_node, putobject, ID2SYM(rb_intern("deconstruct_keys")));
        ADD_SEND(ret, line_node, idRespond_to, INT2FIX(1)); // (1)
        if (in_single_pattern) {
            CHECK(iseq_compile_pattern_set_general_errmsg(iseq, ret, node, rb_fstring_lit("%p does not respond to #deconstruct_keys"), base_index + 1 /* (1) */));
        }
        ADD_INSNL(ret, line_node, branchunless, match_failed);

        if (NIL_P(keys)) {
            ADD_INSN(ret, line_node, putnil);
        }
        else {
            ADD_INSN1(ret, line_node, duparray, keys);
            RB_OBJ_WRITTEN(iseq, Qundef, rb_obj_hide(keys));
        }
        ADD_SEND(ret, line_node, rb_intern("deconstruct_keys"), INT2FIX(1)); // (2)

        ADD_INSN(ret, line_node, dup);
        ADD_INSN1(ret, line_node, checktype, INT2FIX(T_HASH));
        ADD_INSNL(ret, line_node, branchunless, type_error);

        if (RNODE_HSHPTN(node)->nd_pkwrestarg) {
            ADD_SEND(ret, line_node, rb_intern("dup"), INT2FIX(0));
        }

        if (RNODE_HSHPTN(node)->nd_pkwargs) {
            int i;
            int keys_num;
            const NODE *args;
            args = RNODE_HASH(RNODE_HSHPTN(node)->nd_pkwargs)->nd_head;
            if (args) {
                DECL_ANCHOR(match_values);
                INIT_ANCHOR(match_values);
                keys_num = rb_long2int(RNODE_LIST(args)->as.nd_alen) / 2;
                for (i = 0; i < keys_num; i++) {
                    NODE *key_node = RNODE_LIST(args)->nd_head;
                    NODE *value_node = RNODE_LIST(RNODE_LIST(args)->nd_next)->nd_head;
                    VALUE key = get_symbol_value(iseq, key_node);

                    ADD_INSN(ret, line_node, dup);
                    ADD_INSN1(ret, line_node, putobject, key);
                    ADD_SEND(ret, line_node, rb_intern("key?"), INT2FIX(1)); // (3)
                    if (in_single_pattern) {
                        LABEL *match_succeeded;
                        match_succeeded = NEW_LABEL(line);

                        ADD_INSN(ret, line_node, dup);
                        ADD_INSNL(ret, line_node, branchif, match_succeeded);

                        ADD_INSN1(ret, line_node, putobject, rb_str_freeze(rb_sprintf("key not found: %+"PRIsVALUE, key))); // (4)
                        ADD_INSN1(ret, line_node, setn, INT2FIX(base_index + CASE3_BI_OFFSET_ERROR_STRING + 2 /* (3), (4) */));
                        ADD_INSN1(ret, line_node, putobject, Qtrue); // (5)
                        ADD_INSN1(ret, line_node, setn, INT2FIX(base_index + CASE3_BI_OFFSET_KEY_ERROR_P + 3 /* (3), (4), (5) */));
                        ADD_INSN1(ret, line_node, topn, INT2FIX(3)); // (6)
                        ADD_INSN1(ret, line_node, setn, INT2FIX(base_index + CASE3_BI_OFFSET_KEY_ERROR_MATCHEE + 4 /* (3), (4), (5), (6) */));
                        ADD_INSN1(ret, line_node, putobject, key); // (7)
                        ADD_INSN1(ret, line_node, setn, INT2FIX(base_index + CASE3_BI_OFFSET_KEY_ERROR_KEY + 5 /* (3), (4), (5), (6), (7) */));

                        ADD_INSN1(ret, line_node, adjuststack, INT2FIX(4));

                        ADD_LABEL(ret, match_succeeded);
                    }
                    ADD_INSNL(ret, line_node, branchunless, match_failed);

                    ADD_INSN(match_values, line_node, dup);
                    ADD_INSN1(match_values, line_node, putobject, key);
                    ADD_SEND(match_values, line_node, RNODE_HSHPTN(node)->nd_pkwrestarg ? rb_intern("delete") : idAREF, INT2FIX(1)); // (8)
                    CHECK(iseq_compile_pattern_match(iseq, match_values, value_node, match_failed, in_single_pattern, in_alt_pattern, base_index + 1 /* (8) */, false));
                    args = RNODE_LIST(RNODE_LIST(args)->nd_next)->nd_next;
                }
                ADD_SEQ(ret, match_values);
            }
        }
        else {
            ADD_INSN(ret, line_node, dup);
            ADD_SEND(ret, line_node, idEmptyP, INT2FIX(0)); // (9)
            if (in_single_pattern) {
                CHECK(iseq_compile_pattern_set_general_errmsg(iseq, ret, node, rb_fstring_lit("%p is not empty"), base_index + 1 /* (9) */));
            }
            ADD_INSNL(ret, line_node, branchunless, match_failed);
        }

        if (RNODE_HSHPTN(node)->nd_pkwrestarg) {
            if (RNODE_HSHPTN(node)->nd_pkwrestarg == NODE_SPECIAL_NO_REST_KEYWORD) {
                ADD_INSN(ret, line_node, dup);
                ADD_SEND(ret, line_node, idEmptyP, INT2FIX(0)); // (10)
                if (in_single_pattern) {
                    CHECK(iseq_compile_pattern_set_general_errmsg(iseq, ret, node, rb_fstring_lit("rest of %p is not empty"), base_index + 1 /* (10) */));
                }
                ADD_INSNL(ret, line_node, branchunless, match_failed);
            }
            else {
                ADD_INSN(ret, line_node, dup); // (11)
                CHECK(iseq_compile_pattern_match(iseq, ret, RNODE_HSHPTN(node)->nd_pkwrestarg, match_failed, in_single_pattern, in_alt_pattern, base_index + 1 /* (11) */, false));
            }
        }

        ADD_INSN(ret, line_node, pop);
        ADD_INSNL(ret, line_node, jump, matched);
        ADD_INSN(ret, line_node, putnil);

        ADD_LABEL(ret, type_error);
        ADD_INSN1(ret, line_node, putspecialobject, INT2FIX(VM_SPECIAL_OBJECT_VMCORE));
        ADD_INSN1(ret, line_node, putobject, rb_eTypeError);
        ADD_INSN1(ret, line_node, putobject, rb_fstring_lit("deconstruct_keys must return Hash"));
        ADD_SEND(ret, line_node, id_core_raise, INT2FIX(2));
        ADD_INSN(ret, line_node, pop);

        ADD_LABEL(ret, match_failed);
        ADD_INSN(ret, line_node, pop);
        ADD_INSNL(ret, line_node, jump, unmatched);
        break;
      }
      case NODE_SYM:
      case NODE_REGX:
      case NODE_LINE:
      case NODE_INTEGER:
      case NODE_FLOAT:
      case NODE_RATIONAL:
      case NODE_IMAGINARY:
      case NODE_FILE:
      case NODE_ENCODING:
      case NODE_STR:
      case NODE_XSTR:
      case NODE_DSTR:
      case NODE_DSYM:
      case NODE_DREGX:
      case NODE_LIST:
      case NODE_ZLIST:
      case NODE_LAMBDA:
      case NODE_DOT2:
      case NODE_DOT3:
      case NODE_CONST:
      case NODE_LVAR:
      case NODE_DVAR:
      case NODE_IVAR:
      case NODE_CVAR:
      case NODE_GVAR:
      case NODE_TRUE:
      case NODE_FALSE:
      case NODE_SELF:
      case NODE_NIL:
      case NODE_COLON2:
      case NODE_COLON3:
      case NODE_BEGIN:
      case NODE_BLOCK:
      case NODE_ONCE:
        CHECK(COMPILE(ret, "case in literal", node)); // (1)
        if (in_single_pattern) {
            ADD_INSN1(ret, line_node, dupn, INT2FIX(2));
        }
        ADD_INSN1(ret, line_node, checkmatch, INT2FIX(VM_CHECKMATCH_TYPE_CASE)); // (2)
        if (in_single_pattern) {
            CHECK(iseq_compile_pattern_set_eqq_errmsg(iseq, ret, node, base_index + 2 /* (1), (2) */));
        }
        ADD_INSNL(ret, line_node, branchif, matched);
        ADD_INSNL(ret, line_node, jump, unmatched);
        break;
      case NODE_LASGN: {
        struct rb_iseq_constant_body *const body = ISEQ_BODY(iseq);
        ID id = RNODE_LASGN(node)->nd_vid;
        int idx = ISEQ_BODY(body->local_iseq)->local_table_size - get_local_var_idx(iseq, id);

        if (in_alt_pattern) {
            const char *name = rb_id2name(id);
            if (name && strlen(name) > 0 && name[0] != '_') {
                COMPILE_ERROR(ERROR_ARGS "illegal variable in alternative pattern (%"PRIsVALUE")",
                              rb_id2str(id));
                return COMPILE_NG;
            }
        }

        ADD_SETLOCAL(ret, line_node, idx, get_lvar_level(iseq));
        ADD_INSNL(ret, line_node, jump, matched);
        break;
      }
      case NODE_DASGN: {
        int idx, lv, ls;
        ID id = RNODE_DASGN(node)->nd_vid;

        idx = get_dyna_var_idx(iseq, id, &lv, &ls);

        if (in_alt_pattern) {
            const char *name = rb_id2name(id);
            if (name && strlen(name) > 0 && name[0] != '_') {
                COMPILE_ERROR(ERROR_ARGS "illegal variable in alternative pattern (%"PRIsVALUE")",
                              rb_id2str(id));
                return COMPILE_NG;
            }
        }

        if (idx < 0) {
            COMPILE_ERROR(ERROR_ARGS "NODE_DASGN: unknown id (%"PRIsVALUE")",
                          rb_id2str(id));
            return COMPILE_NG;
        }
        ADD_SETLOCAL(ret, line_node, ls - idx, lv);
        ADD_INSNL(ret, line_node, jump, matched);
        break;
      }
      case NODE_IF:
      case NODE_UNLESS: {
        LABEL *match_failed;
        match_failed = unmatched;
        CHECK(iseq_compile_pattern_match(iseq, ret, RNODE_IF(node)->nd_body, unmatched, in_single_pattern, in_alt_pattern, base_index, use_deconstructed_cache));
        CHECK(COMPILE(ret, "case in if", RNODE_IF(node)->nd_cond));
        if (in_single_pattern) {
            LABEL *match_succeeded;
            match_succeeded = NEW_LABEL(line);

            ADD_INSN(ret, line_node, dup);
            if (nd_type_p(node, NODE_IF)) {
                ADD_INSNL(ret, line_node, branchif, match_succeeded);
            }
            else {
                ADD_INSNL(ret, line_node, branchunless, match_succeeded);
            }

            ADD_INSN1(ret, line_node, putobject, rb_fstring_lit("guard clause does not return true")); // (1)
            ADD_INSN1(ret, line_node, setn, INT2FIX(base_index + CASE3_BI_OFFSET_ERROR_STRING + 1 /* (1) */)); // (2)
            ADD_INSN1(ret, line_node, putobject, Qfalse);
            ADD_INSN1(ret, line_node, setn, INT2FIX(base_index + CASE3_BI_OFFSET_KEY_ERROR_P + 2 /* (1), (2) */));

            ADD_INSN(ret, line_node, pop);
            ADD_INSN(ret, line_node, pop);

            ADD_LABEL(ret, match_succeeded);
        }
        if (nd_type_p(node, NODE_IF)) {
            ADD_INSNL(ret, line_node, branchunless, match_failed);
        }
        else {
            ADD_INSNL(ret, line_node, branchif, match_failed);
        }
        ADD_INSNL(ret, line_node, jump, matched);
        break;
      }
      case NODE_HASH: {
        NODE *n;
        LABEL *match_failed;
        match_failed = NEW_LABEL(line);

        n = RNODE_HASH(node)->nd_head;
        if (! (nd_type_p(n, NODE_LIST) && RNODE_LIST(n)->as.nd_alen == 2)) {
            COMPILE_ERROR(ERROR_ARGS "unexpected node");
            return COMPILE_NG;
        }

        ADD_INSN(ret, line_node, dup); // (1)
        CHECK(iseq_compile_pattern_match(iseq, ret, RNODE_LIST(n)->nd_head, match_failed, in_single_pattern, in_alt_pattern, base_index + 1 /* (1) */, use_deconstructed_cache));
        CHECK(iseq_compile_pattern_each(iseq, ret, RNODE_LIST(RNODE_LIST(n)->nd_next)->nd_head, matched, match_failed, in_single_pattern, in_alt_pattern, base_index, false));
        ADD_INSN(ret, line_node, putnil);

        ADD_LABEL(ret, match_failed);
        ADD_INSN(ret, line_node, pop);
        ADD_INSNL(ret, line_node, jump, unmatched);
        break;
      }
      case NODE_OR: {
        LABEL *match_succeeded, *fin;
        match_succeeded = NEW_LABEL(line);
        fin = NEW_LABEL(line);

        ADD_INSN(ret, line_node, dup); // (1)
        CHECK(iseq_compile_pattern_each(iseq, ret, RNODE_OR(node)->nd_1st, match_succeeded, fin, in_single_pattern, true, base_index + 1 /* (1) */, use_deconstructed_cache));
        ADD_LABEL(ret, match_succeeded);
        ADD_INSN(ret, line_node, pop);
        ADD_INSNL(ret, line_node, jump, matched);
        ADD_INSN(ret, line_node, putnil);
        ADD_LABEL(ret, fin);
        CHECK(iseq_compile_pattern_each(iseq, ret, RNODE_OR(node)->nd_2nd, matched, unmatched, in_single_pattern, true, base_index, use_deconstructed_cache));
        break;
      }
      default:
        UNKNOWN_NODE("NODE_IN", node, COMPILE_NG);
    }
    return COMPILE_OK;
}

static int
iseq_compile_pattern_match(rb_iseq_t *iseq, LINK_ANCHOR *const ret, const NODE *const node, LABEL *unmatched, bool in_single_pattern, bool in_alt_pattern, int base_index, bool use_deconstructed_cache)
{
    LABEL *fin = NEW_LABEL(nd_line(node));
    CHECK(iseq_compile_pattern_each(iseq, ret, node, fin, unmatched, in_single_pattern, in_alt_pattern, base_index, use_deconstructed_cache));
    ADD_LABEL(ret, fin);
    return COMPILE_OK;
}

static int
iseq_compile_pattern_constant(rb_iseq_t *iseq, LINK_ANCHOR *const ret, const NODE *const node, LABEL *match_failed, bool in_single_pattern, int base_index)
{
    const NODE *line_node = node;

    if (RNODE_ARYPTN(node)->nd_pconst) {
        ADD_INSN(ret, line_node, dup); // (1)
        CHECK(COMPILE(ret, "constant", RNODE_ARYPTN(node)->nd_pconst)); // (2)
        if (in_single_pattern) {
            ADD_INSN1(ret, line_node, dupn, INT2FIX(2));
        }
        ADD_INSN1(ret, line_node, checkmatch, INT2FIX(VM_CHECKMATCH_TYPE_CASE)); // (3)
        if (in_single_pattern) {
            CHECK(iseq_compile_pattern_set_eqq_errmsg(iseq, ret, node, base_index + 3 /* (1), (2), (3) */));
        }
        ADD_INSNL(ret, line_node, branchunless, match_failed);
    }
    return COMPILE_OK;
}


static int
iseq_compile_array_deconstruct(rb_iseq_t *iseq, LINK_ANCHOR *const ret, const NODE *const node, LABEL *deconstruct, LABEL *deconstructed, LABEL *match_failed, LABEL *type_error, bool in_single_pattern, int base_index, bool use_deconstructed_cache)
{
    const NODE *line_node = node;

    // NOTE: this optimization allows us to re-use the #deconstruct value
    // (or its absence).
    if (use_deconstructed_cache) {
        // If value is nil then we haven't tried to deconstruct
        ADD_INSN1(ret, line_node, topn, INT2FIX(base_index + CASE3_BI_OFFSET_DECONSTRUCTED_CACHE));
        ADD_INSNL(ret, line_node, branchnil, deconstruct);

        // If false then the value is not deconstructable
        ADD_INSN1(ret, line_node, topn, INT2FIX(base_index + CASE3_BI_OFFSET_DECONSTRUCTED_CACHE));
        ADD_INSNL(ret, line_node, branchunless, match_failed);

        // Drop value, add deconstructed to the stack and jump
        ADD_INSN(ret, line_node, pop); // (1)
        ADD_INSN1(ret, line_node, topn, INT2FIX(base_index + CASE3_BI_OFFSET_DECONSTRUCTED_CACHE - 1 /* (1) */));
        ADD_INSNL(ret, line_node, jump, deconstructed);
    }
    else {
        ADD_INSNL(ret, line_node, jump, deconstruct);
    }

    ADD_LABEL(ret, deconstruct);
    ADD_INSN(ret, line_node, dup);
    ADD_INSN1(ret, line_node, putobject, ID2SYM(rb_intern("deconstruct")));
    ADD_SEND(ret, line_node, idRespond_to, INT2FIX(1)); // (2)

    // Cache the result of respond_to? (in case it's false is stays there, if true - it's overwritten after #deconstruct)
    if (use_deconstructed_cache) {
        ADD_INSN1(ret, line_node, setn, INT2FIX(base_index + CASE3_BI_OFFSET_DECONSTRUCTED_CACHE + 1 /* (2) */));
    }

    if (in_single_pattern) {
        CHECK(iseq_compile_pattern_set_general_errmsg(iseq, ret, node, rb_fstring_lit("%p does not respond to #deconstruct"), base_index + 1 /* (2) */));
    }

    ADD_INSNL(ret, line_node, branchunless, match_failed);

    ADD_SEND(ret, line_node, rb_intern("deconstruct"), INT2FIX(0));

    // Cache the result (if it's cacheable - currently, only top-level array patterns)
    if (use_deconstructed_cache) {
        ADD_INSN1(ret, line_node, setn, INT2FIX(base_index + CASE3_BI_OFFSET_DECONSTRUCTED_CACHE));
    }

    ADD_INSN(ret, line_node, dup);
    ADD_INSN1(ret, line_node, checktype, INT2FIX(T_ARRAY));
    ADD_INSNL(ret, line_node, branchunless, type_error);

    ADD_LABEL(ret, deconstructed);

    return COMPILE_OK;
}

static int
iseq_compile_pattern_set_general_errmsg(rb_iseq_t *iseq, LINK_ANCHOR *const ret, const NODE *const node, VALUE errmsg, int base_index)
{
    /*
     *   if match_succeeded?
     *     goto match_succeeded
     *   end
     *   error_string = FrozenCore.sprintf(errmsg, matchee)
     *   key_error_p = false
     * match_succeeded:
     */
    const int line = nd_line(node);
    const NODE *line_node = node;
    LABEL *match_succeeded = NEW_LABEL(line);

    ADD_INSN(ret, line_node, dup);
    ADD_INSNL(ret, line_node, branchif, match_succeeded);

    ADD_INSN1(ret, line_node, putspecialobject, INT2FIX(VM_SPECIAL_OBJECT_VMCORE));
    ADD_INSN1(ret, line_node, putobject, errmsg);
    ADD_INSN1(ret, line_node, topn, INT2FIX(3));
    ADD_SEND(ret, line_node, id_core_sprintf, INT2FIX(2)); // (1)
    ADD_INSN1(ret, line_node, setn, INT2FIX(base_index + CASE3_BI_OFFSET_ERROR_STRING + 1 /* (1) */)); // (2)

    ADD_INSN1(ret, line_node, putobject, Qfalse);
    ADD_INSN1(ret, line_node, setn, INT2FIX(base_index + CASE3_BI_OFFSET_KEY_ERROR_P + 2 /* (1), (2) */));

    ADD_INSN(ret, line_node, pop);
    ADD_INSN(ret, line_node, pop);
    ADD_LABEL(ret, match_succeeded);

    return COMPILE_OK;
}

static int
iseq_compile_pattern_set_length_errmsg(rb_iseq_t *iseq, LINK_ANCHOR *const ret, const NODE *const node, VALUE errmsg, VALUE pattern_length, int base_index)
{
    /*
     *   if match_succeeded?
     *     goto match_succeeded
     *   end
     *   error_string = FrozenCore.sprintf(errmsg, matchee, matchee.length, pat.length)
     *   key_error_p = false
     * match_succeeded:
     */
    const int line = nd_line(node);
    const NODE *line_node = node;
    LABEL *match_succeeded = NEW_LABEL(line);

    ADD_INSN(ret, line_node, dup);
    ADD_INSNL(ret, line_node, branchif, match_succeeded);

    ADD_INSN1(ret, line_node, putspecialobject, INT2FIX(VM_SPECIAL_OBJECT_VMCORE));
    ADD_INSN1(ret, line_node, putobject, errmsg);
    ADD_INSN1(ret, line_node, topn, INT2FIX(3));
    ADD_INSN(ret, line_node, dup);
    ADD_SEND(ret, line_node, idLength, INT2FIX(0));
    ADD_INSN1(ret, line_node, putobject, pattern_length);
    ADD_SEND(ret, line_node, id_core_sprintf, INT2FIX(4)); // (1)
    ADD_INSN1(ret, line_node, setn, INT2FIX(base_index + CASE3_BI_OFFSET_ERROR_STRING + 1 /* (1) */)); // (2)

    ADD_INSN1(ret, line_node, putobject, Qfalse);
    ADD_INSN1(ret, line_node, setn, INT2FIX(base_index + CASE3_BI_OFFSET_KEY_ERROR_P + 2/* (1), (2) */));

    ADD_INSN(ret, line_node, pop);
    ADD_INSN(ret, line_node, pop);
    ADD_LABEL(ret, match_succeeded);

    return COMPILE_OK;
}

static int
iseq_compile_pattern_set_eqq_errmsg(rb_iseq_t *iseq, LINK_ANCHOR *const ret, const NODE *const node, int base_index)
{
    /*
     *   if match_succeeded?
     *     goto match_succeeded
     *   end
     *   error_string = FrozenCore.sprintf("%p === %p does not return true", pat, matchee)
     *   key_error_p = false
     * match_succeeded:
     */
    const int line = nd_line(node);
    const NODE *line_node = node;
    LABEL *match_succeeded = NEW_LABEL(line);

    ADD_INSN(ret, line_node, dup);
    ADD_INSNL(ret, line_node, branchif, match_succeeded);

    ADD_INSN1(ret, line_node, putspecialobject, INT2FIX(VM_SPECIAL_OBJECT_VMCORE));
    ADD_INSN1(ret, line_node, putobject, rb_fstring_lit("%p === %p does not return true"));
    ADD_INSN1(ret, line_node, topn, INT2FIX(3));
    ADD_INSN1(ret, line_node, topn, INT2FIX(5));
    ADD_SEND(ret, line_node, id_core_sprintf, INT2FIX(3)); // (1)
    ADD_INSN1(ret, line_node, setn, INT2FIX(base_index + CASE3_BI_OFFSET_ERROR_STRING + 1 /* (1) */)); // (2)

    ADD_INSN1(ret, line_node, putobject, Qfalse);
    ADD_INSN1(ret, line_node, setn, INT2FIX(base_index + CASE3_BI_OFFSET_KEY_ERROR_P + 2 /* (1), (2) */));

    ADD_INSN(ret, line_node, pop);
    ADD_INSN(ret, line_node, pop);

    ADD_LABEL(ret, match_succeeded);
    ADD_INSN1(ret, line_node, setn, INT2FIX(2));
    ADD_INSN(ret, line_node, pop);
    ADD_INSN(ret, line_node, pop);

    return COMPILE_OK;
}

static int
compile_case3(rb_iseq_t *iseq, LINK_ANCHOR *const ret, const NODE *const orig_node, int popped)
{
    const NODE *pattern;
    const NODE *node = orig_node;
    LABEL *endlabel, *elselabel;
    DECL_ANCHOR(head);
    DECL_ANCHOR(body_seq);
    DECL_ANCHOR(cond_seq);
    int line;
    enum node_type type;
    const NODE *line_node;
    VALUE branches = 0;
    int branch_id = 0;
    bool single_pattern;

    INIT_ANCHOR(head);
    INIT_ANCHOR(body_seq);
    INIT_ANCHOR(cond_seq);

    branches = decl_branch_base(iseq, PTR2NUM(node), nd_code_loc(node), "case");

    node = RNODE_CASE3(node)->nd_body;
    EXPECT_NODE("NODE_CASE3", node, NODE_IN, COMPILE_NG);
    type = nd_type(node);
    line = nd_line(node);
    line_node = node;
    single_pattern = !RNODE_IN(node)->nd_next;

    endlabel = NEW_LABEL(line);
    elselabel = NEW_LABEL(line);

    if (single_pattern) {
        /* allocate stack for ... */
        ADD_INSN(head, line_node, putnil); /* key_error_key */
        ADD_INSN(head, line_node, putnil); /* key_error_matchee */
        ADD_INSN1(head, line_node, putobject, Qfalse); /* key_error_p */
        ADD_INSN(head, line_node, putnil); /* error_string */
    }
    ADD_INSN(head, line_node, putnil); /* allocate stack for cached #deconstruct value */

    CHECK(COMPILE(head, "case base", RNODE_CASE3(orig_node)->nd_head));

    ADD_SEQ(ret, head);	/* case VAL */

    while (type == NODE_IN) {
        LABEL *l1;

        if (branch_id) {
            ADD_INSN(body_seq, line_node, putnil);
        }
        l1 = NEW_LABEL(line);
        ADD_LABEL(body_seq, l1);
        ADD_INSN1(body_seq, line_node, adjuststack, INT2FIX(single_pattern ? 6 : 2));

        const NODE *const coverage_node = RNODE_IN(node)->nd_body ? RNODE_IN(node)->nd_body : node;
        add_trace_branch_coverage(
            iseq,
            body_seq,
            nd_code_loc(coverage_node),
            nd_node_id(coverage_node),
            branch_id++,
            "in",
            branches);

        CHECK(COMPILE_(body_seq, "in body", RNODE_IN(node)->nd_body, popped));
        ADD_INSNL(body_seq, line_node, jump, endlabel);

        pattern = RNODE_IN(node)->nd_head;
        if (pattern) {
            int pat_line = nd_line(pattern);
            LABEL *next_pat = NEW_LABEL(pat_line);
            ADD_INSN (cond_seq, pattern, dup); /* dup case VAL */
            // NOTE: set base_index (it's "under" the matchee value, so it's position is 2)
            CHECK(iseq_compile_pattern_each(iseq, cond_seq, pattern, l1, next_pat, single_pattern, false, 2, true));
            ADD_LABEL(cond_seq, next_pat);
            LABEL_UNREMOVABLE(next_pat);
        }
        else {
            COMPILE_ERROR(ERROR_ARGS "unexpected node");
            return COMPILE_NG;
        }

        node = RNODE_IN(node)->nd_next;
        if (!node) {
            break;
        }
        type = nd_type(node);
        line = nd_line(node);
        line_node = node;
    }
    /* else */
    if (node) {
        ADD_LABEL(cond_seq, elselabel);
        ADD_INSN(cond_seq, line_node, pop);
        ADD_INSN(cond_seq, line_node, pop); /* discard cached #deconstruct value */
        add_trace_branch_coverage(iseq, cond_seq, nd_code_loc(node), nd_node_id(node), branch_id, "else", branches);
        CHECK(COMPILE_(cond_seq, "else", node, popped));
        ADD_INSNL(cond_seq, line_node, jump, endlabel);
        ADD_INSN(cond_seq, line_node, putnil);
        if (popped) {
            ADD_INSN(cond_seq, line_node, putnil);
        }
    }
    else {
        debugs("== else (implicit)\n");
        ADD_LABEL(cond_seq, elselabel);
        add_trace_branch_coverage(iseq, cond_seq, nd_code_loc(orig_node), nd_node_id(orig_node), branch_id, "else", branches);
        ADD_INSN1(cond_seq, orig_node, putspecialobject, INT2FIX(VM_SPECIAL_OBJECT_VMCORE));

        if (single_pattern) {
            /*
             *   if key_error_p
             *     FrozenCore.raise NoMatchingPatternKeyError.new(FrozenCore.sprintf("%p: %s", case_val, error_string), matchee: key_error_matchee, key: key_error_key)
             *   else
             *     FrozenCore.raise NoMatchingPatternError, FrozenCore.sprintf("%p: %s", case_val, error_string)
             *   end
             */
            LABEL *key_error, *fin;
            struct rb_callinfo_kwarg *kw_arg;

            key_error = NEW_LABEL(line);
            fin = NEW_LABEL(line);

            kw_arg = rb_xmalloc_mul_add(2, sizeof(VALUE), sizeof(struct rb_callinfo_kwarg));
            kw_arg->references = 0;
            kw_arg->keyword_len = 2;
            kw_arg->keywords[0] = ID2SYM(rb_intern("matchee"));
            kw_arg->keywords[1] = ID2SYM(rb_intern("key"));

            ADD_INSN1(cond_seq, orig_node, topn, INT2FIX(CASE3_BI_OFFSET_KEY_ERROR_P + 2));
            ADD_INSNL(cond_seq, orig_node, branchif, key_error);
            ADD_INSN1(cond_seq, orig_node, putobject, rb_eNoMatchingPatternError);
            ADD_INSN1(cond_seq, orig_node, putspecialobject, INT2FIX(VM_SPECIAL_OBJECT_VMCORE));
            ADD_INSN1(cond_seq, orig_node, putobject, rb_fstring_lit("%p: %s"));
            ADD_INSN1(cond_seq, orig_node, topn, INT2FIX(4)); /* case VAL */
            ADD_INSN1(cond_seq, orig_node, topn, INT2FIX(CASE3_BI_OFFSET_ERROR_STRING + 6));
            ADD_SEND(cond_seq, orig_node, id_core_sprintf, INT2FIX(3));
            ADD_SEND(cond_seq, orig_node, id_core_raise, INT2FIX(2));
            ADD_INSNL(cond_seq, orig_node, jump, fin);

            ADD_LABEL(cond_seq, key_error);
            ADD_INSN1(cond_seq, orig_node, putobject, rb_eNoMatchingPatternKeyError);
            ADD_INSN1(cond_seq, orig_node, putspecialobject, INT2FIX(VM_SPECIAL_OBJECT_VMCORE));
            ADD_INSN1(cond_seq, orig_node, putobject, rb_fstring_lit("%p: %s"));
            ADD_INSN1(cond_seq, orig_node, topn, INT2FIX(4)); /* case VAL */
            ADD_INSN1(cond_seq, orig_node, topn, INT2FIX(CASE3_BI_OFFSET_ERROR_STRING + 6));
            ADD_SEND(cond_seq, orig_node, id_core_sprintf, INT2FIX(3));
            ADD_INSN1(cond_seq, orig_node, topn, INT2FIX(CASE3_BI_OFFSET_KEY_ERROR_MATCHEE + 4));
            ADD_INSN1(cond_seq, orig_node, topn, INT2FIX(CASE3_BI_OFFSET_KEY_ERROR_KEY + 5));
            ADD_SEND_R(cond_seq, orig_node, rb_intern("new"), INT2FIX(1), NULL, INT2FIX(VM_CALL_KWARG), kw_arg);
            ADD_SEND(cond_seq, orig_node, id_core_raise, INT2FIX(1));

            ADD_LABEL(cond_seq, fin);
        }
        else {
            ADD_INSN1(cond_seq, orig_node, putobject, rb_eNoMatchingPatternError);
            ADD_INSN1(cond_seq, orig_node, topn, INT2FIX(2));
            ADD_SEND(cond_seq, orig_node, id_core_raise, INT2FIX(2));
        }
        ADD_INSN1(cond_seq, orig_node, adjuststack, INT2FIX(single_pattern ? 7 : 3));
        if (!popped) {
            ADD_INSN(cond_seq, orig_node, putnil);
        }
        ADD_INSNL(cond_seq, orig_node, jump, endlabel);
        ADD_INSN1(cond_seq, orig_node, dupn, INT2FIX(single_pattern ? 5 : 1));
        if (popped) {
            ADD_INSN(cond_seq, line_node, putnil);
        }
    }

    ADD_SEQ(ret, cond_seq);
    ADD_SEQ(ret, body_seq);
    ADD_LABEL(ret, endlabel);
    return COMPILE_OK;
}

#undef CASE3_BI_OFFSET_DECONSTRUCTED_CACHE
#undef CASE3_BI_OFFSET_ERROR_STRING
#undef CASE3_BI_OFFSET_KEY_ERROR_P
#undef CASE3_BI_OFFSET_KEY_ERROR_MATCHEE
#undef CASE3_BI_OFFSET_KEY_ERROR_KEY

static int
compile_loop(rb_iseq_t *iseq, LINK_ANCHOR *const ret, const NODE *const node, int popped, const enum node_type type)
{
    const int line = (int)nd_line(node);
    const NODE *line_node = node;

    LABEL *prev_start_label = ISEQ_COMPILE_DATA(iseq)->start_label;
    LABEL *prev_end_label = ISEQ_COMPILE_DATA(iseq)->end_label;
    LABEL *prev_redo_label = ISEQ_COMPILE_DATA(iseq)->redo_label;
    int prev_loopval_popped = ISEQ_COMPILE_DATA(iseq)->loopval_popped;
    VALUE branches = Qfalse;

    struct iseq_compile_data_ensure_node_stack enl;

    LABEL *next_label = ISEQ_COMPILE_DATA(iseq)->start_label = NEW_LABEL(line);	/* next  */
    LABEL *redo_label = ISEQ_COMPILE_DATA(iseq)->redo_label = NEW_LABEL(line);	/* redo  */
    LABEL *break_label = ISEQ_COMPILE_DATA(iseq)->end_label = NEW_LABEL(line);	/* break */
    LABEL *end_label = NEW_LABEL(line);
    LABEL *adjust_label = NEW_LABEL(line);

    LABEL *next_catch_label = NEW_LABEL(line);
    LABEL *tmp_label = NULL;

    ISEQ_COMPILE_DATA(iseq)->loopval_popped = 0;
    push_ensure_entry(iseq, &enl, NULL, NULL);

    if (RNODE_WHILE(node)->nd_state == 1) {
        ADD_INSNL(ret, line_node, jump, next_label);
    }
    else {
        tmp_label = NEW_LABEL(line);
        ADD_INSNL(ret, line_node, jump, tmp_label);
    }
    ADD_LABEL(ret, adjust_label);
    ADD_INSN(ret, line_node, putnil);
    ADD_LABEL(ret, next_catch_label);
    ADD_INSN(ret, line_node, pop);
    ADD_INSNL(ret, line_node, jump, next_label);
    if (tmp_label) ADD_LABEL(ret, tmp_label);

    ADD_LABEL(ret, redo_label);
    branches = decl_branch_base(iseq, PTR2NUM(node), nd_code_loc(node), type == NODE_WHILE ? "while" : "until");

    const NODE *const coverage_node = RNODE_WHILE(node)->nd_body ? RNODE_WHILE(node)->nd_body : node;
    add_trace_branch_coverage(
        iseq,
        ret,
        nd_code_loc(coverage_node),
        nd_node_id(coverage_node),
        0,
        "body",
        branches);

    CHECK(COMPILE_POPPED(ret, "while body", RNODE_WHILE(node)->nd_body));
    ADD_LABEL(ret, next_label);	/* next */

    if (type == NODE_WHILE) {
        CHECK(compile_branch_condition(iseq, ret, RNODE_WHILE(node)->nd_cond,
                                       redo_label, end_label));
    }
    else {
        /* until */
        CHECK(compile_branch_condition(iseq, ret, RNODE_WHILE(node)->nd_cond,
                                       end_label, redo_label));
    }

    ADD_LABEL(ret, end_label);
    ADD_ADJUST_RESTORE(ret, adjust_label);

    if (UNDEF_P(RNODE_WHILE(node)->nd_state)) {
        /* ADD_INSN(ret, line_node, putundef); */
        COMPILE_ERROR(ERROR_ARGS "unsupported: putundef");
        return COMPILE_NG;
    }
    else {
        ADD_INSN(ret, line_node, putnil);
    }

    ADD_LABEL(ret, break_label);	/* break */

    if (popped) {
        ADD_INSN(ret, line_node, pop);
    }

    ADD_CATCH_ENTRY(CATCH_TYPE_BREAK, redo_label, break_label, NULL,
                    break_label);
    ADD_CATCH_ENTRY(CATCH_TYPE_NEXT, redo_label, break_label, NULL,
                    next_catch_label);
    ADD_CATCH_ENTRY(CATCH_TYPE_REDO, redo_label, break_label, NULL,
                    ISEQ_COMPILE_DATA(iseq)->redo_label);

    ISEQ_COMPILE_DATA(iseq)->start_label = prev_start_label;
    ISEQ_COMPILE_DATA(iseq)->end_label = prev_end_label;
    ISEQ_COMPILE_DATA(iseq)->redo_label = prev_redo_label;
    ISEQ_COMPILE_DATA(iseq)->loopval_popped = prev_loopval_popped;
    ISEQ_COMPILE_DATA(iseq)->ensure_node_stack = ISEQ_COMPILE_DATA(iseq)->ensure_node_stack->prev;
    return COMPILE_OK;
}

static int
compile_iter(rb_iseq_t *iseq, LINK_ANCHOR *const ret, const NODE *const node, int popped)
{
    const int line = nd_line(node);
    const NODE *line_node = node;
    const rb_iseq_t *prevblock = ISEQ_COMPILE_DATA(iseq)->current_block;
    LABEL *retry_label = NEW_LABEL(line);
    LABEL *retry_end_l = NEW_LABEL(line);
    const rb_iseq_t *child_iseq;

    ADD_LABEL(ret, retry_label);
    if (nd_type_p(node, NODE_FOR)) {
        CHECK(COMPILE(ret, "iter caller (for)", RNODE_FOR(node)->nd_iter));

        ISEQ_COMPILE_DATA(iseq)->current_block = child_iseq =
            NEW_CHILD_ISEQ(RNODE_FOR(node)->nd_body, make_name_for_block(iseq),
                           ISEQ_TYPE_BLOCK, line);
        ADD_SEND_WITH_BLOCK(ret, line_node, idEach, INT2FIX(0), child_iseq);
    }
    else {
        ISEQ_COMPILE_DATA(iseq)->current_block = child_iseq =
            NEW_CHILD_ISEQ(RNODE_ITER(node)->nd_body, make_name_for_block(iseq),
                           ISEQ_TYPE_BLOCK, line);
        CHECK(COMPILE(ret, "iter caller", RNODE_ITER(node)->nd_iter));
    }

    {
        // We need to put the label "retry_end_l" immediately after the last "send" instruction.
        // This because vm_throw checks if the break cont is equal to the index of next insn of the "send".
        // (Otherwise, it is considered "break from proc-closure". See "TAG_BREAK" handling in "vm_throw_start".)
        //
        // Normally, "send" instruction is at the last.
        // However, qcall under branch coverage measurement adds some instructions after the "send".
        //
        // Note that "invokesuper", "invokesuperforward" appears instead of "send".
        INSN *iobj;
        LINK_ELEMENT *last_elem = LAST_ELEMENT(ret);
        iobj = IS_INSN(last_elem) ? (INSN*) last_elem : (INSN*) get_prev_insn((INSN*) last_elem);
        while (!IS_INSN_ID(iobj, send) && !IS_INSN_ID(iobj, invokesuper) && !IS_INSN_ID(iobj, sendforward) && !IS_INSN_ID(iobj, invokesuperforward)) {
            iobj = (INSN*) get_prev_insn(iobj);
        }
        ELEM_INSERT_NEXT(&iobj->link, (LINK_ELEMENT*) retry_end_l);

        // LINK_ANCHOR has a pointer to the last element, but ELEM_INSERT_NEXT does not update it
        // even if we add an insn to the last of LINK_ANCHOR. So this updates it manually.
        if (&iobj->link == LAST_ELEMENT(ret)) {
            ret->last = (LINK_ELEMENT*) retry_end_l;
        }
    }

    if (popped) {
        ADD_INSN(ret, line_node, pop);
    }

    ISEQ_COMPILE_DATA(iseq)->current_block = prevblock;

    ADD_CATCH_ENTRY(CATCH_TYPE_BREAK, retry_label, retry_end_l, child_iseq, retry_end_l);
    return COMPILE_OK;
}

static int
compile_for_masgn(rb_iseq_t *iseq, LINK_ANCHOR *const ret, const NODE *const node, int popped)
{
    /* massign to var in "for"
     * (args.length == 1 && Array.try_convert(args[0])) || args
     */
    const NODE *line_node = node;
    const NODE *var = RNODE_FOR_MASGN(node)->nd_var;
    LABEL *not_single = NEW_LABEL(nd_line(var));
    LABEL *not_ary = NEW_LABEL(nd_line(var));
    CHECK(COMPILE(ret, "for var", var));
    ADD_INSN(ret, line_node, dup);
    ADD_CALL(ret, line_node, idLength, INT2FIX(0));
    ADD_INSN1(ret, line_node, putobject, INT2FIX(1));
    ADD_CALL(ret, line_node, idEq, INT2FIX(1));
    ADD_INSNL(ret, line_node, branchunless, not_single);
    ADD_INSN(ret, line_node, dup);
    ADD_INSN1(ret, line_node, putobject, INT2FIX(0));
    ADD_CALL(ret, line_node, idAREF, INT2FIX(1));
    ADD_INSN1(ret, line_node, putobject, rb_cArray);
    ADD_INSN(ret, line_node, swap);
    ADD_CALL(ret, line_node, rb_intern("try_convert"), INT2FIX(1));
    ADD_INSN(ret, line_node, dup);
    ADD_INSNL(ret, line_node, branchunless, not_ary);
    ADD_INSN(ret, line_node, swap);
    ADD_LABEL(ret, not_ary);
    ADD_INSN(ret, line_node, pop);
    ADD_LABEL(ret, not_single);
    return COMPILE_OK;
}

static int
compile_break(rb_iseq_t *iseq, LINK_ANCHOR *const ret, const NODE *const node, int popped)
{
    const NODE *line_node = node;
    unsigned long throw_flag = 0;

    if (ISEQ_COMPILE_DATA(iseq)->redo_label != 0 && can_add_ensure_iseq(iseq)) {
        /* while/until */
        LABEL *splabel = NEW_LABEL(0);
        ADD_LABEL(ret, splabel);
        ADD_ADJUST(ret, line_node, ISEQ_COMPILE_DATA(iseq)->redo_label);
        CHECK(COMPILE_(ret, "break val (while/until)", RNODE_BREAK(node)->nd_stts,
                       ISEQ_COMPILE_DATA(iseq)->loopval_popped));
        add_ensure_iseq(ret, iseq, 0);
        ADD_INSNL(ret, line_node, jump, ISEQ_COMPILE_DATA(iseq)->end_label);
        ADD_ADJUST_RESTORE(ret, splabel);

        if (!popped) {
            ADD_INSN(ret, line_node, putnil);
        }
    }
    else {
        const rb_iseq_t *ip = iseq;

        while (ip) {
            if (!ISEQ_COMPILE_DATA(ip)) {
                ip = 0;
                break;
            }

            if (ISEQ_COMPILE_DATA(ip)->redo_label != 0) {
                throw_flag = VM_THROW_NO_ESCAPE_FLAG;
            }
            else if (ISEQ_BODY(ip)->type == ISEQ_TYPE_BLOCK) {
                throw_flag = 0;
            }
            else if (ISEQ_BODY(ip)->type == ISEQ_TYPE_EVAL) {
                COMPILE_ERROR(ERROR_ARGS "Can't escape from eval with break");
                return COMPILE_NG;
            }
            else {
                ip = ISEQ_BODY(ip)->parent_iseq;
                continue;
            }

            /* escape from block */
            CHECK(COMPILE(ret, "break val (block)", RNODE_BREAK(node)->nd_stts));
            ADD_INSN1(ret, line_node, throw, INT2FIX(throw_flag | TAG_BREAK));
            if (popped) {
                ADD_INSN(ret, line_node, pop);
            }
            return COMPILE_OK;
        }
        COMPILE_ERROR(ERROR_ARGS "Invalid break");
        return COMPILE_NG;
    }
    return COMPILE_OK;
}

static int
compile_next(rb_iseq_t *iseq, LINK_ANCHOR *const ret, const NODE *const node, int popped)
{
    const NODE *line_node = node;
    unsigned long throw_flag = 0;

    if (ISEQ_COMPILE_DATA(iseq)->redo_label != 0 && can_add_ensure_iseq(iseq)) {
        LABEL *splabel = NEW_LABEL(0);
        debugs("next in while loop\n");
        ADD_LABEL(ret, splabel);
        CHECK(COMPILE(ret, "next val/valid syntax?", RNODE_NEXT(node)->nd_stts));
        add_ensure_iseq(ret, iseq, 0);
        ADD_ADJUST(ret, line_node, ISEQ_COMPILE_DATA(iseq)->redo_label);
        ADD_INSNL(ret, line_node, jump, ISEQ_COMPILE_DATA(iseq)->start_label);
        ADD_ADJUST_RESTORE(ret, splabel);
        if (!popped) {
            ADD_INSN(ret, line_node, putnil);
        }
    }
    else if (ISEQ_COMPILE_DATA(iseq)->end_label && can_add_ensure_iseq(iseq)) {
        LABEL *splabel = NEW_LABEL(0);
        debugs("next in block\n");
        ADD_LABEL(ret, splabel);
        ADD_ADJUST(ret, line_node, ISEQ_COMPILE_DATA(iseq)->start_label);
        CHECK(COMPILE(ret, "next val", RNODE_NEXT(node)->nd_stts));
        add_ensure_iseq(ret, iseq, 0);
        ADD_INSNL(ret, line_node, jump, ISEQ_COMPILE_DATA(iseq)->end_label);
        ADD_ADJUST_RESTORE(ret, splabel);

        if (!popped) {
            ADD_INSN(ret, line_node, putnil);
        }
    }
    else {
        const rb_iseq_t *ip = iseq;

        while (ip) {
            if (!ISEQ_COMPILE_DATA(ip)) {
                ip = 0;
                break;
            }

            throw_flag = VM_THROW_NO_ESCAPE_FLAG;
            if (ISEQ_COMPILE_DATA(ip)->redo_label != 0) {
                /* while loop */
                break;
            }
            else if (ISEQ_BODY(ip)->type == ISEQ_TYPE_BLOCK) {
                break;
            }
            else if (ISEQ_BODY(ip)->type == ISEQ_TYPE_EVAL) {
                COMPILE_ERROR(ERROR_ARGS "Can't escape from eval with next");
                return COMPILE_NG;
            }

            ip = ISEQ_BODY(ip)->parent_iseq;
        }
        if (ip != 0) {
            CHECK(COMPILE(ret, "next val", RNODE_NEXT(node)->nd_stts));
            ADD_INSN1(ret, line_node, throw, INT2FIX(throw_flag | TAG_NEXT));

            if (popped) {
                ADD_INSN(ret, line_node, pop);
            }
        }
        else {
            COMPILE_ERROR(ERROR_ARGS "Invalid next");
            return COMPILE_NG;
        }
    }
    return COMPILE_OK;
}

static int
compile_redo(rb_iseq_t *iseq, LINK_ANCHOR *const ret, const NODE *const node, int popped)
{
    const NODE *line_node = node;

    if (ISEQ_COMPILE_DATA(iseq)->redo_label && can_add_ensure_iseq(iseq)) {
        LABEL *splabel = NEW_LABEL(0);
        debugs("redo in while");
        ADD_LABEL(ret, splabel);
        ADD_ADJUST(ret, line_node, ISEQ_COMPILE_DATA(iseq)->redo_label);
        add_ensure_iseq(ret, iseq, 0);
        ADD_INSNL(ret, line_node, jump, ISEQ_COMPILE_DATA(iseq)->redo_label);
        ADD_ADJUST_RESTORE(ret, splabel);
        if (!popped) {
            ADD_INSN(ret, line_node, putnil);
        }
    }
    else if (ISEQ_BODY(iseq)->type != ISEQ_TYPE_EVAL && ISEQ_COMPILE_DATA(iseq)->start_label && can_add_ensure_iseq(iseq)) {
        LABEL *splabel = NEW_LABEL(0);

        debugs("redo in block");
        ADD_LABEL(ret, splabel);
        add_ensure_iseq(ret, iseq, 0);
        ADD_ADJUST(ret, line_node, ISEQ_COMPILE_DATA(iseq)->start_label);
        ADD_INSNL(ret, line_node, jump, ISEQ_COMPILE_DATA(iseq)->start_label);
        ADD_ADJUST_RESTORE(ret, splabel);

        if (!popped) {
            ADD_INSN(ret, line_node, putnil);
        }
    }
    else {
        const rb_iseq_t *ip = iseq;

        while (ip) {
            if (!ISEQ_COMPILE_DATA(ip)) {
                ip = 0;
                break;
            }

            if (ISEQ_COMPILE_DATA(ip)->redo_label != 0) {
                break;
            }
            else if (ISEQ_BODY(ip)->type == ISEQ_TYPE_BLOCK) {
                break;
            }
            else if (ISEQ_BODY(ip)->type == ISEQ_TYPE_EVAL) {
                COMPILE_ERROR(ERROR_ARGS "Can't escape from eval with redo");
                return COMPILE_NG;
            }

            ip = ISEQ_BODY(ip)->parent_iseq;
        }
        if (ip != 0) {
            ADD_INSN(ret, line_node, putnil);
            ADD_INSN1(ret, line_node, throw, INT2FIX(VM_THROW_NO_ESCAPE_FLAG | TAG_REDO));

            if (popped) {
                ADD_INSN(ret, line_node, pop);
            }
        }
        else {
            COMPILE_ERROR(ERROR_ARGS "Invalid redo");
            return COMPILE_NG;
        }
    }
    return COMPILE_OK;
}

static int
compile_retry(rb_iseq_t *iseq, LINK_ANCHOR *const ret, const NODE *const node, int popped)
{
    const NODE *line_node = node;

    if (ISEQ_BODY(iseq)->type == ISEQ_TYPE_RESCUE) {
        ADD_INSN(ret, line_node, putnil);
        ADD_INSN1(ret, line_node, throw, INT2FIX(TAG_RETRY));

        if (popped) {
            ADD_INSN(ret, line_node, pop);
        }
    }
    else {
        COMPILE_ERROR(ERROR_ARGS "Invalid retry");
        return COMPILE_NG;
    }
    return COMPILE_OK;
}

static int
compile_rescue(rb_iseq_t *iseq, LINK_ANCHOR *const ret, const NODE *const node, int popped)
{
    const int line = nd_line(node);
    const NODE *line_node = node;
    LABEL *lstart = NEW_LABEL(line);
    LABEL *lend = NEW_LABEL(line);
    LABEL *lcont = NEW_LABEL(line);
    const rb_iseq_t *rescue = NEW_CHILD_ISEQ(RNODE_RESCUE(node)->nd_resq,
                                             rb_str_concat(rb_str_new2("rescue in "),
                                                           ISEQ_BODY(iseq)->location.label),
                                             ISEQ_TYPE_RESCUE, line);

    lstart->rescued = LABEL_RESCUE_BEG;
    lend->rescued = LABEL_RESCUE_END;
    ADD_LABEL(ret, lstart);

    bool prev_in_rescue = ISEQ_COMPILE_DATA(iseq)->in_rescue;
    ISEQ_COMPILE_DATA(iseq)->in_rescue = true;
    {
        CHECK(COMPILE(ret, "rescue head", RNODE_RESCUE(node)->nd_head));
    }
    ISEQ_COMPILE_DATA(iseq)->in_rescue = prev_in_rescue;

    ADD_LABEL(ret, lend);
    if (RNODE_RESCUE(node)->nd_else) {
        ADD_INSN(ret, line_node, pop);
        CHECK(COMPILE(ret, "rescue else", RNODE_RESCUE(node)->nd_else));
    }
    ADD_INSN(ret, line_node, nop);
    ADD_LABEL(ret, lcont);

    if (popped) {
        ADD_INSN(ret, line_node, pop);
    }

    /* register catch entry */
    ADD_CATCH_ENTRY(CATCH_TYPE_RESCUE, lstart, lend, rescue, lcont);
    ADD_CATCH_ENTRY(CATCH_TYPE_RETRY, lend, lcont, NULL, lstart);
    return COMPILE_OK;
}

static int
compile_resbody(rb_iseq_t *iseq, LINK_ANCHOR *const ret, const NODE *const node, int popped)
{
    const int line = nd_line(node);
    const NODE *line_node = node;
    const NODE *resq = node;
    const NODE *narg;
    LABEL *label_miss, *label_hit;

    while (resq) {
        label_miss = NEW_LABEL(line);
        label_hit = NEW_LABEL(line);

        narg = RNODE_RESBODY(resq)->nd_args;
        if (narg) {
            switch (nd_type(narg)) {
              case NODE_LIST:
                while (narg) {
                    ADD_GETLOCAL(ret, line_node, LVAR_ERRINFO, 0);
                    CHECK(COMPILE(ret, "rescue arg", RNODE_LIST(narg)->nd_head));
                    ADD_INSN1(ret, line_node, checkmatch, INT2FIX(VM_CHECKMATCH_TYPE_RESCUE));
                    ADD_INSNL(ret, line_node, branchif, label_hit);
                    narg = RNODE_LIST(narg)->nd_next;
                }
                break;
              case NODE_SPLAT:
              case NODE_ARGSCAT:
              case NODE_ARGSPUSH:
                ADD_GETLOCAL(ret, line_node, LVAR_ERRINFO, 0);
                CHECK(COMPILE(ret, "rescue/cond splat", narg));
                ADD_INSN1(ret, line_node, checkmatch, INT2FIX(VM_CHECKMATCH_TYPE_RESCUE | VM_CHECKMATCH_ARRAY));
                ADD_INSNL(ret, line_node, branchif, label_hit);
                break;
              default:
                UNKNOWN_NODE("NODE_RESBODY", narg, COMPILE_NG);
            }
        }
        else {
            ADD_GETLOCAL(ret, line_node, LVAR_ERRINFO, 0);
            ADD_INSN1(ret, line_node, putobject, rb_eStandardError);
            ADD_INSN1(ret, line_node, checkmatch, INT2FIX(VM_CHECKMATCH_TYPE_RESCUE));
            ADD_INSNL(ret, line_node, branchif, label_hit);
        }
        ADD_INSNL(ret, line_node, jump, label_miss);
        ADD_LABEL(ret, label_hit);
        ADD_TRACE(ret, RUBY_EVENT_RESCUE);

        if (RNODE_RESBODY(resq)->nd_exc_var) {
            CHECK(COMPILE_POPPED(ret, "resbody exc_var", RNODE_RESBODY(resq)->nd_exc_var));
        }

        if (nd_type(RNODE_RESBODY(resq)->nd_body) == NODE_BEGIN && RNODE_BEGIN(RNODE_RESBODY(resq)->nd_body)->nd_body == NULL && !RNODE_RESBODY(resq)->nd_exc_var) {
            // empty body
            ADD_SYNTHETIC_INSN(ret, nd_line(RNODE_RESBODY(resq)->nd_body), -1, putnil);
        }
        else {
            CHECK(COMPILE(ret, "resbody body", RNODE_RESBODY(resq)->nd_body));
        }

        if (ISEQ_COMPILE_DATA(iseq)->option->tailcall_optimization) {
            ADD_INSN(ret, line_node, nop);
        }
        ADD_INSN(ret, line_node, leave);
        ADD_LABEL(ret, label_miss);
        resq = RNODE_RESBODY(resq)->nd_next;
    }
    return COMPILE_OK;
}

static int
compile_ensure(rb_iseq_t *iseq, LINK_ANCHOR *const ret, const NODE *const node, int popped)
{
    const int line = nd_line(RNODE_ENSURE(node)->nd_ensr);
    const NODE *line_node = node;
    DECL_ANCHOR(ensr);
    const rb_iseq_t *ensure = NEW_CHILD_ISEQ(RNODE_ENSURE(node)->nd_ensr,
                                             rb_str_concat(rb_str_new2 ("ensure in "), ISEQ_BODY(iseq)->location.label),
                                             ISEQ_TYPE_ENSURE, line);
    LABEL *lstart = NEW_LABEL(line);
    LABEL *lend = NEW_LABEL(line);
    LABEL *lcont = NEW_LABEL(line);
    LINK_ELEMENT *last;
    int last_leave = 0;
    struct ensure_range er;
    struct iseq_compile_data_ensure_node_stack enl;
    struct ensure_range *erange;

    INIT_ANCHOR(ensr);
    CHECK(COMPILE_POPPED(ensr, "ensure ensr", RNODE_ENSURE(node)->nd_ensr));
    last = ensr->last;
    last_leave = last && IS_INSN(last) && IS_INSN_ID(last, leave);

    er.begin = lstart;
    er.end = lend;
    er.next = 0;
    push_ensure_entry(iseq, &enl, &er, RNODE_ENSURE(node)->nd_ensr);

    ADD_LABEL(ret, lstart);
    CHECK(COMPILE_(ret, "ensure head", RNODE_ENSURE(node)->nd_head, (popped | last_leave)));
    ADD_LABEL(ret, lend);
    ADD_SEQ(ret, ensr);
    if (!popped && last_leave) ADD_INSN(ret, line_node, putnil);
    ADD_LABEL(ret, lcont);
    if (last_leave) ADD_INSN(ret, line_node, pop);

    erange = ISEQ_COMPILE_DATA(iseq)->ensure_node_stack->erange;
    if (lstart->link.next != &lend->link) {
        while (erange) {
            ADD_CATCH_ENTRY(CATCH_TYPE_ENSURE, erange->begin, erange->end,
                            ensure, lcont);
            erange = erange->next;
        }
    }

    ISEQ_COMPILE_DATA(iseq)->ensure_node_stack = enl.prev;
    return COMPILE_OK;
}

static int
compile_return(rb_iseq_t *iseq, LINK_ANCHOR *const ret, const NODE *const node, int popped)
{
    const NODE *line_node = node;

    if (iseq) {
        enum rb_iseq_type type = ISEQ_BODY(iseq)->type;
        const rb_iseq_t *is = iseq;
        enum rb_iseq_type t = type;
        const NODE *retval = RNODE_RETURN(node)->nd_stts;
        LABEL *splabel = 0;

        while (t == ISEQ_TYPE_RESCUE || t == ISEQ_TYPE_ENSURE) {
            if (!(is = ISEQ_BODY(is)->parent_iseq)) break;
            t = ISEQ_BODY(is)->type;
        }
        switch (t) {
          case ISEQ_TYPE_TOP:
          case ISEQ_TYPE_MAIN:
            if (retval) {
                rb_warn("argument of top-level return is ignored");
            }
            if (is == iseq) {
                /* plain top-level, leave directly */
                type = ISEQ_TYPE_METHOD;
            }
            break;
          default:
            break;
        }

        if (type == ISEQ_TYPE_METHOD) {
            splabel = NEW_LABEL(0);
            ADD_LABEL(ret, splabel);
            ADD_ADJUST(ret, line_node, 0);
        }

        CHECK(COMPILE(ret, "return nd_stts (return val)", retval));

        if (type == ISEQ_TYPE_METHOD && can_add_ensure_iseq(iseq)) {
            add_ensure_iseq(ret, iseq, 1);
            ADD_TRACE(ret, RUBY_EVENT_RETURN);
            ADD_INSN(ret, line_node, leave);
            ADD_ADJUST_RESTORE(ret, splabel);

            if (!popped) {
                ADD_INSN(ret, line_node, putnil);
            }
        }
        else {
            ADD_INSN1(ret, line_node, throw, INT2FIX(TAG_RETURN));
            if (popped) {
                ADD_INSN(ret, line_node, pop);
            }
        }
    }
    return COMPILE_OK;
}

static bool
drop_unreachable_return(LINK_ANCHOR *ret)
{
    LINK_ELEMENT *i = ret->last, *last;
    if (!i) return false;
    if (IS_TRACE(i)) i = i->prev;
    if (!IS_INSN(i) || !IS_INSN_ID(i, putnil)) return false;
    last = i = i->prev;
    if (IS_ADJUST(i)) i = i->prev;
    if (!IS_INSN(i)) return false;
    switch (INSN_OF(i)) {
      case BIN(leave):
      case BIN(jump):
        break;
      default:
        return false;
    }
    (ret->last = last->prev)->next = NULL;
    return true;
}

static int
compile_evstr(rb_iseq_t *iseq, LINK_ANCHOR *const ret, const NODE *const node, int popped)
{
    CHECK(COMPILE_(ret, "nd_body", node, popped));

    if (!popped && !all_string_result_p(node)) {
        const NODE *line_node = node;
        const unsigned int flag = VM_CALL_FCALL;

        // Note, this dup could be removed if we are willing to change anytostring. It pops
        // two VALUEs off the stack when it could work by replacing the top most VALUE.
        ADD_INSN(ret, line_node, dup);
        ADD_INSN1(ret, line_node, objtostring, new_callinfo(iseq, idTo_s, 0, flag, NULL, FALSE));
        ADD_INSN(ret, line_node, anytostring);
    }
    return COMPILE_OK;
}

static void
compile_lvar(rb_iseq_t *iseq, LINK_ANCHOR *const ret, const NODE *line_node, ID id)
{
    int idx = ISEQ_BODY(ISEQ_BODY(iseq)->local_iseq)->local_table_size - get_local_var_idx(iseq, id);

    debugs("id: %s idx: %d\n", rb_id2name(id), idx);
    ADD_GETLOCAL(ret, line_node, idx, get_lvar_level(iseq));
}

static LABEL *
qcall_branch_start(rb_iseq_t *iseq, LINK_ANCHOR *const recv, VALUE *branches, const NODE *node, const NODE *line_node)
{
    LABEL *else_label = NEW_LABEL(nd_line(line_node));
    VALUE br = 0;

    br = decl_branch_base(iseq, PTR2NUM(node), nd_code_loc(node), "&.");
    *branches = br;
    ADD_INSN(recv, line_node, dup);
    ADD_INSNL(recv, line_node, branchnil, else_label);
    add_trace_branch_coverage(iseq, recv, nd_code_loc(node), nd_node_id(node), 0, "then", br);
    return else_label;
}

static void
qcall_branch_end(rb_iseq_t *iseq, LINK_ANCHOR *const ret, LABEL *else_label, VALUE branches, const NODE *node, const NODE *line_node)
{
    LABEL *end_label;
    if (!else_label) return;
    end_label = NEW_LABEL(nd_line(line_node));
    ADD_INSNL(ret, line_node, jump, end_label);
    ADD_LABEL(ret, else_label);
    add_trace_branch_coverage(iseq, ret, nd_code_loc(node), nd_node_id(node), 1, "else", branches);
    ADD_LABEL(ret, end_label);
}

static int
compile_call_precheck_freeze(rb_iseq_t *iseq, LINK_ANCHOR *const ret, const NODE *const node, const NODE *line_node, int popped)
{
    /* optimization shortcut
     *   "literal".freeze -> opt_str_freeze("literal")
     */
    if (get_nd_recv(node) &&
        (nd_type_p(get_nd_recv(node), NODE_STR) || nd_type_p(get_nd_recv(node), NODE_FILE)) &&
        (get_node_call_nd_mid(node) == idFreeze || get_node_call_nd_mid(node) == idUMinus) &&
        get_nd_args(node) == NULL &&
        ISEQ_COMPILE_DATA(iseq)->current_block == NULL &&
        ISEQ_COMPILE_DATA(iseq)->option->specialized_instruction) {
        VALUE str = get_string_value(get_nd_recv(node));
        if (get_node_call_nd_mid(node) == idUMinus) {
            ADD_INSN2(ret, line_node, opt_str_uminus, str,
                      new_callinfo(iseq, idUMinus, 0, 0, NULL, FALSE));
        }
        else {
            ADD_INSN2(ret, line_node, opt_str_freeze, str,
                      new_callinfo(iseq, idFreeze, 0, 0, NULL, FALSE));
        }
        RB_OBJ_WRITTEN(iseq, Qundef, str);
        if (popped) {
            ADD_INSN(ret, line_node, pop);
        }
        return TRUE;
    }
    /* optimization shortcut
     *   obj["literal"] -> opt_aref_with(obj, "literal")
     */
    if (get_node_call_nd_mid(node) == idAREF && !private_recv_p(node) && get_nd_args(node) &&
        nd_type_p(get_nd_args(node), NODE_LIST) && RNODE_LIST(get_nd_args(node))->as.nd_alen == 1 &&
        (nd_type_p(RNODE_LIST(get_nd_args(node))->nd_head, NODE_STR) || nd_type_p(RNODE_LIST(get_nd_args(node))->nd_head, NODE_FILE)) &&
        ISEQ_COMPILE_DATA(iseq)->current_block == NULL &&
        !frozen_string_literal_p(iseq) &&
        ISEQ_COMPILE_DATA(iseq)->option->specialized_instruction) {
        VALUE str = get_string_value(RNODE_LIST(get_nd_args(node))->nd_head);
        CHECK(COMPILE(ret, "recv", get_nd_recv(node)));
        ADD_INSN2(ret, line_node, opt_aref_with, str,
                  new_callinfo(iseq, idAREF, 1, 0, NULL, FALSE));
        RB_OBJ_WRITTEN(iseq, Qundef, str);
        if (popped) {
            ADD_INSN(ret, line_node, pop);
        }
        return TRUE;
    }
    return FALSE;
}

static int
iseq_has_builtin_function_table(const rb_iseq_t *iseq)
{
    return ISEQ_COMPILE_DATA(iseq)->builtin_function_table != NULL;
}

static const struct rb_builtin_function *
iseq_builtin_function_lookup(const rb_iseq_t *iseq, const char *name)
{
    int i;
    const struct rb_builtin_function *table = ISEQ_COMPILE_DATA(iseq)->builtin_function_table;
    for (i=0; table[i].index != -1; i++) {
        if (strcmp(table[i].name, name) == 0) {
            return &table[i];
        }
    }
    return NULL;
}

static const char *
iseq_builtin_function_name(const enum node_type type, const NODE *recv, ID mid)
{
    const char *name = rb_id2name(mid);
    static const char prefix[] = "__builtin_";
    const size_t prefix_len = sizeof(prefix) - 1;

    switch (type) {
      case NODE_CALL:
        if (recv) {
            switch (nd_type(recv)) {
              case NODE_VCALL:
                if (RNODE_VCALL(recv)->nd_mid == rb_intern("__builtin")) {
                    return name;
                }
                break;
              case NODE_CONST:
                if (RNODE_CONST(recv)->nd_vid == rb_intern("Primitive")) {
                    return name;
                }
                break;
              default: break;
            }
        }
        break;
      case NODE_VCALL:
      case NODE_FCALL:
        if (UNLIKELY(strncmp(prefix, name, prefix_len) == 0)) {
            return &name[prefix_len];
        }
        break;
      default: break;
    }
    return NULL;
}

static int
delegate_call_p(const rb_iseq_t *iseq, unsigned int argc, const LINK_ANCHOR *args, unsigned int *pstart_index)
{

    if (argc == 0) {
        *pstart_index = 0;
        return TRUE;
    }
    else if (argc <= ISEQ_BODY(iseq)->local_table_size) {
        unsigned int start=0;

        // local_table: [p1, p2, p3, l1, l2, l3]
        // arguments:           [p3, l1, l2]     -> 2
        for (start = 0;
             argc + start <= ISEQ_BODY(iseq)->local_table_size;
             start++) {
            const LINK_ELEMENT *elem = FIRST_ELEMENT(args);

            for (unsigned int i=start; i-start<argc; i++) {
                if (IS_INSN(elem) &&
                    INSN_OF(elem) == BIN(getlocal)) {
                    int local_index = FIX2INT(OPERAND_AT(elem, 0));
                    int local_level = FIX2INT(OPERAND_AT(elem, 1));

                    if (local_level == 0) {
                        unsigned int index = ISEQ_BODY(iseq)->local_table_size - (local_index - VM_ENV_DATA_SIZE + 1);
                        if (0) { // for debug
                            fprintf(stderr, "lvar:%s (%d), id:%s (%d) local_index:%d, local_size:%d\n",
                                    rb_id2name(ISEQ_BODY(iseq)->local_table[i]),     i,
                                    rb_id2name(ISEQ_BODY(iseq)->local_table[index]), index,
                                    local_index, (int)ISEQ_BODY(iseq)->local_table_size);
                        }
                        if (i == index) {
                            elem = elem->next;
                            continue; /* for */
                        }
                        else {
                            goto next;
                        }
                    }
                    else {
                        goto fail; // level != 0 is unsupported
                    }
                }
                else {
                    goto fail; // insn is not a getlocal
                }
            }
            goto success;
          next:;
        }
      fail:
        return FALSE;
      success:
        *pstart_index = start;
        return TRUE;
    }
    else {
        return FALSE;
    }
}

// Compile Primitive.attr! :leaf, ...
static int
compile_builtin_attr(rb_iseq_t *iseq, const NODE *node)
{
    VALUE symbol;
    VALUE string;
    if (!node) goto no_arg;
    while (node) {
        if (!nd_type_p(node, NODE_LIST)) goto bad_arg;
        const NODE *next = RNODE_LIST(node)->nd_next;

        node = RNODE_LIST(node)->nd_head;
        if (!node) goto no_arg;
        switch (nd_type(node)) {
          case NODE_SYM:
            symbol = rb_node_sym_string_val(node);
            break;
          default:
            goto bad_arg;
        }

        if (!SYMBOL_P(symbol)) goto non_symbol_arg;

        string = rb_sym2str(symbol);
        if (strcmp(RSTRING_PTR(string), "leaf") == 0) {
            ISEQ_BODY(iseq)->builtin_attrs |= BUILTIN_ATTR_LEAF;
        }
        else if (strcmp(RSTRING_PTR(string), "inline_block") == 0) {
            ISEQ_BODY(iseq)->builtin_attrs |= BUILTIN_ATTR_INLINE_BLOCK;
        }
        else if (strcmp(RSTRING_PTR(string), "use_block") == 0) {
            iseq_set_use_block(iseq);
        }
        else if (strcmp(RSTRING_PTR(string), "c_trace") == 0) {
            // Let the iseq act like a C method in backtraces
            ISEQ_BODY(iseq)->builtin_attrs |= BUILTIN_ATTR_C_TRACE;
        }
        else {
            goto unknown_arg;
        }
        node = next;
    }
    return COMPILE_OK;
  no_arg:
    COMPILE_ERROR(ERROR_ARGS "attr!: no argument");
    return COMPILE_NG;
  non_symbol_arg:
    COMPILE_ERROR(ERROR_ARGS "non symbol argument to attr!: %s", rb_builtin_class_name(symbol));
    return COMPILE_NG;
  unknown_arg:
    COMPILE_ERROR(ERROR_ARGS "unknown argument to attr!: %s", RSTRING_PTR(string));
    return COMPILE_NG;
  bad_arg:
    UNKNOWN_NODE("attr!", node, COMPILE_NG);
}

static int
compile_builtin_arg(rb_iseq_t *iseq, LINK_ANCHOR *const ret, const NODE *node, const NODE *line_node, int popped)
{
    VALUE name;

    if (!node) goto no_arg;
    if (!nd_type_p(node, NODE_LIST)) goto bad_arg;
    if (RNODE_LIST(node)->nd_next) goto too_many_arg;
    node = RNODE_LIST(node)->nd_head;
    if (!node) goto no_arg;
    switch (nd_type(node)) {
      case NODE_SYM:
        name = rb_node_sym_string_val(node);
        break;
      default:
        goto bad_arg;
    }
    if (!SYMBOL_P(name)) goto non_symbol_arg;
    if (!popped) {
        compile_lvar(iseq, ret, line_node, SYM2ID(name));
    }
    return COMPILE_OK;
  no_arg:
    COMPILE_ERROR(ERROR_ARGS "arg!: no argument");
    return COMPILE_NG;
  too_many_arg:
    COMPILE_ERROR(ERROR_ARGS "arg!: too many argument");
    return COMPILE_NG;
  non_symbol_arg:
    COMPILE_ERROR(ERROR_ARGS "non symbol argument to arg!: %s",
                  rb_builtin_class_name(name));
    return COMPILE_NG;
  bad_arg:
    UNKNOWN_NODE("arg!", node, COMPILE_NG);
}

static NODE *
mandatory_node(const rb_iseq_t *iseq, const NODE *cond_node)
{
    const NODE *node = ISEQ_COMPILE_DATA(iseq)->root_node;
    if (nd_type(node) == NODE_IF && RNODE_IF(node)->nd_cond == cond_node) {
        return RNODE_IF(node)->nd_body;
    }
    else {
        rb_bug("mandatory_node: can't find mandatory node");
    }
}

static int
compile_builtin_mandatory_only_method(rb_iseq_t *iseq, const NODE *node, const NODE *line_node)
{
    // arguments
    struct rb_args_info args = {
        .pre_args_num = ISEQ_BODY(iseq)->param.lead_num,
    };
    rb_node_args_t args_node;
    rb_node_init(RNODE(&args_node), NODE_ARGS);
    args_node.nd_ainfo = args;

    // local table without non-mandatory parameters
    const int skip_local_size = ISEQ_BODY(iseq)->param.size - ISEQ_BODY(iseq)->param.lead_num;
    const int table_size = ISEQ_BODY(iseq)->local_table_size - skip_local_size;

    VALUE idtmp = 0;
    rb_ast_id_table_t *tbl = ALLOCV(idtmp, sizeof(rb_ast_id_table_t) + table_size * sizeof(ID));
    tbl->size = table_size;

    int i;

    // lead parameters
    for (i=0; i<ISEQ_BODY(iseq)->param.lead_num; i++) {
        tbl->ids[i] = ISEQ_BODY(iseq)->local_table[i];
    }
    // local variables
    for (; i<table_size; i++) {
        tbl->ids[i] = ISEQ_BODY(iseq)->local_table[i + skip_local_size];
    }

    rb_node_scope_t scope_node;
    rb_node_init(RNODE(&scope_node), NODE_SCOPE);
    scope_node.nd_tbl = tbl;
    scope_node.nd_body = mandatory_node(iseq, node);
    scope_node.nd_args = &args_node;

    VALUE ast_value = rb_ruby_ast_new(RNODE(&scope_node));

    const rb_iseq_t *mandatory_only_iseq =
      rb_iseq_new_with_opt(ast_value, rb_iseq_base_label(iseq),
                           rb_iseq_path(iseq), rb_iseq_realpath(iseq),
                           nd_line(line_node), NULL, 0,
                           ISEQ_TYPE_METHOD, ISEQ_COMPILE_DATA(iseq)->option,
                           ISEQ_BODY(iseq)->variable.script_lines);
    RB_OBJ_WRITE(iseq, &ISEQ_BODY(iseq)->mandatory_only_iseq, (VALUE)mandatory_only_iseq);

    ALLOCV_END(idtmp);
    return COMPILE_OK;
}

static int
compile_builtin_function_call(rb_iseq_t *iseq, LINK_ANCHOR *const ret, const NODE *const node, const NODE *line_node, int popped,
                              const rb_iseq_t *parent_block, LINK_ANCHOR *args, const char *builtin_func)
{
    NODE *args_node = get_nd_args(node);

    if (parent_block != NULL) {
        COMPILE_ERROR(ERROR_ARGS_AT(line_node) "should not call builtins here.");
        return COMPILE_NG;
    }
    else {
# define BUILTIN_INLINE_PREFIX "_bi"
        char inline_func[sizeof(BUILTIN_INLINE_PREFIX) + DECIMAL_SIZE_OF(int)];
        bool cconst = false;
      retry:;
        const struct rb_builtin_function *bf = iseq_builtin_function_lookup(iseq, builtin_func);

        if (bf == NULL) {
            if (strcmp("cstmt!", builtin_func) == 0 ||
                strcmp("cexpr!", builtin_func) == 0) {
                // ok
            }
            else if (strcmp("cconst!", builtin_func) == 0) {
                cconst = true;
            }
            else if (strcmp("cinit!", builtin_func) == 0) {
                // ignore
                return COMPILE_OK;
            }
            else if (strcmp("attr!", builtin_func) == 0) {
                return compile_builtin_attr(iseq, args_node);
            }
            else if (strcmp("arg!", builtin_func) == 0) {
                return compile_builtin_arg(iseq, ret, args_node, line_node, popped);
            }
            else if (strcmp("mandatory_only?", builtin_func) == 0) {
                if (popped) {
                    rb_bug("mandatory_only? should be in if condition");
                }
                else if (!LIST_INSN_SIZE_ZERO(ret)) {
                    rb_bug("mandatory_only? should be put on top");
                }

                ADD_INSN1(ret, line_node, putobject, Qfalse);
                return compile_builtin_mandatory_only_method(iseq, node, line_node);
            }
            else if (1) {
                rb_bug("can't find builtin function:%s", builtin_func);
            }
            else {
                COMPILE_ERROR(ERROR_ARGS "can't find builtin function:%s", builtin_func);
                return COMPILE_NG;
            }

            int inline_index = nd_line(node);
            snprintf(inline_func, sizeof(inline_func), BUILTIN_INLINE_PREFIX "%d", inline_index);
            builtin_func = inline_func;
            args_node = NULL;
            goto retry;
        }

        if (cconst) {
            typedef VALUE(*builtin_func0)(void *, VALUE);
            VALUE const_val = (*(builtin_func0)(uintptr_t)bf->func_ptr)(NULL, Qnil);
            ADD_INSN1(ret, line_node, putobject, const_val);
            return COMPILE_OK;
        }

        // fprintf(stderr, "func_name:%s -> %p\n", builtin_func, bf->func_ptr);

        unsigned int flag = 0;
        struct rb_callinfo_kwarg *keywords = NULL;
        VALUE argc = setup_args(iseq, args, args_node, &flag, &keywords);

        if (FIX2INT(argc) != bf->argc) {
            COMPILE_ERROR(ERROR_ARGS "argc is not match for builtin function:%s (expect %d but %d)",
                          builtin_func, bf->argc, FIX2INT(argc));
            return COMPILE_NG;
        }

        unsigned int start_index;
        if (delegate_call_p(iseq, FIX2INT(argc), args, &start_index)) {
            ADD_INSN2(ret, line_node, opt_invokebuiltin_delegate, bf, INT2FIX(start_index));
        }
        else {
            ADD_SEQ(ret, args);
            ADD_INSN1(ret, line_node, invokebuiltin, bf);
        }

        if (popped) ADD_INSN(ret, line_node, pop);
        return COMPILE_OK;
    }
}

static int
compile_call(rb_iseq_t *iseq, LINK_ANCHOR *const ret, const NODE *const node, const enum node_type type, const NODE *const line_node, int popped, bool assume_receiver)
{
    /* call:  obj.method(...)
     * fcall: func(...)
     * vcall: func
     */
    DECL_ANCHOR(recv);
    DECL_ANCHOR(args);
    ID mid = get_node_call_nd_mid(node);
    VALUE argc;
    unsigned int flag = 0;
    struct rb_callinfo_kwarg *keywords = NULL;
    const rb_iseq_t *parent_block = ISEQ_COMPILE_DATA(iseq)->current_block;
    LABEL *else_label = NULL;
    VALUE branches = Qfalse;

    ISEQ_COMPILE_DATA(iseq)->current_block = NULL;

    INIT_ANCHOR(recv);
    INIT_ANCHOR(args);

#if OPT_SUPPORT_JOKE
    if (nd_type_p(node, NODE_VCALL)) {
        ID id_bitblt;
        ID id_answer;

        CONST_ID(id_bitblt, "bitblt");
        CONST_ID(id_answer, "the_answer_to_life_the_universe_and_everything");

        if (mid == id_bitblt) {
            ADD_INSN(ret, line_node, bitblt);
            return COMPILE_OK;
        }
        else if (mid == id_answer) {
            ADD_INSN(ret, line_node, answer);
            return COMPILE_OK;
        }
    }
    /* only joke */
    {
        ID goto_id;
        ID label_id;

        CONST_ID(goto_id, "__goto__");
        CONST_ID(label_id, "__label__");

        if (nd_type_p(node, NODE_FCALL) &&
            (mid == goto_id || mid == label_id)) {
            LABEL *label;
            st_data_t data;
            st_table *labels_table = ISEQ_COMPILE_DATA(iseq)->labels_table;
            VALUE label_name;

            if (!labels_table) {
                labels_table = st_init_numtable();
                ISEQ_COMPILE_DATA(iseq)->labels_table = labels_table;
            }
            {
                COMPILE_ERROR(ERROR_ARGS "invalid goto/label format");
                return COMPILE_NG;
            }

            if (mid == goto_id) {
                ADD_INSNL(ret, line_node, jump, label);
            }
            else {
                ADD_LABEL(ret, label);
            }
            return COMPILE_OK;
        }
    }
#endif

    const char *builtin_func;
    if (UNLIKELY(iseq_has_builtin_function_table(iseq)) &&
        (builtin_func = iseq_builtin_function_name(type, get_nd_recv(node), mid)) != NULL) {
        return compile_builtin_function_call(iseq, ret, node, line_node, popped, parent_block, args, builtin_func);
    }

    /* receiver */
    if (!assume_receiver) {
        if (type == NODE_CALL || type == NODE_OPCALL || type == NODE_QCALL) {
            int idx, level;

            if (mid == idCall &&
                nd_type_p(get_nd_recv(node), NODE_LVAR) &&
                iseq_block_param_id_p(iseq, RNODE_LVAR(get_nd_recv(node))->nd_vid, &idx, &level)) {
                ADD_INSN2(recv, get_nd_recv(node), getblockparamproxy, INT2FIX(idx + VM_ENV_DATA_SIZE - 1), INT2FIX(level));
            }
            else if (private_recv_p(node)) {
                ADD_INSN(recv, node, putself);
                flag |= VM_CALL_FCALL;
            }
            else {
                CHECK(COMPILE(recv, "recv", get_nd_recv(node)));
            }

            if (type == NODE_QCALL) {
                else_label = qcall_branch_start(iseq, recv, &branches, node, line_node);
            }
        }
        else if (type == NODE_FCALL || type == NODE_VCALL) {
            ADD_CALL_RECEIVER(recv, line_node);
        }
    }

    /* args */
    if (type != NODE_VCALL) {
        argc = setup_args(iseq, args, get_nd_args(node), &flag, &keywords);
        CHECK(!NIL_P(argc));
    }
    else {
        argc = INT2FIX(0);
    }

    ADD_SEQ(ret, recv);

    bool inline_new = ISEQ_COMPILE_DATA(iseq)->option->specialized_instruction &&
        mid == rb_intern("new") &&
        parent_block == NULL &&
        !(flag & VM_CALL_ARGS_BLOCKARG);

    if (inline_new) {
        ADD_INSN(ret, node, putnil);
        ADD_INSN(ret, node, swap);
    }

    ADD_SEQ(ret, args);

    debugp_param("call args argc", argc);
    debugp_param("call method", ID2SYM(mid));

    switch ((int)type) {
      case NODE_VCALL:
        flag |= VM_CALL_VCALL;
        /* VCALL is funcall, so fall through */
      case NODE_FCALL:
        flag |= VM_CALL_FCALL;
    }

    if ((flag & VM_CALL_ARGS_BLOCKARG) && (flag & VM_CALL_KW_SPLAT) && !(flag & VM_CALL_KW_SPLAT_MUT)) {
        ADD_INSN(ret, line_node, splatkw);
    }

    LABEL *not_basic_new = NEW_LABEL(nd_line(node));
    LABEL *not_basic_new_finish = NEW_LABEL(nd_line(node));

    if (inline_new) {
        // Jump unless the receiver uses the "basic" implementation of "new"
        VALUE ci;
        if (flag & VM_CALL_FORWARDING) {
            ci = (VALUE)new_callinfo(iseq, mid, NUM2INT(argc) + 1, flag, keywords, 0);
        }
        else {
            ci = (VALUE)new_callinfo(iseq, mid, NUM2INT(argc), flag, keywords, 0);
        }
        ADD_INSN2(ret, node, opt_new, ci, not_basic_new);
        LABEL_REF(not_basic_new);

        // optimized path
        ADD_SEND_R(ret, line_node, rb_intern("initialize"), argc, parent_block, INT2FIX(flag | VM_CALL_FCALL), keywords);
        ADD_INSNL(ret, line_node, jump, not_basic_new_finish);

        ADD_LABEL(ret, not_basic_new);
        // Fall back to normal send
        ADD_SEND_R(ret, line_node, mid, argc, parent_block, INT2FIX(flag), keywords);
        ADD_INSN(ret, line_node, swap);

        ADD_LABEL(ret, not_basic_new_finish);
        ADD_INSN(ret, line_node, pop);
    }
    else {
        ADD_SEND_R(ret, line_node, mid, argc, parent_block, INT2FIX(flag), keywords);
    }

    qcall_branch_end(iseq, ret, else_label, branches, node, line_node);
    if (popped) {
        ADD_INSN(ret, line_node, pop);
    }
    return COMPILE_OK;
}

static int
compile_op_asgn1(rb_iseq_t *iseq, LINK_ANCHOR *const ret, const NODE *const node, int popped)
{
    const int line = nd_line(node);
    VALUE argc;
    unsigned int flag = 0;
    int asgnflag = 0;
    ID id = RNODE_OP_ASGN1(node)->nd_mid;

    /*
     * a[x] (op)= y
     *
     * nil       # nil
     * eval a    # nil a
     * eval x    # nil a x
     * dupn 2    # nil a x a x
     * send :[]  # nil a x a[x]
     * eval y    # nil a x a[x] y
     * send op   # nil a x ret
     * setn 3    # ret a x ret
     * send []=  # ret ?
     * pop       # ret
     */

    /*
     * nd_recv[nd_args->nd_body] (nd_mid)= nd_args->nd_head;
     * NODE_OP_ASGN nd_recv
     *              nd_args->nd_head
     *              nd_args->nd_body
     *              nd_mid
     */

    if (!popped) {
        ADD_INSN(ret, node, putnil);
    }
    asgnflag = COMPILE_RECV(ret, "NODE_OP_ASGN1 recv", node, RNODE_OP_ASGN1(node)->nd_recv);
    CHECK(asgnflag != -1);
    switch (nd_type(RNODE_OP_ASGN1(node)->nd_index)) {
      case NODE_ZLIST:
        argc = INT2FIX(0);
        break;
      default:
        argc = setup_args(iseq, ret, RNODE_OP_ASGN1(node)->nd_index, &flag, NULL);
        CHECK(!NIL_P(argc));
    }
    int dup_argn = FIX2INT(argc) + 1;
    ADD_INSN1(ret, node, dupn, INT2FIX(dup_argn));
    flag |= asgnflag;
    ADD_SEND_R(ret, node, idAREF, argc, NULL, INT2FIX(flag & ~VM_CALL_ARGS_SPLAT_MUT), NULL);

    if (id == idOROP || id == idANDOP) {
        /* a[x] ||= y  or  a[x] &&= y

           unless/if a[x]
           a[x]= y
           else
           nil
           end
        */
        LABEL *label = NEW_LABEL(line);
        LABEL *lfin = NEW_LABEL(line);

        ADD_INSN(ret, node, dup);
        if (id == idOROP) {
            ADD_INSNL(ret, node, branchif, label);
        }
        else { /* idANDOP */
            ADD_INSNL(ret, node, branchunless, label);
        }
        ADD_INSN(ret, node, pop);

        CHECK(COMPILE(ret, "NODE_OP_ASGN1 nd_rvalue: ", RNODE_OP_ASGN1(node)->nd_rvalue));
        if (!popped) {
            ADD_INSN1(ret, node, setn, INT2FIX(dup_argn+1));
        }
        if (flag & VM_CALL_ARGS_SPLAT) {
            if (!(flag & VM_CALL_ARGS_SPLAT_MUT)) {
                ADD_INSN(ret, node, swap);
                ADD_INSN1(ret, node, splatarray, Qtrue);
                ADD_INSN(ret, node, swap);
                flag |= VM_CALL_ARGS_SPLAT_MUT;
            }
            ADD_INSN1(ret, node, pushtoarray, INT2FIX(1));
            ADD_SEND_R(ret, node, idASET, argc, NULL, INT2FIX(flag), NULL);
        }
        else {
            ADD_SEND_R(ret, node, idASET, FIXNUM_INC(argc, 1), NULL, INT2FIX(flag), NULL);
        }
        ADD_INSN(ret, node, pop);
        ADD_INSNL(ret, node, jump, lfin);
        ADD_LABEL(ret, label);
        if (!popped) {
            ADD_INSN1(ret, node, setn, INT2FIX(dup_argn+1));
        }
        ADD_INSN1(ret, node, adjuststack, INT2FIX(dup_argn+1));
        ADD_LABEL(ret, lfin);
    }
    else {
        CHECK(COMPILE(ret, "NODE_OP_ASGN1 nd_rvalue: ", RNODE_OP_ASGN1(node)->nd_rvalue));
        ADD_SEND(ret, node, id, INT2FIX(1));
        if (!popped) {
            ADD_INSN1(ret, node, setn, INT2FIX(dup_argn+1));
        }
        if (flag & VM_CALL_ARGS_SPLAT) {
            if (flag & VM_CALL_KW_SPLAT) {
                ADD_INSN1(ret, node, topn, INT2FIX(2));
                if (!(flag & VM_CALL_ARGS_SPLAT_MUT)) {
                    ADD_INSN1(ret, node, splatarray, Qtrue);
                    flag |= VM_CALL_ARGS_SPLAT_MUT;
                }
                ADD_INSN(ret, node, swap);
                ADD_INSN1(ret, node, pushtoarray, INT2FIX(1));
                ADD_INSN1(ret, node, setn, INT2FIX(2));
                ADD_INSN(ret, node, pop);
            }
            else {
                if (!(flag & VM_CALL_ARGS_SPLAT_MUT)) {
                    ADD_INSN(ret, node, swap);
                    ADD_INSN1(ret, node, splatarray, Qtrue);
                    ADD_INSN(ret, node, swap);
                    flag |= VM_CALL_ARGS_SPLAT_MUT;
                }
                ADD_INSN1(ret, node, pushtoarray, INT2FIX(1));
            }
            ADD_SEND_R(ret, node, idASET, argc, NULL, INT2FIX(flag), NULL);
        }
        else {
            ADD_SEND_R(ret, node, idASET, FIXNUM_INC(argc, 1), NULL, INT2FIX(flag), NULL);
        }
        ADD_INSN(ret, node, pop);
    }
    return COMPILE_OK;
}

static int
compile_op_asgn2(rb_iseq_t *iseq, LINK_ANCHOR *const ret, const NODE *const node, int popped)
{
    const int line = nd_line(node);
    ID atype = RNODE_OP_ASGN2(node)->nd_mid;
    ID vid = RNODE_OP_ASGN2(node)->nd_vid, aid = rb_id_attrset(vid);
    int asgnflag;
    LABEL *lfin = NEW_LABEL(line);
    LABEL *lcfin = NEW_LABEL(line);
    LABEL *lskip = 0;
    /*
      class C; attr_accessor :c; end
      r = C.new
      r.a &&= v # asgn2

      eval r    # r
      dup       # r r
      eval r.a  # r o

      # or
      dup       # r o o
      if lcfin  # r o
      pop       # r
      eval v    # r v
      swap      # v r
      topn 1    # v r v
      send a=   # v ?
      jump lfin # v ?

      lcfin:      # r o
      swap      # o r

      lfin:       # o ?
      pop       # o

      # or (popped)
      if lcfin  # r
      eval v    # r v
      send a=   # ?
      jump lfin # ?

      lcfin:      # r

      lfin:       # ?
      pop       #

      # and
      dup       # r o o
      unless lcfin
      pop       # r
      eval v    # r v
      swap      # v r
      topn 1    # v r v
      send a=   # v ?
      jump lfin # v ?

      # others
      eval v    # r o v
      send ??   # r w
      send a=   # w

    */

    asgnflag = COMPILE_RECV(ret, "NODE_OP_ASGN2#recv", node, RNODE_OP_ASGN2(node)->nd_recv);
    CHECK(asgnflag != -1);
    if (RNODE_OP_ASGN2(node)->nd_aid) {
        lskip = NEW_LABEL(line);
        ADD_INSN(ret, node, dup);
        ADD_INSNL(ret, node, branchnil, lskip);
    }
    ADD_INSN(ret, node, dup);
    ADD_SEND_WITH_FLAG(ret, node, vid, INT2FIX(0), INT2FIX(asgnflag));

    if (atype == idOROP || atype == idANDOP) {
        if (!popped) {
            ADD_INSN(ret, node, dup);
        }
        if (atype == idOROP) {
            ADD_INSNL(ret, node, branchif, lcfin);
        }
        else { /* idANDOP */
            ADD_INSNL(ret, node, branchunless, lcfin);
        }
        if (!popped) {
            ADD_INSN(ret, node, pop);
        }
        CHECK(COMPILE(ret, "NODE_OP_ASGN2 val", RNODE_OP_ASGN2(node)->nd_value));
        if (!popped) {
            ADD_INSN(ret, node, swap);
            ADD_INSN1(ret, node, topn, INT2FIX(1));
        }
        ADD_SEND_WITH_FLAG(ret, node, aid, INT2FIX(1), INT2FIX(asgnflag));
        ADD_INSNL(ret, node, jump, lfin);

        ADD_LABEL(ret, lcfin);
        if (!popped) {
            ADD_INSN(ret, node, swap);
        }

        ADD_LABEL(ret, lfin);
    }
    else {
        CHECK(COMPILE(ret, "NODE_OP_ASGN2 val", RNODE_OP_ASGN2(node)->nd_value));
        ADD_SEND(ret, node, atype, INT2FIX(1));
        if (!popped) {
            ADD_INSN(ret, node, swap);
            ADD_INSN1(ret, node, topn, INT2FIX(1));
        }
        ADD_SEND_WITH_FLAG(ret, node, aid, INT2FIX(1), INT2FIX(asgnflag));
    }
    if (lskip && popped) {
        ADD_LABEL(ret, lskip);
    }
    ADD_INSN(ret, node, pop);
    if (lskip && !popped) {
        ADD_LABEL(ret, lskip);
    }
    return COMPILE_OK;
}

static int compile_shareable_constant_value(rb_iseq_t *iseq, LINK_ANCHOR *ret, enum rb_parser_shareability shareable, const NODE *lhs, const NODE *value);

static int
compile_op_cdecl(rb_iseq_t *iseq, LINK_ANCHOR *const ret, const NODE *const node, int popped)
{
    const int line = nd_line(node);
    LABEL *lfin = 0;
    LABEL *lassign = 0;
    ID mid;

    switch (nd_type(RNODE_OP_CDECL(node)->nd_head)) {
      case NODE_COLON3:
        ADD_INSN1(ret, node, putobject, rb_cObject);
        break;
      case NODE_COLON2:
        CHECK(COMPILE(ret, "NODE_OP_CDECL/colon2#nd_head", RNODE_COLON2(RNODE_OP_CDECL(node)->nd_head)->nd_head));
        break;
      default:
        COMPILE_ERROR(ERROR_ARGS "%s: invalid node in NODE_OP_CDECL",
                      ruby_node_name(nd_type(RNODE_OP_CDECL(node)->nd_head)));
        return COMPILE_NG;
    }
    mid = get_node_colon_nd_mid(RNODE_OP_CDECL(node)->nd_head);
    /* cref */
    if (RNODE_OP_CDECL(node)->nd_aid == idOROP) {
        lassign = NEW_LABEL(line);
        ADD_INSN(ret, node, dup); /* cref cref */
        ADD_INSN3(ret, node, defined, INT2FIX(DEFINED_CONST_FROM),
                  ID2SYM(mid), Qtrue); /* cref bool */
        ADD_INSNL(ret, node, branchunless, lassign); /* cref */
    }
    ADD_INSN(ret, node, dup); /* cref cref */
    ADD_INSN1(ret, node, putobject, Qtrue);
    ADD_INSN1(ret, node, getconstant, ID2SYM(mid)); /* cref obj */

    if (RNODE_OP_CDECL(node)->nd_aid == idOROP || RNODE_OP_CDECL(node)->nd_aid == idANDOP) {
        lfin = NEW_LABEL(line);
        if (!popped) ADD_INSN(ret, node, dup); /* cref [obj] obj */
        if (RNODE_OP_CDECL(node)->nd_aid == idOROP)
            ADD_INSNL(ret, node, branchif, lfin);
        else /* idANDOP */
            ADD_INSNL(ret, node, branchunless, lfin);
        /* cref [obj] */
        if (!popped) ADD_INSN(ret, node, pop); /* cref */
        if (lassign) ADD_LABEL(ret, lassign);
        CHECK(compile_shareable_constant_value(iseq, ret, RNODE_OP_CDECL(node)->shareability, RNODE_OP_CDECL(node)->nd_head, RNODE_OP_CDECL(node)->nd_value));
        /* cref value */
        if (popped)
            ADD_INSN1(ret, node, topn, INT2FIX(1)); /* cref value cref */
        else {
            ADD_INSN1(ret, node, dupn, INT2FIX(2)); /* cref value cref value */
            ADD_INSN(ret, node, swap); /* cref value value cref */
        }
        ADD_INSN1(ret, node, setconstant, ID2SYM(mid)); /* cref [value] */
        ADD_LABEL(ret, lfin);			    /* cref [value] */
        if (!popped) ADD_INSN(ret, node, swap); /* [value] cref */
        ADD_INSN(ret, node, pop); /* [value] */
    }
    else {
        CHECK(compile_shareable_constant_value(iseq, ret, RNODE_OP_CDECL(node)->shareability, RNODE_OP_CDECL(node)->nd_head, RNODE_OP_CDECL(node)->nd_value));
        /* cref obj value */
        ADD_CALL(ret, node, RNODE_OP_CDECL(node)->nd_aid, INT2FIX(1));
        /* cref value */
        ADD_INSN(ret, node, swap); /* value cref */
        if (!popped) {
            ADD_INSN1(ret, node, topn, INT2FIX(1)); /* value cref value */
            ADD_INSN(ret, node, swap); /* value value cref */
        }
        ADD_INSN1(ret, node, setconstant, ID2SYM(mid));
    }
    return COMPILE_OK;
}

static int
compile_op_log(rb_iseq_t *iseq, LINK_ANCHOR *const ret, const NODE *const node, int popped, const enum node_type type)
{
    const int line = nd_line(node);
    LABEL *lfin = NEW_LABEL(line);
    LABEL *lassign;

    if (type == NODE_OP_ASGN_OR && !nd_type_p(RNODE_OP_ASGN_OR(node)->nd_head, NODE_IVAR)) {
        LABEL *lfinish[2];
        lfinish[0] = lfin;
        lfinish[1] = 0;
        defined_expr(iseq, ret, RNODE_OP_ASGN_OR(node)->nd_head, lfinish, Qfalse, false);
        lassign = lfinish[1];
        if (!lassign) {
            lassign = NEW_LABEL(line);
        }
        ADD_INSNL(ret, node, branchunless, lassign);
    }
    else {
        lassign = NEW_LABEL(line);
    }

    CHECK(COMPILE(ret, "NODE_OP_ASGN_AND/OR#nd_head", RNODE_OP_ASGN_OR(node)->nd_head));

    if (!popped) {
        ADD_INSN(ret, node, dup);
    }

    if (type == NODE_OP_ASGN_AND) {
        ADD_INSNL(ret, node, branchunless, lfin);
    }
    else {
        ADD_INSNL(ret, node, branchif, lfin);
    }

    if (!popped) {
        ADD_INSN(ret, node, pop);
    }

    ADD_LABEL(ret, lassign);
    CHECK(COMPILE_(ret, "NODE_OP_ASGN_AND/OR#nd_value", RNODE_OP_ASGN_OR(node)->nd_value, popped));
    ADD_LABEL(ret, lfin);
    return COMPILE_OK;
}

static int
compile_super(rb_iseq_t *iseq, LINK_ANCHOR *const ret, const NODE *const node, int popped, const enum node_type type)
{
    struct rb_iseq_constant_body *const body = ISEQ_BODY(iseq);
    DECL_ANCHOR(args);
    int argc;
    unsigned int flag = 0;
    struct rb_callinfo_kwarg *keywords = NULL;
    const rb_iseq_t *parent_block = ISEQ_COMPILE_DATA(iseq)->current_block;
    int use_block = 1;

    INIT_ANCHOR(args);
    ISEQ_COMPILE_DATA(iseq)->current_block = NULL;

    if (type == NODE_SUPER) {
        VALUE vargc = setup_args(iseq, args, RNODE_SUPER(node)->nd_args, &flag, &keywords);
        CHECK(!NIL_P(vargc));
        argc = FIX2INT(vargc);
        if ((flag & VM_CALL_ARGS_BLOCKARG) && (flag & VM_CALL_KW_SPLAT) && !(flag & VM_CALL_KW_SPLAT_MUT)) {
            ADD_INSN(args, node, splatkw);
        }

        if (flag & VM_CALL_ARGS_BLOCKARG) {
            use_block = 0;
        }
    }
    else {
        /* NODE_ZSUPER */
        int i;
        const rb_iseq_t *liseq = body->local_iseq;
        const struct rb_iseq_constant_body *const local_body = ISEQ_BODY(liseq);
        const struct rb_iseq_param_keyword *const local_kwd = local_body->param.keyword;
        int lvar_level = get_lvar_level(iseq);

        argc = local_body->param.lead_num;

        /* normal arguments */
        for (i = 0; i < local_body->param.lead_num; i++) {
            int idx = local_body->local_table_size - i;
            ADD_GETLOCAL(args, node, idx, lvar_level);
        }

        /* forward ... */
        if (local_body->param.flags.forwardable) {
            flag |= VM_CALL_FORWARDING;
            int idx = local_body->local_table_size - get_local_var_idx(liseq, idDot3);
            ADD_GETLOCAL(args, node, idx, lvar_level);
        }

        if (local_body->param.flags.has_opt) {
            /* optional arguments */
            int j;
            for (j = 0; j < local_body->param.opt_num; j++) {
                int idx = local_body->local_table_size - (i + j);
                ADD_GETLOCAL(args, node, idx, lvar_level);
            }
            i += j;
            argc = i;
        }
        if (local_body->param.flags.has_rest) {
            /* rest argument */
            int idx = local_body->local_table_size - local_body->param.rest_start;
            ADD_GETLOCAL(args, node, idx, lvar_level);
            ADD_INSN1(args, node, splatarray, RBOOL(local_body->param.flags.has_post));

            argc = local_body->param.rest_start + 1;
            flag |= VM_CALL_ARGS_SPLAT;
        }
        if (local_body->param.flags.has_post) {
            /* post arguments */
            int post_len = local_body->param.post_num;
            int post_start = local_body->param.post_start;

            if (local_body->param.flags.has_rest) {
                int j;
                for (j=0; j<post_len; j++) {
                    int idx = local_body->local_table_size - (post_start + j);
                    ADD_GETLOCAL(args, node, idx, lvar_level);
                }
                ADD_INSN1(args, node, pushtoarray, INT2FIX(j));
                flag |= VM_CALL_ARGS_SPLAT_MUT;
                /* argc is settled at above */
            }
            else {
                int j;
                for (j=0; j<post_len; j++) {
                    int idx = local_body->local_table_size - (post_start + j);
                    ADD_GETLOCAL(args, node, idx, lvar_level);
                }
                argc = post_len + post_start;
            }
        }

        if (local_body->param.flags.has_kw) { /* TODO: support keywords */
            int local_size = local_body->local_table_size;
            argc++;

            ADD_INSN1(args, node, putspecialobject, INT2FIX(VM_SPECIAL_OBJECT_VMCORE));

            if (local_body->param.flags.has_kwrest) {
                int idx = local_body->local_table_size - local_kwd->rest_start;
                ADD_GETLOCAL(args, node, idx, lvar_level);
                RUBY_ASSERT(local_kwd->num > 0);
                ADD_SEND (args, node, rb_intern("dup"), INT2FIX(0));
            }
            else {
                ADD_INSN1(args, node, newhash, INT2FIX(0));
            }
            for (i = 0; i < local_kwd->num; ++i) {
                ID id = local_kwd->table[i];
                int idx = local_size - get_local_var_idx(liseq, id);
                ADD_INSN1(args, node, putobject, ID2SYM(id));
                ADD_GETLOCAL(args, node, idx, lvar_level);
            }
            ADD_SEND(args, node, id_core_hash_merge_ptr, INT2FIX(i * 2 + 1));
            flag |= VM_CALL_KW_SPLAT| VM_CALL_KW_SPLAT_MUT;
        }
        else if (local_body->param.flags.has_kwrest) {
            int idx = local_body->local_table_size - local_kwd->rest_start;
            ADD_GETLOCAL(args, node, idx, lvar_level);
            argc++;
            flag |= VM_CALL_KW_SPLAT;
        }
    }

    if (use_block && parent_block == NULL) {
        iseq_set_use_block(ISEQ_BODY(iseq)->local_iseq);
    }

    flag |= VM_CALL_SUPER | VM_CALL_FCALL;
    if (type == NODE_ZSUPER) flag |= VM_CALL_ZSUPER;
    ADD_INSN(ret, node, putself);
    ADD_SEQ(ret, args);

    const struct rb_callinfo * ci = new_callinfo(iseq, 0, argc, flag, keywords, parent_block != NULL);

    if (vm_ci_flag(ci) & VM_CALL_FORWARDING) {
        ADD_INSN2(ret, node, invokesuperforward, ci, parent_block);
    }
    else {
        ADD_INSN2(ret, node, invokesuper, ci, parent_block);
    }

    if (popped) {
        ADD_INSN(ret, node, pop);
    }
    return COMPILE_OK;
}

static int
compile_yield(rb_iseq_t *iseq, LINK_ANCHOR *const ret, const NODE *const node, int popped)
{
    DECL_ANCHOR(args);
    VALUE argc;
    unsigned int flag = 0;
    struct rb_callinfo_kwarg *keywords = NULL;

    INIT_ANCHOR(args);

    switch (ISEQ_BODY(ISEQ_BODY(iseq)->local_iseq)->type) {
      case ISEQ_TYPE_TOP:
      case ISEQ_TYPE_MAIN:
      case ISEQ_TYPE_CLASS:
        COMPILE_ERROR(ERROR_ARGS "Invalid yield");
        return COMPILE_NG;
      default: /* valid */;
    }

    if (RNODE_YIELD(node)->nd_head) {
        argc = setup_args(iseq, args, RNODE_YIELD(node)->nd_head, &flag, &keywords);
        CHECK(!NIL_P(argc));
    }
    else {
        argc = INT2FIX(0);
    }

    ADD_SEQ(ret, args);
    ADD_INSN1(ret, node, invokeblock, new_callinfo(iseq, 0, FIX2INT(argc), flag, keywords, FALSE));
    iseq_set_use_block(ISEQ_BODY(iseq)->local_iseq);

    if (popped) {
        ADD_INSN(ret, node, pop);
    }

    int level = 0;
    const rb_iseq_t *tmp_iseq = iseq;
    for (; tmp_iseq != ISEQ_BODY(iseq)->local_iseq; level++ ) {
        tmp_iseq = ISEQ_BODY(tmp_iseq)->parent_iseq;
    }
    if (level > 0) access_outer_variables(iseq, level, rb_intern("yield"), true);

    return COMPILE_OK;
}

static int
compile_match(rb_iseq_t *iseq, LINK_ANCHOR *const ret, const NODE *const node, int popped, const enum node_type type)
{
    DECL_ANCHOR(recv);
    DECL_ANCHOR(val);

    INIT_ANCHOR(recv);
    INIT_ANCHOR(val);
    switch ((int)type) {
      case NODE_MATCH:
        ADD_INSN1(recv, node, putobject, rb_node_regx_string_val(node));
        ADD_INSN2(val, node, getspecial, INT2FIX(0),
                  INT2FIX(0));
        break;
      case NODE_MATCH2:
        CHECK(COMPILE(recv, "receiver", RNODE_MATCH2(node)->nd_recv));
        CHECK(COMPILE(val, "value", RNODE_MATCH2(node)->nd_value));
        break;
      case NODE_MATCH3:
        CHECK(COMPILE(recv, "receiver", RNODE_MATCH3(node)->nd_value));
        CHECK(COMPILE(val, "value", RNODE_MATCH3(node)->nd_recv));
        break;
    }

    ADD_SEQ(ret, recv);
    ADD_SEQ(ret, val);
    ADD_SEND(ret, node, idEqTilde, INT2FIX(1));

    if (nd_type_p(node, NODE_MATCH2) && RNODE_MATCH2(node)->nd_args) {
        compile_named_capture_assign(iseq, ret, RNODE_MATCH2(node)->nd_args);
    }

    if (popped) {
        ADD_INSN(ret, node, pop);
    }
    return COMPILE_OK;
}

static int
compile_colon2(rb_iseq_t *iseq, LINK_ANCHOR *const ret, const NODE *const node, int popped)
{
    if (rb_is_const_id(RNODE_COLON2(node)->nd_mid)) {
        /* constant */
        VALUE segments;
        if (ISEQ_COMPILE_DATA(iseq)->option->inline_const_cache &&
                (segments = collect_const_segments(iseq, node))) {
            ISEQ_BODY(iseq)->ic_size++;
            ADD_INSN1(ret, node, opt_getconstant_path, segments);
            RB_OBJ_WRITTEN(iseq, Qundef, segments);
        }
        else {
            /* constant */
            DECL_ANCHOR(pref);
            DECL_ANCHOR(body);

            INIT_ANCHOR(pref);
            INIT_ANCHOR(body);
            CHECK(compile_const_prefix(iseq, node, pref, body));
            if (LIST_INSN_SIZE_ZERO(pref)) {
                ADD_INSN(ret, node, putnil);
                ADD_SEQ(ret, body);
            }
            else {
                ADD_SEQ(ret, pref);
                ADD_SEQ(ret, body);
            }
        }
    }
    else {
        /* function call */
        ADD_CALL_RECEIVER(ret, node);
        CHECK(COMPILE(ret, "colon2#nd_head", RNODE_COLON2(node)->nd_head));
        ADD_CALL(ret, node, RNODE_COLON2(node)->nd_mid, INT2FIX(1));
    }
    if (popped) {
        ADD_INSN(ret, node, pop);
    }
    return COMPILE_OK;
}

static int
compile_colon3(rb_iseq_t *iseq, LINK_ANCHOR *const ret, const NODE *const node, int popped)
{
    debugi("colon3#nd_mid", RNODE_COLON3(node)->nd_mid);

    /* add cache insn */
    if (ISEQ_COMPILE_DATA(iseq)->option->inline_const_cache) {
        ISEQ_BODY(iseq)->ic_size++;
        VALUE segments = rb_ary_new_from_args(2, ID2SYM(idNULL), ID2SYM(RNODE_COLON3(node)->nd_mid));
        ADD_INSN1(ret, node, opt_getconstant_path, segments);
        RB_OBJ_WRITTEN(iseq, Qundef, segments);
    }
    else {
        ADD_INSN1(ret, node, putobject, rb_cObject);
        ADD_INSN1(ret, node, putobject, Qtrue);
        ADD_INSN1(ret, node, getconstant, ID2SYM(RNODE_COLON3(node)->nd_mid));
    }

    if (popped) {
        ADD_INSN(ret, node, pop);
    }
    return COMPILE_OK;
}

static int
compile_dots(rb_iseq_t *iseq, LINK_ANCHOR *const ret, const NODE *const node, int popped, const int excl)
{
    VALUE flag = INT2FIX(excl);
    const NODE *b = RNODE_DOT2(node)->nd_beg;
    const NODE *e = RNODE_DOT2(node)->nd_end;

    if (optimizable_range_item_p(b) && optimizable_range_item_p(e)) {
        if (!popped) {
            VALUE bv = optimized_range_item(b);
            VALUE ev = optimized_range_item(e);
            VALUE val = rb_range_new(bv, ev, excl);
            ADD_INSN1(ret, node, putobject, val);
            RB_OBJ_WRITTEN(iseq, Qundef, val);
        }
    }
    else {
        CHECK(COMPILE_(ret, "min", b, popped));
        CHECK(COMPILE_(ret, "max", e, popped));
        if (!popped) {
            ADD_INSN1(ret, node, newrange, flag);
        }
    }
    return COMPILE_OK;
}

static int
compile_errinfo(rb_iseq_t *iseq, LINK_ANCHOR *const ret, const NODE *const node, int popped)
{
    if (!popped) {
        if (ISEQ_BODY(iseq)->type == ISEQ_TYPE_RESCUE) {
            ADD_GETLOCAL(ret, node, LVAR_ERRINFO, 0);
        }
        else {
            const rb_iseq_t *ip = iseq;
            int level = 0;
            while (ip) {
                if (ISEQ_BODY(ip)->type == ISEQ_TYPE_RESCUE) {
                    break;
                }
                ip = ISEQ_BODY(ip)->parent_iseq;
                level++;
            }
            if (ip) {
                ADD_GETLOCAL(ret, node, LVAR_ERRINFO, level);
            }
            else {
                ADD_INSN(ret, node, putnil);
            }
        }
    }
    return COMPILE_OK;
}

static int
compile_kw_arg(rb_iseq_t *iseq, LINK_ANCHOR *const ret, const NODE *const node, int popped)
{
    struct rb_iseq_constant_body *const body = ISEQ_BODY(iseq);
    LABEL *end_label = NEW_LABEL(nd_line(node));
    const NODE *default_value = get_nd_value(RNODE_KW_ARG(node)->nd_body);

    if (default_value == NODE_SPECIAL_REQUIRED_KEYWORD) {
        /* required argument. do nothing */
        COMPILE_ERROR(ERROR_ARGS "unreachable");
        return COMPILE_NG;
    }
    else if (nd_type_p(default_value, NODE_SYM) ||
             nd_type_p(default_value, NODE_REGX) ||
             nd_type_p(default_value, NODE_LINE) ||
             nd_type_p(default_value, NODE_INTEGER) ||
             nd_type_p(default_value, NODE_FLOAT) ||
             nd_type_p(default_value, NODE_RATIONAL) ||
             nd_type_p(default_value, NODE_IMAGINARY) ||
             nd_type_p(default_value, NODE_NIL) ||
             nd_type_p(default_value, NODE_TRUE) ||
             nd_type_p(default_value, NODE_FALSE)) {
        COMPILE_ERROR(ERROR_ARGS "unreachable");
        return COMPILE_NG;
    }
    else {
        /* if keywordcheck(_kw_bits, nth_keyword)
         *   kw = default_value
         * end
         */
        int kw_bits_idx = body->local_table_size - body->param.keyword->bits_start;
        int keyword_idx = body->param.keyword->num;

        ADD_INSN2(ret, node, checkkeyword, INT2FIX(kw_bits_idx + VM_ENV_DATA_SIZE - 1), INT2FIX(keyword_idx));
        ADD_INSNL(ret, node, branchif, end_label);
        CHECK(COMPILE_POPPED(ret, "keyword default argument", RNODE_KW_ARG(node)->nd_body));
        ADD_LABEL(ret, end_label);
    }
    return COMPILE_OK;
}

static int
compile_attrasgn(rb_iseq_t *iseq, LINK_ANCHOR *const ret, const NODE *const node, int popped)
{
    DECL_ANCHOR(recv);
    DECL_ANCHOR(args);
    unsigned int flag = 0;
    ID mid = RNODE_ATTRASGN(node)->nd_mid;
    VALUE argc;
    LABEL *else_label = NULL;
    VALUE branches = Qfalse;

    /* optimization shortcut
     *   obj["literal"] = value -> opt_aset_with(obj, "literal", value)
     */
    if (!ISEQ_COMPILE_DATA(iseq)->in_masgn &&
        mid == idASET && !private_recv_p(node) && RNODE_ATTRASGN(node)->nd_args &&
        nd_type_p(RNODE_ATTRASGN(node)->nd_args, NODE_LIST) && RNODE_LIST(RNODE_ATTRASGN(node)->nd_args)->as.nd_alen == 2 &&
        (nd_type_p(RNODE_LIST(RNODE_ATTRASGN(node)->nd_args)->nd_head, NODE_STR) || nd_type_p(RNODE_LIST(RNODE_ATTRASGN(node)->nd_args)->nd_head, NODE_FILE)) &&
        ISEQ_COMPILE_DATA(iseq)->current_block == NULL &&
        !frozen_string_literal_p(iseq) &&
        ISEQ_COMPILE_DATA(iseq)->option->specialized_instruction)
    {
        VALUE str = get_string_value(RNODE_LIST(RNODE_ATTRASGN(node)->nd_args)->nd_head);
        CHECK(COMPILE(ret, "recv", RNODE_ATTRASGN(node)->nd_recv));
        CHECK(COMPILE(ret, "value", RNODE_LIST(RNODE_LIST(RNODE_ATTRASGN(node)->nd_args)->nd_next)->nd_head));
        if (!popped) {
            ADD_INSN(ret, node, swap);
            ADD_INSN1(ret, node, topn, INT2FIX(1));
        }
        ADD_INSN2(ret, node, opt_aset_with, str,
                  new_callinfo(iseq, idASET, 2, 0, NULL, FALSE));
        RB_OBJ_WRITTEN(iseq, Qundef, str);
        ADD_INSN(ret, node, pop);
        return COMPILE_OK;
    }

    INIT_ANCHOR(recv);
    INIT_ANCHOR(args);
    argc = setup_args(iseq, args, RNODE_ATTRASGN(node)->nd_args, &flag, NULL);
    CHECK(!NIL_P(argc));

    int asgnflag = COMPILE_RECV(recv, "recv", node, RNODE_ATTRASGN(node)->nd_recv);
    CHECK(asgnflag != -1);
    flag |= (unsigned int)asgnflag;

    debugp_param("argc", argc);
    debugp_param("nd_mid", ID2SYM(mid));

    if (!rb_is_attrset_id(mid)) {
        /* safe nav attr */
        mid = rb_id_attrset(mid);
        else_label = qcall_branch_start(iseq, recv, &branches, node, node);
    }
    if (!popped) {
        ADD_INSN(ret, node, putnil);
        ADD_SEQ(ret, recv);
        ADD_SEQ(ret, args);

        if (flag & VM_CALL_ARGS_SPLAT) {
            ADD_INSN(ret, node, dup);
            ADD_INSN1(ret, node, putobject, INT2FIX(-1));
            ADD_SEND_WITH_FLAG(ret, node, idAREF, INT2FIX(1), INT2FIX(asgnflag));
            ADD_INSN1(ret, node, setn, FIXNUM_INC(argc, 2));
            ADD_INSN (ret, node, pop);
        }
        else {
            ADD_INSN1(ret, node, setn, FIXNUM_INC(argc, 1));
        }
    }
    else {
        ADD_SEQ(ret, recv);
        ADD_SEQ(ret, args);
    }
    ADD_SEND_WITH_FLAG(ret, node, mid, argc, INT2FIX(flag));
    qcall_branch_end(iseq, ret, else_label, branches, node, node);
    ADD_INSN(ret, node, pop);
    return COMPILE_OK;
}

static int
compile_make_shareable_node(rb_iseq_t *iseq, LINK_ANCHOR *ret, LINK_ANCHOR *sub, const NODE *value, bool copy)
{
    ADD_INSN1(ret, value, putspecialobject, INT2FIX(VM_SPECIAL_OBJECT_VMCORE));
    ADD_SEQ(ret, sub);

    if (copy) {
        /*
         * NEW_CALL(fcore, rb_intern("make_shareable_copy"),
         *          NEW_LIST(value, loc), loc);
         */
        ADD_SEND_WITH_FLAG(ret, value, rb_intern("make_shareable_copy"), INT2FIX(1), INT2FIX(VM_CALL_ARGS_SIMPLE));
    }
    else {
        /*
         * NEW_CALL(fcore, rb_intern("make_shareable"),
         *          NEW_LIST(value, loc), loc);
         */
        ADD_SEND_WITH_FLAG(ret, value, rb_intern("make_shareable"), INT2FIX(1), INT2FIX(VM_CALL_ARGS_SIMPLE));
    }

    return COMPILE_OK;
}

static VALUE
node_const_decl_val(const NODE *node)
{
    VALUE path;
    switch (nd_type(node)) {
      case NODE_CDECL:
        if (RNODE_CDECL(node)->nd_vid) {
            path = rb_id2str(RNODE_CDECL(node)->nd_vid);
            goto end;
        }
        else {
            node = RNODE_CDECL(node)->nd_else;
        }
        break;
      case NODE_COLON2:
        break;
      case NODE_COLON3:
        // ::Const
        path = rb_str_new_cstr("::");
        rb_str_append(path, rb_id2str(RNODE_COLON3(node)->nd_mid));
        goto end;
      default:
        rb_bug("unexpected node: %s", ruby_node_name(nd_type(node)));
        UNREACHABLE_RETURN(0);
    }

    path = rb_ary_new();
    if (node) {
        for (; node && nd_type_p(node, NODE_COLON2); node = RNODE_COLON2(node)->nd_head) {
            rb_ary_push(path, rb_id2str(RNODE_COLON2(node)->nd_mid));
        }
        if (node && nd_type_p(node, NODE_CONST)) {
            // Const::Name
            rb_ary_push(path, rb_id2str(RNODE_CONST(node)->nd_vid));
        }
        else if (node && nd_type_p(node, NODE_COLON3)) {
            // ::Const::Name
            rb_ary_push(path, rb_id2str(RNODE_COLON3(node)->nd_mid));
            rb_ary_push(path, rb_str_new(0, 0));
        }
        else {
            // expression::Name
            rb_ary_push(path, rb_str_new_cstr("..."));
        }
        path = rb_ary_join(rb_ary_reverse(path), rb_str_new_cstr("::"));
    }
  end:
    path = rb_fstring(path);
    return path;
}

static VALUE
const_decl_path(NODE *dest)
{
    VALUE path = Qnil;
    if (!nd_type_p(dest, NODE_CALL)) {
        path = node_const_decl_val(dest);
    }
    return path;
}

static int
compile_ensure_shareable_node(rb_iseq_t *iseq, LINK_ANCHOR *ret, NODE *dest, const NODE *value)
{
    /*
     *. RubyVM::FrozenCore.ensure_shareable(value, const_decl_path(dest))
     */
    VALUE path = const_decl_path(dest);
    ADD_INSN1(ret, value, putspecialobject, INT2FIX(VM_SPECIAL_OBJECT_VMCORE));
    CHECK(COMPILE(ret, "compile_ensure_shareable_node", value));
    ADD_INSN1(ret, value, putobject, path);
    RB_OBJ_WRITTEN(iseq, Qundef, path);
    ADD_SEND_WITH_FLAG(ret, value, rb_intern("ensure_shareable"), INT2FIX(2), INT2FIX(VM_CALL_ARGS_SIMPLE));

    return COMPILE_OK;
}

#ifndef SHAREABLE_BARE_EXPRESSION
#define SHAREABLE_BARE_EXPRESSION 1
#endif

static int
compile_shareable_literal_constant(rb_iseq_t *iseq, LINK_ANCHOR *ret, enum rb_parser_shareability shareable, NODE *dest, const NODE *node, size_t level, VALUE *value_p, int *shareable_literal_p)
{
# define compile_shareable_literal_constant_next(node, anchor, value_p, shareable_literal_p) \
    compile_shareable_literal_constant(iseq, anchor, shareable, dest, node, level+1, value_p, shareable_literal_p)
    VALUE lit = Qnil;
    DECL_ANCHOR(anchor);

    enum node_type type = node ? nd_type(node) : NODE_NIL;
    switch (type) {
      case NODE_TRUE:
        *value_p = Qtrue;
        goto compile;
      case NODE_FALSE:
        *value_p = Qfalse;
        goto compile;
      case NODE_NIL:
        *value_p = Qnil;
        goto compile;
      case NODE_SYM:
        *value_p = rb_node_sym_string_val(node);
        goto compile;
      case NODE_REGX:
        *value_p = rb_node_regx_string_val(node);
        goto compile;
      case NODE_LINE:
        *value_p = rb_node_line_lineno_val(node);
        goto compile;
      case NODE_INTEGER:
        *value_p = rb_node_integer_literal_val(node);
        goto compile;
      case NODE_FLOAT:
        *value_p = rb_node_float_literal_val(node);
        goto compile;
      case NODE_RATIONAL:
        *value_p = rb_node_rational_literal_val(node);
        goto compile;
      case NODE_IMAGINARY:
        *value_p = rb_node_imaginary_literal_val(node);
        goto compile;
      case NODE_ENCODING:
        *value_p = rb_node_encoding_val(node);

      compile:
        CHECK(COMPILE(ret, "shareable_literal_constant", node));
        *shareable_literal_p = 1;
        return COMPILE_OK;

      case NODE_DSTR:
        CHECK(COMPILE(ret, "shareable_literal_constant", node));
        if (shareable == rb_parser_shareable_literal) {
            /*
             *  NEW_CALL(node, idUMinus, 0, loc);
             *
             *  -"#{var}"
             */
            ADD_SEND_WITH_FLAG(ret, node, idUMinus, INT2FIX(0), INT2FIX(VM_CALL_ARGS_SIMPLE));
        }
        *value_p = Qundef;
        *shareable_literal_p = 1;
        return COMPILE_OK;

      case NODE_STR:{
        VALUE lit = rb_node_str_string_val(node);
        ADD_INSN1(ret, node, putobject, lit);
        RB_OBJ_WRITTEN(iseq, Qundef, lit);
        *value_p = lit;
        *shareable_literal_p = 1;

        return COMPILE_OK;
      }

      case NODE_FILE:{
        VALUE lit = rb_node_file_path_val(node);
        ADD_INSN1(ret, node, putobject, lit);
        RB_OBJ_WRITTEN(iseq, Qundef, lit);
        *value_p = lit;
        *shareable_literal_p = 1;

        return COMPILE_OK;
      }

      case NODE_ZLIST:{
        VALUE lit = rb_ary_new();
        OBJ_FREEZE(lit);
        ADD_INSN1(ret, node, putobject, lit);
        RB_OBJ_WRITTEN(iseq, Qundef, lit);
        *value_p = lit;
        *shareable_literal_p = 1;

        return COMPILE_OK;
      }

      case NODE_LIST:{
        INIT_ANCHOR(anchor);
        lit = rb_ary_new();
        for (NODE *n = (NODE *)node; n; n = RNODE_LIST(n)->nd_next) {
            VALUE val;
            int shareable_literal_p2;
            NODE *elt = RNODE_LIST(n)->nd_head;
            if (elt) {
                CHECK(compile_shareable_literal_constant_next(elt, anchor, &val, &shareable_literal_p2));
                if (shareable_literal_p2) {
                    /* noop */
                }
                else if (RTEST(lit)) {
                    rb_ary_clear(lit);
                    lit = Qfalse;
                }
            }
            if (RTEST(lit)) {
                if (!UNDEF_P(val)) {
                    rb_ary_push(lit, val);
                }
                else {
                    rb_ary_clear(lit);
                    lit = Qnil; /* make shareable at runtime */
                }
            }
        }
        break;
      }
      case NODE_HASH:{
        if (!RNODE_HASH(node)->nd_brace) {
            *value_p = Qundef;
            *shareable_literal_p = 0;
            return COMPILE_OK;
        }
        for (NODE *n = RNODE_HASH(node)->nd_head; n; n = RNODE_LIST(RNODE_LIST(n)->nd_next)->nd_next) {
            if (!RNODE_LIST(n)->nd_head) {
                // If the hash node have a keyword splat, fall back to the default case.
                goto compile_shareable;
            }
        }

        INIT_ANCHOR(anchor);
        lit = rb_hash_new();
        for (NODE *n = RNODE_HASH(node)->nd_head; n; n = RNODE_LIST(RNODE_LIST(n)->nd_next)->nd_next) {
            VALUE key_val = 0;
            VALUE value_val = 0;
            int shareable_literal_p2;
            NODE *key = RNODE_LIST(n)->nd_head;
            NODE *val = RNODE_LIST(RNODE_LIST(n)->nd_next)->nd_head;
            CHECK(compile_shareable_literal_constant_next(key, anchor, &key_val, &shareable_literal_p2));
            if (shareable_literal_p2) {
                /* noop */
            }
            else if (RTEST(lit)) {
                rb_hash_clear(lit);
                lit = Qfalse;
            }
            CHECK(compile_shareable_literal_constant_next(val, anchor, &value_val, &shareable_literal_p2));
            if (shareable_literal_p2) {
                /* noop */
            }
            else if (RTEST(lit)) {
                rb_hash_clear(lit);
                lit = Qfalse;
            }
            if (RTEST(lit)) {
                if (!UNDEF_P(key_val) && !UNDEF_P(value_val)) {
                    rb_hash_aset(lit, key_val, value_val);
                }
                else {
                    rb_hash_clear(lit);
                    lit = Qnil; /* make shareable at runtime */
                }
            }
        }
        break;
      }

      default:

      compile_shareable:
        if (shareable == rb_parser_shareable_literal &&
            (SHAREABLE_BARE_EXPRESSION || level > 0)) {
            CHECK(compile_ensure_shareable_node(iseq, ret, dest, node));
            *value_p = Qundef;
            *shareable_literal_p = 1;
            return COMPILE_OK;
        }
        CHECK(COMPILE(ret, "shareable_literal_constant", node));
        *value_p = Qundef;
        *shareable_literal_p = 0;
        return COMPILE_OK;
    }

    /* Array or Hash that does not have keyword splat */
    if (!lit) {
        if (nd_type(node) == NODE_LIST) {
            ADD_INSN1(anchor, node, newarray, INT2FIX(RNODE_LIST(node)->as.nd_alen));
        }
        else if (nd_type(node) == NODE_HASH) {
            int len = (int)RNODE_LIST(RNODE_HASH(node)->nd_head)->as.nd_alen;
            ADD_INSN1(anchor, node, newhash, INT2FIX(len));
        }
        *value_p = Qundef;
        *shareable_literal_p = 0;
        ADD_SEQ(ret, anchor);
        return COMPILE_OK;
    }
    if (NIL_P(lit)) {
        // if shareable_literal, all elements should have been ensured
        // as shareable
        if (nd_type(node) == NODE_LIST) {
            ADD_INSN1(anchor, node, newarray, INT2FIX(RNODE_LIST(node)->as.nd_alen));
        }
        else if (nd_type(node) == NODE_HASH) {
            int len = (int)RNODE_LIST(RNODE_HASH(node)->nd_head)->as.nd_alen;
            ADD_INSN1(anchor, node, newhash, INT2FIX(len));
        }
        CHECK(compile_make_shareable_node(iseq, ret, anchor, node, false));
        *value_p = Qundef;
        *shareable_literal_p = 1;
    }
    else {
        VALUE val = rb_ractor_make_shareable(lit);
        ADD_INSN1(ret, node, putobject, val);
        RB_OBJ_WRITTEN(iseq, Qundef, val);
        *value_p = val;
        *shareable_literal_p = 1;
    }

    return COMPILE_OK;
}

static int
compile_shareable_constant_value(rb_iseq_t *iseq, LINK_ANCHOR *ret, enum rb_parser_shareability shareable, const NODE *lhs, const NODE *value)
{
    int literal_p = 0;
    VALUE val;
    DECL_ANCHOR(anchor);
    INIT_ANCHOR(anchor);

    switch (shareable) {
      case rb_parser_shareable_none:
        CHECK(COMPILE(ret, "compile_shareable_constant_value", value));
        return COMPILE_OK;

      case rb_parser_shareable_literal:
        CHECK(compile_shareable_literal_constant(iseq, anchor, shareable, (NODE *)lhs, value, 0, &val, &literal_p));
        ADD_SEQ(ret, anchor);
        return COMPILE_OK;

      case rb_parser_shareable_copy:
      case rb_parser_shareable_everything:
        CHECK(compile_shareable_literal_constant(iseq, anchor, shareable, (NODE *)lhs, value, 0, &val, &literal_p));
        if (!literal_p) {
            CHECK(compile_make_shareable_node(iseq, ret, anchor, value, shareable == rb_parser_shareable_copy));
        }
        else {
            ADD_SEQ(ret, anchor);
        }
        return COMPILE_OK;
      default:
        rb_bug("unexpected rb_parser_shareability: %d", shareable);
    }
}

static int iseq_compile_each0(rb_iseq_t *iseq, LINK_ANCHOR *const ret, const NODE *const node, int popped);
/**
  compile each node

  self:  InstructionSequence
  node:  Ruby compiled node
  popped: This node will be popped
 */
static int
iseq_compile_each(rb_iseq_t *iseq, LINK_ANCHOR *ret, const NODE *node, int popped)
{
    if (node == 0) {
        if (!popped) {
            int lineno = ISEQ_COMPILE_DATA(iseq)->last_line;
            if (lineno == 0) lineno = FIX2INT(rb_iseq_first_lineno(iseq));
            debugs("node: NODE_NIL(implicit)\n");
            ADD_SYNTHETIC_INSN(ret, lineno, -1, putnil);
        }
        return COMPILE_OK;
    }
    return iseq_compile_each0(iseq, ret, node, popped);
}

static int
iseq_compile_each0(rb_iseq_t *iseq, LINK_ANCHOR *const ret, const NODE *const node, int popped)
{
    const int line = (int)nd_line(node);
    const enum node_type type = nd_type(node);
    struct rb_iseq_constant_body *const body = ISEQ_BODY(iseq);

    if (ISEQ_COMPILE_DATA(iseq)->last_line == line) {
        /* ignore */
    }
    else {
        if (nd_fl_newline(node)) {
            int event = RUBY_EVENT_LINE;
            ISEQ_COMPILE_DATA(iseq)->last_line = line;
            if (line > 0 && ISEQ_COVERAGE(iseq) && ISEQ_LINE_COVERAGE(iseq)) {
                event |= RUBY_EVENT_COVERAGE_LINE;
            }
            ADD_TRACE(ret, event);
        }
    }

    debug_node_start(node);
#undef BEFORE_RETURN
#define BEFORE_RETURN debug_node_end()

    switch (type) {
      case NODE_BLOCK:
        CHECK(compile_block(iseq, ret, node, popped));
        break;
      case NODE_IF:
      case NODE_UNLESS:
        CHECK(compile_if(iseq, ret, node, popped, type));
        break;
      case NODE_CASE:
        CHECK(compile_case(iseq, ret, node, popped));
        break;
      case NODE_CASE2:
        CHECK(compile_case2(iseq, ret, node, popped));
        break;
      case NODE_CASE3:
        CHECK(compile_case3(iseq, ret, node, popped));
        break;
      case NODE_WHILE:
      case NODE_UNTIL:
        CHECK(compile_loop(iseq, ret, node, popped, type));
        break;
      case NODE_FOR:
      case NODE_ITER:
        CHECK(compile_iter(iseq, ret, node, popped));
        break;
      case NODE_FOR_MASGN:
        CHECK(compile_for_masgn(iseq, ret, node, popped));
        break;
      case NODE_BREAK:
        CHECK(compile_break(iseq, ret, node, popped));
        break;
      case NODE_NEXT:
        CHECK(compile_next(iseq, ret, node, popped));
        break;
      case NODE_REDO:
        CHECK(compile_redo(iseq, ret, node, popped));
        break;
      case NODE_RETRY:
        CHECK(compile_retry(iseq, ret, node, popped));
        break;
      case NODE_BEGIN:{
        CHECK(COMPILE_(ret, "NODE_BEGIN", RNODE_BEGIN(node)->nd_body, popped));
        break;
      }
      case NODE_RESCUE:
        CHECK(compile_rescue(iseq, ret, node, popped));
        break;
      case NODE_RESBODY:
        CHECK(compile_resbody(iseq, ret, node, popped));
        break;
      case NODE_ENSURE:
        CHECK(compile_ensure(iseq, ret, node, popped));
        break;

      case NODE_AND:
      case NODE_OR:{
        LABEL *end_label = NEW_LABEL(line);
        CHECK(COMPILE(ret, "nd_1st", RNODE_OR(node)->nd_1st));
        if (!popped) {
            ADD_INSN(ret, node, dup);
        }
        if (type == NODE_AND) {
            ADD_INSNL(ret, node, branchunless, end_label);
        }
        else {
            ADD_INSNL(ret, node, branchif, end_label);
        }
        if (!popped) {
            ADD_INSN(ret, node, pop);
        }
        CHECK(COMPILE_(ret, "nd_2nd", RNODE_OR(node)->nd_2nd, popped));
        ADD_LABEL(ret, end_label);
        break;
      }

      case NODE_MASGN:{
        bool prev_in_masgn = ISEQ_COMPILE_DATA(iseq)->in_masgn;
        ISEQ_COMPILE_DATA(iseq)->in_masgn = true;
        compile_massign(iseq, ret, node, popped);
        ISEQ_COMPILE_DATA(iseq)->in_masgn = prev_in_masgn;
        break;
      }

      case NODE_LASGN:{
        ID id = RNODE_LASGN(node)->nd_vid;
        int idx = ISEQ_BODY(body->local_iseq)->local_table_size - get_local_var_idx(iseq, id);

        debugs("lvar: %s idx: %d\n", rb_id2name(id), idx);
        CHECK(COMPILE(ret, "rvalue", RNODE_LASGN(node)->nd_value));

        if (!popped) {
            ADD_INSN(ret, node, dup);
        }
        ADD_SETLOCAL(ret, node, idx, get_lvar_level(iseq));
        break;
      }
      case NODE_DASGN: {
        int idx, lv, ls;
        ID id = RNODE_DASGN(node)->nd_vid;
        CHECK(COMPILE(ret, "dvalue", RNODE_DASGN(node)->nd_value));
        debugi("dassn id", rb_id2str(id) ? id : '*');

        if (!popped) {
            ADD_INSN(ret, node, dup);
        }

        idx = get_dyna_var_idx(iseq, id, &lv, &ls);

        if (idx < 0) {
            COMPILE_ERROR(ERROR_ARGS "NODE_DASGN: unknown id (%"PRIsVALUE")",
                          rb_id2str(id));
            goto ng;
        }
        ADD_SETLOCAL(ret, node, ls - idx, lv);
        break;
      }
      case NODE_GASGN:{
        CHECK(COMPILE(ret, "lvalue", RNODE_GASGN(node)->nd_value));

        if (!popped) {
            ADD_INSN(ret, node, dup);
        }
        ADD_INSN1(ret, node, setglobal, ID2SYM(RNODE_GASGN(node)->nd_vid));
        break;
      }
      case NODE_IASGN:{
        CHECK(COMPILE(ret, "lvalue", RNODE_IASGN(node)->nd_value));
        if (!popped) {
            ADD_INSN(ret, node, dup);
        }
        ADD_INSN2(ret, node, setinstancevariable,
                  ID2SYM(RNODE_IASGN(node)->nd_vid),
                  get_ivar_ic_value(iseq,RNODE_IASGN(node)->nd_vid));
        break;
      }
      case NODE_CDECL:{
        if (RNODE_CDECL(node)->nd_vid) {
            CHECK(compile_shareable_constant_value(iseq, ret, RNODE_CDECL(node)->shareability, node, RNODE_CDECL(node)->nd_value));

            if (!popped) {
                ADD_INSN(ret, node, dup);
            }

            ADD_INSN1(ret, node, putspecialobject,
                      INT2FIX(VM_SPECIAL_OBJECT_CONST_BASE));
            ADD_INSN1(ret, node, setconstant, ID2SYM(RNODE_CDECL(node)->nd_vid));
        }
        else {
            compile_cpath(ret, iseq, RNODE_CDECL(node)->nd_else);
            CHECK(compile_shareable_constant_value(iseq, ret, RNODE_CDECL(node)->shareability, node, RNODE_CDECL(node)->nd_value));
            ADD_INSN(ret, node, swap);

            if (!popped) {
                ADD_INSN1(ret, node, topn, INT2FIX(1));
                ADD_INSN(ret, node, swap);
            }

            ADD_INSN1(ret, node, setconstant, ID2SYM(get_node_colon_nd_mid(RNODE_CDECL(node)->nd_else)));
        }
        break;
      }
      case NODE_CVASGN:{
        CHECK(COMPILE(ret, "cvasgn val", RNODE_CVASGN(node)->nd_value));
        if (!popped) {
            ADD_INSN(ret, node, dup);
        }
        ADD_INSN2(ret, node, setclassvariable,
                  ID2SYM(RNODE_CVASGN(node)->nd_vid),
                  get_cvar_ic_value(iseq, RNODE_CVASGN(node)->nd_vid));
        break;
      }
      case NODE_OP_ASGN1:
        CHECK(compile_op_asgn1(iseq, ret, node, popped));
        break;
      case NODE_OP_ASGN2:
        CHECK(compile_op_asgn2(iseq, ret, node, popped));
        break;
      case NODE_OP_CDECL:
        CHECK(compile_op_cdecl(iseq, ret, node, popped));
        break;
      case NODE_OP_ASGN_AND:
      case NODE_OP_ASGN_OR:
        CHECK(compile_op_log(iseq, ret, node, popped, type));
        break;
      case NODE_CALL:   /* obj.foo */
      case NODE_OPCALL: /* foo[] */
        if (compile_call_precheck_freeze(iseq, ret, node, node, popped) == TRUE) {
            break;
        }
      case NODE_QCALL: /* obj&.foo */
      case NODE_FCALL: /* foo() */
      case NODE_VCALL: /* foo (variable or call) */
        if (compile_call(iseq, ret, node, type, node, popped, false) == COMPILE_NG) {
            goto ng;
        }
        break;
      case NODE_SUPER:
      case NODE_ZSUPER:
        CHECK(compile_super(iseq, ret, node, popped, type));
        break;
      case NODE_LIST:{
        CHECK(compile_array(iseq, ret, node, popped, TRUE) >= 0);
        break;
      }
      case NODE_ZLIST:{
        if (!popped) {
            ADD_INSN1(ret, node, newarray, INT2FIX(0));
        }
        break;
      }
      case NODE_HASH:
        CHECK(compile_hash(iseq, ret, node, FALSE, popped) >= 0);
        break;
      case NODE_RETURN:
        CHECK(compile_return(iseq, ret, node, popped));
        break;
      case NODE_YIELD:
        CHECK(compile_yield(iseq, ret, node, popped));
        break;
      case NODE_LVAR:{
        if (!popped) {
            compile_lvar(iseq, ret, node, RNODE_LVAR(node)->nd_vid);
        }
        break;
      }
      case NODE_DVAR:{
        int lv, idx, ls;
        debugi("nd_vid", RNODE_DVAR(node)->nd_vid);
        if (!popped) {
            idx = get_dyna_var_idx(iseq, RNODE_DVAR(node)->nd_vid, &lv, &ls);
            if (idx < 0) {
                COMPILE_ERROR(ERROR_ARGS "unknown dvar (%"PRIsVALUE")",
                              rb_id2str(RNODE_DVAR(node)->nd_vid));
                goto ng;
            }
            ADD_GETLOCAL(ret, node, ls - idx, lv);
        }
        break;
      }
      case NODE_GVAR:{
        ADD_INSN1(ret, node, getglobal, ID2SYM(RNODE_GVAR(node)->nd_vid));
        if (popped) {
            ADD_INSN(ret, node, pop);
        }
        break;
      }
      case NODE_IVAR:{
        debugi("nd_vid", RNODE_IVAR(node)->nd_vid);
        if (!popped) {
            ADD_INSN2(ret, node, getinstancevariable,
                      ID2SYM(RNODE_IVAR(node)->nd_vid),
                      get_ivar_ic_value(iseq, RNODE_IVAR(node)->nd_vid));
        }
        break;
      }
      case NODE_CONST:{
        debugi("nd_vid", RNODE_CONST(node)->nd_vid);

        if (ISEQ_COMPILE_DATA(iseq)->option->inline_const_cache) {
            body->ic_size++;
            VALUE segments = rb_ary_new_from_args(1, ID2SYM(RNODE_CONST(node)->nd_vid));
            ADD_INSN1(ret, node, opt_getconstant_path, segments);
            RB_OBJ_WRITTEN(iseq, Qundef, segments);
        }
        else {
            ADD_INSN(ret, node, putnil);
            ADD_INSN1(ret, node, putobject, Qtrue);
            ADD_INSN1(ret, node, getconstant, ID2SYM(RNODE_CONST(node)->nd_vid));
        }

        if (popped) {
            ADD_INSN(ret, node, pop);
        }
        break;
      }
      case NODE_CVAR:{
        if (!popped) {
            ADD_INSN2(ret, node, getclassvariable,
                      ID2SYM(RNODE_CVAR(node)->nd_vid),
                      get_cvar_ic_value(iseq, RNODE_CVAR(node)->nd_vid));
        }
        break;
      }
      case NODE_NTH_REF:{
        if (!popped) {
            if (!RNODE_NTH_REF(node)->nd_nth) {
                ADD_INSN(ret, node, putnil);
                break;
            }
            ADD_INSN2(ret, node, getspecial, INT2FIX(1) /* '~'  */,
                      INT2FIX(RNODE_NTH_REF(node)->nd_nth << 1));
        }
        break;
      }
      case NODE_BACK_REF:{
        if (!popped) {
            ADD_INSN2(ret, node, getspecial, INT2FIX(1) /* '~' */,
                      INT2FIX(0x01 | (RNODE_BACK_REF(node)->nd_nth << 1)));
        }
        break;
      }
      case NODE_MATCH:
      case NODE_MATCH2:
      case NODE_MATCH3:
        CHECK(compile_match(iseq, ret, node, popped, type));
        break;
      case NODE_SYM:{
        if (!popped) {
            ADD_INSN1(ret, node, putobject, rb_node_sym_string_val(node));
        }
        break;
      }
      case NODE_LINE:{
        if (!popped) {
            ADD_INSN1(ret, node, putobject, rb_node_line_lineno_val(node));
        }
        break;
      }
      case NODE_ENCODING:{
        if (!popped) {
            ADD_INSN1(ret, node, putobject, rb_node_encoding_val(node));
        }
        break;
      }
      case NODE_INTEGER:{
        VALUE lit = rb_node_integer_literal_val(node);
        debugp_param("integer", lit);
        if (!popped) {
            ADD_INSN1(ret, node, putobject, lit);
            RB_OBJ_WRITTEN(iseq, Qundef, lit);
        }
        break;
      }
      case NODE_FLOAT:{
        VALUE lit = rb_node_float_literal_val(node);
        debugp_param("float", lit);
        if (!popped) {
            ADD_INSN1(ret, node, putobject, lit);
            RB_OBJ_WRITTEN(iseq, Qundef, lit);
        }
        break;
      }
      case NODE_RATIONAL:{
        VALUE lit = rb_node_rational_literal_val(node);
        debugp_param("rational", lit);
        if (!popped) {
            ADD_INSN1(ret, node, putobject, lit);
            RB_OBJ_WRITTEN(iseq, Qundef, lit);
        }
        break;
      }
      case NODE_IMAGINARY:{
        VALUE lit = rb_node_imaginary_literal_val(node);
        debugp_param("imaginary", lit);
        if (!popped) {
            ADD_INSN1(ret, node, putobject, lit);
            RB_OBJ_WRITTEN(iseq, Qundef, lit);
        }
        break;
      }
      case NODE_FILE:
      case NODE_STR:{
        debugp_param("nd_lit", get_string_value(node));
        if (!popped) {
            VALUE lit = get_string_value(node);
            const rb_compile_option_t *option = ISEQ_COMPILE_DATA(iseq)->option;
            if ((option->debug_frozen_string_literal || RTEST(ruby_debug)) &&
                option->frozen_string_literal != ISEQ_FROZEN_STRING_LITERAL_DISABLED) {
                lit = rb_str_with_debug_created_info(lit, rb_iseq_path(iseq), line);
            }
            switch (option->frozen_string_literal) {
              case ISEQ_FROZEN_STRING_LITERAL_UNSET:
                ADD_INSN1(ret, node, putchilledstring, lit);
                break;
              case ISEQ_FROZEN_STRING_LITERAL_DISABLED:
                ADD_INSN1(ret, node, putstring, lit);
                break;
              case ISEQ_FROZEN_STRING_LITERAL_ENABLED:
                ADD_INSN1(ret, node, putobject, lit);
                break;
              default:
                rb_bug("invalid frozen_string_literal");
            }
            RB_OBJ_WRITTEN(iseq, Qundef, lit);
        }
        break;
      }
      case NODE_DSTR:{
        compile_dstr(iseq, ret, node);

        if (popped) {
            ADD_INSN(ret, node, pop);
        }
        break;
      }
      case NODE_XSTR:{
        ADD_CALL_RECEIVER(ret, node);
        VALUE str = rb_node_str_string_val(node);
        ADD_INSN1(ret, node, putobject, str);
        RB_OBJ_WRITTEN(iseq, Qundef, str);
        ADD_CALL(ret, node, idBackquote, INT2FIX(1));

        if (popped) {
            ADD_INSN(ret, node, pop);
        }
        break;
      }
      case NODE_DXSTR:{
        ADD_CALL_RECEIVER(ret, node);
        compile_dstr(iseq, ret, node);
        ADD_CALL(ret, node, idBackquote, INT2FIX(1));

        if (popped) {
            ADD_INSN(ret, node, pop);
        }
        break;
      }
      case NODE_EVSTR:
        CHECK(compile_evstr(iseq, ret, RNODE_EVSTR(node)->nd_body, popped));
        break;
      case NODE_REGX:{
        if (!popped) {
            VALUE lit = rb_node_regx_string_val(node);
            ADD_INSN1(ret, node, putobject, lit);
            RB_OBJ_WRITTEN(iseq, Qundef, lit);
        }
        break;
      }
      case NODE_DREGX:
        compile_dregx(iseq, ret, node, popped);
        break;
      case NODE_ONCE:{
        int ic_index = body->ise_size++;
        const rb_iseq_t *block_iseq;
        block_iseq = NEW_CHILD_ISEQ(RNODE_ONCE(node)->nd_body, make_name_for_block(iseq), ISEQ_TYPE_PLAIN, line);

        ADD_INSN2(ret, node, once, block_iseq, INT2FIX(ic_index));
        RB_OBJ_WRITTEN(iseq, Qundef, (VALUE)block_iseq);

        if (popped) {
            ADD_INSN(ret, node, pop);
        }
        break;
      }
      case NODE_ARGSCAT:{
        if (popped) {
            CHECK(COMPILE(ret, "argscat head", RNODE_ARGSCAT(node)->nd_head));
            ADD_INSN1(ret, node, splatarray, Qfalse);
            ADD_INSN(ret, node, pop);
            CHECK(COMPILE(ret, "argscat body", RNODE_ARGSCAT(node)->nd_body));
            ADD_INSN1(ret, node, splatarray, Qfalse);
            ADD_INSN(ret, node, pop);
        }
        else {
            CHECK(COMPILE(ret, "argscat head", RNODE_ARGSCAT(node)->nd_head));
            const NODE *body_node = RNODE_ARGSCAT(node)->nd_body;
            if (nd_type_p(body_node, NODE_LIST)) {
                CHECK(compile_array(iseq, ret, body_node, popped, FALSE) >= 0);
            }
            else {
                CHECK(COMPILE(ret, "argscat body", body_node));
                ADD_INSN(ret, node, concattoarray);
            }
        }
        break;
      }
      case NODE_ARGSPUSH:{
        if (popped) {
            CHECK(COMPILE(ret, "argspush head", RNODE_ARGSPUSH(node)->nd_head));
            ADD_INSN1(ret, node, splatarray, Qfalse);
            ADD_INSN(ret, node, pop);
            CHECK(COMPILE_(ret, "argspush body", RNODE_ARGSPUSH(node)->nd_body, popped));
        }
        else {
            CHECK(COMPILE(ret, "argspush head", RNODE_ARGSPUSH(node)->nd_head));
            const NODE *body_node = RNODE_ARGSPUSH(node)->nd_body;
            if (keyword_node_p(body_node)) {
                CHECK(COMPILE_(ret, "array element", body_node, FALSE));
                ADD_INSN(ret, node, pushtoarraykwsplat);
            }
            else if (static_literal_node_p(body_node, iseq, false)) {
                ADD_INSN1(ret, body_node, putobject, static_literal_value(body_node, iseq));
                ADD_INSN1(ret, node, pushtoarray, INT2FIX(1));
            }
            else {
                CHECK(COMPILE_(ret, "array element", body_node, FALSE));
                ADD_INSN1(ret, node, pushtoarray, INT2FIX(1));
            }
        }
        break;
      }
      case NODE_SPLAT:{
        CHECK(COMPILE(ret, "splat", RNODE_SPLAT(node)->nd_head));
        ADD_INSN1(ret, node, splatarray, Qtrue);

        if (popped) {
            ADD_INSN(ret, node, pop);
        }
        break;
      }
      case NODE_DEFN:{
        ID mid = RNODE_DEFN(node)->nd_mid;
        const rb_iseq_t *method_iseq = NEW_ISEQ(RNODE_DEFN(node)->nd_defn,
                                                rb_id2str(mid),
                                                ISEQ_TYPE_METHOD, line);

        debugp_param("defn/iseq", rb_iseqw_new(method_iseq));
        ADD_INSN2(ret, node, definemethod, ID2SYM(mid), method_iseq);
        RB_OBJ_WRITTEN(iseq, Qundef, (VALUE)method_iseq);

        if (!popped) {
            ADD_INSN1(ret, node, putobject, ID2SYM(mid));
        }

        break;
      }
      case NODE_DEFS:{
        ID mid = RNODE_DEFS(node)->nd_mid;
        const rb_iseq_t * singleton_method_iseq = NEW_ISEQ(RNODE_DEFS(node)->nd_defn,
                                                           rb_id2str(mid),
                                                           ISEQ_TYPE_METHOD, line);

        debugp_param("defs/iseq", rb_iseqw_new(singleton_method_iseq));
        CHECK(COMPILE(ret, "defs: recv", RNODE_DEFS(node)->nd_recv));
        ADD_INSN2(ret, node, definesmethod, ID2SYM(mid), singleton_method_iseq);
        RB_OBJ_WRITTEN(iseq, Qundef, (VALUE)singleton_method_iseq);

        if (!popped) {
            ADD_INSN1(ret, node, putobject, ID2SYM(mid));
        }
        break;
      }
      case NODE_ALIAS:{
        ADD_INSN1(ret, node, putspecialobject, INT2FIX(VM_SPECIAL_OBJECT_VMCORE));
        ADD_INSN1(ret, node, putspecialobject, INT2FIX(VM_SPECIAL_OBJECT_CBASE));
        CHECK(COMPILE(ret, "alias arg1", RNODE_ALIAS(node)->nd_1st));
        CHECK(COMPILE(ret, "alias arg2", RNODE_ALIAS(node)->nd_2nd));
        ADD_SEND(ret, node, id_core_set_method_alias, INT2FIX(3));

        if (popped) {
            ADD_INSN(ret, node, pop);
        }
        break;
      }
      case NODE_VALIAS:{
        ADD_INSN1(ret, node, putspecialobject, INT2FIX(VM_SPECIAL_OBJECT_VMCORE));
        ADD_INSN1(ret, node, putobject, ID2SYM(RNODE_VALIAS(node)->nd_alias));
        ADD_INSN1(ret, node, putobject, ID2SYM(RNODE_VALIAS(node)->nd_orig));
        ADD_SEND(ret, node, id_core_set_variable_alias, INT2FIX(2));

        if (popped) {
            ADD_INSN(ret, node, pop);
        }
        break;
      }
      case NODE_UNDEF:{
        const rb_parser_ary_t *ary = RNODE_UNDEF(node)->nd_undefs;

        for (long i = 0; i < ary->len; i++) {
            ADD_INSN1(ret, node, putspecialobject, INT2FIX(VM_SPECIAL_OBJECT_VMCORE));
            ADD_INSN1(ret, node, putspecialobject, INT2FIX(VM_SPECIAL_OBJECT_CBASE));
            CHECK(COMPILE(ret, "undef arg", ary->data[i]));
            ADD_SEND(ret, node, id_core_undef_method, INT2FIX(2));

            if (i < ary->len - 1) {
                ADD_INSN(ret, node, pop);
            }
        }

        if (popped) {
            ADD_INSN(ret, node, pop);
        }
        break;
      }
      case NODE_CLASS:{
        const rb_iseq_t *class_iseq = NEW_CHILD_ISEQ(RNODE_CLASS(node)->nd_body,
                                                     rb_str_freeze(rb_sprintf("<class:%"PRIsVALUE">", rb_id2str(get_node_colon_nd_mid(RNODE_CLASS(node)->nd_cpath)))),
                                                     ISEQ_TYPE_CLASS, line);
        const int flags = VM_DEFINECLASS_TYPE_CLASS |
            (RNODE_CLASS(node)->nd_super ? VM_DEFINECLASS_FLAG_HAS_SUPERCLASS : 0) |
            compile_cpath(ret, iseq, RNODE_CLASS(node)->nd_cpath);

        CHECK(COMPILE(ret, "super", RNODE_CLASS(node)->nd_super));
        ADD_INSN3(ret, node, defineclass, ID2SYM(get_node_colon_nd_mid(RNODE_CLASS(node)->nd_cpath)), class_iseq, INT2FIX(flags));
        RB_OBJ_WRITTEN(iseq, Qundef, (VALUE)class_iseq);

        if (popped) {
            ADD_INSN(ret, node, pop);
        }
        break;
      }
      case NODE_MODULE:{
        const rb_iseq_t *module_iseq = NEW_CHILD_ISEQ(RNODE_MODULE(node)->nd_body,
                                                      rb_str_freeze(rb_sprintf("<module:%"PRIsVALUE">", rb_id2str(get_node_colon_nd_mid(RNODE_MODULE(node)->nd_cpath)))),
                                                      ISEQ_TYPE_CLASS, line);
        const int flags = VM_DEFINECLASS_TYPE_MODULE |
            compile_cpath(ret, iseq, RNODE_MODULE(node)->nd_cpath);

        ADD_INSN (ret, node, putnil); /* dummy */
        ADD_INSN3(ret, node, defineclass, ID2SYM(get_node_colon_nd_mid(RNODE_MODULE(node)->nd_cpath)), module_iseq, INT2FIX(flags));
        RB_OBJ_WRITTEN(iseq, Qundef, (VALUE)module_iseq);

        if (popped) {
            ADD_INSN(ret, node, pop);
        }
        break;
      }
      case NODE_SCLASS:{
        ID singletonclass;
        const rb_iseq_t *singleton_class = NEW_ISEQ(RNODE_SCLASS(node)->nd_body, rb_fstring_lit("singleton class"),
                                                    ISEQ_TYPE_CLASS, line);

        CHECK(COMPILE(ret, "sclass#recv", RNODE_SCLASS(node)->nd_recv));
        ADD_INSN (ret, node, putnil);
        CONST_ID(singletonclass, "singletonclass");
        ADD_INSN3(ret, node, defineclass,
                  ID2SYM(singletonclass), singleton_class,
                  INT2FIX(VM_DEFINECLASS_TYPE_SINGLETON_CLASS));
        RB_OBJ_WRITTEN(iseq, Qundef, (VALUE)singleton_class);

        if (popped) {
            ADD_INSN(ret, node, pop);
        }
        break;
      }
      case NODE_COLON2:
        CHECK(compile_colon2(iseq, ret, node, popped));
        break;
      case NODE_COLON3:
        CHECK(compile_colon3(iseq, ret, node, popped));
        break;
      case NODE_DOT2:
        CHECK(compile_dots(iseq, ret, node, popped, FALSE));
        break;
      case NODE_DOT3:
        CHECK(compile_dots(iseq, ret, node, popped, TRUE));
        break;
      case NODE_FLIP2:
      case NODE_FLIP3:{
        LABEL *lend = NEW_LABEL(line);
        LABEL *ltrue = NEW_LABEL(line);
        LABEL *lfalse = NEW_LABEL(line);
        CHECK(compile_flip_flop(iseq, ret, node, type == NODE_FLIP2,
                                ltrue, lfalse));
        ADD_LABEL(ret, ltrue);
        ADD_INSN1(ret, node, putobject, Qtrue);
        ADD_INSNL(ret, node, jump, lend);
        ADD_LABEL(ret, lfalse);
        ADD_INSN1(ret, node, putobject, Qfalse);
        ADD_LABEL(ret, lend);
        break;
      }
      case NODE_SELF:{
        if (!popped) {
            ADD_INSN(ret, node, putself);
        }
        break;
      }
      case NODE_NIL:{
        if (!popped) {
            ADD_INSN(ret, node, putnil);
        }
        break;
      }
      case NODE_TRUE:{
        if (!popped) {
            ADD_INSN1(ret, node, putobject, Qtrue);
        }
        break;
      }
      case NODE_FALSE:{
        if (!popped) {
            ADD_INSN1(ret, node, putobject, Qfalse);
        }
        break;
      }
      case NODE_ERRINFO:
        CHECK(compile_errinfo(iseq, ret, node, popped));
        break;
      case NODE_DEFINED:
        if (!popped) {
            CHECK(compile_defined_expr(iseq, ret, node, Qtrue, false));
        }
        break;
      case NODE_POSTEXE:{
        /* compiled to:
         *   ONCE{ rb_mRubyVMFrozenCore::core#set_postexe{ ... } }
         */
        int is_index = body->ise_size++;
        struct rb_iseq_new_with_callback_callback_func *ifunc =
            rb_iseq_new_with_callback_new_callback(build_postexe_iseq, RNODE_POSTEXE(node)->nd_body);
        const rb_iseq_t *once_iseq =
            NEW_CHILD_ISEQ_WITH_CALLBACK(ifunc, rb_fstring(make_name_for_block(iseq)), ISEQ_TYPE_BLOCK, line);

        ADD_INSN2(ret, node, once, once_iseq, INT2FIX(is_index));
        RB_OBJ_WRITTEN(iseq, Qundef, (VALUE)once_iseq);

        if (popped) {
            ADD_INSN(ret, node, pop);
        }
        break;
      }
      case NODE_KW_ARG:
        CHECK(compile_kw_arg(iseq, ret, node, popped));
        break;
      case NODE_DSYM:{
        compile_dstr(iseq, ret, node);
        if (!popped) {
            ADD_INSN(ret, node, intern);
        }
        else {
            ADD_INSN(ret, node, pop);
        }
        break;
      }
      case NODE_ATTRASGN:
        CHECK(compile_attrasgn(iseq, ret, node, popped));
        break;
      case NODE_LAMBDA:{
        /* compile same as lambda{...} */
        const rb_iseq_t *block = NEW_CHILD_ISEQ(RNODE_LAMBDA(node)->nd_body, make_name_for_block(iseq), ISEQ_TYPE_BLOCK, line);
        VALUE argc = INT2FIX(0);

        ADD_INSN1(ret, node, putspecialobject, INT2FIX(VM_SPECIAL_OBJECT_VMCORE));
        ADD_CALL_WITH_BLOCK(ret, node, idLambda, argc, block);
        RB_OBJ_WRITTEN(iseq, Qundef, (VALUE)block);

        if (popped) {
            ADD_INSN(ret, node, pop);
        }
        break;
      }
      default:
        UNKNOWN_NODE("iseq_compile_each", node, COMPILE_NG);
      ng:
        debug_node_end();
        return COMPILE_NG;
    }

    debug_node_end();
    return COMPILE_OK;
}

/***************************/
/* instruction information */
/***************************/

static int
insn_data_length(INSN *iobj)
{
    return insn_len(iobj->insn_id);
}

static int
calc_sp_depth(int depth, INSN *insn)
{
    return comptime_insn_stack_increase(depth, insn->insn_id, insn->operands);
}

static VALUE
opobj_inspect(VALUE obj)
{
    if (!SPECIAL_CONST_P(obj) && !RBASIC_CLASS(obj)) {
        switch (BUILTIN_TYPE(obj)) {
          case T_STRING:
            obj = rb_str_new_cstr(RSTRING_PTR(obj));
            break;
          case T_ARRAY:
            obj = rb_ary_dup(obj);
            break;
          default:
            break;
        }
    }
    return rb_inspect(obj);
}



static VALUE
insn_data_to_s_detail(INSN *iobj)
{
    VALUE str = rb_sprintf("%-20s ", insn_name(iobj->insn_id));

    if (iobj->operands) {
        const char *types = insn_op_types(iobj->insn_id);
        int j;

        for (j = 0; types[j]; j++) {
            char type = types[j];

            switch (type) {
              case TS_OFFSET:	/* label(destination position) */
                {
                    LABEL *lobj = (LABEL *)OPERAND_AT(iobj, j);
                    rb_str_catf(str, LABEL_FORMAT, lobj->label_no);
                    break;
                }
                break;
              case TS_ISEQ:	/* iseq */
                {
                    rb_iseq_t *iseq = (rb_iseq_t *)OPERAND_AT(iobj, j);
                    VALUE val = Qnil;
                    if (0 && iseq) { /* TODO: invalidate now */
                        val = (VALUE)iseq;
                    }
                    rb_str_concat(str, opobj_inspect(val));
                }
                break;
              case TS_LINDEX:
              case TS_NUM:	/* ulong */
              case TS_VALUE:	/* VALUE */
                {
                    VALUE v = OPERAND_AT(iobj, j);
                    if (!CLASS_OF(v))
                        rb_str_cat2(str, "<hidden>");
                    else {
                        rb_str_concat(str, opobj_inspect(v));
                    }
                    break;
                }
              case TS_ID:	/* ID */
                rb_str_concat(str, opobj_inspect(OPERAND_AT(iobj, j)));
                break;
              case TS_IC:	/* inline cache */
                rb_str_concat(str, opobj_inspect(OPERAND_AT(iobj, j)));
                break;
              case TS_IVC:	/* inline ivar cache */
                rb_str_catf(str, "<ivc:%d>", FIX2INT(OPERAND_AT(iobj, j)));
                break;
              case TS_ICVARC:   /* inline cvar cache */
                rb_str_catf(str, "<icvarc:%d>", FIX2INT(OPERAND_AT(iobj, j)));
                break;
              case TS_ISE:	/* inline storage entry */
                rb_str_catf(str, "<ise:%d>", FIX2INT(OPERAND_AT(iobj, j)));
                break;
              case TS_CALLDATA: /* we store these as call infos at compile time */
                {
                    const struct rb_callinfo *ci = (struct rb_callinfo *)OPERAND_AT(iobj, j);
                    rb_str_cat2(str, "<calldata:");
                    if (vm_ci_mid(ci)) rb_str_catf(str, "%"PRIsVALUE, rb_id2str(vm_ci_mid(ci)));
                    rb_str_catf(str, ", %d>", vm_ci_argc(ci));
                    break;
                }
              case TS_CDHASH:	/* case/when condition cache */
                rb_str_cat2(str, "<ch>");
                break;
              case TS_FUNCPTR:
                {
                    void *func = (void *)OPERAND_AT(iobj, j);
#ifdef HAVE_DLADDR
                    Dl_info info;
                    if (dladdr(func, &info) && info.dli_sname) {
                        rb_str_cat2(str, info.dli_sname);
                        break;
                    }
#endif
                    rb_str_catf(str, "<%p>", func);
                }
                break;
              case TS_BUILTIN:
                rb_str_cat2(str, "<TS_BUILTIN>");
                break;
              default:{
                rb_raise(rb_eSyntaxError, "unknown operand type: %c", type);
              }
            }
            if (types[j + 1]) {
                rb_str_cat2(str, ", ");
            }
        }
    }
    return str;
}

static void
dump_disasm_list(const LINK_ELEMENT *link)
{
    dump_disasm_list_with_cursor(link, NULL, NULL);
}

static void
dump_disasm_list_with_cursor(const LINK_ELEMENT *link, const LINK_ELEMENT *curr, const LABEL *dest)
{
    int pos = 0;
    INSN *iobj;
    LABEL *lobj;
    VALUE str;

    printf("-- raw disasm--------\n");

    while (link) {
        if (curr) printf(curr == link ? "*" : " ");
        switch (link->type) {
          case ISEQ_ELEMENT_INSN:
            {
                iobj = (INSN *)link;
                str = insn_data_to_s_detail(iobj);
                printf("  %04d %-65s(%4u)\n", pos, StringValueCStr(str), iobj->insn_info.line_no);
                pos += insn_data_length(iobj);
                break;
            }
          case ISEQ_ELEMENT_LABEL:
            {
                lobj = (LABEL *)link;
                printf(LABEL_FORMAT" [sp: %d, unremovable: %d, refcnt: %d]%s\n", lobj->label_no, lobj->sp, lobj->unremovable, lobj->refcnt,
                       dest == lobj ? " <---" : "");
                break;
            }
          case ISEQ_ELEMENT_TRACE:
            {
                TRACE *trace = (TRACE *)link;
                printf("  trace: %0x\n", trace->event);
                break;
            }
          case ISEQ_ELEMENT_ADJUST:
            {
                ADJUST *adjust = (ADJUST *)link;
                printf("  adjust: [label: %d]\n", adjust->label ? adjust->label->label_no : -1);
                break;
            }
          default:
            /* ignore */
            rb_raise(rb_eSyntaxError, "dump_disasm_list error: %d\n", (int)link->type);
        }
        link = link->next;
    }
    printf("---------------------\n");
    fflush(stdout);
}

int
rb_insn_len(VALUE insn)
{
    return insn_len(insn);
}

const char *
rb_insns_name(int i)
{
    return insn_name(i);
}

VALUE
rb_insns_name_array(void)
{
    VALUE ary = rb_ary_new_capa(VM_INSTRUCTION_SIZE);
    int i;
    for (i = 0; i < VM_INSTRUCTION_SIZE; i++) {
        rb_ary_push(ary, rb_fstring_cstr(insn_name(i)));
    }
    return rb_ary_freeze(ary);
}

static LABEL *
register_label(rb_iseq_t *iseq, struct st_table *labels_table, VALUE obj)
{
    LABEL *label = 0;
    st_data_t tmp;
    obj = rb_to_symbol_type(obj);

    if (st_lookup(labels_table, obj, &tmp) == 0) {
        label = NEW_LABEL(0);
        st_insert(labels_table, obj, (st_data_t)label);
    }
    else {
        label = (LABEL *)tmp;
    }
    LABEL_REF(label);
    return label;
}

static VALUE
get_exception_sym2type(VALUE sym)
{
    static VALUE symRescue, symEnsure, symRetry;
    static VALUE symBreak, symRedo, symNext;

    if (symRescue == 0) {
        symRescue = ID2SYM(rb_intern_const("rescue"));
        symEnsure = ID2SYM(rb_intern_const("ensure"));
        symRetry  = ID2SYM(rb_intern_const("retry"));
        symBreak  = ID2SYM(rb_intern_const("break"));
        symRedo   = ID2SYM(rb_intern_const("redo"));
        symNext   = ID2SYM(rb_intern_const("next"));
    }

    if (sym == symRescue) return CATCH_TYPE_RESCUE;
    if (sym == symEnsure) return CATCH_TYPE_ENSURE;
    if (sym == symRetry)  return CATCH_TYPE_RETRY;
    if (sym == symBreak)  return CATCH_TYPE_BREAK;
    if (sym == symRedo)   return CATCH_TYPE_REDO;
    if (sym == symNext)   return CATCH_TYPE_NEXT;
    rb_raise(rb_eSyntaxError, "invalid exception symbol: %+"PRIsVALUE, sym);
    return 0;
}

static int
iseq_build_from_ary_exception(rb_iseq_t *iseq, struct st_table *labels_table,
                     VALUE exception)
{
    int i;

    for (i=0; i<RARRAY_LEN(exception); i++) {
        const rb_iseq_t *eiseq;
        VALUE v, type;
        LABEL *lstart, *lend, *lcont;
        unsigned int sp;

        v = rb_to_array_type(RARRAY_AREF(exception, i));
        if (RARRAY_LEN(v) != 6) {
            rb_raise(rb_eSyntaxError, "wrong exception entry");
        }
        type = get_exception_sym2type(RARRAY_AREF(v, 0));
        if (NIL_P(RARRAY_AREF(v, 1))) {
            eiseq = NULL;
        }
        else {
            eiseq = rb_iseqw_to_iseq(rb_iseq_load(RARRAY_AREF(v, 1), (VALUE)iseq, Qnil));
        }

        lstart = register_label(iseq, labels_table, RARRAY_AREF(v, 2));
        lend   = register_label(iseq, labels_table, RARRAY_AREF(v, 3));
        lcont  = register_label(iseq, labels_table, RARRAY_AREF(v, 4));
        sp     = NUM2UINT(RARRAY_AREF(v, 5));

        /* TODO: Dirty Hack!  Fix me */
        if (type == CATCH_TYPE_RESCUE ||
            type == CATCH_TYPE_BREAK ||
            type == CATCH_TYPE_NEXT) {
            ++sp;
        }

        lcont->sp = sp;

        ADD_CATCH_ENTRY(type, lstart, lend, eiseq, lcont);

        RB_GC_GUARD(v);
    }
    return COMPILE_OK;
}

static struct st_table *
insn_make_insn_table(void)
{
    struct st_table *table;
    int i;
    table = st_init_numtable_with_size(VM_INSTRUCTION_SIZE);

    for (i=0; i<VM_INSTRUCTION_SIZE; i++) {
        st_insert(table, ID2SYM(rb_intern_const(insn_name(i))), i);
    }

    return table;
}

static const rb_iseq_t *
iseq_build_load_iseq(const rb_iseq_t *iseq, VALUE op)
{
    VALUE iseqw;
    const rb_iseq_t *loaded_iseq;

    if (RB_TYPE_P(op, T_ARRAY)) {
        iseqw = rb_iseq_load(op, (VALUE)iseq, Qnil);
    }
    else if (CLASS_OF(op) == rb_cISeq) {
        iseqw = op;
    }
    else {
        rb_raise(rb_eSyntaxError, "ISEQ is required");
    }

    loaded_iseq = rb_iseqw_to_iseq(iseqw);
    return loaded_iseq;
}

static VALUE
iseq_build_callinfo_from_hash(rb_iseq_t *iseq, VALUE op)
{
    ID mid = 0;
    int orig_argc = 0;
    unsigned int flag = 0;
    struct rb_callinfo_kwarg *kw_arg = 0;

    if (!NIL_P(op)) {
        VALUE vmid = rb_hash_aref(op, ID2SYM(rb_intern_const("mid")));
        VALUE vflag = rb_hash_aref(op, ID2SYM(rb_intern_const("flag")));
        VALUE vorig_argc = rb_hash_aref(op, ID2SYM(rb_intern_const("orig_argc")));
        VALUE vkw_arg = rb_hash_aref(op, ID2SYM(rb_intern_const("kw_arg")));

        if (!NIL_P(vmid)) mid = SYM2ID(vmid);
        if (!NIL_P(vflag)) flag = NUM2UINT(vflag);
        if (!NIL_P(vorig_argc)) orig_argc = FIX2INT(vorig_argc);

        if (!NIL_P(vkw_arg)) {
            int i;
            int len = RARRAY_LENINT(vkw_arg);
            size_t n = rb_callinfo_kwarg_bytes(len);

            kw_arg = xmalloc(n);
            kw_arg->references = 0;
            kw_arg->keyword_len = len;
            for (i = 0; i < len; i++) {
                VALUE kw = RARRAY_AREF(vkw_arg, i);
                SYM2ID(kw);	/* make immortal */
                kw_arg->keywords[i] = kw;
            }
        }
    }

    const struct rb_callinfo *ci = new_callinfo(iseq, mid, orig_argc, flag, kw_arg, (flag & VM_CALL_ARGS_SIMPLE) == 0);
    RB_OBJ_WRITTEN(iseq, Qundef, ci);
    return (VALUE)ci;
}

static rb_event_flag_t
event_name_to_flag(VALUE sym)
{
#define CHECK_EVENT(ev) if (sym == ID2SYM(rb_intern_const(#ev))) return ev;
                CHECK_EVENT(RUBY_EVENT_LINE);
                CHECK_EVENT(RUBY_EVENT_CLASS);
                CHECK_EVENT(RUBY_EVENT_END);
                CHECK_EVENT(RUBY_EVENT_CALL);
                CHECK_EVENT(RUBY_EVENT_RETURN);
                CHECK_EVENT(RUBY_EVENT_B_CALL);
                CHECK_EVENT(RUBY_EVENT_B_RETURN);
                CHECK_EVENT(RUBY_EVENT_RESCUE);
#undef CHECK_EVENT
    return RUBY_EVENT_NONE;
}

static int
iseq_build_from_ary_body(rb_iseq_t *iseq, LINK_ANCHOR *const anchor,
                         VALUE body, VALUE node_ids, VALUE labels_wrapper)
{
    /* TODO: body should be frozen */
    long i, len = RARRAY_LEN(body);
    struct st_table *labels_table = RTYPEDDATA_DATA(labels_wrapper);
    int j;
    int line_no = 0, node_id = -1, insn_idx = 0;
    int ret = COMPILE_OK;

    /*
     * index -> LABEL *label
     */
    static struct st_table *insn_table;

    if (insn_table == 0) {
        insn_table = insn_make_insn_table();
    }

    for (i=0; i<len; i++) {
        VALUE obj = RARRAY_AREF(body, i);

        if (SYMBOL_P(obj)) {
            rb_event_flag_t event;
            if ((event = event_name_to_flag(obj)) != RUBY_EVENT_NONE) {
                ADD_TRACE(anchor, event);
            }
            else {
                LABEL *label = register_label(iseq, labels_table, obj);
                ADD_LABEL(anchor, label);
            }
        }
        else if (FIXNUM_P(obj)) {
            line_no = NUM2INT(obj);
        }
        else if (RB_TYPE_P(obj, T_ARRAY)) {
            VALUE *argv = 0;
            int argc = RARRAY_LENINT(obj) - 1;
            st_data_t insn_id;
            VALUE insn;

            if (node_ids) {
                node_id = NUM2INT(rb_ary_entry(node_ids, insn_idx++));
            }

            insn = (argc < 0) ? Qnil : RARRAY_AREF(obj, 0);
            if (st_lookup(insn_table, (st_data_t)insn, &insn_id) == 0) {
                /* TODO: exception */
                COMPILE_ERROR(iseq, line_no,
                              "unknown instruction: %+"PRIsVALUE, insn);
                ret = COMPILE_NG;
                break;
            }

            if (argc != insn_len((VALUE)insn_id)-1) {
                COMPILE_ERROR(iseq, line_no,
                              "operand size mismatch");
                ret = COMPILE_NG;
                break;
            }

            if (argc > 0) {
                argv = compile_data_calloc2(iseq, sizeof(VALUE), argc);

                // add element before operand setup to make GC root
                ADD_ELEM(anchor,
                         (LINK_ELEMENT*)new_insn_core(iseq, line_no, node_id,
                                                      (enum ruby_vminsn_type)insn_id, argc, argv));

                for (j=0; j<argc; j++) {
                    VALUE op = rb_ary_entry(obj, j+1);
                    switch (insn_op_type((VALUE)insn_id, j)) {
                      case TS_OFFSET: {
                        LABEL *label = register_label(iseq, labels_table, op);
                        argv[j] = (VALUE)label;
                        break;
                      }
                      case TS_LINDEX:
                      case TS_NUM:
                        (void)NUM2INT(op);
                        argv[j] = op;
                        break;
                      case TS_VALUE:
                        argv[j] = op;
                        RB_OBJ_WRITTEN(iseq, Qundef, op);
                        break;
                      case TS_ISEQ:
                        {
                            if (op != Qnil) {
                                VALUE v = (VALUE)iseq_build_load_iseq(iseq, op);
                                argv[j] = v;
                                RB_OBJ_WRITTEN(iseq, Qundef, v);
                            }
                            else {
                                argv[j] = 0;
                            }
                        }
                        break;
                      case TS_ISE:
                        argv[j] = op;
                        if (NUM2UINT(op) >= ISEQ_BODY(iseq)->ise_size) {
                            ISEQ_BODY(iseq)->ise_size = NUM2INT(op) + 1;
                        }
                        break;
                      case TS_IC:
                        {
                            VALUE segments = rb_ary_new();
                            op = rb_to_array_type(op);

                            for (int i = 0; i < RARRAY_LEN(op); i++) {
                                VALUE sym = RARRAY_AREF(op, i);
                                sym = rb_to_symbol_type(sym);
                                rb_ary_push(segments, sym);
                            }

                            RB_GC_GUARD(op);
                            argv[j] = segments;
                            RB_OBJ_WRITTEN(iseq, Qundef, segments);
                            ISEQ_BODY(iseq)->ic_size++;
                        }
                        break;
                      case TS_IVC:  /* inline ivar cache */
                        argv[j] = op;
                        if (NUM2UINT(op) >= ISEQ_BODY(iseq)->ivc_size) {
                            ISEQ_BODY(iseq)->ivc_size = NUM2INT(op) + 1;
                        }
                        break;
                      case TS_ICVARC:  /* inline cvar cache */
                        argv[j] = op;
                        if (NUM2UINT(op) >= ISEQ_BODY(iseq)->icvarc_size) {
                            ISEQ_BODY(iseq)->icvarc_size = NUM2INT(op) + 1;
                        }
                        break;
                      case TS_CALLDATA:
                        argv[j] = iseq_build_callinfo_from_hash(iseq, op);
                        break;
                      case TS_ID:
                        argv[j] = rb_to_symbol_type(op);
                        break;
                      case TS_CDHASH:
                        {
                            int i;
                            VALUE map = rb_hash_new_with_size(RARRAY_LEN(op)/2);

                            RHASH_TBL_RAW(map)->type = &cdhash_type;
                            op = rb_to_array_type(op);
                            for (i=0; i<RARRAY_LEN(op); i+=2) {
                                VALUE key = RARRAY_AREF(op, i);
                                VALUE sym = RARRAY_AREF(op, i+1);
                                LABEL *label =
                                  register_label(iseq, labels_table, sym);
                                rb_hash_aset(map, key, (VALUE)label | 1);
                            }
                            RB_GC_GUARD(op);
                            argv[j] = map;
                            RB_OBJ_WRITTEN(iseq, Qundef, map);
                        }
                        break;
                      case TS_FUNCPTR:
                        {
#if SIZEOF_VALUE <= SIZEOF_LONG
                            long funcptr = NUM2LONG(op);
#else
                            LONG_LONG funcptr = NUM2LL(op);
#endif
                            argv[j] = (VALUE)funcptr;
                        }
                        break;
                      default:
                        rb_raise(rb_eSyntaxError, "unknown operand: %c", insn_op_type((VALUE)insn_id, j));
                    }
                }
            }
            else {
                ADD_ELEM(anchor,
                         (LINK_ELEMENT*)new_insn_core(iseq, line_no, node_id,
                                                      (enum ruby_vminsn_type)insn_id, argc, NULL));
            }
        }
        else {
            rb_raise(rb_eTypeError, "unexpected object for instruction");
        }
    }
    RTYPEDDATA_DATA(labels_wrapper) = 0;
    RB_GC_GUARD(labels_wrapper);
    validate_labels(iseq, labels_table);
    if (!ret) return ret;
    return iseq_setup(iseq, anchor);
}

#define CHECK_ARRAY(v)   rb_to_array_type(v)
#define CHECK_SYMBOL(v)  rb_to_symbol_type(v)

static int
int_param(int *dst, VALUE param, VALUE sym)
{
    VALUE val = rb_hash_aref(param, sym);
    if (FIXNUM_P(val)) {
        *dst = FIX2INT(val);
        return TRUE;
    }
    else if (!NIL_P(val)) {
        rb_raise(rb_eTypeError, "invalid %+"PRIsVALUE" Fixnum: %+"PRIsVALUE,
                 sym, val);
    }
    return FALSE;
}

static const struct rb_iseq_param_keyword *
iseq_build_kw(rb_iseq_t *iseq, VALUE params, VALUE keywords)
{
    int i, j;
    int len = RARRAY_LENINT(keywords);
    int default_len;
    VALUE key, sym, default_val;
    VALUE *dvs;
    ID *ids;
    struct rb_iseq_param_keyword *keyword = ZALLOC(struct rb_iseq_param_keyword);

    ISEQ_BODY(iseq)->param.flags.has_kw = TRUE;

    keyword->num = len;
#define SYM(s) ID2SYM(rb_intern_const(#s))
    (void)int_param(&keyword->bits_start, params, SYM(kwbits));
    i = keyword->bits_start - keyword->num;
    ids = (ID *)&ISEQ_BODY(iseq)->local_table[i];
#undef SYM

    /* required args */
    for (i = 0; i < len; i++) {
        VALUE val = RARRAY_AREF(keywords, i);

        if (!SYMBOL_P(val)) {
            goto default_values;
        }
        ids[i] = SYM2ID(val);
        keyword->required_num++;
    }

  default_values: /* note: we intentionally preserve `i' from previous loop */
    default_len = len - i;
    if (default_len == 0) {
        keyword->table = ids;
        return keyword;
    }
    else if (default_len < 0) {
        UNREACHABLE;
    }

    dvs = ALLOC_N(VALUE, (unsigned int)default_len);

    for (j = 0; i < len; i++, j++) {
        key = RARRAY_AREF(keywords, i);
        CHECK_ARRAY(key);

        switch (RARRAY_LEN(key)) {
          case 1:
            sym = RARRAY_AREF(key, 0);
            default_val = Qundef;
            break;
          case 2:
            sym = RARRAY_AREF(key, 0);
            default_val = RARRAY_AREF(key, 1);
            break;
          default:
            rb_raise(rb_eTypeError, "keyword default has unsupported len %+"PRIsVALUE, key);
        }
        ids[i] = SYM2ID(sym);
        RB_OBJ_WRITE(iseq, &dvs[j], default_val);
    }

    keyword->table = ids;
    keyword->default_values = dvs;

    return keyword;
}

static void
iseq_insn_each_object_mark_and_move(VALUE * obj, VALUE _)
{
    rb_gc_mark_and_move(obj);
}

void
rb_iseq_mark_and_move_insn_storage(struct iseq_compile_data_storage *storage)
{
    INSN *iobj = 0;
    size_t size = sizeof(INSN);
    unsigned int pos = 0;

    while (storage) {
#ifdef STRICT_ALIGNMENT
        size_t padding = calc_padding((void *)&storage->buff[pos], size);
#else
        const size_t padding = 0; /* expected to be optimized by compiler */
#endif /* STRICT_ALIGNMENT */
        size_t offset = pos + size + padding;
        if (offset > storage->size || offset > storage->pos) {
            pos = 0;
            storage = storage->next;
        }
        else {
#ifdef STRICT_ALIGNMENT
            pos += (int)padding;
#endif /* STRICT_ALIGNMENT */

            iobj = (INSN *)&storage->buff[pos];

            if (iobj->operands) {
                iseq_insn_each_markable_object(iobj, iseq_insn_each_object_mark_and_move, (VALUE)0);
            }
            pos += (int)size;
        }
    }
}

static const rb_data_type_t labels_wrapper_type = {
    .wrap_struct_name = "compiler/labels_wrapper",
    .function = {
        .dmark = (RUBY_DATA_FUNC)rb_mark_set,
        .dfree = (RUBY_DATA_FUNC)st_free_table,
    },
    .flags = RUBY_TYPED_FREE_IMMEDIATELY | RUBY_TYPED_WB_PROTECTED,
};

void
rb_iseq_build_from_ary(rb_iseq_t *iseq, VALUE misc, VALUE locals, VALUE params,
                         VALUE exception, VALUE body)
{
#define SYM(s) ID2SYM(rb_intern_const(#s))
    int i, len;
    unsigned int arg_size, local_size, stack_max;
    ID *tbl;
    struct st_table *labels_table = st_init_numtable();
    VALUE labels_wrapper = TypedData_Wrap_Struct(0, &labels_wrapper_type, labels_table);
    VALUE arg_opt_labels = rb_hash_aref(params, SYM(opt));
    VALUE keywords = rb_hash_aref(params, SYM(keyword));
    VALUE sym_arg_rest = ID2SYM(rb_intern_const("#arg_rest"));
    DECL_ANCHOR(anchor);
    INIT_ANCHOR(anchor);

    len = RARRAY_LENINT(locals);
    ISEQ_BODY(iseq)->local_table_size = len;
    ISEQ_BODY(iseq)->local_table = tbl = len > 0 ? (ID *)ALLOC_N(ID, ISEQ_BODY(iseq)->local_table_size) : NULL;

    for (i = 0; i < len; i++) {
        VALUE lv = RARRAY_AREF(locals, i);

        if (sym_arg_rest == lv) {
            tbl[i] = 0;
        }
        else {
            tbl[i] = FIXNUM_P(lv) ? (ID)FIX2LONG(lv) : SYM2ID(CHECK_SYMBOL(lv));
        }
    }

#define INT_PARAM(F) int_param(&ISEQ_BODY(iseq)->param.F, params, SYM(F))
    if (INT_PARAM(lead_num)) {
        ISEQ_BODY(iseq)->param.flags.has_lead = TRUE;
    }
    if (INT_PARAM(post_num)) ISEQ_BODY(iseq)->param.flags.has_post = TRUE;
    if (INT_PARAM(post_start)) ISEQ_BODY(iseq)->param.flags.has_post = TRUE;
    if (INT_PARAM(rest_start)) ISEQ_BODY(iseq)->param.flags.has_rest = TRUE;
    if (INT_PARAM(block_start)) ISEQ_BODY(iseq)->param.flags.has_block = TRUE;
#undef INT_PARAM
    {
#define INT_PARAM(F) F = (int_param(&x, misc, SYM(F)) ? (unsigned int)x : 0)
        int x;
        INT_PARAM(arg_size);
        INT_PARAM(local_size);
        INT_PARAM(stack_max);
#undef INT_PARAM
    }

    VALUE node_ids = Qfalse;
#ifdef USE_ISEQ_NODE_ID
    node_ids = rb_hash_aref(misc, ID2SYM(rb_intern("node_ids")));
    if (!RB_TYPE_P(node_ids, T_ARRAY)) {
        rb_raise(rb_eTypeError, "node_ids is not an array");
    }
#endif

    if (RB_TYPE_P(arg_opt_labels, T_ARRAY)) {
        len = RARRAY_LENINT(arg_opt_labels);
        ISEQ_BODY(iseq)->param.flags.has_opt = !!(len - 1 >= 0);

        if (ISEQ_BODY(iseq)->param.flags.has_opt) {
            VALUE *opt_table = ALLOC_N(VALUE, len);

            for (i = 0; i < len; i++) {
                VALUE ent = RARRAY_AREF(arg_opt_labels, i);
                LABEL *label = register_label(iseq, labels_table, ent);
                opt_table[i] = (VALUE)label;
            }

            ISEQ_BODY(iseq)->param.opt_num = len - 1;
            ISEQ_BODY(iseq)->param.opt_table = opt_table;
        }
    }
    else if (!NIL_P(arg_opt_labels)) {
        rb_raise(rb_eTypeError, ":opt param is not an array: %+"PRIsVALUE,
                 arg_opt_labels);
    }

    if (RB_TYPE_P(keywords, T_ARRAY)) {
        ISEQ_BODY(iseq)->param.keyword = iseq_build_kw(iseq, params, keywords);
    }
    else if (!NIL_P(keywords)) {
        rb_raise(rb_eTypeError, ":keywords param is not an array: %+"PRIsVALUE,
                 keywords);
    }

    if (Qtrue == rb_hash_aref(params, SYM(ambiguous_param0))) {
        ISEQ_BODY(iseq)->param.flags.ambiguous_param0 = TRUE;
    }

    if (Qtrue == rb_hash_aref(params, SYM(use_block))) {
        ISEQ_BODY(iseq)->param.flags.use_block = TRUE;
    }

    if (int_param(&i, params, SYM(kwrest))) {
        struct rb_iseq_param_keyword *keyword = (struct rb_iseq_param_keyword *)ISEQ_BODY(iseq)->param.keyword;
        if (keyword == NULL) {
            ISEQ_BODY(iseq)->param.keyword = keyword = ZALLOC(struct rb_iseq_param_keyword);
        }
        keyword->rest_start = i;
        ISEQ_BODY(iseq)->param.flags.has_kwrest = TRUE;
    }
#undef SYM
    iseq_calc_param_size(iseq);

    /* exception */
    iseq_build_from_ary_exception(iseq, labels_table, exception);

    /* body */
    iseq_build_from_ary_body(iseq, anchor, body, node_ids, labels_wrapper);

    ISEQ_BODY(iseq)->param.size = arg_size;
    ISEQ_BODY(iseq)->local_table_size = local_size;
    ISEQ_BODY(iseq)->stack_max = stack_max;
}

/* for parser */

int
rb_dvar_defined(ID id, const rb_iseq_t *iseq)
{
    if (iseq) {
        const struct rb_iseq_constant_body *body = ISEQ_BODY(iseq);
        while (body->type == ISEQ_TYPE_BLOCK ||
               body->type == ISEQ_TYPE_RESCUE ||
               body->type == ISEQ_TYPE_ENSURE ||
               body->type == ISEQ_TYPE_EVAL ||
               body->type == ISEQ_TYPE_MAIN
               ) {
            unsigned int i;

            for (i = 0; i < body->local_table_size; i++) {
                if (body->local_table[i] == id) {
                    return 1;
                }
            }
            iseq = body->parent_iseq;
            body = ISEQ_BODY(iseq);
        }
    }
    return 0;
}

int
rb_local_defined(ID id, const rb_iseq_t *iseq)
{
    if (iseq) {
        unsigned int i;
        const struct rb_iseq_constant_body *const body = ISEQ_BODY(ISEQ_BODY(iseq)->local_iseq);

        for (i=0; i<body->local_table_size; i++) {
            if (body->local_table[i] == id) {
                return 1;
            }
        }
    }
    return 0;
}

/* ISeq binary format */

#ifndef IBF_ISEQ_DEBUG
#define IBF_ISEQ_DEBUG 0
#endif

#ifndef IBF_ISEQ_ENABLE_LOCAL_BUFFER
#define IBF_ISEQ_ENABLE_LOCAL_BUFFER 0
#endif

typedef uint32_t ibf_offset_t;
#define IBF_OFFSET(ptr) ((ibf_offset_t)(VALUE)(ptr))

#define IBF_MAJOR_VERSION ISEQ_MAJOR_VERSION
#ifdef RUBY_DEVEL
#define IBF_DEVEL_VERSION 4
#define IBF_MINOR_VERSION (ISEQ_MINOR_VERSION * 10000 + IBF_DEVEL_VERSION)
#else
#define IBF_MINOR_VERSION ISEQ_MINOR_VERSION
#endif

static const char IBF_ENDIAN_MARK =
#ifdef WORDS_BIGENDIAN
    'b'
#else
    'l'
#endif
    ;

struct ibf_header {
    char magic[4]; /* YARB */
    uint32_t major_version;
    uint32_t minor_version;
    uint32_t size;
    uint32_t extra_size;

    uint32_t iseq_list_size;
    uint32_t global_object_list_size;
    ibf_offset_t iseq_list_offset;
    ibf_offset_t global_object_list_offset;
    uint8_t endian;
    uint8_t wordsize;           /* assume no 2048-bit CPU */
};

struct ibf_dump_buffer {
    VALUE str;
    st_table *obj_table; /* obj -> obj number */
};

struct ibf_dump {
    st_table *iseq_table; /* iseq -> iseq number */
    struct ibf_dump_buffer global_buffer;
    struct ibf_dump_buffer *current_buffer;
};

struct ibf_load_buffer {
    const char *buff;
    ibf_offset_t size;

    VALUE obj_list; /* [obj0, ...] */
    unsigned int obj_list_size;
    ibf_offset_t obj_list_offset;
};

struct ibf_load {
    const struct ibf_header *header;
    VALUE iseq_list;       /* [iseq0, ...] */
    struct ibf_load_buffer global_buffer;
    VALUE loader_obj;
    rb_iseq_t *iseq;
    VALUE str;
    struct ibf_load_buffer *current_buffer;
};

struct pinned_list {
    long size;
    VALUE buffer[1];
};

static void
pinned_list_mark(void *ptr)
{
    long i;
    struct pinned_list *list = (struct pinned_list *)ptr;
    for (i = 0; i < list->size; i++) {
        if (list->buffer[i]) {
            rb_gc_mark(list->buffer[i]);
        }
    }
}

static const rb_data_type_t pinned_list_type = {
    "pinned_list",
    {
        pinned_list_mark,
        RUBY_DEFAULT_FREE,
        NULL, // No external memory to report,
    },
    0, 0, RUBY_TYPED_WB_PROTECTED | RUBY_TYPED_FREE_IMMEDIATELY | RUBY_TYPED_EMBEDDABLE
};

static VALUE
pinned_list_fetch(VALUE list, long offset)
{
    struct pinned_list * ptr;

    TypedData_Get_Struct(list, struct pinned_list, &pinned_list_type, ptr);

    if (offset >= ptr->size) {
        rb_raise(rb_eIndexError, "object index out of range: %ld", offset);
    }

    return ptr->buffer[offset];
}

static void
pinned_list_store(VALUE list, long offset, VALUE object)
{
    struct pinned_list * ptr;

    TypedData_Get_Struct(list, struct pinned_list, &pinned_list_type, ptr);

    if (offset >= ptr->size) {
        rb_raise(rb_eIndexError, "object index out of range: %ld", offset);
    }

    RB_OBJ_WRITE(list, &ptr->buffer[offset], object);
}

static VALUE
pinned_list_new(long size)
{
    size_t memsize = offsetof(struct pinned_list, buffer) + size * sizeof(VALUE);
    VALUE obj_list = rb_data_typed_object_zalloc(0, memsize, &pinned_list_type);
    struct pinned_list * ptr = RTYPEDDATA_GET_DATA(obj_list);
    ptr->size = size;
    return obj_list;
}

static ibf_offset_t
ibf_dump_pos(struct ibf_dump *dump)
{
    long pos = RSTRING_LEN(dump->current_buffer->str);
#if SIZEOF_LONG > SIZEOF_INT
    if (pos >= UINT_MAX) {
        rb_raise(rb_eRuntimeError, "dump size exceeds");
    }
#endif
    return (unsigned int)pos;
}

static void
ibf_dump_align(struct ibf_dump *dump, size_t align)
{
    ibf_offset_t pos = ibf_dump_pos(dump);
    if (pos % align) {
        static const char padding[sizeof(VALUE)];
        size_t size = align - ((size_t)pos % align);
#if SIZEOF_LONG > SIZEOF_INT
        if (pos + size >= UINT_MAX) {
            rb_raise(rb_eRuntimeError, "dump size exceeds");
        }
#endif
        for (; size > sizeof(padding); size -= sizeof(padding)) {
            rb_str_cat(dump->current_buffer->str, padding, sizeof(padding));
        }
        rb_str_cat(dump->current_buffer->str, padding, size);
    }
}

static ibf_offset_t
ibf_dump_write(struct ibf_dump *dump, const void *buff, unsigned long size)
{
    ibf_offset_t pos = ibf_dump_pos(dump);
#if SIZEOF_LONG > SIZEOF_INT
    /* ensure the resulting dump does not exceed UINT_MAX */
    if (size >= UINT_MAX || pos + size >= UINT_MAX) {
        rb_raise(rb_eRuntimeError, "dump size exceeds");
    }
#endif
    rb_str_cat(dump->current_buffer->str, (const char *)buff, size);
    return pos;
}

static ibf_offset_t
ibf_dump_write_byte(struct ibf_dump *dump, unsigned char byte)
{
    return ibf_dump_write(dump, &byte, sizeof(unsigned char));
}

static void
ibf_dump_overwrite(struct ibf_dump *dump, void *buff, unsigned int size, long offset)
{
    VALUE str = dump->current_buffer->str;
    char *ptr = RSTRING_PTR(str);
    if ((unsigned long)(size + offset) > (unsigned long)RSTRING_LEN(str))
        rb_bug("ibf_dump_overwrite: overflow");
    memcpy(ptr + offset, buff, size);
}

static const void *
ibf_load_ptr(const struct ibf_load *load, ibf_offset_t *offset, int size)
{
    ibf_offset_t beg = *offset;
    *offset += size;
    return load->current_buffer->buff + beg;
}

static void *
ibf_load_alloc(const struct ibf_load *load, ibf_offset_t offset, size_t x, size_t y)
{
    void *buff = ruby_xmalloc2(x, y);
    size_t size = x * y;
    memcpy(buff, load->current_buffer->buff + offset, size);
    return buff;
}

#define IBF_W_ALIGN(type) (RUBY_ALIGNOF(type) > 1 ? ibf_dump_align(dump, RUBY_ALIGNOF(type)) : (void)0)

#define IBF_W(b, type, n) (IBF_W_ALIGN(type), (type *)(VALUE)IBF_WP(b, type, n))
#define IBF_WV(variable)   ibf_dump_write(dump, &(variable), sizeof(variable))
#define IBF_WP(b, type, n) ibf_dump_write(dump, (b), sizeof(type) * (n))
#define IBF_R(val, type, n) (type *)ibf_load_alloc(load, IBF_OFFSET(val), sizeof(type), (n))
#define IBF_ZERO(variable) memset(&(variable), 0, sizeof(variable))

static int
ibf_table_lookup(struct st_table *table, st_data_t key)
{
    st_data_t val;

    if (st_lookup(table, key, &val)) {
        return (int)val;
    }
    else {
        return -1;
    }
}

static int
ibf_table_find_or_insert(struct st_table *table, st_data_t key)
{
    int index = ibf_table_lookup(table, key);

    if (index < 0) { /* not found */
        index = (int)table->num_entries;
        st_insert(table, key, (st_data_t)index);
    }

    return index;
}

/* dump/load generic */

static void ibf_dump_object_list(struct ibf_dump *dump, ibf_offset_t *obj_list_offset, unsigned int *obj_list_size);

static VALUE ibf_load_object(const struct ibf_load *load, VALUE object_index);
static rb_iseq_t *ibf_load_iseq(const struct ibf_load *load, const rb_iseq_t *index_iseq);

static st_table *
ibf_dump_object_table_new(void)
{
    st_table *obj_table = st_init_numtable(); /* need free */
    st_insert(obj_table, (st_data_t)Qnil, (st_data_t)0); /* 0th is nil */

    return obj_table;
}

static VALUE
ibf_dump_object(struct ibf_dump *dump, VALUE obj)
{
    return ibf_table_find_or_insert(dump->current_buffer->obj_table, (st_data_t)obj);
}

static VALUE
ibf_dump_id(struct ibf_dump *dump, ID id)
{
    if (id == 0 || rb_id2name(id) == NULL) {
        return 0;
    }
    return ibf_dump_object(dump, rb_id2sym(id));
}

static ID
ibf_load_id(const struct ibf_load *load, const ID id_index)
{
    if (id_index == 0) {
        return 0;
    }
    VALUE sym = ibf_load_object(load, id_index);
    if (rb_integer_type_p(sym)) {
        /* Load hidden local variables as indexes */
        return NUM2ULONG(sym);
    }
    return rb_sym2id(sym);
}

/* dump/load: code */

static ibf_offset_t ibf_dump_iseq_each(struct ibf_dump *dump, const rb_iseq_t *iseq);

static int
ibf_dump_iseq(struct ibf_dump *dump, const rb_iseq_t *iseq)
{
    if (iseq == NULL) {
        return -1;
    }
    else {
        return ibf_table_find_or_insert(dump->iseq_table, (st_data_t)iseq);
    }
}

static unsigned char
ibf_load_byte(const struct ibf_load *load, ibf_offset_t *offset)
{
    if (*offset >= load->current_buffer->size) { rb_raise(rb_eRuntimeError, "invalid bytecode"); }
    return (unsigned char)load->current_buffer->buff[(*offset)++];
}

/*
 * Small uint serialization
 * 0x00000000_00000000 - 0x00000000_0000007f: 1byte | XXXX XXX1 |
 * 0x00000000_00000080 - 0x00000000_00003fff: 2byte | XXXX XX10 | XXXX XXXX |
 * 0x00000000_00004000 - 0x00000000_001fffff: 3byte | XXXX X100 | XXXX XXXX | XXXX XXXX |
 * 0x00000000_00020000 - 0x00000000_0fffffff: 4byte | XXXX 1000 | XXXX XXXX | XXXX XXXX | XXXX XXXX |
 * ...
 * 0x00010000_00000000 - 0x00ffffff_ffffffff: 8byte | 1000 0000 | XXXX XXXX | XXXX XXXX | XXXX XXXX | XXXX XXXX | XXXX XXXX | XXXX XXXX | XXXX XXXX |
 * 0x01000000_00000000 - 0xffffffff_ffffffff: 9byte | 0000 0000 | XXXX XXXX | XXXX XXXX | XXXX XXXX | XXXX XXXX | XXXX XXXX | XXXX XXXX | XXXX XXXX | XXXX XXXX |
 */
static void
ibf_dump_write_small_value(struct ibf_dump *dump, VALUE x)
{
    if (sizeof(VALUE) > 8 || CHAR_BIT != 8) {
        ibf_dump_write(dump, &x, sizeof(VALUE));
        return;
    }

    enum { max_byte_length = sizeof(VALUE) + 1 };

    unsigned char bytes[max_byte_length];
    ibf_offset_t n;

    for (n = 0; n < sizeof(VALUE) && (x >> (7 - n)); n++, x >>= 8) {
        bytes[max_byte_length - 1 - n] = (unsigned char)x;
    }

    x <<= 1;
    x |= 1;
    x <<= n;
    bytes[max_byte_length - 1 - n] = (unsigned char)x;
    n++;

    ibf_dump_write(dump, bytes + max_byte_length - n, n);
}

static VALUE
ibf_load_small_value(const struct ibf_load *load, ibf_offset_t *offset)
{
    if (sizeof(VALUE) > 8 || CHAR_BIT != 8) {
        union { char s[sizeof(VALUE)]; VALUE v; } x;

        memcpy(x.s, load->current_buffer->buff + *offset, sizeof(VALUE));
        *offset += sizeof(VALUE);

        return x.v;
    }

    enum { max_byte_length = sizeof(VALUE) + 1 };

    const unsigned char *buffer = (const unsigned char *)load->current_buffer->buff;
    const unsigned char c = buffer[*offset];

    ibf_offset_t n =
        c & 1 ? 1 :
        c == 0 ? 9 : ntz_int32(c) + 1;
    VALUE x = (VALUE)c >> n;

    if (*offset + n > load->current_buffer->size) {
        rb_raise(rb_eRuntimeError, "invalid byte sequence");
    }

    ibf_offset_t i;
    for (i = 1; i < n; i++) {
        x <<= 8;
        x |= (VALUE)buffer[*offset + i];
    }

    *offset += n;
    return x;
}

static void
ibf_dump_builtin(struct ibf_dump *dump, const struct rb_builtin_function *bf)
{
    // short: index
    // short: name.length
    // bytes: name
    // // omit argc (only verify with name)
    ibf_dump_write_small_value(dump, (VALUE)bf->index);

    size_t len = strlen(bf->name);
    ibf_dump_write_small_value(dump, (VALUE)len);
    ibf_dump_write(dump, bf->name, len);
}

static const struct rb_builtin_function *
ibf_load_builtin(const struct ibf_load *load, ibf_offset_t *offset)
{
    int i = (int)ibf_load_small_value(load, offset);
    int len = (int)ibf_load_small_value(load, offset);
    const char *name = (char *)ibf_load_ptr(load, offset, len);

    if (0) {
        fprintf(stderr, "%.*s!!\n", len, name);
    }

    const struct rb_builtin_function *table = GET_VM()->builtin_function_table;
    if (table == NULL) rb_raise(rb_eArgError, "builtin function table is not provided");
    if (strncmp(table[i].name, name, len) != 0) {
        rb_raise(rb_eArgError, "builtin function index (%d) mismatch (expect %s but %s)", i, name, table[i].name);
    }
    // fprintf(stderr, "load-builtin: name:%s(%d)\n", table[i].name, table[i].argc);

    return &table[i];
}

static ibf_offset_t
ibf_dump_code(struct ibf_dump *dump, const rb_iseq_t *iseq)
{
    const struct rb_iseq_constant_body *const body = ISEQ_BODY(iseq);
    const int iseq_size = body->iseq_size;
    int code_index;
    const VALUE *orig_code = rb_iseq_original_iseq(iseq);

    ibf_offset_t offset = ibf_dump_pos(dump);

    for (code_index=0; code_index<iseq_size;) {
        const VALUE insn = orig_code[code_index++];
        const char *types = insn_op_types(insn);
        int op_index;

        /* opcode */
        if (insn >= 0x100) { rb_raise(rb_eRuntimeError, "invalid instruction"); }
        ibf_dump_write_small_value(dump, insn);

        /* operands */
        for (op_index=0; types[op_index]; op_index++, code_index++) {
            VALUE op = orig_code[code_index];
            VALUE wv;

            switch (types[op_index]) {
              case TS_CDHASH:
              case TS_VALUE:
                wv = ibf_dump_object(dump, op);
                break;
              case TS_ISEQ:
                wv = (VALUE)ibf_dump_iseq(dump, (const rb_iseq_t *)op);
                break;
              case TS_IC:
                {
                    IC ic = (IC)op;
                    VALUE arr = idlist_to_array(ic->segments);
                    wv = ibf_dump_object(dump, arr);
                }
                break;
              case TS_ISE:
              case TS_IVC:
              case TS_ICVARC:
                {
                    union iseq_inline_storage_entry *is = (union iseq_inline_storage_entry *)op;
                    wv = is - ISEQ_IS_ENTRY_START(body, types[op_index]);
                }
                break;
              case TS_CALLDATA:
                {
                    goto skip_wv;
                }
              case TS_ID:
                wv = ibf_dump_id(dump, (ID)op);
                break;
              case TS_FUNCPTR:
                rb_raise(rb_eRuntimeError, "TS_FUNCPTR is not supported");
                goto skip_wv;
              case TS_BUILTIN:
                ibf_dump_builtin(dump, (const struct rb_builtin_function *)op);
                goto skip_wv;
              default:
                wv = op;
                break;
            }
            ibf_dump_write_small_value(dump, wv);
          skip_wv:;
        }
        RUBY_ASSERT(insn_len(insn) == op_index+1);
    }

    return offset;
}

static VALUE *
ibf_load_code(const struct ibf_load *load, rb_iseq_t *iseq, ibf_offset_t bytecode_offset, ibf_offset_t bytecode_size, unsigned int iseq_size)
{
    VALUE iseqv = (VALUE)iseq;
    unsigned int code_index;
    ibf_offset_t reading_pos = bytecode_offset;
    VALUE *code = ALLOC_N(VALUE, iseq_size);

    struct rb_iseq_constant_body *load_body = ISEQ_BODY(iseq);
    struct rb_call_data *cd_entries = load_body->call_data;
    int ic_index = 0;

    iseq_bits_t * mark_offset_bits;

    iseq_bits_t tmp[1] = {0};

    if (ISEQ_MBITS_BUFLEN(iseq_size) == 1) {
        mark_offset_bits = tmp;
    }
    else {
        mark_offset_bits = ZALLOC_N(iseq_bits_t, ISEQ_MBITS_BUFLEN(iseq_size));
    }
    bool needs_bitmap = false;

    for (code_index=0; code_index<iseq_size;) {
        /* opcode */
        const VALUE insn = code[code_index] = ibf_load_small_value(load, &reading_pos);
        const char *types = insn_op_types(insn);
        int op_index;

        code_index++;

        /* operands */
        for (op_index=0; types[op_index]; op_index++, code_index++) {
            const char operand_type = types[op_index];
            switch (operand_type) {
              case TS_VALUE:
                {
                    VALUE op = ibf_load_small_value(load, &reading_pos);
                    VALUE v = ibf_load_object(load, op);
                    code[code_index] = v;
                    if (!SPECIAL_CONST_P(v)) {
                        RB_OBJ_WRITTEN(iseqv, Qundef, v);
                        ISEQ_MBITS_SET(mark_offset_bits, code_index);
                        needs_bitmap = true;
                    }
                    break;
                }
              case TS_CDHASH:
                {
                    VALUE op = ibf_load_small_value(load, &reading_pos);
                    VALUE v = ibf_load_object(load, op);
                    v = rb_hash_dup(v); // hash dumped as frozen
                    RHASH_TBL_RAW(v)->type = &cdhash_type;
                    rb_hash_rehash(v); // hash function changed
                    freeze_hide_obj(v);

                    // Overwrite the existing hash in the object list.  This
                    // is to keep the object alive during load time.
                    // [Bug #17984] [ruby-core:104259]
                    pinned_list_store(load->current_buffer->obj_list, (long)op, v);

                    code[code_index] = v;
                    ISEQ_MBITS_SET(mark_offset_bits, code_index);
                    RB_OBJ_WRITTEN(iseqv, Qundef, v);
                    needs_bitmap = true;
                    break;
                }
              case TS_ISEQ:
                {
                    VALUE op = (VALUE)ibf_load_small_value(load, &reading_pos);
                    VALUE v = (VALUE)ibf_load_iseq(load, (const rb_iseq_t *)op);
                    code[code_index] = v;
                    if (!SPECIAL_CONST_P(v)) {
                        RB_OBJ_WRITTEN(iseqv, Qundef, v);
                        ISEQ_MBITS_SET(mark_offset_bits, code_index);
                        needs_bitmap = true;
                    }
                    break;
                }
              case TS_IC:
                {
                    VALUE op = ibf_load_small_value(load, &reading_pos);
                    VALUE arr = ibf_load_object(load, op);

                    IC ic = &ISEQ_IS_IC_ENTRY(load_body, ic_index++);
                    ic->segments = array_to_idlist(arr);

                    code[code_index] = (VALUE)ic;
                }
                break;
              case TS_ISE:
              case TS_ICVARC:
              case TS_IVC:
                {
                    unsigned int op = (unsigned int)ibf_load_small_value(load, &reading_pos);

                    ISE ic = ISEQ_IS_ENTRY_START(load_body, operand_type) + op;
                    code[code_index] = (VALUE)ic;

                    if (operand_type == TS_IVC) {
                        IVC cache = (IVC)ic;

                        if (insn == BIN(setinstancevariable)) {
                            ID iv_name = (ID)code[code_index - 1];
                            cache->iv_set_name = iv_name;
                        }
                        else {
                            cache->iv_set_name = 0;
                        }

                        vm_ic_attr_index_initialize(cache, INVALID_SHAPE_ID);
                    }

                }
                break;
              case TS_CALLDATA:
                {
                    code[code_index] = (VALUE)cd_entries++;
                }
                break;
              case TS_ID:
                {
                    VALUE op = ibf_load_small_value(load, &reading_pos);
                    code[code_index] = ibf_load_id(load, (ID)(VALUE)op);
                }
                break;
              case TS_FUNCPTR:
                rb_raise(rb_eRuntimeError, "TS_FUNCPTR is not supported");
                break;
              case TS_BUILTIN:
                code[code_index] = (VALUE)ibf_load_builtin(load, &reading_pos);
                break;
              default:
                code[code_index] = ibf_load_small_value(load, &reading_pos);
                continue;
            }
        }
        if (insn_len(insn) != op_index+1) {
            rb_raise(rb_eRuntimeError, "operand size mismatch");
        }
    }

    load_body->iseq_encoded = code;
    load_body->iseq_size = code_index;

    if (ISEQ_MBITS_BUFLEN(load_body->iseq_size) == 1) {
        load_body->mark_bits.single = mark_offset_bits[0];
    }
    else {
        if (needs_bitmap) {
            load_body->mark_bits.list = mark_offset_bits;
        }
        else {
            load_body->mark_bits.list = 0;
            ruby_xfree(mark_offset_bits);
        }
    }

    RUBY_ASSERT(code_index == iseq_size);
    RUBY_ASSERT(reading_pos == bytecode_offset + bytecode_size);
    return code;
}

static ibf_offset_t
ibf_dump_param_opt_table(struct ibf_dump *dump, const rb_iseq_t *iseq)
{
    int opt_num = ISEQ_BODY(iseq)->param.opt_num;

    if (opt_num > 0) {
        IBF_W_ALIGN(VALUE);
        return ibf_dump_write(dump, ISEQ_BODY(iseq)->param.opt_table, sizeof(VALUE) * (opt_num + 1));
    }
    else {
        return ibf_dump_pos(dump);
    }
}

static VALUE *
ibf_load_param_opt_table(const struct ibf_load *load, ibf_offset_t opt_table_offset, int opt_num)
{
    if (opt_num > 0) {
        VALUE *table = ALLOC_N(VALUE, opt_num+1);
        MEMCPY(table, load->current_buffer->buff + opt_table_offset, VALUE, opt_num+1);
        return table;
    }
    else {
        return NULL;
    }
}

static ibf_offset_t
ibf_dump_param_keyword(struct ibf_dump *dump, const rb_iseq_t *iseq)
{
    const struct rb_iseq_param_keyword *kw = ISEQ_BODY(iseq)->param.keyword;

    if (kw) {
        struct rb_iseq_param_keyword dump_kw = *kw;
        int dv_num = kw->num - kw->required_num;
        ID *ids = kw->num > 0 ? ALLOCA_N(ID, kw->num) : NULL;
        VALUE *dvs = dv_num > 0 ? ALLOCA_N(VALUE, dv_num) : NULL;
        int i;

        for (i=0; i<kw->num; i++) ids[i] = (ID)ibf_dump_id(dump, kw->table[i]);
        for (i=0; i<dv_num; i++) dvs[i] = (VALUE)ibf_dump_object(dump, kw->default_values[i]);

        dump_kw.table = IBF_W(ids, ID, kw->num);
        dump_kw.default_values = IBF_W(dvs, VALUE, dv_num);
        IBF_W_ALIGN(struct rb_iseq_param_keyword);
        return ibf_dump_write(dump, &dump_kw, sizeof(struct rb_iseq_param_keyword) * 1);
    }
    else {
        return 0;
    }
}

static const struct rb_iseq_param_keyword *
ibf_load_param_keyword(const struct ibf_load *load, ibf_offset_t param_keyword_offset)
{
    if (param_keyword_offset) {
        struct rb_iseq_param_keyword *kw = IBF_R(param_keyword_offset, struct rb_iseq_param_keyword, 1);
        int dv_num = kw->num - kw->required_num;
        VALUE *dvs = dv_num ? IBF_R(kw->default_values, VALUE, dv_num) : NULL;

        int i;
        for (i=0; i<dv_num; i++) {
            dvs[i] = ibf_load_object(load, dvs[i]);
        }

        // Will be set once the local table is loaded.
        kw->table = NULL;

        kw->default_values = dvs;
        return kw;
    }
    else {
        return NULL;
    }
}

static ibf_offset_t
ibf_dump_insns_info_body(struct ibf_dump *dump, const rb_iseq_t *iseq)
{
    ibf_offset_t offset = ibf_dump_pos(dump);
    const struct iseq_insn_info_entry *entries = ISEQ_BODY(iseq)->insns_info.body;

    unsigned int i;
    for (i = 0; i < ISEQ_BODY(iseq)->insns_info.size; i++) {
        ibf_dump_write_small_value(dump, entries[i].line_no);
#ifdef USE_ISEQ_NODE_ID
        ibf_dump_write_small_value(dump, entries[i].node_id);
#endif
        ibf_dump_write_small_value(dump, entries[i].events);
    }

    return offset;
}

static struct iseq_insn_info_entry *
ibf_load_insns_info_body(const struct ibf_load *load, ibf_offset_t body_offset, unsigned int size)
{
    ibf_offset_t reading_pos = body_offset;
    struct iseq_insn_info_entry *entries = ALLOC_N(struct iseq_insn_info_entry, size);

    unsigned int i;
    for (i = 0; i < size; i++) {
        entries[i].line_no = (int)ibf_load_small_value(load, &reading_pos);
#ifdef USE_ISEQ_NODE_ID
        entries[i].node_id = (int)ibf_load_small_value(load, &reading_pos);
#endif
        entries[i].events = (rb_event_flag_t)ibf_load_small_value(load, &reading_pos);
    }

    return entries;
}

static ibf_offset_t
ibf_dump_insns_info_positions(struct ibf_dump *dump, const unsigned int *positions, unsigned int size)
{
    ibf_offset_t offset = ibf_dump_pos(dump);

    unsigned int last = 0;
    unsigned int i;
    for (i = 0; i < size; i++) {
        ibf_dump_write_small_value(dump, positions[i] - last);
        last = positions[i];
    }

    return offset;
}

static unsigned int *
ibf_load_insns_info_positions(const struct ibf_load *load, ibf_offset_t positions_offset, unsigned int size)
{
    ibf_offset_t reading_pos = positions_offset;
    unsigned int *positions = ALLOC_N(unsigned int, size);

    unsigned int last = 0;
    unsigned int i;
    for (i = 0; i < size; i++) {
        positions[i] = last + (unsigned int)ibf_load_small_value(load, &reading_pos);
        last = positions[i];
    }

    return positions;
}

static ibf_offset_t
ibf_dump_local_table(struct ibf_dump *dump, const rb_iseq_t *iseq)
{
    const struct rb_iseq_constant_body *const body = ISEQ_BODY(iseq);
    const int size = body->local_table_size;
    ID *table = ALLOCA_N(ID, size);
    int i;

    for (i=0; i<size; i++) {
        VALUE v = ibf_dump_id(dump, body->local_table[i]);
        if (v == 0) {
            /* Dump hidden local variables as indexes, so load_from_binary will work with them */
            v = ibf_dump_object(dump, ULONG2NUM(body->local_table[i]));
        }
        table[i] = v;
    }

    IBF_W_ALIGN(ID);
    return ibf_dump_write(dump, table, sizeof(ID) * size);
}

static ID *
ibf_load_local_table(const struct ibf_load *load, ibf_offset_t local_table_offset, int size)
{
    if (size > 0) {
        ID *table = IBF_R(local_table_offset, ID, size);
        int i;

        for (i=0; i<size; i++) {
            table[i] = ibf_load_id(load, table[i]);
        }
        return table;
    }
    else {
        return NULL;
    }
}

static ibf_offset_t
ibf_dump_catch_table(struct ibf_dump *dump, const rb_iseq_t *iseq)
{
    const struct iseq_catch_table *table = ISEQ_BODY(iseq)->catch_table;

    if (table) {
        int *iseq_indices = ALLOCA_N(int, table->size);
        unsigned int i;

        for (i=0; i<table->size; i++) {
            iseq_indices[i] = ibf_dump_iseq(dump, table->entries[i].iseq);
        }

        const ibf_offset_t offset = ibf_dump_pos(dump);

        for (i=0; i<table->size; i++) {
            ibf_dump_write_small_value(dump, iseq_indices[i]);
            ibf_dump_write_small_value(dump, table->entries[i].type);
            ibf_dump_write_small_value(dump, table->entries[i].start);
            ibf_dump_write_small_value(dump, table->entries[i].end);
            ibf_dump_write_small_value(dump, table->entries[i].cont);
            ibf_dump_write_small_value(dump, table->entries[i].sp);
        }
        return offset;
    }
    else {
        return ibf_dump_pos(dump);
    }
}

static void
ibf_load_catch_table(const struct ibf_load *load, ibf_offset_t catch_table_offset, unsigned int size, const rb_iseq_t *parent_iseq)
{
    if (size) {
        struct iseq_catch_table *table = ruby_xcalloc(1, iseq_catch_table_bytes(size));
        table->size = size;
        ISEQ_BODY(parent_iseq)->catch_table = table;

        ibf_offset_t reading_pos = catch_table_offset;

        unsigned int i;
        for (i=0; i<table->size; i++) {
            int iseq_index = (int)ibf_load_small_value(load, &reading_pos);
            table->entries[i].type = (enum rb_catch_type)ibf_load_small_value(load, &reading_pos);
            table->entries[i].start = (unsigned int)ibf_load_small_value(load, &reading_pos);
            table->entries[i].end = (unsigned int)ibf_load_small_value(load, &reading_pos);
            table->entries[i].cont = (unsigned int)ibf_load_small_value(load, &reading_pos);
            table->entries[i].sp = (unsigned int)ibf_load_small_value(load, &reading_pos);

            rb_iseq_t *catch_iseq = (rb_iseq_t *)ibf_load_iseq(load, (const rb_iseq_t *)(VALUE)iseq_index);
            RB_OBJ_WRITE(parent_iseq, UNALIGNED_MEMBER_PTR(&table->entries[i], iseq), catch_iseq);
        }
    }
    else {
        ISEQ_BODY(parent_iseq)->catch_table = NULL;
    }
}

static ibf_offset_t
ibf_dump_ci_entries(struct ibf_dump *dump, const rb_iseq_t *iseq)
{
    const struct rb_iseq_constant_body *const body = ISEQ_BODY(iseq);
    const unsigned int ci_size = body->ci_size;
    const struct rb_call_data *cds = body->call_data;

    ibf_offset_t offset = ibf_dump_pos(dump);

    unsigned int i;

    for (i = 0; i < ci_size; i++) {
        const struct rb_callinfo *ci = cds[i].ci;
        if (ci != NULL) {
            ibf_dump_write_small_value(dump, ibf_dump_id(dump, vm_ci_mid(ci)));
            ibf_dump_write_small_value(dump, vm_ci_flag(ci));
            ibf_dump_write_small_value(dump, vm_ci_argc(ci));

            const struct rb_callinfo_kwarg *kwarg = vm_ci_kwarg(ci);
            if (kwarg) {
                int len = kwarg->keyword_len;
                ibf_dump_write_small_value(dump, len);
                for (int j=0; j<len; j++) {
                    VALUE keyword = ibf_dump_object(dump, kwarg->keywords[j]);
                    ibf_dump_write_small_value(dump, keyword);
                }
            }
            else {
                ibf_dump_write_small_value(dump, 0);
            }
        }
        else {
            // TODO: truncate NULL ci from call_data.
            ibf_dump_write_small_value(dump, (VALUE)-1);
        }
    }

    return offset;
}

struct outer_variable_pair {
    ID id;
    VALUE name;
    VALUE val;
};

struct outer_variable_list {
    size_t num;
    struct outer_variable_pair pairs[1];
};

static enum rb_id_table_iterator_result
store_outer_variable(ID id, VALUE val, void *dump)
{
    struct outer_variable_list *ovlist = dump;
    struct outer_variable_pair *pair = &ovlist->pairs[ovlist->num++];
    pair->id = id;
    pair->name = rb_id2str(id);
    pair->val = val;
    return ID_TABLE_CONTINUE;
}

static int
outer_variable_cmp(const void *a, const void *b, void *arg)
{
    const struct outer_variable_pair *ap = (const struct outer_variable_pair *)a;
    const struct outer_variable_pair *bp = (const struct outer_variable_pair *)b;

    if (!ap->name) {
        return -1;
    }
    else if (!bp->name) {
        return 1;
    }

    return rb_str_cmp(ap->name, bp->name);
}

static ibf_offset_t
ibf_dump_outer_variables(struct ibf_dump *dump, const rb_iseq_t *iseq)
{
    struct rb_id_table * ovs = ISEQ_BODY(iseq)->outer_variables;

    ibf_offset_t offset = ibf_dump_pos(dump);

    size_t size = ovs ? rb_id_table_size(ovs) : 0;
    ibf_dump_write_small_value(dump, (VALUE)size);
    if (size > 0) {
        VALUE buff;
        size_t buffsize =
            rb_size_mul_add_or_raise(sizeof(struct outer_variable_pair), size,
                                     offsetof(struct outer_variable_list, pairs),
                                     rb_eArgError);
        struct outer_variable_list *ovlist = RB_ALLOCV(buff, buffsize);
        ovlist->num = 0;
        rb_id_table_foreach(ovs, store_outer_variable, ovlist);
        ruby_qsort(ovlist->pairs, size, sizeof(struct outer_variable_pair), outer_variable_cmp, NULL);
        for (size_t i = 0; i < size; ++i) {
            ID id = ovlist->pairs[i].id;
            ID val = ovlist->pairs[i].val;
            ibf_dump_write_small_value(dump, ibf_dump_id(dump, id));
            ibf_dump_write_small_value(dump, val);
        }
    }

    return offset;
}

/* note that we dump out rb_call_info but load back rb_call_data */
static void
ibf_load_ci_entries(const struct ibf_load *load,
                    ibf_offset_t ci_entries_offset,
                    unsigned int ci_size,
                    struct rb_call_data **cd_ptr)
{
    if (!ci_size) {
        *cd_ptr = NULL;
        return;
    }

    ibf_offset_t reading_pos = ci_entries_offset;

    unsigned int i;

    struct rb_call_data *cds = ZALLOC_N(struct rb_call_data, ci_size);
    *cd_ptr = cds;

    for (i = 0; i < ci_size; i++) {
        VALUE mid_index = ibf_load_small_value(load, &reading_pos);
        if (mid_index != (VALUE)-1) {
            ID mid = ibf_load_id(load, mid_index);
            unsigned int flag = (unsigned int)ibf_load_small_value(load, &reading_pos);
            unsigned int argc = (unsigned int)ibf_load_small_value(load, &reading_pos);

            struct rb_callinfo_kwarg *kwarg = NULL;
            int kwlen = (int)ibf_load_small_value(load, &reading_pos);
            if (kwlen > 0) {
                kwarg = rb_xmalloc_mul_add(kwlen, sizeof(VALUE), sizeof(struct rb_callinfo_kwarg));
                kwarg->references = 0;
                kwarg->keyword_len = kwlen;
                for (int j=0; j<kwlen; j++) {
                    VALUE keyword = ibf_load_small_value(load, &reading_pos);
                    kwarg->keywords[j] = ibf_load_object(load, keyword);
                }
            }

            cds[i].ci = vm_ci_new(mid, flag, argc, kwarg);
            RB_OBJ_WRITTEN(load->iseq, Qundef, cds[i].ci);
            cds[i].cc = vm_cc_empty();
        }
        else {
            // NULL ci
            cds[i].ci = NULL;
            cds[i].cc = NULL;
        }
    }
}

static struct rb_id_table *
ibf_load_outer_variables(const struct ibf_load * load, ibf_offset_t outer_variables_offset)
{
    ibf_offset_t reading_pos = outer_variables_offset;

    struct rb_id_table *tbl = NULL;

    size_t table_size = (size_t)ibf_load_small_value(load, &reading_pos);

    if (table_size > 0) {
        tbl = rb_id_table_create(table_size);
    }

    for (size_t i = 0; i < table_size; i++) {
        ID key = ibf_load_id(load, (ID)ibf_load_small_value(load, &reading_pos));
        VALUE value = ibf_load_small_value(load, &reading_pos);
        if (!key) key = rb_make_temporary_id(i);
        rb_id_table_insert(tbl, key, value);
    }

    return tbl;
}

static ibf_offset_t
ibf_dump_iseq_each(struct ibf_dump *dump, const rb_iseq_t *iseq)
{
    RUBY_ASSERT(dump->current_buffer == &dump->global_buffer);

    unsigned int *positions;

    const struct rb_iseq_constant_body *body = ISEQ_BODY(iseq);

    const VALUE location_pathobj_index = ibf_dump_object(dump, body->location.pathobj); /* TODO: freeze */
    const VALUE location_base_label_index = ibf_dump_object(dump, body->location.base_label);
    const VALUE location_label_index = ibf_dump_object(dump, body->location.label);

#if IBF_ISEQ_ENABLE_LOCAL_BUFFER
    ibf_offset_t iseq_start = ibf_dump_pos(dump);

    struct ibf_dump_buffer *saved_buffer = dump->current_buffer;
    struct ibf_dump_buffer buffer;
    buffer.str = rb_str_new(0, 0);
    buffer.obj_table = ibf_dump_object_table_new();
    dump->current_buffer = &buffer;
#endif

    const ibf_offset_t bytecode_offset =        ibf_dump_code(dump, iseq);
    const ibf_offset_t bytecode_size =          ibf_dump_pos(dump) - bytecode_offset;
    const ibf_offset_t param_opt_table_offset = ibf_dump_param_opt_table(dump, iseq);
    const ibf_offset_t param_keyword_offset =   ibf_dump_param_keyword(dump, iseq);
    const ibf_offset_t insns_info_body_offset = ibf_dump_insns_info_body(dump, iseq);

    positions = rb_iseq_insns_info_decode_positions(ISEQ_BODY(iseq));
    const ibf_offset_t insns_info_positions_offset = ibf_dump_insns_info_positions(dump, positions, body->insns_info.size);
    ruby_xfree(positions);

    const ibf_offset_t local_table_offset = ibf_dump_local_table(dump, iseq);
    const unsigned int catch_table_size =   body->catch_table ? body->catch_table->size : 0;
    const ibf_offset_t catch_table_offset = ibf_dump_catch_table(dump, iseq);
    const int parent_iseq_index =           ibf_dump_iseq(dump, ISEQ_BODY(iseq)->parent_iseq);
    const int local_iseq_index =            ibf_dump_iseq(dump, ISEQ_BODY(iseq)->local_iseq);
    const int mandatory_only_iseq_index =   ibf_dump_iseq(dump, ISEQ_BODY(iseq)->mandatory_only_iseq);
    const ibf_offset_t ci_entries_offset =  ibf_dump_ci_entries(dump, iseq);
    const ibf_offset_t outer_variables_offset = ibf_dump_outer_variables(dump, iseq);

#if IBF_ISEQ_ENABLE_LOCAL_BUFFER
    ibf_offset_t local_obj_list_offset;
    unsigned int local_obj_list_size;

    ibf_dump_object_list(dump, &local_obj_list_offset, &local_obj_list_size);
#endif

    ibf_offset_t body_offset = ibf_dump_pos(dump);

    /* dump the constant body */
    unsigned int param_flags =
        (body->param.flags.has_lead         << 0) |
        (body->param.flags.has_opt          << 1) |
        (body->param.flags.has_rest         << 2) |
        (body->param.flags.has_post         << 3) |
        (body->param.flags.has_kw           << 4) |
        (body->param.flags.has_kwrest       << 5) |
        (body->param.flags.has_block        << 6) |
        (body->param.flags.ambiguous_param0 << 7) |
        (body->param.flags.accepts_no_kwarg << 8) |
        (body->param.flags.ruby2_keywords   << 9) |
        (body->param.flags.anon_rest        << 10) |
        (body->param.flags.anon_kwrest      << 11) |
        (body->param.flags.use_block        << 12) |
        (body->param.flags.forwardable      << 13) ;

#if IBF_ISEQ_ENABLE_LOCAL_BUFFER
#  define IBF_BODY_OFFSET(x) (x)
#else
#  define IBF_BODY_OFFSET(x) (body_offset - (x))
#endif

    ibf_dump_write_small_value(dump, body->type);
    ibf_dump_write_small_value(dump, body->iseq_size);
    ibf_dump_write_small_value(dump, IBF_BODY_OFFSET(bytecode_offset));
    ibf_dump_write_small_value(dump, bytecode_size);
    ibf_dump_write_small_value(dump, param_flags);
    ibf_dump_write_small_value(dump, body->param.size);
    ibf_dump_write_small_value(dump, body->param.lead_num);
    ibf_dump_write_small_value(dump, body->param.opt_num);
    ibf_dump_write_small_value(dump, body->param.rest_start);
    ibf_dump_write_small_value(dump, body->param.post_start);
    ibf_dump_write_small_value(dump, body->param.post_num);
    ibf_dump_write_small_value(dump, body->param.block_start);
    ibf_dump_write_small_value(dump, IBF_BODY_OFFSET(param_opt_table_offset));
    ibf_dump_write_small_value(dump, param_keyword_offset);
    ibf_dump_write_small_value(dump, location_pathobj_index);
    ibf_dump_write_small_value(dump, location_base_label_index);
    ibf_dump_write_small_value(dump, location_label_index);
    ibf_dump_write_small_value(dump, body->location.first_lineno);
    ibf_dump_write_small_value(dump, body->location.node_id);
    ibf_dump_write_small_value(dump, body->location.code_location.beg_pos.lineno);
    ibf_dump_write_small_value(dump, body->location.code_location.beg_pos.column);
    ibf_dump_write_small_value(dump, body->location.code_location.end_pos.lineno);
    ibf_dump_write_small_value(dump, body->location.code_location.end_pos.column);
    ibf_dump_write_small_value(dump, IBF_BODY_OFFSET(insns_info_body_offset));
    ibf_dump_write_small_value(dump, IBF_BODY_OFFSET(insns_info_positions_offset));
    ibf_dump_write_small_value(dump, body->insns_info.size);
    ibf_dump_write_small_value(dump, IBF_BODY_OFFSET(local_table_offset));
    ibf_dump_write_small_value(dump, catch_table_size);
    ibf_dump_write_small_value(dump, IBF_BODY_OFFSET(catch_table_offset));
    ibf_dump_write_small_value(dump, parent_iseq_index);
    ibf_dump_write_small_value(dump, local_iseq_index);
    ibf_dump_write_small_value(dump, mandatory_only_iseq_index);
    ibf_dump_write_small_value(dump, IBF_BODY_OFFSET(ci_entries_offset));
    ibf_dump_write_small_value(dump, IBF_BODY_OFFSET(outer_variables_offset));
    ibf_dump_write_small_value(dump, body->variable.flip_count);
    ibf_dump_write_small_value(dump, body->local_table_size);
    ibf_dump_write_small_value(dump, body->ivc_size);
    ibf_dump_write_small_value(dump, body->icvarc_size);
    ibf_dump_write_small_value(dump, body->ise_size);
    ibf_dump_write_small_value(dump, body->ic_size);
    ibf_dump_write_small_value(dump, body->ci_size);
    ibf_dump_write_small_value(dump, body->stack_max);
    ibf_dump_write_small_value(dump, body->builtin_attrs);
    ibf_dump_write_small_value(dump, body->prism ? 1 : 0);

#undef IBF_BODY_OFFSET

#if IBF_ISEQ_ENABLE_LOCAL_BUFFER
    ibf_offset_t iseq_length_bytes = ibf_dump_pos(dump);

    dump->current_buffer = saved_buffer;
    ibf_dump_write(dump, RSTRING_PTR(buffer.str), iseq_length_bytes);

    ibf_offset_t offset = ibf_dump_pos(dump);
    ibf_dump_write_small_value(dump, iseq_start);
    ibf_dump_write_small_value(dump, iseq_length_bytes);
    ibf_dump_write_small_value(dump, body_offset);

    ibf_dump_write_small_value(dump, local_obj_list_offset);
    ibf_dump_write_small_value(dump, local_obj_list_size);

    st_free_table(buffer.obj_table); // TODO: this leaks in case of exception

    return offset;
#else
    return body_offset;
#endif
}

static VALUE
ibf_load_location_str(const struct ibf_load *load, VALUE str_index)
{
    VALUE str = ibf_load_object(load, str_index);
    if (str != Qnil) {
        str = rb_fstring(str);
    }
    return str;
}

static void
ibf_load_iseq_each(struct ibf_load *load, rb_iseq_t *iseq, ibf_offset_t offset)
{
    struct rb_iseq_constant_body *load_body = ISEQ_BODY(iseq) = rb_iseq_constant_body_alloc();

    ibf_offset_t reading_pos = offset;

#if IBF_ISEQ_ENABLE_LOCAL_BUFFER
    struct ibf_load_buffer *saved_buffer = load->current_buffer;
    load->current_buffer = &load->global_buffer;

    const ibf_offset_t iseq_start = (ibf_offset_t)ibf_load_small_value(load, &reading_pos);
    const ibf_offset_t iseq_length_bytes = (ibf_offset_t)ibf_load_small_value(load, &reading_pos);
    const ibf_offset_t body_offset = (ibf_offset_t)ibf_load_small_value(load, &reading_pos);

    struct ibf_load_buffer buffer;
    buffer.buff = load->global_buffer.buff + iseq_start;
    buffer.size = iseq_length_bytes;
    buffer.obj_list_offset = (ibf_offset_t)ibf_load_small_value(load, &reading_pos);
    buffer.obj_list_size = (ibf_offset_t)ibf_load_small_value(load, &reading_pos);
    buffer.obj_list = pinned_list_new(buffer.obj_list_size);

    load->current_buffer = &buffer;
    reading_pos = body_offset;
#endif

#if IBF_ISEQ_ENABLE_LOCAL_BUFFER
#  define IBF_BODY_OFFSET(x) (x)
#else
#  define IBF_BODY_OFFSET(x) (offset - (x))
#endif

    const unsigned int type = (unsigned int)ibf_load_small_value(load, &reading_pos);
    const unsigned int iseq_size = (unsigned int)ibf_load_small_value(load, &reading_pos);
    const ibf_offset_t bytecode_offset = (ibf_offset_t)IBF_BODY_OFFSET(ibf_load_small_value(load, &reading_pos));
    const ibf_offset_t bytecode_size = (ibf_offset_t)ibf_load_small_value(load, &reading_pos);
    const unsigned int param_flags = (unsigned int)ibf_load_small_value(load, &reading_pos);
    const unsigned int param_size = (unsigned int)ibf_load_small_value(load, &reading_pos);
    const int param_lead_num = (int)ibf_load_small_value(load, &reading_pos);
    const int param_opt_num = (int)ibf_load_small_value(load, &reading_pos);
    const int param_rest_start = (int)ibf_load_small_value(load, &reading_pos);
    const int param_post_start = (int)ibf_load_small_value(load, &reading_pos);
    const int param_post_num = (int)ibf_load_small_value(load, &reading_pos);
    const int param_block_start = (int)ibf_load_small_value(load, &reading_pos);
    const ibf_offset_t param_opt_table_offset = (ibf_offset_t)IBF_BODY_OFFSET(ibf_load_small_value(load, &reading_pos));
    const ibf_offset_t param_keyword_offset = (ibf_offset_t)ibf_load_small_value(load, &reading_pos);
    const VALUE location_pathobj_index = ibf_load_small_value(load, &reading_pos);
    const VALUE location_base_label_index = ibf_load_small_value(load, &reading_pos);
    const VALUE location_label_index = ibf_load_small_value(load, &reading_pos);
    const int location_first_lineno = (int)ibf_load_small_value(load, &reading_pos);
    const int location_node_id = (int)ibf_load_small_value(load, &reading_pos);
    const int location_code_location_beg_pos_lineno = (int)ibf_load_small_value(load, &reading_pos);
    const int location_code_location_beg_pos_column = (int)ibf_load_small_value(load, &reading_pos);
    const int location_code_location_end_pos_lineno = (int)ibf_load_small_value(load, &reading_pos);
    const int location_code_location_end_pos_column = (int)ibf_load_small_value(load, &reading_pos);
    const ibf_offset_t insns_info_body_offset = (ibf_offset_t)IBF_BODY_OFFSET(ibf_load_small_value(load, &reading_pos));
    const ibf_offset_t insns_info_positions_offset = (ibf_offset_t)IBF_BODY_OFFSET(ibf_load_small_value(load, &reading_pos));
    const unsigned int insns_info_size = (unsigned int)ibf_load_small_value(load, &reading_pos);
    const ibf_offset_t local_table_offset = (ibf_offset_t)IBF_BODY_OFFSET(ibf_load_small_value(load, &reading_pos));
    const unsigned int catch_table_size = (unsigned int)ibf_load_small_value(load, &reading_pos);
    const ibf_offset_t catch_table_offset = (ibf_offset_t)IBF_BODY_OFFSET(ibf_load_small_value(load, &reading_pos));
    const int parent_iseq_index = (int)ibf_load_small_value(load, &reading_pos);
    const int local_iseq_index = (int)ibf_load_small_value(load, &reading_pos);
    const int mandatory_only_iseq_index = (int)ibf_load_small_value(load, &reading_pos);
    const ibf_offset_t ci_entries_offset = (ibf_offset_t)IBF_BODY_OFFSET(ibf_load_small_value(load, &reading_pos));
    const ibf_offset_t outer_variables_offset = (ibf_offset_t)IBF_BODY_OFFSET(ibf_load_small_value(load, &reading_pos));
    const rb_snum_t variable_flip_count = (rb_snum_t)ibf_load_small_value(load, &reading_pos);
    const unsigned int local_table_size = (unsigned int)ibf_load_small_value(load, &reading_pos);

    const unsigned int ivc_size = (unsigned int)ibf_load_small_value(load, &reading_pos);
    const unsigned int icvarc_size = (unsigned int)ibf_load_small_value(load, &reading_pos);
    const unsigned int ise_size = (unsigned int)ibf_load_small_value(load, &reading_pos);
    const unsigned int ic_size = (unsigned int)ibf_load_small_value(load, &reading_pos);

    const unsigned int ci_size = (unsigned int)ibf_load_small_value(load, &reading_pos);
    const unsigned int stack_max = (unsigned int)ibf_load_small_value(load, &reading_pos);
    const unsigned int builtin_attrs = (unsigned int)ibf_load_small_value(load, &reading_pos);
    const bool prism = (bool)ibf_load_small_value(load, &reading_pos);

    // setup fname and dummy frame
    VALUE path = ibf_load_object(load, location_pathobj_index);
    {
        VALUE realpath = Qnil;

        if (RB_TYPE_P(path, T_STRING)) {
            realpath = path = rb_fstring(path);
        }
        else if (RB_TYPE_P(path, T_ARRAY)) {
            VALUE pathobj = path;
            if (RARRAY_LEN(pathobj) != 2) {
                rb_raise(rb_eRuntimeError, "path object size mismatch");
            }
            path = rb_fstring(RARRAY_AREF(pathobj, 0));
            realpath = RARRAY_AREF(pathobj, 1);
            if (!NIL_P(realpath)) {
                if (!RB_TYPE_P(realpath, T_STRING)) {
                    rb_raise(rb_eArgError, "unexpected realpath %"PRIxVALUE
                             "(%x), path=%+"PRIsVALUE,
                             realpath, TYPE(realpath), path);
                }
                realpath = rb_fstring(realpath);
            }
        }
        else {
            rb_raise(rb_eRuntimeError, "unexpected path object");
        }
        rb_iseq_pathobj_set(iseq, path, realpath);
    }

    // push dummy frame
    rb_execution_context_t *ec = GET_EC();
    VALUE dummy_frame = rb_vm_push_frame_fname(ec, path);

#undef IBF_BODY_OFFSET

    load_body->type = type;
    load_body->stack_max = stack_max;
    load_body->param.flags.has_lead = (param_flags >> 0) & 1;
    load_body->param.flags.has_opt = (param_flags >> 1) & 1;
    load_body->param.flags.has_rest = (param_flags >> 2) & 1;
    load_body->param.flags.has_post = (param_flags >> 3) & 1;
    load_body->param.flags.has_kw = FALSE;
    load_body->param.flags.has_kwrest = (param_flags >> 5) & 1;
    load_body->param.flags.has_block = (param_flags >> 6) & 1;
    load_body->param.flags.ambiguous_param0 = (param_flags >> 7) & 1;
    load_body->param.flags.accepts_no_kwarg = (param_flags >> 8) & 1;
    load_body->param.flags.ruby2_keywords = (param_flags >> 9) & 1;
    load_body->param.flags.anon_rest = (param_flags >> 10) & 1;
    load_body->param.flags.anon_kwrest = (param_flags >> 11) & 1;
    load_body->param.flags.use_block = (param_flags >> 12) & 1;
    load_body->param.flags.forwardable = (param_flags >> 13) & 1;
    load_body->param.size = param_size;
    load_body->param.lead_num = param_lead_num;
    load_body->param.opt_num = param_opt_num;
    load_body->param.rest_start = param_rest_start;
    load_body->param.post_start = param_post_start;
    load_body->param.post_num = param_post_num;
    load_body->param.block_start = param_block_start;
    load_body->local_table_size = local_table_size;
    load_body->ci_size = ci_size;
    load_body->insns_info.size = insns_info_size;

    ISEQ_COVERAGE_SET(iseq, Qnil);
    ISEQ_ORIGINAL_ISEQ_CLEAR(iseq);
    load_body->variable.flip_count = variable_flip_count;
    load_body->variable.script_lines = Qnil;

    load_body->location.first_lineno = location_first_lineno;
    load_body->location.node_id = location_node_id;
    load_body->location.code_location.beg_pos.lineno = location_code_location_beg_pos_lineno;
    load_body->location.code_location.beg_pos.column = location_code_location_beg_pos_column;
    load_body->location.code_location.end_pos.lineno = location_code_location_end_pos_lineno;
    load_body->location.code_location.end_pos.column = location_code_location_end_pos_column;
    load_body->builtin_attrs = builtin_attrs;
    load_body->prism = prism;

    load_body->ivc_size             = ivc_size;
    load_body->icvarc_size          = icvarc_size;
    load_body->ise_size             = ise_size;
    load_body->ic_size              = ic_size;

    if (ISEQ_IS_SIZE(load_body)) {
        load_body->is_entries       = ZALLOC_N(union iseq_inline_storage_entry, ISEQ_IS_SIZE(load_body));
    }
    else {
        load_body->is_entries       = NULL;
    }
                                      ibf_load_ci_entries(load, ci_entries_offset, ci_size, &load_body->call_data);
    load_body->outer_variables      = ibf_load_outer_variables(load, outer_variables_offset);
    load_body->param.opt_table      = ibf_load_param_opt_table(load, param_opt_table_offset, param_opt_num);
    load_body->param.keyword        = ibf_load_param_keyword(load, param_keyword_offset);
    load_body->param.flags.has_kw   = (param_flags >> 4) & 1;
    load_body->insns_info.body      = ibf_load_insns_info_body(load, insns_info_body_offset, insns_info_size);
    load_body->insns_info.positions = ibf_load_insns_info_positions(load, insns_info_positions_offset, insns_info_size);
    load_body->local_table          = ibf_load_local_table(load, local_table_offset, local_table_size);
    ibf_load_catch_table(load, catch_table_offset, catch_table_size, iseq);

    const rb_iseq_t *parent_iseq = ibf_load_iseq(load, (const rb_iseq_t *)(VALUE)parent_iseq_index);
    const rb_iseq_t *local_iseq = ibf_load_iseq(load, (const rb_iseq_t *)(VALUE)local_iseq_index);
    const rb_iseq_t *mandatory_only_iseq = ibf_load_iseq(load, (const rb_iseq_t *)(VALUE)mandatory_only_iseq_index);

    RB_OBJ_WRITE(iseq, &load_body->parent_iseq, parent_iseq);
    RB_OBJ_WRITE(iseq, &load_body->local_iseq, local_iseq);
    RB_OBJ_WRITE(iseq, &load_body->mandatory_only_iseq, mandatory_only_iseq);

    // This must be done after the local table is loaded.
    if (load_body->param.keyword != NULL) {
        RUBY_ASSERT(load_body->local_table);
        struct rb_iseq_param_keyword *keyword = (struct rb_iseq_param_keyword *) load_body->param.keyword;
        keyword->table = &load_body->local_table[keyword->bits_start - keyword->num];
    }

    ibf_load_code(load, iseq, bytecode_offset, bytecode_size, iseq_size);
#if VM_INSN_INFO_TABLE_IMPL == 2
    rb_iseq_insns_info_encode_positions(iseq);
#endif

    rb_iseq_translate_threaded_code(iseq);

#if IBF_ISEQ_ENABLE_LOCAL_BUFFER
    load->current_buffer = &load->global_buffer;
#endif

    RB_OBJ_WRITE(iseq, &load_body->location.base_label,    ibf_load_location_str(load, location_base_label_index));
    RB_OBJ_WRITE(iseq, &load_body->location.label,         ibf_load_location_str(load, location_label_index));

#if IBF_ISEQ_ENABLE_LOCAL_BUFFER
    load->current_buffer = saved_buffer;
#endif
    verify_call_cache(iseq);

    RB_GC_GUARD(dummy_frame);
    rb_vm_pop_frame_no_int(ec);
}

struct ibf_dump_iseq_list_arg
{
    struct ibf_dump *dump;
    VALUE offset_list;
};

static int
ibf_dump_iseq_list_i(st_data_t key, st_data_t val, st_data_t ptr)
{
    const rb_iseq_t *iseq = (const rb_iseq_t *)key;
    struct ibf_dump_iseq_list_arg *args = (struct ibf_dump_iseq_list_arg *)ptr;

    ibf_offset_t offset = ibf_dump_iseq_each(args->dump, iseq);
    rb_ary_push(args->offset_list, UINT2NUM(offset));

    return ST_CONTINUE;
}

static void
ibf_dump_iseq_list(struct ibf_dump *dump, struct ibf_header *header)
{
    VALUE offset_list = rb_ary_hidden_new(dump->iseq_table->num_entries);

    struct ibf_dump_iseq_list_arg args;
    args.dump = dump;
    args.offset_list = offset_list;

    st_foreach(dump->iseq_table, ibf_dump_iseq_list_i, (st_data_t)&args);

    st_index_t i;
    st_index_t size = dump->iseq_table->num_entries;
    ibf_offset_t *offsets = ALLOCA_N(ibf_offset_t, size);

    for (i = 0; i < size; i++) {
        offsets[i] = NUM2UINT(RARRAY_AREF(offset_list, i));
    }

    ibf_dump_align(dump, sizeof(ibf_offset_t));
    header->iseq_list_offset = ibf_dump_write(dump, offsets, sizeof(ibf_offset_t) * size);
    header->iseq_list_size = (unsigned int)size;
}

#define IBF_OBJECT_INTERNAL FL_PROMOTED0

/*
 * Binary format
 * - ibf_object_header
 * - ibf_object_xxx (xxx is type)
 */

struct ibf_object_header {
    unsigned int type: 5;
    unsigned int special_const: 1;
    unsigned int frozen: 1;
    unsigned int internal: 1;
};

enum ibf_object_class_index {
    IBF_OBJECT_CLASS_OBJECT,
    IBF_OBJECT_CLASS_ARRAY,
    IBF_OBJECT_CLASS_STANDARD_ERROR,
    IBF_OBJECT_CLASS_NO_MATCHING_PATTERN_ERROR,
    IBF_OBJECT_CLASS_TYPE_ERROR,
    IBF_OBJECT_CLASS_NO_MATCHING_PATTERN_KEY_ERROR,
};

struct ibf_object_regexp {
    long srcstr;
    char option;
};

struct ibf_object_hash {
    long len;
    long keyval[FLEX_ARY_LEN];
};

struct ibf_object_struct_range {
    long class_index;
    long len;
    long beg;
    long end;
    int excl;
};

struct ibf_object_bignum {
    ssize_t slen;
    BDIGIT digits[FLEX_ARY_LEN];
};

enum ibf_object_data_type {
    IBF_OBJECT_DATA_ENCODING,
};

struct ibf_object_complex_rational {
    long a, b;
};

struct ibf_object_symbol {
    long str;
};

#define IBF_ALIGNED_OFFSET(align, offset) /* offset > 0 */ \
    ((((offset) - 1) / (align) + 1) * (align))
#define IBF_OBJBODY(type, offset) (const type *)\
    ibf_load_check_offset(load, IBF_ALIGNED_OFFSET(RUBY_ALIGNOF(type), offset))

static const void *
ibf_load_check_offset(const struct ibf_load *load, size_t offset)
{
    if (offset >= load->current_buffer->size) {
        rb_raise(rb_eIndexError, "object offset out of range: %"PRIdSIZE, offset);
    }
    return load->current_buffer->buff + offset;
}

NORETURN(static void ibf_dump_object_unsupported(struct ibf_dump *dump, VALUE obj));

static void
ibf_dump_object_unsupported(struct ibf_dump *dump, VALUE obj)
{
    char buff[0x100];
    rb_raw_obj_info(buff, sizeof(buff), obj);
    rb_raise(rb_eNotImpError, "ibf_dump_object_unsupported: %s", buff);
}

NORETURN(static VALUE ibf_load_object_unsupported(const struct ibf_load *load, const struct ibf_object_header *header, ibf_offset_t offset));

static VALUE
ibf_load_object_unsupported(const struct ibf_load *load, const struct ibf_object_header *header, ibf_offset_t offset)
{
    rb_raise(rb_eArgError, "unsupported");
    UNREACHABLE_RETURN(Qnil);
}

static void
ibf_dump_object_class(struct ibf_dump *dump, VALUE obj)
{
    enum ibf_object_class_index cindex;
    if (obj == rb_cObject) {
        cindex = IBF_OBJECT_CLASS_OBJECT;
    }
    else if (obj == rb_cArray) {
        cindex = IBF_OBJECT_CLASS_ARRAY;
    }
    else if (obj == rb_eStandardError) {
        cindex = IBF_OBJECT_CLASS_STANDARD_ERROR;
    }
    else if (obj == rb_eNoMatchingPatternError) {
        cindex = IBF_OBJECT_CLASS_NO_MATCHING_PATTERN_ERROR;
    }
    else if (obj == rb_eTypeError) {
        cindex = IBF_OBJECT_CLASS_TYPE_ERROR;
    }
    else if (obj == rb_eNoMatchingPatternKeyError) {
        cindex = IBF_OBJECT_CLASS_NO_MATCHING_PATTERN_KEY_ERROR;
    }
    else {
        rb_obj_info_dump(obj);
        rb_p(obj);
        rb_bug("unsupported class");
    }
    ibf_dump_write_small_value(dump, (VALUE)cindex);
}

static VALUE
ibf_load_object_class(const struct ibf_load *load, const struct ibf_object_header *header, ibf_offset_t offset)
{
    enum ibf_object_class_index cindex = (enum ibf_object_class_index)ibf_load_small_value(load, &offset);

    switch (cindex) {
      case IBF_OBJECT_CLASS_OBJECT:
        return rb_cObject;
      case IBF_OBJECT_CLASS_ARRAY:
        return rb_cArray;
      case IBF_OBJECT_CLASS_STANDARD_ERROR:
        return rb_eStandardError;
      case IBF_OBJECT_CLASS_NO_MATCHING_PATTERN_ERROR:
        return rb_eNoMatchingPatternError;
      case IBF_OBJECT_CLASS_TYPE_ERROR:
        return rb_eTypeError;
      case IBF_OBJECT_CLASS_NO_MATCHING_PATTERN_KEY_ERROR:
        return rb_eNoMatchingPatternKeyError;
    }

    rb_raise(rb_eArgError, "ibf_load_object_class: unknown class (%d)", (int)cindex);
}


static void
ibf_dump_object_float(struct ibf_dump *dump, VALUE obj)
{
    double dbl = RFLOAT_VALUE(obj);
    (void)IBF_W(&dbl, double, 1);
}

static VALUE
ibf_load_object_float(const struct ibf_load *load, const struct ibf_object_header *header, ibf_offset_t offset)
{
    const double *dblp = IBF_OBJBODY(double, offset);
    return DBL2NUM(*dblp);
}

static void
ibf_dump_object_string(struct ibf_dump *dump, VALUE obj)
{
    long encindex = (long)rb_enc_get_index(obj);
    long len = RSTRING_LEN(obj);
    const char *ptr = RSTRING_PTR(obj);

    if (encindex > RUBY_ENCINDEX_BUILTIN_MAX) {
        rb_encoding *enc = rb_enc_from_index((int)encindex);
        const char *enc_name = rb_enc_name(enc);
        encindex = RUBY_ENCINDEX_BUILTIN_MAX + ibf_dump_object(dump, rb_str_new2(enc_name));
    }

    ibf_dump_write_small_value(dump, encindex);
    ibf_dump_write_small_value(dump, len);
    IBF_WP(ptr, char, len);
}

static VALUE
ibf_load_object_string(const struct ibf_load *load, const struct ibf_object_header *header, ibf_offset_t offset)
{
    ibf_offset_t reading_pos = offset;

    int encindex = (int)ibf_load_small_value(load, &reading_pos);
    const long len = (long)ibf_load_small_value(load, &reading_pos);
    const char *ptr = load->current_buffer->buff + reading_pos;

    if (encindex > RUBY_ENCINDEX_BUILTIN_MAX) {
        VALUE enc_name_str = ibf_load_object(load, encindex - RUBY_ENCINDEX_BUILTIN_MAX);
        encindex = rb_enc_find_index(RSTRING_PTR(enc_name_str));
    }

    VALUE str;
    if (header->frozen && !header->internal) {
        str = rb_enc_literal_str(ptr, len, rb_enc_from_index(encindex));
    }
    else {
        str = rb_enc_str_new(ptr, len, rb_enc_from_index(encindex));

        if (header->internal) rb_obj_hide(str);
        if (header->frozen)   str = rb_fstring(str);
    }
    return str;
}

static void
ibf_dump_object_regexp(struct ibf_dump *dump, VALUE obj)
{
    VALUE srcstr = RREGEXP_SRC(obj);
    struct ibf_object_regexp regexp;
    regexp.option = (char)rb_reg_options(obj);
    regexp.srcstr = (long)ibf_dump_object(dump, srcstr);

    ibf_dump_write_byte(dump, (unsigned char)regexp.option);
    ibf_dump_write_small_value(dump, regexp.srcstr);
}

static VALUE
ibf_load_object_regexp(const struct ibf_load *load, const struct ibf_object_header *header, ibf_offset_t offset)
{
    struct ibf_object_regexp regexp;
    regexp.option = ibf_load_byte(load, &offset);
    regexp.srcstr = ibf_load_small_value(load, &offset);

    VALUE srcstr = ibf_load_object(load, regexp.srcstr);
    VALUE reg = rb_reg_compile(srcstr, (int)regexp.option, NULL, 0);

    if (header->internal) rb_obj_hide(reg);
    if (header->frozen)   rb_obj_freeze(reg);

    return reg;
}

static void
ibf_dump_object_array(struct ibf_dump *dump, VALUE obj)
{
    long i, len = RARRAY_LEN(obj);
    ibf_dump_write_small_value(dump, len);
    for (i=0; i<len; i++) {
        long index = (long)ibf_dump_object(dump, RARRAY_AREF(obj, i));
        ibf_dump_write_small_value(dump, index);
    }
}

static VALUE
ibf_load_object_array(const struct ibf_load *load, const struct ibf_object_header *header, ibf_offset_t offset)
{
    ibf_offset_t reading_pos = offset;

    const long len = (long)ibf_load_small_value(load, &reading_pos);

    VALUE ary = header->internal ? rb_ary_hidden_new(len) : rb_ary_new_capa(len);
    int i;

    for (i=0; i<len; i++) {
        const VALUE index = ibf_load_small_value(load, &reading_pos);
        rb_ary_push(ary, ibf_load_object(load, index));
    }

    if (header->frozen) rb_ary_freeze(ary);

    return ary;
}

static int
ibf_dump_object_hash_i(st_data_t key, st_data_t val, st_data_t ptr)
{
    struct ibf_dump *dump = (struct ibf_dump *)ptr;

    VALUE key_index = ibf_dump_object(dump, (VALUE)key);
    VALUE val_index = ibf_dump_object(dump, (VALUE)val);

    ibf_dump_write_small_value(dump, key_index);
    ibf_dump_write_small_value(dump, val_index);
    return ST_CONTINUE;
}

static void
ibf_dump_object_hash(struct ibf_dump *dump, VALUE obj)
{
    long len = RHASH_SIZE(obj);
    ibf_dump_write_small_value(dump, (VALUE)len);

    if (len > 0) rb_hash_foreach(obj, ibf_dump_object_hash_i, (VALUE)dump);
}

static VALUE
ibf_load_object_hash(const struct ibf_load *load, const struct ibf_object_header *header, ibf_offset_t offset)
{
    long len = (long)ibf_load_small_value(load, &offset);
    VALUE obj = rb_hash_new_with_size(len);
    int i;

    for (i = 0; i < len; i++) {
        VALUE key_index = ibf_load_small_value(load, &offset);
        VALUE val_index = ibf_load_small_value(load, &offset);

        VALUE key = ibf_load_object(load, key_index);
        VALUE val = ibf_load_object(load, val_index);
        rb_hash_aset(obj, key, val);
    }
    rb_hash_rehash(obj);

    if (header->internal) rb_obj_hide(obj);
    if (header->frozen)   rb_obj_freeze(obj);

    return obj;
}

static void
ibf_dump_object_struct(struct ibf_dump *dump, VALUE obj)
{
    if (rb_obj_is_kind_of(obj, rb_cRange)) {
        struct ibf_object_struct_range range;
        VALUE beg, end;
        IBF_ZERO(range);
        range.len = 3;
        range.class_index = 0;

        rb_range_values(obj, &beg, &end, &range.excl);
        range.beg = (long)ibf_dump_object(dump, beg);
        range.end = (long)ibf_dump_object(dump, end);

        IBF_W_ALIGN(struct ibf_object_struct_range);
        IBF_WV(range);
    }
    else {
        rb_raise(rb_eNotImpError, "ibf_dump_object_struct: unsupported class %"PRIsVALUE,
                 rb_class_name(CLASS_OF(obj)));
    }
}

static VALUE
ibf_load_object_struct(const struct ibf_load *load, const struct ibf_object_header *header, ibf_offset_t offset)
{
    const struct ibf_object_struct_range *range = IBF_OBJBODY(struct ibf_object_struct_range, offset);
    VALUE beg = ibf_load_object(load, range->beg);
    VALUE end = ibf_load_object(load, range->end);
    VALUE obj = rb_range_new(beg, end, range->excl);
    if (header->internal) rb_obj_hide(obj);
    if (header->frozen)   rb_obj_freeze(obj);
    return obj;
}

static void
ibf_dump_object_bignum(struct ibf_dump *dump, VALUE obj)
{
    ssize_t len = BIGNUM_LEN(obj);
    ssize_t slen = BIGNUM_SIGN(obj) > 0 ? len : len * -1;
    BDIGIT *d = BIGNUM_DIGITS(obj);

    (void)IBF_W(&slen, ssize_t, 1);
    IBF_WP(d, BDIGIT, len);
}

static VALUE
ibf_load_object_bignum(const struct ibf_load *load, const struct ibf_object_header *header, ibf_offset_t offset)
{
    const struct ibf_object_bignum *bignum = IBF_OBJBODY(struct ibf_object_bignum, offset);
    int sign = bignum->slen > 0;
    ssize_t len = sign > 0 ? bignum->slen : -1 * bignum->slen;
    const int big_unpack_flags = /* c.f. rb_big_unpack() */
        INTEGER_PACK_LSWORD_FIRST |
        INTEGER_PACK_NATIVE_BYTE_ORDER;
    VALUE obj = rb_integer_unpack(bignum->digits, len, sizeof(BDIGIT), 0,
                                  big_unpack_flags |
                                  (sign == 0 ? INTEGER_PACK_NEGATIVE : 0));
    if (header->internal) rb_obj_hide(obj);
    if (header->frozen)   rb_obj_freeze(obj);
    return obj;
}

static void
ibf_dump_object_data(struct ibf_dump *dump, VALUE obj)
{
    if (rb_data_is_encoding(obj)) {
        rb_encoding *enc = rb_to_encoding(obj);
        const char *name = rb_enc_name(enc);
        long len = strlen(name) + 1;
        long data[2];
        data[0] = IBF_OBJECT_DATA_ENCODING;
        data[1] = len;
        (void)IBF_W(data, long, 2);
        IBF_WP(name, char, len);
    }
    else {
        ibf_dump_object_unsupported(dump, obj);
    }
}

static VALUE
ibf_load_object_data(const struct ibf_load *load, const struct ibf_object_header *header, ibf_offset_t offset)
{
    const long *body = IBF_OBJBODY(long, offset);
    const enum ibf_object_data_type type = (enum ibf_object_data_type)body[0];
    /* const long len = body[1]; */
    const char *data = (const char *)&body[2];

    switch (type) {
      case IBF_OBJECT_DATA_ENCODING:
        {
            VALUE encobj = rb_enc_from_encoding(rb_enc_find(data));
            return encobj;
        }
    }

    return ibf_load_object_unsupported(load, header, offset);
}

static void
ibf_dump_object_complex_rational(struct ibf_dump *dump, VALUE obj)
{
    long data[2];
    data[0] = (long)ibf_dump_object(dump, RCOMPLEX(obj)->real);
    data[1] = (long)ibf_dump_object(dump, RCOMPLEX(obj)->imag);

    (void)IBF_W(data, long, 2);
}

static VALUE
ibf_load_object_complex_rational(const struct ibf_load *load, const struct ibf_object_header *header, ibf_offset_t offset)
{
    const struct ibf_object_complex_rational *nums = IBF_OBJBODY(struct ibf_object_complex_rational, offset);
    VALUE a = ibf_load_object(load, nums->a);
    VALUE b = ibf_load_object(load, nums->b);
    VALUE obj = header->type == T_COMPLEX ?
      rb_complex_new(a, b) : rb_rational_new(a, b);

    if (header->internal) rb_obj_hide(obj);
    if (header->frozen)   rb_obj_freeze(obj);
    return obj;
}

static void
ibf_dump_object_symbol(struct ibf_dump *dump, VALUE obj)
{
    ibf_dump_object_string(dump, rb_sym2str(obj));
}

static VALUE
ibf_load_object_symbol(const struct ibf_load *load, const struct ibf_object_header *header, ibf_offset_t offset)
{
    ibf_offset_t reading_pos = offset;

    int encindex = (int)ibf_load_small_value(load, &reading_pos);
    const long len = (long)ibf_load_small_value(load, &reading_pos);
    const char *ptr = load->current_buffer->buff + reading_pos;

    if (encindex > RUBY_ENCINDEX_BUILTIN_MAX) {
        VALUE enc_name_str = ibf_load_object(load, encindex - RUBY_ENCINDEX_BUILTIN_MAX);
        encindex = rb_enc_find_index(RSTRING_PTR(enc_name_str));
    }

    ID id = rb_intern3(ptr, len, rb_enc_from_index(encindex));
    return ID2SYM(id);
}

typedef void (*ibf_dump_object_function)(struct ibf_dump *dump, VALUE obj);
static const ibf_dump_object_function dump_object_functions[RUBY_T_MASK+1] = {
    ibf_dump_object_unsupported, /* T_NONE */
    ibf_dump_object_unsupported, /* T_OBJECT */
    ibf_dump_object_class,       /* T_CLASS */
    ibf_dump_object_unsupported, /* T_MODULE */
    ibf_dump_object_float,       /* T_FLOAT */
    ibf_dump_object_string,      /* T_STRING */
    ibf_dump_object_regexp,      /* T_REGEXP */
    ibf_dump_object_array,       /* T_ARRAY */
    ibf_dump_object_hash,        /* T_HASH */
    ibf_dump_object_struct,      /* T_STRUCT */
    ibf_dump_object_bignum,      /* T_BIGNUM */
    ibf_dump_object_unsupported, /* T_FILE */
    ibf_dump_object_data,        /* T_DATA */
    ibf_dump_object_unsupported, /* T_MATCH */
    ibf_dump_object_complex_rational, /* T_COMPLEX */
    ibf_dump_object_complex_rational, /* T_RATIONAL */
    ibf_dump_object_unsupported, /* 0x10 */
    ibf_dump_object_unsupported, /* 0x11 T_NIL */
    ibf_dump_object_unsupported, /* 0x12 T_TRUE */
    ibf_dump_object_unsupported, /* 0x13 T_FALSE */
    ibf_dump_object_symbol,      /* 0x14 T_SYMBOL */
    ibf_dump_object_unsupported, /* T_FIXNUM */
    ibf_dump_object_unsupported, /* T_UNDEF */
    ibf_dump_object_unsupported, /* 0x17 */
    ibf_dump_object_unsupported, /* 0x18 */
    ibf_dump_object_unsupported, /* 0x19 */
    ibf_dump_object_unsupported, /* T_IMEMO 0x1a */
    ibf_dump_object_unsupported, /* T_NODE 0x1b */
    ibf_dump_object_unsupported, /* T_ICLASS 0x1c */
    ibf_dump_object_unsupported, /* T_ZOMBIE 0x1d */
    ibf_dump_object_unsupported, /* 0x1e */
    ibf_dump_object_unsupported, /* 0x1f */
};

static void
ibf_dump_object_object_header(struct ibf_dump *dump, const struct ibf_object_header header)
{
    unsigned char byte =
        (header.type          << 0) |
        (header.special_const << 5) |
        (header.frozen        << 6) |
        (header.internal      << 7);

    IBF_WV(byte);
}

static struct ibf_object_header
ibf_load_object_object_header(const struct ibf_load *load, ibf_offset_t *offset)
{
    unsigned char byte = ibf_load_byte(load, offset);

    struct ibf_object_header header;
    header.type          = (byte >> 0) & 0x1f;
    header.special_const = (byte >> 5) & 0x01;
    header.frozen        = (byte >> 6) & 0x01;
    header.internal      = (byte >> 7) & 0x01;

    return header;
}

static ibf_offset_t
ibf_dump_object_object(struct ibf_dump *dump, VALUE obj)
{
    struct ibf_object_header obj_header;
    ibf_offset_t current_offset;
    IBF_ZERO(obj_header);
    obj_header.type = TYPE(obj);

    IBF_W_ALIGN(ibf_offset_t);
    current_offset = ibf_dump_pos(dump);

    if (SPECIAL_CONST_P(obj) &&
        ! (SYMBOL_P(obj) ||
           RB_FLOAT_TYPE_P(obj))) {
        obj_header.special_const = TRUE;
        obj_header.frozen = TRUE;
        obj_header.internal = TRUE;
        ibf_dump_object_object_header(dump, obj_header);
        ibf_dump_write_small_value(dump, obj);
    }
    else {
        obj_header.internal = SPECIAL_CONST_P(obj) ? FALSE : (RBASIC_CLASS(obj) == 0) ? TRUE : FALSE;
        obj_header.special_const = FALSE;
        obj_header.frozen = OBJ_FROZEN(obj) ? TRUE : FALSE;
        ibf_dump_object_object_header(dump, obj_header);
        (*dump_object_functions[obj_header.type])(dump, obj);
    }

    return current_offset;
}

typedef VALUE (*ibf_load_object_function)(const struct ibf_load *load, const struct ibf_object_header *header, ibf_offset_t offset);
static const ibf_load_object_function load_object_functions[RUBY_T_MASK+1] = {
    ibf_load_object_unsupported, /* T_NONE */
    ibf_load_object_unsupported, /* T_OBJECT */
    ibf_load_object_class,       /* T_CLASS */
    ibf_load_object_unsupported, /* T_MODULE */
    ibf_load_object_float,       /* T_FLOAT */
    ibf_load_object_string,      /* T_STRING */
    ibf_load_object_regexp,      /* T_REGEXP */
    ibf_load_object_array,       /* T_ARRAY */
    ibf_load_object_hash,        /* T_HASH */
    ibf_load_object_struct,      /* T_STRUCT */
    ibf_load_object_bignum,      /* T_BIGNUM */
    ibf_load_object_unsupported, /* T_FILE */
    ibf_load_object_data,        /* T_DATA */
    ibf_load_object_unsupported, /* T_MATCH */
    ibf_load_object_complex_rational, /* T_COMPLEX */
    ibf_load_object_complex_rational, /* T_RATIONAL */
    ibf_load_object_unsupported, /* 0x10 */
    ibf_load_object_unsupported, /* T_NIL */
    ibf_load_object_unsupported, /* T_TRUE */
    ibf_load_object_unsupported, /* T_FALSE */
    ibf_load_object_symbol,
    ibf_load_object_unsupported, /* T_FIXNUM */
    ibf_load_object_unsupported, /* T_UNDEF */
    ibf_load_object_unsupported, /* 0x17 */
    ibf_load_object_unsupported, /* 0x18 */
    ibf_load_object_unsupported, /* 0x19 */
    ibf_load_object_unsupported, /* T_IMEMO 0x1a */
    ibf_load_object_unsupported, /* T_NODE 0x1b */
    ibf_load_object_unsupported, /* T_ICLASS 0x1c */
    ibf_load_object_unsupported, /* T_ZOMBIE 0x1d */
    ibf_load_object_unsupported, /* 0x1e */
    ibf_load_object_unsupported, /* 0x1f */
};

static VALUE
ibf_load_object(const struct ibf_load *load, VALUE object_index)
{
    if (object_index == 0) {
        return Qnil;
    }
    else {
        VALUE obj = pinned_list_fetch(load->current_buffer->obj_list, (long)object_index);
        if (!obj) {
            ibf_offset_t *offsets = (ibf_offset_t *)(load->current_buffer->obj_list_offset + load->current_buffer->buff);
            ibf_offset_t offset = offsets[object_index];
            const struct ibf_object_header header = ibf_load_object_object_header(load, &offset);

#if IBF_ISEQ_DEBUG
            fprintf(stderr, "ibf_load_object: list=%#x offsets=%p offset=%#x\n",
                    load->current_buffer->obj_list_offset, (void *)offsets, offset);
            fprintf(stderr, "ibf_load_object: type=%#x special=%d frozen=%d internal=%d\n",
                    header.type, header.special_const, header.frozen, header.internal);
#endif
            if (offset >= load->current_buffer->size) {
                rb_raise(rb_eIndexError, "object offset out of range: %u", offset);
            }

            if (header.special_const) {
                ibf_offset_t reading_pos = offset;

                obj = ibf_load_small_value(load, &reading_pos);
            }
            else {
                obj = (*load_object_functions[header.type])(load, &header, offset);
            }

            pinned_list_store(load->current_buffer->obj_list, (long)object_index, obj);
        }
#if IBF_ISEQ_DEBUG
        fprintf(stderr, "ibf_load_object: index=%#"PRIxVALUE" obj=%#"PRIxVALUE"\n",
                object_index, obj);
#endif
        return obj;
    }
}

struct ibf_dump_object_list_arg
{
    struct ibf_dump *dump;
    VALUE offset_list;
};

static int
ibf_dump_object_list_i(st_data_t key, st_data_t val, st_data_t ptr)
{
    VALUE obj = (VALUE)key;
    struct ibf_dump_object_list_arg *args = (struct ibf_dump_object_list_arg *)ptr;

    ibf_offset_t offset = ibf_dump_object_object(args->dump, obj);
    rb_ary_push(args->offset_list, UINT2NUM(offset));

    return ST_CONTINUE;
}

static void
ibf_dump_object_list(struct ibf_dump *dump, ibf_offset_t *obj_list_offset, unsigned int *obj_list_size)
{
    st_table *obj_table = dump->current_buffer->obj_table;
    VALUE offset_list = rb_ary_hidden_new(obj_table->num_entries);

    struct ibf_dump_object_list_arg args;
    args.dump = dump;
    args.offset_list = offset_list;

    st_foreach(obj_table, ibf_dump_object_list_i, (st_data_t)&args);

    IBF_W_ALIGN(ibf_offset_t);
    *obj_list_offset = ibf_dump_pos(dump);

    st_index_t size = obj_table->num_entries;
    st_index_t i;

    for (i=0; i<size; i++) {
        ibf_offset_t offset = NUM2UINT(RARRAY_AREF(offset_list, i));
        IBF_WV(offset);
    }

    *obj_list_size = (unsigned int)size;
}

static void
ibf_dump_mark(void *ptr)
{
    struct ibf_dump *dump = (struct ibf_dump *)ptr;
    rb_gc_mark(dump->global_buffer.str);

    rb_mark_set(dump->global_buffer.obj_table);
    rb_mark_set(dump->iseq_table);
}

static void
ibf_dump_free(void *ptr)
{
    struct ibf_dump *dump = (struct ibf_dump *)ptr;
    if (dump->global_buffer.obj_table) {
        st_free_table(dump->global_buffer.obj_table);
        dump->global_buffer.obj_table = 0;
    }
    if (dump->iseq_table) {
        st_free_table(dump->iseq_table);
        dump->iseq_table = 0;
    }
}

static size_t
ibf_dump_memsize(const void *ptr)
{
    struct ibf_dump *dump = (struct ibf_dump *)ptr;
    size_t size = 0;
    if (dump->iseq_table) size += st_memsize(dump->iseq_table);
    if (dump->global_buffer.obj_table) size += st_memsize(dump->global_buffer.obj_table);
    return size;
}

static const rb_data_type_t ibf_dump_type = {
    "ibf_dump",
    {ibf_dump_mark, ibf_dump_free, ibf_dump_memsize,},
    0, 0, RUBY_TYPED_FREE_IMMEDIATELY | RUBY_TYPED_EMBEDDABLE
};

static void
ibf_dump_setup(struct ibf_dump *dump, VALUE dumper_obj)
{
    dump->global_buffer.obj_table = NULL; // GC may run before a value is assigned
    dump->iseq_table = NULL;

    RB_OBJ_WRITE(dumper_obj, &dump->global_buffer.str, rb_str_new(0, 0));
    dump->global_buffer.obj_table = ibf_dump_object_table_new();
    dump->iseq_table = st_init_numtable(); /* need free */

    dump->current_buffer = &dump->global_buffer;
}

VALUE
rb_iseq_ibf_dump(const rb_iseq_t *iseq, VALUE opt)
{
    struct ibf_dump *dump;
    struct ibf_header header = {{0}};
    VALUE dump_obj;
    VALUE str;

    if (ISEQ_BODY(iseq)->parent_iseq != NULL ||
        ISEQ_BODY(iseq)->local_iseq != iseq) {
        rb_raise(rb_eRuntimeError, "should be top of iseq");
    }
    if (RTEST(ISEQ_COVERAGE(iseq))) {
        rb_raise(rb_eRuntimeError, "should not compile with coverage");
    }

    dump_obj = TypedData_Make_Struct(0, struct ibf_dump, &ibf_dump_type, dump);
    ibf_dump_setup(dump, dump_obj);

    ibf_dump_write(dump, &header, sizeof(header));
    ibf_dump_iseq(dump, iseq);

    header.magic[0] = 'Y'; /* YARB */
    header.magic[1] = 'A';
    header.magic[2] = 'R';
    header.magic[3] = 'B';
    header.major_version = IBF_MAJOR_VERSION;
    header.minor_version = IBF_MINOR_VERSION;
    header.endian = IBF_ENDIAN_MARK;
    header.wordsize = (uint8_t)SIZEOF_VALUE;
    ibf_dump_iseq_list(dump, &header);
    ibf_dump_object_list(dump, &header.global_object_list_offset, &header.global_object_list_size);
    header.size = ibf_dump_pos(dump);

    if (RTEST(opt)) {
        VALUE opt_str = opt;
        const char *ptr = StringValuePtr(opt_str);
        header.extra_size = RSTRING_LENINT(opt_str);
        ibf_dump_write(dump, ptr, header.extra_size);
    }
    else {
        header.extra_size = 0;
    }

    ibf_dump_overwrite(dump, &header, sizeof(header), 0);

    str = dump->global_buffer.str;
    RB_GC_GUARD(dump_obj);
    return str;
}

static const ibf_offset_t *
ibf_iseq_list(const struct ibf_load *load)
{
    return (const ibf_offset_t *)(load->global_buffer.buff + load->header->iseq_list_offset);
}

void
rb_ibf_load_iseq_complete(rb_iseq_t *iseq)
{
    struct ibf_load *load = RTYPEDDATA_DATA(iseq->aux.loader.obj);
    rb_iseq_t *prev_src_iseq = load->iseq;
    ibf_offset_t offset = ibf_iseq_list(load)[iseq->aux.loader.index];
    load->iseq = iseq;
#if IBF_ISEQ_DEBUG
    fprintf(stderr, "rb_ibf_load_iseq_complete: index=%#x offset=%#x size=%#x\n",
            iseq->aux.loader.index, offset,
            load->header->size);
#endif
    ibf_load_iseq_each(load, iseq, offset);
    ISEQ_COMPILE_DATA_CLEAR(iseq);
    FL_UNSET((VALUE)iseq, ISEQ_NOT_LOADED_YET);
    rb_iseq_init_trace(iseq);
    load->iseq = prev_src_iseq;
}

#if USE_LAZY_LOAD
const rb_iseq_t *
rb_iseq_complete(const rb_iseq_t *iseq)
{
    rb_ibf_load_iseq_complete((rb_iseq_t *)iseq);
    return iseq;
}
#endif

static rb_iseq_t *
ibf_load_iseq(const struct ibf_load *load, const rb_iseq_t *index_iseq)
{
    int iseq_index = (int)(VALUE)index_iseq;

#if IBF_ISEQ_DEBUG
    fprintf(stderr, "ibf_load_iseq: index_iseq=%p iseq_list=%p\n",
            (void *)index_iseq, (void *)load->iseq_list);
#endif
    if (iseq_index == -1) {
        return NULL;
    }
    else {
        VALUE iseqv = pinned_list_fetch(load->iseq_list, iseq_index);

#if IBF_ISEQ_DEBUG
        fprintf(stderr, "ibf_load_iseq: iseqv=%p\n", (void *)iseqv);
#endif
        if (iseqv) {
            return (rb_iseq_t *)iseqv;
        }
        else {
            rb_iseq_t *iseq = iseq_imemo_alloc();
#if IBF_ISEQ_DEBUG
            fprintf(stderr, "ibf_load_iseq: new iseq=%p\n", (void *)iseq);
#endif
            FL_SET((VALUE)iseq, ISEQ_NOT_LOADED_YET);
            iseq->aux.loader.obj = load->loader_obj;
            iseq->aux.loader.index = iseq_index;
#if IBF_ISEQ_DEBUG
            fprintf(stderr, "ibf_load_iseq: iseq=%p loader_obj=%p index=%d\n",
                    (void *)iseq, (void *)load->loader_obj, iseq_index);
#endif
            pinned_list_store(load->iseq_list, iseq_index, (VALUE)iseq);

            if (!USE_LAZY_LOAD || GET_VM()->builtin_function_table) {
#if IBF_ISEQ_DEBUG
                fprintf(stderr, "ibf_load_iseq: loading iseq=%p\n", (void *)iseq);
#endif
                rb_ibf_load_iseq_complete(iseq);
            }

#if IBF_ISEQ_DEBUG
            fprintf(stderr, "ibf_load_iseq: iseq=%p loaded %p\n",
                    (void *)iseq, (void *)load->iseq);
#endif
            return iseq;
        }
    }
}

static void
ibf_load_setup_bytes(struct ibf_load *load, VALUE loader_obj, const char *bytes, size_t size)
{
    struct ibf_header *header = (struct ibf_header *)bytes;
    load->loader_obj = loader_obj;
    load->global_buffer.buff = bytes;
    load->header = header;
    load->global_buffer.size = header->size;
    load->global_buffer.obj_list_offset = header->global_object_list_offset;
    load->global_buffer.obj_list_size = header->global_object_list_size;
    RB_OBJ_WRITE(loader_obj, &load->iseq_list, pinned_list_new(header->iseq_list_size));
    RB_OBJ_WRITE(loader_obj, &load->global_buffer.obj_list, pinned_list_new(load->global_buffer.obj_list_size));
    load->iseq = NULL;

    load->current_buffer = &load->global_buffer;

    if (size < header->size) {
        rb_raise(rb_eRuntimeError, "broken binary format");
    }
    if (strncmp(header->magic, "YARB", 4) != 0) {
        rb_raise(rb_eRuntimeError, "unknown binary format");
    }
    if (header->major_version != IBF_MAJOR_VERSION ||
        header->minor_version != IBF_MINOR_VERSION) {
        rb_raise(rb_eRuntimeError, "unmatched version file (%u.%u for %u.%u)",
                 header->major_version, header->minor_version, IBF_MAJOR_VERSION, IBF_MINOR_VERSION);
    }
    if (header->endian != IBF_ENDIAN_MARK) {
        rb_raise(rb_eRuntimeError, "unmatched endian: %c", header->endian);
    }
    if (header->wordsize != SIZEOF_VALUE) {
        rb_raise(rb_eRuntimeError, "unmatched word size: %d", header->wordsize);
    }
    if (header->iseq_list_offset % RUBY_ALIGNOF(ibf_offset_t)) {
        rb_raise(rb_eArgError, "unaligned iseq list offset: %u",
                 header->iseq_list_offset);
    }
    if (load->global_buffer.obj_list_offset % RUBY_ALIGNOF(ibf_offset_t)) {
        rb_raise(rb_eArgError, "unaligned object list offset: %u",
                 load->global_buffer.obj_list_offset);
    }
}

static void
ibf_load_setup(struct ibf_load *load, VALUE loader_obj, VALUE str)
{
    StringValue(str);

    if (RSTRING_LENINT(str) < (int)sizeof(struct ibf_header)) {
        rb_raise(rb_eRuntimeError, "broken binary format");
    }

    if (USE_LAZY_LOAD) {
        str = rb_str_new(RSTRING_PTR(str), RSTRING_LEN(str));
    }

    ibf_load_setup_bytes(load, loader_obj, RSTRING_PTR(str), RSTRING_LEN(str));
    RB_OBJ_WRITE(loader_obj, &load->str, str);
}

static void
ibf_loader_mark(void *ptr)
{
    struct ibf_load *load = (struct ibf_load *)ptr;
    rb_gc_mark(load->str);
    rb_gc_mark(load->iseq_list);
    rb_gc_mark(load->global_buffer.obj_list);
}

static void
ibf_loader_free(void *ptr)
{
    struct ibf_load *load = (struct ibf_load *)ptr;
    ruby_xfree(load);
}

static size_t
ibf_loader_memsize(const void *ptr)
{
    return sizeof(struct ibf_load);
}

static const rb_data_type_t ibf_load_type = {
    "ibf_loader",
    {ibf_loader_mark, ibf_loader_free, ibf_loader_memsize,},
    0, 0, RUBY_TYPED_WB_PROTECTED | RUBY_TYPED_FREE_IMMEDIATELY
};

const rb_iseq_t *
rb_iseq_ibf_load(VALUE str)
{
    struct ibf_load *load;
    rb_iseq_t *iseq;
    VALUE loader_obj = TypedData_Make_Struct(0, struct ibf_load, &ibf_load_type, load);

    ibf_load_setup(load, loader_obj, str);
    iseq = ibf_load_iseq(load, 0);

    RB_GC_GUARD(loader_obj);
    return iseq;
}

const rb_iseq_t *
rb_iseq_ibf_load_bytes(const char *bytes, size_t size)
{
    struct ibf_load *load;
    rb_iseq_t *iseq;
    VALUE loader_obj = TypedData_Make_Struct(0, struct ibf_load, &ibf_load_type, load);

    ibf_load_setup_bytes(load, loader_obj, bytes, size);
    iseq = ibf_load_iseq(load, 0);

    RB_GC_GUARD(loader_obj);
    return iseq;
}

VALUE
rb_iseq_ibf_load_extra_data(VALUE str)
{
    struct ibf_load *load;
    VALUE loader_obj = TypedData_Make_Struct(0, struct ibf_load, &ibf_load_type, load);
    VALUE extra_str;

    ibf_load_setup(load, loader_obj, str);
    extra_str = rb_str_new(load->global_buffer.buff + load->header->size, load->header->extra_size);
    RB_GC_GUARD(loader_obj);
    return extra_str;
}

#include "prism_compile.c"
