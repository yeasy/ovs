/*
 * Copyright (c) 2015, 2016 Nicira, Inc.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at:
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#ifndef OVN_EXPR_H
#define OVN_EXPR_H 1

/* OVN matching expression tree
 * ============================
 *
 * The data structures here form an abstract expression tree for matching
 * expressions in OVN.
 *
 * The abstract syntax tree representation of a matching expression is one of:
 *
 *    - A Boolean literal ("true" or "false").
 *
 *    - A comparison of a field (or part of a field) against a constant
 *      with one of the operators == != < <= > >=.
 *
 *    - The logical AND or OR of two or more matching expressions.
 *
 * Literals and comparisons are called "terminal" nodes, logical AND and OR
 * nodes are "nonterminal" nodes.
 *
 * The syntax for expressions includes a few other concepts that are not part
 * of the abstract syntax tree.  In these examples, x is a field, a, b, and c
 * are constants, and e1 and e2 are arbitrary expressions:
 *
 *    - Logical NOT.  The parser implements NOT by inverting the sense of the
 *      operand: !(x == a) becomes x != a, !(e1 && e2) becomes !e1 || !e2, and
 *      so on.
 *
 *    - Set membership.  The parser translates x == {a, b, c} into
 *      x == a || x == b || x == c.
 *
 *    - Reversed comparisons.  The parser translates a < x into x > a.
 *
 *    - Range expressions.  The parser translates a < x < b into
 *      x > a && x < b.
 */

#include "classifier.h"
#include "lex.h"
#include "hmap.h"
#include "openvswitch/list.h"
#include "match.h"
#include "meta-flow.h"

struct ds;
struct ofpbuf;
struct shash;
struct simap;

/* "Measurement level" of a field.  See "Level of Measurement" in the large
 * comment on struct expr_symbol below for more information. */
enum expr_level {
    EXPR_L_NOMINAL,

    /* Boolean values are nominal, however because of their simple nature OVN
     * can allow both equality and inequality tests on them. */
    EXPR_L_BOOLEAN,

    /* Ordinal values can at least be ordered on a scale.  OVN allows equality
     * and inequality and relational tests on ordinal values.  These are the
     * fields on which OVS allows bitwise matching. */
    EXPR_L_ORDINAL
};

const char *expr_level_to_string(enum expr_level);

