//===----------------------------------------------------------------------===//
// 
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//

#include "SimplifyCallExpr.h"

#include <sstream>

#include "clang/AST/RecursiveASTVisitor.h"
#include "clang/Basic/SourceManager.h"

#include "RewriteUtils.h"
#include "TransformationManager.h"

using namespace clang;
using namespace llvm;

static const char *DescriptionMsg =
"Simplify a call expression to a comma expression. \
Replace function arguments with: \n\
  * 0 for integer and pointer arguments \n\
  * a global temp variable for structs/unions \
and also append a representative return value which is the \
the last inner expression of the comma expression. \n\
For example, assume we have a function foo: \n\
  int foo(int x, int *y, struct z) \
Then this transformation will transform \n\
  foo(i, p, s); \n\
to \n\
  (0, 0, tmp_var, 0);\n";

static RegisterTransformation<SimplifyCallExpr>
         Trans("simplify-callexpr", DescriptionMsg);

class SimplifyCallExprVisitor : public 
  RecursiveASTVisitor<SimplifyCallExprVisitor> {

public:

  explicit SimplifyCallExprVisitor(SimplifyCallExpr *Instance)
    : ConsumerInstance(Instance),
      CurrentFD(NULL)
  { }

  bool VisitCallExpr(CallExpr *CE);

  bool VisitFunctionDecl(FunctionDecl *FD);

private:

  SimplifyCallExpr *ConsumerInstance;

  const FunctionDecl *CurrentFD;
};

bool SimplifyCallExprVisitor::VisitCallExpr(CallExpr *CE)
{
  ConsumerInstance->ValidInstanceNum++;
  if (ConsumerInstance->TransformationCounter != 
      ConsumerInstance->ValidInstanceNum)
    return true;
  
  ConsumerInstance->TheCallExpr = CE;
  ConsumerInstance->CurrentFD = CurrentFD;
  return true;
}

bool SimplifyCallExprVisitor::VisitFunctionDecl(FunctionDecl *FD)
{
  CurrentFD = FD;
  return true;
}

void SimplifyCallExpr::Initialize(ASTContext &context) 
{
  Context = &context;
  SrcManager = &Context->getSourceManager();
  CollectionVisitor = new SimplifyCallExprVisitor(this);
  NameQueryWrap = 
    new TransNameQueryWrap(RewriteUtils::getTmpVarNamePrefix());
  TheRewriter.setSourceMgr(Context->getSourceManager(), 
                           Context->getLangOptions());
}

void SimplifyCallExpr::HandleTopLevelDecl(DeclGroupRef D) 
{
  for (DeclGroupRef::iterator I = D.begin(), E = D.end(); I != E; ++I) {
    FunctionDecl *FD = dyn_cast<FunctionDecl>(*I);
    if (FD && FD->isThisDeclarationADefinition())
      CollectionVisitor->TraverseDecl(FD);
  }
}
 
void SimplifyCallExpr::HandleTranslationUnit(ASTContext &Ctx)
{
  if (QueryInstanceOnly)
    return;

  if (TransformationCounter > ValidInstanceNum) {
    TransError = TransMaxInstanceError;
    return;
  }

  TransAssert(TheCallExpr && "NULL TheCallExpr!");
  TransAssert(CurrentFD && "NULL CurrentFD");

  Ctx.getDiagnostics().setSuppressAllDiagnostics(false);

  NameQueryWrap->TraverseDecl(Ctx.getTranslationUnitDecl());
  NamePostfix = NameQueryWrap->getMaxNamePostfix() + 1;

  replaceCallExpr();

  if (Ctx.getDiagnostics().hasErrorOccurred() ||
      Ctx.getDiagnostics().hasFatalErrorOccurred())
    TransError = TransInternalError;
}

void SimplifyCallExpr::handleOneArgStr(const Expr *Arg, std::string &Str)
{
  Str = "0";
  const Type *ArgT = Arg->getType().getTypePtr();
  if (!ArgT->isUnionType() && !ArgT->isStructureType())
    return;

  RewriteUtils::getTmpTransName(NamePostfix, Str);
  NamePostfix++;

  std::string TmpVarStr = Str;
  Arg->getType().getAsStringInternal(TmpVarStr, Context->getPrintingPolicy());
  TmpVarStr += ";\n";
  RewriteUtils::insertStringBeforeFunc(CurrentFD, TmpVarStr, 
                                    &TheRewriter, SrcManager);
}

void SimplifyCallExpr::replaceCallExpr(void)
{
  std::string CommaStr("");
  unsigned int NumArg = TheCallExpr->getNumArgs();
  if (NumArg == 0) {
    RewriteUtils::replaceExpr(TheCallExpr, CommaStr, 
                              &TheRewriter, SrcManager);
    return;
  }

  const Expr *Arg = TheCallExpr->getArg(0);
  std::string ArgStr;
  handleOneArgStr(Arg, ArgStr);
  CommaStr += ("(" + ArgStr);

  for (unsigned int I = 1; I < NumArg; ++I) {
    Arg = TheCallExpr->getArg(I);
    handleOneArgStr(Arg, ArgStr);
    CommaStr += ("," + ArgStr);
  }

  QualType RVQualType = TheCallExpr->getType();
  const Type *RVType = RVQualType.getTypePtr();
  if (RVType->isVoidType()) {
    // Nothing to do
  }
  else if (RVType->isUnionType() || RVType->isStructureType()) {
    std::string RVStr("");
    RewriteUtils::getTmpTransName(NamePostfix, RVStr);
    NamePostfix++;

    CommaStr += ("," + RVStr);
    RVQualType.getAsStringInternal(RVStr, Context->getPrintingPolicy());
    RVStr += ";\n";
    RewriteUtils::insertStringBeforeFunc(CurrentFD, RVStr, 
                                    &TheRewriter, SrcManager);
  }
  else {
    CommaStr += ",0";
  }

  CommaStr += ")";
  RewriteUtils::replaceExpr(TheCallExpr, CommaStr, 
                            &TheRewriter, SrcManager);
}

SimplifyCallExpr::~SimplifyCallExpr(void)
{
  if (CollectionVisitor)
    delete CollectionVisitor;
}

