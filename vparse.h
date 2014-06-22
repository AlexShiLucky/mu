/*
 *  Parser for V
 */

#ifndef V_PARSE
#define V_PARSE

#include "var.h"
#include "vm.h"

// Representation of parsing product
enum vproduct {
    VP_NONE = 0,
    VP_REF  = 1,
    VP_VAL  = 2
};

// State of a parse
struct vstate {
    str_t *pos;
    str_t *end;
    ref_t *ref;

    uint8_t indirect;
    uint8_t paren;

    int tok;
    var_t val;

    int ins;
    uint8_t *bcode;
    tbl_t *vars;
    int (*encode)(uint8_t *, enum vop, uint16_t);
};


// Parses V source code and evaluates the result
int vparse(struct vstate *);


#endif
