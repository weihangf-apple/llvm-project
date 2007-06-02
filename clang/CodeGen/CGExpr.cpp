//===--- CGExpr.cpp - Emit LLVM Code from Expressions ---------------------===//
//
//                     The LLVM Compiler Infrastructure
//
// This file was developed by Chris Lattner and is distributed under
// the University of Illinois Open Source License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//
//
// This contains code to emit Expr nodes as LLVM code.
//
//===----------------------------------------------------------------------===//

#include "CodeGenFunction.h"
#include "clang/AST/AST.h"
#include "llvm/Constants.h"
#include "llvm/DerivedTypes.h"
using namespace llvm;
using namespace clang;
using namespace CodeGen;

//===--------------------------------------------------------------------===//
//                        Miscellaneous Helper Methods
//===--------------------------------------------------------------------===//

/// EvaluateScalarValueToBool - Evaluate the specified expression value to a
/// boolean (i1) truth value.  This is equivalent to "Val == 0".
Value *CodeGenFunction::EvaluateScalarValueToBool(ExprResult Val, QualType Ty) {
  Ty = Ty.getCanonicalType();
  Value *Result;
  if (const BuiltinType *BT = dyn_cast<BuiltinType>(Ty)) {
    switch (BT->getKind()) {
    default: assert(0 && "Unknown scalar value");
    case BuiltinType::Bool:
      Result = Val.getVal();
      // Bool is already evaluated right.
      assert(Result->getType() == llvm::Type::Int1Ty &&
             "Unexpected bool value type!");
      return Result;
    case BuiltinType::Char:
    case BuiltinType::SChar:
    case BuiltinType::UChar:
    case BuiltinType::Short:
    case BuiltinType::UShort:
    case BuiltinType::Int:
    case BuiltinType::UInt:
    case BuiltinType::Long:
    case BuiltinType::ULong:
    case BuiltinType::LongLong:
    case BuiltinType::ULongLong:
      // Code below handles simple integers.
      break;
    case BuiltinType::Float:
    case BuiltinType::Double:
    case BuiltinType::LongDouble: {
      // Compare against 0.0 for fp scalars.
      Result = Val.getVal();
      llvm::Value *Zero = Constant::getNullValue(Result->getType());
      // FIXME: llvm-gcc produces a une comparison: validate this is right.
      Result = Builder.CreateFCmpUNE(Result, Zero, "tobool");
      return Result;
    }
      
    case BuiltinType::FloatComplex:
    case BuiltinType::DoubleComplex:
    case BuiltinType::LongDoubleComplex:
      assert(0 && "comparisons against complex not implemented yet");
    }
  } else {
    assert((isa<PointerType>(Ty) || 
           cast<TagType>(Ty)->getDecl()->getKind() == Decl::Enum) &&
           "Unknown scalar type");
    // Code below handles this fine.
  }
  
  // Usual case for integers, pointers, and enums: compare against zero.
  Result = Val.getVal();
  
  // Because of the type rules of C, we often end up computing a logical value,
  // then zero extending it to int, then wanting it as a logical value again.
  // Optimize this common case.
  if (llvm::ZExtInst *ZI = dyn_cast<ZExtInst>(Result)) {
    if (ZI->getOperand(0)->getType() == llvm::Type::Int1Ty) {
      Result = ZI->getOperand(0);
      ZI->eraseFromParent();
      return Result;
    }
  }
  
  llvm::Value *Zero = Constant::getNullValue(Result->getType());
  return Builder.CreateICmpNE(Result, Zero, "tobool");
}

//===----------------------------------------------------------------------===//
//                         LValue Expression Emission
//===----------------------------------------------------------------------===//

LValue CodeGenFunction::EmitLValue(const Expr *E) {
  switch (E->getStmtClass()) {
  default:
    printf("Unimplemented lvalue expr!\n");
    E->dump();
    return LValue::getAddr(UndefValue::get(
                              llvm::PointerType::get(llvm::Type::Int32Ty)));

  case Expr::DeclRefExprClass: return EmitDeclRefLValue(cast<DeclRefExpr>(E));
  }
}


LValue CodeGenFunction::EmitDeclRefLValue(const DeclRefExpr *E) {
  const Decl *D = E->getDecl();
  if (isa<BlockVarDecl>(D)) {
    Value *V = LocalDeclMap[D];
    assert(V && "BlockVarDecl not entered in LocalDeclMap?");
    return LValue::getAddr(V);
  }
  assert(0 && "Unimp declref");
}

//===--------------------------------------------------------------------===//
//                             Expression Emission
//===--------------------------------------------------------------------===//

ExprResult CodeGenFunction::EmitExpr(const Expr *E) {
  assert(E && "Null expression?");
  
  switch (E->getStmtClass()) {
  default:
    printf("Unimplemented expr!\n");
    E->dump();
    return ExprResult::get(UndefValue::get(llvm::Type::Int32Ty));
    
  // l-values.
  case Expr::DeclRefExprClass: {
    // FIXME: EnumConstantDecl's are not lvalues.
    LValue LV = EmitLValue(E);
    // FIXME: this is silly.
    assert(!LV.isBitfield());
    return ExprResult::get(Builder.CreateLoad(LV.getAddress(), "tmp"));
  }
    
  // Leaf expressions.
  case Expr::IntegerLiteralClass:
    return EmitIntegerLiteral(cast<IntegerLiteral>(E)); 
    
  // Operators.  
  case Expr::ParenExprClass:
    return EmitExpr(cast<ParenExpr>(E)->getSubExpr());
  case Expr::UnaryOperatorClass:
    return EmitUnaryOperator(cast<UnaryOperator>(E));
  case Expr::BinaryOperatorClass:
    return EmitBinaryOperator(cast<BinaryOperator>(E));
  }
  
}