/* A symbol.
 *
 *
 * Name
 * ====
 *
 * Every symbol must have a name.  To be useful, the name must satisfy the
 * lexer's syntax for an identifier.
 *
 *
 * Width
 * =====
 *
 * Every symbol has a width.  For integer symbols, this is the number of bits
 * in the value; for string symbols, this is 0.
 *
 *
 * Types
 * =====
 *
 * There are three kinds of symbols:
 *
 *   Fields:
 *
 *     One might, for example, define a field named "vlan.tci" to refer to
 *     MFF_VLAN_TCI.  For integer fields, 'field' specifies the referent; for
 *     string fields, 'field' is NULL.
 *
 *     'expansion' is NULL.
 *
 *     Integer fields can be nominal or ordinal (see below).  String fields are
 *     always nominal.
 *
 *   Subfields:
 *
 *     'expansion' is a string that specifies a subfield of some larger field,
 *     e.g. "vlan.tci[0..11]" for a field that represents a VLAN VID.
 *
 *     'field' is NULL.
 *
 *     Only ordinal fields (see below) may have subfields, and subfields are
 *     always ordinal.
 *
 *   Predicates:
 *
 *     A predicate is an arbitrary Boolean expression that can be used in an
 *     expression much like a 1-bit field.  'expansion' specifies the Boolean
 *     expression, e.g. "ip4" might expand to "eth.type == 0x800".  The
 *     expansion of a predicate might refer to other predicates, e.g. "icmp4"
 *     might expand to "ip4 && ip4.proto == 1".
 *
 *     'field' is NULL.
 *
 *     A predicate whose expansion refers to any nominal field or predicate
 *     (see below) is nominal; other predicates have Boolean level of
 *     measurement.
 *
 *
 * Level of Measurement
 * ====================
 *
 * See http://en.wikipedia.org/wiki/Level_of_measurement for the statistical
 * concept on which this classification is based.  There are three levels:
 *
 *   Ordinal:
 *
 *     In statistics, ordinal values can be ordered on a scale.  Here, we
 *     consider a field (or subfield) to be ordinal if its bits can be examined
 *     individually.  This is true for the OpenFlow fields that OpenFlow or
 *     Open vSwitch makes "maskable".
 *
 *     OVN supports all the usual arithmetic relations (== != < <= > >=) on
 *     ordinal fields and their subfields, because all of these can be
 *     implemented as collections of bitwise tests.
 *
 *   Nominal:
 *
 *     In statistics, nominal values cannot be usefully compared except for
 *     equality.  This is true of OpenFlow port numbers, Ethernet types, and IP
 *     protocols are examples: all of these are just identifiers assigned
 *     arbitrarily with no deeper meaning.  In OpenFlow and Open vSwitch, bits
 *     in these fields generally aren't individually addressable.
 *
 *     OVN only supports arithmetic tests for equality on nominal fields,
 *     because OpenFlow and Open vSwitch provide no way for a flow to
 *     efficiently implement other comparisons on them.  (A test for inequality
 *     can be sort of built out of two flows with different priorities, but OVN
 *     matching expressions always generate flows with a single priority.)
 *
 *     String fields are always nominal.
 *
 *   Boolean:
 *
 *     A nominal field that has only two values, 0 and 1, is somewhat
 *     exceptional, since it is easy to support both equality and inequality
 *     tests on such a field: either one can be implemented as a test for 0 or
 *     1.
 *
 *     Only predicates (see above) have a Boolean level of measurement.
 *
 *     This isn't a standard level of measurement.
 *
 *
 * Prerequisites
 * =============
 *
 * Any symbol can have prerequisites, which are specified as a string giving an
 * additional expression that must be true whenever the symbol is referenced.
 * For example, the "icmp4.type" symbol might have prerequisite "icmp4", which
 * would cause an expression "icmp4.type == 0" to be interpreted as "icmp4.type
 * == 0 && icmp4", which would in turn expand to "icmp4.type == 0 && eth.type
 * == 0x800 && ip4.proto == 1" (assuming "icmp4" is a predicate defined as
 * suggested under "Types" above).
 *
 *
 * Crossproducting
 * ===============
 *
 * Ordinarily OVN is willing to consider using any field as a dimension in the
 * Open vSwitch "conjunctive match" extension (see ovs-ofctl(8)).  However,
 * some fields can't actually be used that way because they are necessary as
 * prerequisites.  For example, from an expression like "tcp.src == {1,2,3}
 * && tcp.dst == {4,5,6}", OVN might naturally generate flows like this:
 *
 *     conj_id=1,actions=...
 *     ip,actions=conjunction(1,1/3)
 *     ip6,actions=conjunction(1,1/3)
 *     tp_src=1,actions=conjunction(1,2/3)
 *     tp_src=2,actions=conjunction(1,2/3)
 *     tp_src=3,actions=conjunction(1,2/3)
 *     tp_dst=4,actions=conjunction(1,3/3)
 *     tp_dst=5,actions=conjunction(1,3/3)
 *     tp_dst=6,actions=conjunction(1,3/3)
 *
 * but that's not valid because any flow that matches on tp_src or tp_dst must
 * also match on either ip or ip6.  Thus, one would mark eth.type as "must
 * crossproduct", to force generating flows like this:
 *
 *     conj_id=1,actions=...
 *     ip,tp_src=1,actions=conjunction(1,1/2)
 *     ip,tp_src=2,actions=conjunction(1,1/2)
 *     ip,tp_src=3,actions=conjunction(1,1/2)
 *     ip6,tp_src=1,actions=conjunction(1,1/2)
 *     ip6,tp_src=2,actions=conjunction(1,1/2)
 *     ip6,tp_src=3,actions=conjunction(1,1/2)
 *     ip,tp_dst=4,actions=conjunction(1,2/2)
 *     ip,tp_dst=5,actions=conjunction(1,2/2)
 *     ip,tp_dst=6,actions=conjunction(1,2/2)
 *     ip6,tp_dst=4,actions=conjunction(1,2/2)
 *     ip6,tp_dst=5,actions=conjunction(1,2/2)
 *     ip6,tp_dst=6,actions=conjunction(1,2/2)
 *
 * which are acceptable.
 */
struct expr_symbol {
    char *name;
    int width;

    const struct mf_field *field;
    char *expansion;

    enum expr_level level;

    char *prereqs;
    bool must_crossproduct;
};

struct expr_symbol *expr_symtab_add_field(struct shash *symtab,
                                          const char *name, enum mf_field_id,
                                          const char *prereqs,
                                          bool must_crossproduct);
struct expr_symbol *expr_symtab_add_subfield(struct shash *symtab,
                                             const char *name,
                                             const char *prereqs,
                                             const char *subfield);
struct expr_symbol *expr_symtab_add_string(struct shash *symtab,
                                           const char *name, enum mf_field_id,
                                           const char *prereqs);
