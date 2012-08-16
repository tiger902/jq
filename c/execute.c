#include <jansson.h>
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>


#include "opcode.h"
#include "bytecode.h"
#include "compile.h"

#include "forkable_stack.h"


typedef struct {
  json_t* value;
  int pathidx;
} stackval;


json_t** pathbuf;
int pathsize; // number of allocated elements


int path_push(stackval sv, json_t* val) {
  int pos = sv.pathidx;
  assert(pos <= pathsize);
  assert(pos >= 0);
  if (pos == pathsize) {
    pathsize = pathsize ? pathsize * 2 : 100;
    pathbuf = realloc(pathbuf, sizeof(pathbuf[0]) * pathsize);
  }
  pathbuf[pos] = val;
  return pos + 1;
}

stackval stackval_replace(stackval value, json_t* newjs) {
  stackval s = {newjs, value.pathidx};
  return s;
}


// Probably all uses of this function are bugs
stackval stackval_root(json_t* v) {
  stackval s = {v, 0};
  return s;
}

struct forkable_stack data_stk;
typedef struct {
  FORKABLE_STACK_HEADER;
  stackval sv;
} data_stk_elem;

data_stk_elem* stk_push_frame(int n) {
  return forkable_stack_push(&data_stk, sizeof(data_stk_elem) * n);
}

void stk_pop_frame(int n) {
  forkable_stack_pop(&data_stk, sizeof(data_stk_elem) * n);
}

void stk_push(stackval val) {
  data_stk_elem* s = stk_push_frame(1);
  s->sv = val;
}

stackval stk_pop() {
  data_stk_elem* s = forkable_stack_peek(&data_stk, sizeof(data_stk_elem));
  stackval sv = s->sv;
  forkable_stack_pop(&data_stk, sizeof(data_stk_elem));
  return sv;
}


typedef struct {
  FORKABLE_STACK_HEADER;
  struct bytecode* bc;
  data_stk_elem* fp;
  uint16_t* pc;
} call_stk_elem;
struct forkable_stack call_stk;


struct forkpoint {
  FORKABLE_STACK_HEADER;
  struct forkable_stack_state saved_data_stack;
  struct forkable_stack_state saved_call_stack;
};

struct forkable_stack fork_stk;

void stack_save(){
  struct forkpoint* fork = forkable_stack_push(&fork_stk, sizeof(struct forkpoint));
  forkable_stack_save(&data_stk, &fork->saved_data_stack);
  forkable_stack_save(&call_stk, &fork->saved_call_stack);
}

void stack_switch() {
  struct forkpoint* fork = forkable_stack_peek(&fork_stk, sizeof(struct forkpoint));
  forkable_stack_switch(&data_stk, &fork->saved_data_stack);
  forkable_stack_switch(&call_stk, &fork->saved_call_stack);
}

void stack_restore(){
  struct forkpoint* fork = forkable_stack_peek(&fork_stk, sizeof(struct forkpoint));
  forkable_stack_restore(&data_stk, &fork->saved_data_stack);
  forkable_stack_restore(&call_stk, &fork->saved_call_stack);
  forkable_stack_pop(&fork_stk, sizeof(struct forkpoint));
}

#define stack_push stk_push
#define stack_pop stk_pop

#define ON_BACKTRACK(op) ((op)+NUM_OPCODES)