ExprResult CodeGenFunction::EmitIntegerLiteral(const IntegerLiteral *E) {
  return ExprResult::get(ConstantInt::get(E->getValue()));
}

//===--------------------------------------------------------------------===//
//                          Unary Operator Emission
//===--------------------------------------------------------------------===//

ExprResult CodeGenFunction::EmitExprWithUsualUnaryConversions(const Expr *E, 
                                                              QualType &ResTy) {
  ResTy = E->getType().getCanonicalType();
  
  if (isa<FunctionType>(ResTy)) { // C99 6.3.2.1p4
    // Functions are promoted to their address.
    ResTy = getContext().getPointerType(ResTy);
    return ExprResult::get(EmitLValue(E).getAddress());
  } else if (const ArrayType *ary = dyn_cast<ArrayType>(ResTy)) {
    // C99 6.3.2.1p3
    ResTy = getContext().getPointerType(ary->getElementType());
    
    // FIXME: For now we assume that all source arrays map to LLVM arrays.  This
    // will not true when we add support for VLAs.
    llvm::Value *V = EmitLValue(E).getAddress();  // Bitfields can't be arrays.
    
    assert(isa<llvm::PointerType>(V->getType()) &&
           isa<llvm::ArrayType>(cast<llvm::PointerType>(V->getType())
                                ->getElementType()) &&
           "Doesn't support VLAs yet!");
    llvm::Constant *Idx0 = llvm::ConstantInt::get(llvm::Type::Int32Ty, 0);
    V = Builder.CreateGEP(V, Idx0, Idx0, "arraydecay");
    return ExprResult::get(V);
  } else if (ResTy->isPromotableIntegerType()) { // C99 6.3.1.1p2
    // FIXME: this probably isn't right, pending clarification from Steve.
    llvm::Value *Val = EmitExpr(E).getVal();
    
    // FIXME: this doesn't handle 'char'!.
    
    // If the input is a signed integer, sign extend to the destination.
    if (ResTy->isSignedIntegerType()) {
      Val = Builder.CreateSExt(Val, LLVMIntTy, "promote");
    } else {
      // This handles unsigned types, including bool.
      Val = Builder.CreateZExt(Val, LLVMIntTy, "promote");
    }
    ResTy = getContext().IntTy;
    
    return ExprResult::get(Val);
  }
  
  // Otherwise, this is a float, double, int, struct, etc.
  return EmitExpr(E);
}


ExprResult CodeGenFunction::EmitUnaryOperator(const UnaryOperator *E) {
  switch (E->getOpcode()) {
  default:
    printf("Unimplemented unary expr!\n");
    E->dump();
    return ExprResult::get(UndefValue::get(llvm::Type::Int32Ty));
  case UnaryOperator::LNot: return EmitUnaryLNot(E);
  }
}

/// C99 6.5.3.3
ExprResult CodeGenFunction::EmitUnaryLNot(const UnaryOperator *E) {
  QualType ResTy;
  ExprResult Op = EmitExprWithUsualUnaryConversions(E->getSubExpr(), ResTy);
  
  // Compare to zero.
  Value *BoolVal = EvaluateScalarValueToBool(Op, ResTy);
  
  // Invert value.
  // TODO: Could dynamically modify easy computations here.  For example, if
  // the operand is an icmp ne, turn into icmp eq.
  BoolVal = Builder.CreateNot(BoolVal, "lnot");
  
  // ZExt result to int.
  const llvm::Type *ResLTy = ConvertType(E->getType(), E->getOperatorLoc());
  return ExprResult::get(Builder.CreateZExt(BoolVal, ResLTy, "lnot.ext"));
}


//===--------------------------------------------------------------------===//
//                         Binary Operator Emission
//===--------------------------------------------------------------------===//

// FIXME describe.
void CodeGenFunction::EmitUsualArithmeticConversions(const BinaryOperator *E,
                                                     ExprResult &LHS, 
                                                     ExprResult &RHS) {
  QualType LHSType, RHSType;
  LHS = EmitExprWithUsualUnaryConversions(E->getLHS(), LHSType);
  RHS = EmitExprWithUsualUnaryConversions(E->getRHS(), RHSType);

}


ExprResult CodeGenFunction::EmitBinaryOperator(const BinaryOperator *E) {
  switch (E->getOpcode()) {
  default:
    printf("Unimplemented expr!\n");
    E->dump();
    return ExprResult::get(UndefValue::get(llvm::Type::Int32Ty));
  case BinaryOperator::Add: return EmitBinaryAdd(E);
  }
}


ExprResult CodeGenFunction::EmitBinaryAdd(const BinaryOperator *E) {
  ExprResult LHS, RHS;
  
  EmitUsualArithmeticConversions(E, LHS, RHS);

  
  return ExprResult::get(Builder.CreateAdd(LHS.getVal(), RHS.getVal(), "tmp"));
}