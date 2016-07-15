//===-- binops.cpp --------------------------------------------------------===//
//
//                         LDC – the LLVM D compiler
//
// This file is distributed under the BSD-style LDC license. See the LICENSE
// file for details.
//
//===----------------------------------------------------------------------===//

#include "gen/binops.h"
#include "declaration.h"
#include "gen/complex.h"
#include "gen/dvalue.h"
#include "gen/irstate.h"
#include "gen/llvm.h"
#include "gen/llvmhelpers.h"
#include "gen/logger.h"
#include "gen/tollvm.h"

//////////////////////////////////////////////////////////////////////////////

dinteger_t undoStrideMul(Loc &loc, Type *t, dinteger_t offset) {
  assert(t->ty == Tpointer);
  d_uns64 elemSize = t->nextOf()->size(loc);
  assert((offset % elemSize) == 0 &&
         "Expected offset by an integer amount of elements");

  return offset / elemSize;
}

//////////////////////////////////////////////////////////////////////////////

namespace {
/// Tries to remove a MulExp by a constant value of baseSize from e. Returns
/// NULL if not possible.
Expression *extractNoStrideInc(Expression *e, d_uns64 baseSize, bool &negate) {
  MulExp *mul;
  while (true) {
    if (e->op == TOKneg) {
      negate = !negate;
      e = static_cast<NegExp *>(e)->e1;
      continue;
    }

    if (e->op == TOKmul) {
      mul = static_cast<MulExp *>(e);
      break;
    }

    return nullptr;
  }

  if (!mul->e2->isConst()) {
    return nullptr;
  }
  dinteger_t stride = mul->e2->toInteger();

  if (stride != baseSize) {
    return nullptr;
  }

  return mul->e1;
}

DValue *emitPointerOffset(Loc loc, Expression *base, Expression *offset,
                          bool negateOffset, Type *resultType) {
  // The operand emitted by the frontend is in units of bytes, and not
  // pointer elements. We try to undo this before resorting to
  // temporarily bitcasting the pointer to i8.

  DRValue *baseVal = toElem(base)->getRVal();

  LLValue *noStrideInc = nullptr;
  if (offset->isConst()) {
    dinteger_t byteOffset = offset->toInteger();
    if (byteOffset == 0) {
      Logger::println("offset is zero");
      return baseVal;
    }
    noStrideInc = DtoConstSize_t(undoStrideMul(loc, baseVal->type, byteOffset));
  } else if (Expression *inc = extractNoStrideInc(
                 offset, baseVal->type->nextOf()->size(loc), negateOffset)) {
    noStrideInc = DtoRVal(inc);
  }

  if (noStrideInc) {
    if (negateOffset) {
      noStrideInc = gIR->ir->CreateNeg(noStrideInc);
    }
    return new DImValue(baseVal->type,
                        DtoGEP1(DtoRVal(baseVal), noStrideInc, false));
  }

  // This might not actually be generated by the frontend, just to be safe.
  LLValue *inc = DtoRVal(offset);
  if (negateOffset) {
    inc = gIR->ir->CreateNeg(inc);
  }
  LLValue *bytePtr = DtoBitCast(DtoRVal(baseVal), getVoidPtrType());
  DValue *result = new DImValue(Type::tvoidptr, DtoGEP1(bytePtr, inc, false));
  return DtoCast(loc, result, resultType);
}
}

//////////////////////////////////////////////////////////////////////////////

namespace {
struct RVals {
  DRValue *lhs, *rhs;
};

RVals evalSides(Expression *lhs, Expression *rhs, bool loadLhsAfterRhs) {
  RVals rvals;
  DValue *lhsVal = toElem(lhs);

  if (!loadLhsAfterRhs) {
    rvals.lhs = lhsVal->getRVal();
    rvals.rhs = toElem(rhs)->getRVal();
  } else {
    rvals.rhs = toElem(rhs)->getRVal();
    rvals.lhs = lhsVal->getRVal();
  }

  return rvals;
}
}

//////////////////////////////////////////////////////////////////////////////

DValue *binAdd(Loc &loc, Type *type, Expression *lhs, Expression *rhs,
               bool loadLhsAfterRhs) {
  Type *lhsType = lhs->type->toBasetype();
  Type *rhsType = rhs->type->toBasetype();

  if (lhsType != rhsType && lhsType->ty == Tpointer && rhsType->isintegral()) {
    Logger::println("Adding integer to pointer");
    return emitPointerOffset(loc, lhs, rhs, false, type);
  }

  auto rvals = evalSides(lhs, rhs, loadLhsAfterRhs);

  if (type->iscomplex())
    return DtoComplexAdd(loc, type, rvals.lhs, rvals.rhs);

  LLValue *l = DtoRVal(rvals.lhs);
  LLValue *r = DtoRVal(rvals.rhs);
  LLValue *res = (lhs->type->isfloating() ? gIR->ir->CreateFAdd(l, r)
                                          : gIR->ir->CreateAdd(l, r));

  return new DImValue(lhs->type, res);
}

//////////////////////////////////////////////////////////////////////////////