struct expr_symbol *expr_symtab_add_predicate(struct shash *symtab,
                                              const char *name,
                                              const char *expansion);
void expr_symtab_destroy(struct shash *symtab);

/* Expression type. */
enum expr_type {
    EXPR_T_CMP,                 /* Compare symbol with constant. */
    EXPR_T_AND,                 /* Logical AND of 2 or more subexpressions. */
    EXPR_T_OR,                  /* Logical OR of 2 or more subexpressions. */
    EXPR_T_BOOLEAN,             /* True or false constant. */
};

/* Relational operator. */
enum expr_relop {
    EXPR_R_EQ,                  /* == */
    EXPR_R_NE,                  /* != */
    EXPR_R_LT,                  /* < */
    EXPR_R_LE,                  /* <= */
    EXPR_R_GT,                  /* > */
    EXPR_R_GE,                  /* >= */
};
const char *expr_relop_to_string(enum expr_relop);
bool expr_relop_from_token(enum lex_type type, enum expr_relop *relop);

/* An abstract syntax tree for a matching expression.
 *
 * The expression code maintains and relies on a few important invariants:
 *
 *     - An EXPR_T_AND or EXPR_T_OR node never has a child of the same type.
 *       (Any such children could be merged into their parent.)  A node may
 *       have grandchildren of its own type.
 *
 *       As a consequence, every nonterminal node at the same distance from the
 *       root has the same type.
 *
 *     - EXPR_T_AND and EXPR_T_OR nodes must have at least two children.
 *
 *     - An EXPR_T_CMP node always has a nonzero mask, and never has a 1-bit
 *       in its value in a position where the mask is a 0-bit.
 *
 * The expr_honors_invariants() function can check invariants. */
struct expr {
    struct ovs_list node;       /* In parent EXPR_T_AND or EXPR_T_OR if any. */
    enum expr_type type;        /* Expression type. */

    union {
        /* EXPR_T_CMP.
         *
         * The symbol is on the left, e.g. "field < constant". */
        struct {
            const struct expr_symbol *symbol;
            enum expr_relop relop;

            union {
                char *string;
                struct {
                    union mf_subvalue value;
                    union mf_subvalue mask;
                };
            };
        } cmp;

        /* EXPR_T_AND, EXPR_T_OR. */
        struct ovs_list andor;

        /* EXPR_T_BOOLEAN. */
        bool boolean;
    };
};

struct expr *expr_create_boolean(bool b);
struct expr *expr_create_andor(enum expr_type);
struct expr *expr_combine(enum expr_type, struct expr *a, struct expr *b);

static inline struct expr *
expr_from_node(const struct ovs_list *node)
{
    return CONTAINER_OF(node, struct expr, node);
}

void expr_format(const struct expr *, struct ds *);
void expr_print(const struct expr *);
struct expr *expr_parse(struct lexer *, const struct shash *symtab,
                        char **errorp);
struct expr *expr_parse_string(const char *, const struct shash *symtab,
                               char **errorp);

struct expr *expr_clone(struct expr *);
void expr_destroy(struct expr *);

struct expr *expr_annotate(struct expr *, const struct shash *symtab,
                           char **errorp);
struct expr *expr_simplify(struct expr *);
struct expr *expr_normalize(struct expr *);

bool expr_honors_invariants(const struct expr *);
bool expr_is_simplified(const struct expr *);
bool expr_is_normalized(const struct expr *);

/* Converting expressions to OpenFlow flows. */

/* An OpenFlow match generated from a Boolean expression.  See
 * expr_to_matches() for more information. */
struct expr_match {
    struct hmap_node hmap_node;
    struct match match;
    struct cls_conjunction *conjunctions;
    size_t n, allocated;
};

uint32_t expr_to_matches(const struct expr *,
                         bool (*lookup_port)(const void *aux,
                                             const char *port_name,
                                             unsigned int *portp),
                         const void *aux,
                         struct hmap *matches);
void expr_matches_destroy(struct hmap *matches);
void expr_matches_print(const struct hmap *matches, FILE *);

/* Action parsing helper. */

char *expr_parse_assignment(struct lexer *lexer, const struct shash *symtab,
                            bool (*lookup_port)(const void *aux,
                                                const char *port_name,
                                                unsigned int *portp),
                            const void *aux, struct ofpbuf *ofpacts,
                            struct expr **prereqsp);
char *expr_parse_field(struct lexer *, int n_bits, bool rw,
                       const struct shash *symtab, struct mf_subfield *,
                       struct expr **prereqsp);

#endif /* ovn/expr.h */