json_t* jq_next() {
  assert(!forkable_stack_empty(&call_stk));
  call_stk_elem* ctx = forkable_stack_peek(&call_stk, sizeof(call_stk_elem));
  struct bytecode* bc = ctx->bc;
  uint16_t* pc = ctx->pc;
  data_stk_elem* fp = ctx->fp;
  json_t* cpool = bc->constants;

  json_t* cfunc_input[MAX_CFUNCTION_ARGS] = {0};
  json_t* cfunc_output[MAX_CFUNCTION_ARGS] = {0};
  int backtracking = 0;
  while (1) {

    dump_operation(bc, pc);

    uint16_t opcode = *pc++;

    printf("\t");
    const struct opcode_description* opdesc = opcode_describe(opcode);
    data_stk_elem* param = forkable_stack_peek(&data_stk, sizeof(data_stk_elem));
    for (int i=0; i<opdesc->stack_in; i++) {
      json_dumpf(param->sv.value, stdout, JSON_ENCODE_ANY);
      if (i < opdesc->stack_in-1) printf(" | ");
      param = forkable_stack_peek_next(&data_stk, param, sizeof(data_stk_elem));
    }

    if (backtracking) {
      printf("\t<backtracking>");
      opcode = ON_BACKTRACK(opcode);
      backtracking = 0;
    }

    printf("\n");

    switch (opcode) {
    default: assert(0 && "invalid instruction");

    case LOADK: {
      json_t* v = json_array_get(cpool, *pc++);
      assert(v);
      stack_push(stackval_replace(stack_pop(), v));
      break;
    }

    case DUP: {
      stackval v = stack_pop();
      stack_push(v);
      stack_push(v);
      break;
    }

    case SWAP: {
      stackval a = stack_pop();
      stackval b = stack_pop();
      stack_push(a);
      stack_push(b);
      break;
    }
      
    case POP: {
      stack_pop();
      break;
    }

    case APPEND: {
      // FIXME paths
      json_t* v = stack_pop().value;
      json_t* array = stack_pop().value;
      array = json_copy(array);
      json_array_append(array, v);
      stack_push(stackval_root(array));
      break;
    }

    case INSERT: {
      stackval stktop = stack_pop();
      json_t* v = stack_pop().value;
      json_t* k = stack_pop().value;
      stackval objv = stack_pop();
      assert(json_is_string(k));
      assert(json_is_object(objv.value));
      json_t* obj = json_copy(objv.value);
      json_object_set(obj, json_string_value(k), v);
      assert(json_is_object(obj));
      stack_push(stackval_replace(objv, obj));
      stack_push(stktop);
      break;
    }

    case LOADV: {
      uint16_t v = *pc++;
      stack_push(stackval_replace(stack_pop(), fp[v].sv.value));
      break;
    }

    case STOREV: {
      uint16_t v = *pc++;
      stackval val = stack_pop();
      printf("V%d = ", v);
      json_dumpf(val.value, stdout, JSON_ENCODE_ANY);
      printf("\n");
      fp[v].sv.value = val.value;
      break;
    }

#if 0
    case DISPLAY: {
      stackval sv = stack_pop();
      if (sv.value) {
        json_dumpf(sv.value, stdout, JSON_ENCODE_ANY);
      } else {
        printf("#ERROR");
      }
      printf(" - ");
      for (int i = 0; i < sv.pathidx; i++) {
        printf("/");
        json_dumpf(pathbuf[i], stdout, JSON_ENCODE_ANY);
      }
      printf("\n");
      return;
    }
#endif

    case INDEX: {
      stackval t = stack_pop();
      json_t* k = stack_pop().value;
      stackval v;
      if (json_is_string(k)) {
        v.value = json_object_get(t.value, json_string_value(k));
      } else if (json_is_integer(k)) {
        v.value = json_array_get(t.value, json_integer_value(k));
      } else {
        assert(0 && "key neither string nor int");
      }
      if (v.value) {
        v.pathidx = path_push(t, k);
        stack_push(v);
      } else {
        assert(0 && "bad lookup");
      }
      break;
    }


    case JUMP: {
      uint16_t offset = *pc++;
      pc += offset;
      break;
    }

    case EACH: 
      stack_push(stackval_root(json_integer(0)));
      // fallthrough
    case ON_BACKTRACK(EACH): {
      json_t* idxj = stack_pop().value;
      int idx = json_integer_value(idxj);
      stackval array = stack_pop();
      if (idx >= json_array_size(array.value)) {
        goto do_backtrack;
      } else {
        stack_save();
        stack_push(array);
        stack_push(stackval_root(json_integer(idx+1)));
        call_stk_elem* ctx = forkable_stack_push(&call_stk, sizeof(call_stk_elem));
        ctx->bc = bc;
        ctx->fp = fp;
        ctx->pc = pc - 1;
        stack_switch();
        
        stackval sv = {json_array_get(array.value, idx), 
                       path_push(array, json_integer(idx))};
        stack_push(sv);
      }
      break;
    }

    do_backtrack:
    case BACKTRACK: {
      if (forkable_stack_empty(&fork_stk)) {
        return 0;
      }
      stack_restore();
      call_stk_elem* ctx = forkable_stack_peek(&call_stk, sizeof(call_stk_elem));
      bc = ctx->bc;
      pc = ctx->pc;
      fp = ctx->fp;
      cpool = bc->constants;
      forkable_stack_pop(&call_stk, sizeof(call_stk_elem));
      backtracking = 1;
      break;
    }

    case FORK: {
      stack_save();
      call_stk_elem* ctx = forkable_stack_push(&call_stk, sizeof(call_stk_elem));
      ctx->bc = bc;
      ctx->fp = fp;
      ctx->pc = pc - 1;
      stack_switch();
      pc++; // skip offset this time
      break;
    }
    case ON_BACKTRACK(FORK): {
      uint16_t offset = *pc++;
      pc += offset;
      break;
    }

    case YIELD: {
      json_t* value = stack_pop().value;
      call_stk_elem* ctx = forkable_stack_push(&call_stk, sizeof(call_stk_elem));
      ctx->bc = bc;
      ctx->fp = fp;
      ctx->pc = pc;
      return value;
    }
      
    case CALL_BUILTIN_1_1: {
      stackval top = stack_pop();
      cfunc_input[0] = top.value;
      struct cfunction* func = &bc->globals->cfunctions[*pc++];
      printf(" call %s\n", func->name);
      func->fptr(cfunc_input, cfunc_output);
      stack_push(stackval_replace(top, cfunc_output[0]));
      break;
    }
      
      
    }
  }
}


void jq_init(struct bytecode* bc, json_t* input) {
  forkable_stack_init(&data_stk, sizeof(stackval) * 100); // FIXME: lower this number, see if it breaks
  forkable_stack_init(&call_stk, 1024); // FIXME: lower this number, see if it breaks
  forkable_stack_init(&fork_stk, 1024); // FIXME: lower this number, see if it breaks
  
  data_stk_elem* frame = stk_push_frame(bc->framesize);
  
  stack_push(stackval_root(input));
  call_stk_elem* ctx = forkable_stack_push(&call_stk, sizeof(call_stk_elem));
  ctx->pc = bc->code;
  ctx->bc = bc;
  ctx->fp = frame;
}

void run_program(struct bytecode* bc) {
  jq_init(bc, json_loadf(stdin, 0, 0));
  json_t* result;
  while ((result = jq_next())) {
    json_dumpf(result, stdout, JSON_ENCODE_ANY);
    printf("\n");
  }
  printf("end of results\n");

  //assert(frame == stack_top_frame(bc->framesize));
  //stk_pop_frame(bc->framesize);
  //assert(stackpos == 0);
}