DValue *binMin(Loc &loc, Type *type, Expression *lhs, Expression *rhs,
               bool loadLhsAfterRhs) {
  Type *lhsType = lhs->type->toBasetype();
  Type *rhsType = rhs->type->toBasetype();

  if (lhsType != rhsType && lhsType->ty == Tpointer && rhsType->isintegral()) {
    Logger::println("Subtracting integer from pointer");
    return emitPointerOffset(loc, lhs, rhs, true, type);
  }

  auto rvals = evalSides(lhs, rhs, loadLhsAfterRhs);

  if (lhsType->ty == Tpointer && rhsType->ty == Tpointer) {
    LLValue *l = DtoRVal(rvals.lhs);
    LLValue *r = DtoRVal(rvals.rhs);
    LLType *llSizeT = DtoSize_t();
    l = gIR->ir->CreatePtrToInt(l, llSizeT);
    r = gIR->ir->CreatePtrToInt(r, llSizeT);
    LLValue *diff = gIR->ir->CreateSub(l, r);
    LLType *llType = DtoType(type);
    if (diff->getType() != llType)
      diff = gIR->ir->CreateIntToPtr(diff, llType);
    return new DImValue(type, diff);
  }

  if (type->iscomplex())
    return DtoComplexMin(loc, type, rvals.lhs, rvals.rhs);

  LLValue *l = DtoRVal(rvals.lhs);
  LLValue *r = DtoRVal(rvals.rhs);
  LLValue *res = (lhs->type->isfloating() ? gIR->ir->CreateFSub(l, r)
                                          : gIR->ir->CreateSub(l, r));

  return new DImValue(lhs->type, res);
}

//////////////////////////////////////////////////////////////////////////////

DValue *binMul(Loc &loc, Type *type, Expression *lhs, Expression *rhs,
               bool loadLhsAfterRhs) {
  auto rvals = evalSides(lhs, rhs, loadLhsAfterRhs);

  if (type->iscomplex())
    return DtoComplexMul(loc, type, rvals.lhs, rvals.rhs);

  LLValue *l = DtoRVal(rvals.lhs);
  LLValue *r = DtoRVal(rvals.rhs);
  LLValue *res = (lhs->type->isfloating() ? gIR->ir->CreateFMul(l, r)
                                          : gIR->ir->CreateMul(l, r));

  return new DImValue(type, res);
}

//////////////////////////////////////////////////////////////////////////////

DValue *binDiv(Loc &loc, Type *type, Expression *lhs, Expression *rhs,
               bool loadLhsAfterRhs) {
  auto rvals = evalSides(lhs, rhs, loadLhsAfterRhs);

  if (type->iscomplex())
    return DtoComplexDiv(loc, type, rvals.lhs, rvals.rhs);

  Type *t = lhs->type;
  LLValue *l = DtoRVal(rvals.lhs);
  LLValue *r = DtoRVal(rvals.rhs);
  LLValue *res;
  if (t->isfloating()) {
    res = gIR->ir->CreateFDiv(l, r);
  } else if (!isLLVMUnsigned(t)) {
    res = gIR->ir->CreateSDiv(l, r);
  } else {
    res = gIR->ir->CreateUDiv(l, r);
  }

  return new DImValue(type, res);
}

//////////////////////////////////////////////////////////////////////////////

DValue *binMod(Loc &loc, Type *type, Expression *lhs, Expression *rhs,
               bool loadLhsAfterRhs) {
  auto rvals = evalSides(lhs, rhs, loadLhsAfterRhs);

  if (type->iscomplex())
    return DtoComplexMod(loc, type, rvals.lhs, rvals.rhs);

  Type *t = lhs->type;
  LLValue *l = DtoRVal(rvals.lhs);
  LLValue *r = DtoRVal(rvals.rhs);
  LLValue *res;
  if (t->isfloating()) {
    res = gIR->ir->CreateFRem(l, r);
  } else if (!isLLVMUnsigned(t)) {
    res = gIR->ir->CreateSRem(l, r);
  } else {
    res = gIR->ir->CreateURem(l, r);
  }

  return new DImValue(type, res);
}

//////////////////////////////////////////////////////////////////////////////

LLValue *DtoBinNumericEquals(Loc &loc, DValue *lhs, DValue *rhs, TOK op) {
  assert(op == TOKequal || op == TOKnotequal || op == TOKidentity ||
         op == TOKnotidentity);
  Type *t = lhs->type->toBasetype();
  assert(t->isfloating());
  Logger::println("numeric equality");

  LLValue *res = nullptr;
  if (t->iscomplex()) {
    Logger::println("complex");
    res = DtoComplexEquals(loc, op, lhs, rhs);
  } else if (t->isfloating()) {
    Logger::println("floating");
    res = DtoBinFloatsEquals(loc, lhs, rhs, op);
  }

  assert(res);
  return res;
}

//////////////////////////////////////////////////////////////////////////////

LLValue *DtoBinFloatsEquals(Loc &loc, DValue *lhs, DValue *rhs, TOK op) {
  LLValue *res = nullptr;
  if (op == TOKequal) {
    res = gIR->ir->CreateFCmpOEQ(DtoRVal(lhs), DtoRVal(rhs));
  } else if (op == TOKnotequal) {
    res = gIR->ir->CreateFCmpUNE(DtoRVal(lhs), DtoRVal(rhs));
  } else {
    llvm::ICmpInst::Predicate cmpop;
    if (op == TOKidentity) {
      cmpop = llvm::ICmpInst::ICMP_EQ;
    } else {
      cmpop = llvm::ICmpInst::ICMP_NE;
    }

    LLValue *sz = DtoConstSize_t(getTypeStoreSize(DtoType(lhs->type)));
    LLValue *val = DtoMemCmp(makeLValue(loc, lhs), makeLValue(loc, rhs), sz);
    res = gIR->ir->CreateICmp(cmpop, val,
                              LLConstantInt::get(val->getType(), 0, false));
  }
  assert(res);
  return res;
}
