//===- SemaSYCL.cpp - Semantic Analysis for SYCL constructs ---------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
// This implements Semantic Analysis for SYCL constructs.
//===----------------------------------------------------------------------===//

#include "TreeTransform.h"
#include "clang/AST/AST.h"
#include "clang/AST/Mangle.h"
#include "clang/AST/QualTypeNames.h"
#include "clang/AST/RecordLayout.h"
#include "clang/AST/RecursiveASTVisitor.h"
#include "clang/Sema/Sema.h"
#include "llvm/ADT/SmallPtrSet.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/Support/FileSystem.h"
#include "llvm/Support/Path.h"
#include "llvm/Support/raw_ostream.h"
#include "clang/Analysis/CallGraph.h"

#include <array>

using namespace clang;

typedef llvm::DenseMap<DeclaratorDecl *, DeclaratorDecl *> DeclMap;

using KernelParamKind = SYCLIntegrationHeader::kernel_param_kind_t;

enum target {
  global_buffer = 2014,
  constant_buffer,
  local,
  image,
  host_buffer,
  host_image,
  image_array
};

enum RestrictKind {
  KernelGlobalVariable,
  KernelRTTI,
  KernelNonConstStaticDataVariable,
  KernelCallVirtualFunction,
  KernelCallRecursiveFunction,
  KernelCallFunctionPointer,
  KernelAllocateStorage,
  KernelUseExceptions,
  KernelUseAssembly
};

using ParamDesc = std::tuple<QualType, IdentifierInfo *, TypeSourceInfo *>;

/// Various utilities.
class Util {
public:
  using DeclContextDesc = std::pair<clang::Decl::Kind, StringRef>;

  /// Checks whether given clang type is a full specialization of the sycl
  /// accessor class.
  static bool isSyclAccessorType(const QualType &Ty);

  /// Checks whether given clang type is the sycl stream class.
  static bool isSyclStreamType(const QualType &Ty);

  /// Checks whether given clang type is declared in the given hierarchy of
  /// declaration contexts.
  /// \param Ty         the clang type being checked
  /// \param Scopes     the declaration scopes leading from the type to the
  ///     translation unit (excluding the latter)
  static bool matchQualifiedTypeName(const QualType &Ty,
                                     ArrayRef<Util::DeclContextDesc> Scopes);
};

static CXXRecordDecl *getKernelObjectType(FunctionDecl *Caller) {
  return (*Caller->param_begin())->getType()->getAsCXXRecordDecl();
}

class MarkDeviceFunction : public RecursiveASTVisitor<MarkDeviceFunction> {
public:
  MarkDeviceFunction(Sema &S)
      : RecursiveASTVisitor<MarkDeviceFunction>(), SemaRef(S) {}

  bool VisitCallExpr(CallExpr *e) {
    for (const auto &Arg : e->arguments())
      CheckSYCLType(Arg->getType(), Arg->getSourceRange());

    if (FunctionDecl *Callee = e->getDirectCallee()) {
      Callee = Callee->getCanonicalDecl();
      // Remember that all SYCL kernel functions have deferred
      // instantiation as template functions. It means that
      // all functions used by kernel have already been parsed and have
      // definitions.
      llvm::SmallPtrSet<FunctionDecl *, 10> VisitedSet;
      if (IsRecursive(Callee, Callee, VisitedSet))
        SemaRef.Diag(e->getExprLoc(), diag::err_sycl_restrict) <<
                     KernelCallRecursiveFunction;

      if (const CXXMethodDecl *Method = dyn_cast<CXXMethodDecl>(Callee))
        if (Method->isVirtual())
          SemaRef.Diag(e->getExprLoc(), diag::err_sycl_restrict) <<
                       KernelCallVirtualFunction;

      CheckSYCLType(Callee->getReturnType(), Callee->getSourceRange());

      if (FunctionDecl *Def = Callee->getDefinition()) {
        if (!Def->hasAttr<SYCLDeviceAttr>()) {
          Def->addAttr(SYCLDeviceAttr::CreateImplicit(SemaRef.Context));
          this->TraverseStmt(Def->getBody());
          SemaRef.AddSyclKernel(Def);
        }
      }
    } else {
      SemaRef.Diag(e->getExprLoc(), diag::err_sycl_restrict) <<
                   KernelCallFunctionPointer;
    }
    return true;
  }

  bool VisitCXXConstructExpr(CXXConstructExpr *E) {
    for (const auto &Arg : E->arguments())
      CheckSYCLType(Arg->getType(), Arg->getSourceRange());

    CXXConstructorDecl *Ctor = E->getConstructor();

    if (FunctionDecl *Def = Ctor->getDefinition()) {
      Def->addAttr(SYCLDeviceAttr::CreateImplicit(SemaRef.Context));
      this->TraverseStmt(Def->getBody());
      SemaRef.AddSyclKernel(Def);
    }

    const auto *ConstructedType = Ctor->getParent();
    if (ConstructedType->hasUserDeclaredDestructor()) {
      CXXDestructorDecl *Dtor = ConstructedType->getDestructor();

      if (FunctionDecl *Def = Dtor->getDefinition()) {
        Def->addAttr(SYCLDeviceAttr::CreateImplicit(SemaRef.Context));
        this->TraverseStmt(Def->getBody());
        SemaRef.AddSyclKernel(Def);
      }
    }
    return true;
  }

  bool VisitCXXTypeidExpr(CXXTypeidExpr *E) {
    SemaRef.Diag(E->getExprLoc(), diag::err_sycl_restrict) << KernelRTTI;
    return true;
  }

  bool VisitCXXDynamicCastExpr(const CXXDynamicCastExpr *E) {
    SemaRef.Diag(E->getExprLoc(), diag::err_sycl_restrict) << KernelRTTI;
    return true;
  }

  bool VisitTypedefNameDecl(TypedefNameDecl *TD) {
    CheckSYCLType(TD->getUnderlyingType(), TD->getLocation());
    return true;
  }

  bool VisitRecordDecl(RecordDecl *RD) {
    CheckSYCLType(QualType{RD->getTypeForDecl(), 0}, RD->getLocation());
    return true;
  }

  bool VisitParmVarDecl(VarDecl *VD) {
    CheckSYCLType(VD->getType(), VD->getLocation());
    return true;
  }

  bool VisitVarDecl(VarDecl *VD) {
    CheckSYCLType(VD->getType(), VD->getLocation());
    return true;
  }

  bool VisitMemberExpr(MemberExpr *E) {
    if (VarDecl *VD = dyn_cast<VarDecl>(E->getMemberDecl())) {
      bool IsConst = VD->getType().getNonReferenceType().isConstQualified();
      if (VD->isStaticDataMember() && !IsConst)
        SemaRef.Diag(E->getExprLoc(), diag::err_sycl_restrict) <<
                     KernelNonConstStaticDataVariable;
    }
    return true;
  }

  bool VisitDeclRefExpr(DeclRefExpr *E) {
    CheckSYCLType(E->getType(), E->getSourceRange());
    if (VarDecl *VD = dyn_cast<VarDecl>(E->getDecl())) {
      bool IsConst = VD->getType().getNonReferenceType().isConstQualified();
      if (!IsConst && VD->hasGlobalStorage() && !VD->isStaticLocal() &&
          !VD->isStaticDataMember() && !isa<ParmVarDecl>(VD))
        SemaRef.Diag(E->getLocation(), diag::err_sycl_restrict) <<
                     KernelGlobalVariable;
    }
    return true;
  }

  bool VisitCXXNewExpr(CXXNewExpr *E) {
  // Memory storage allocation is not allowed in kernels.
  // All memory allocation for the device is done on
  // the host using accessor classes. Consequently, the default
  // allocation operator new overloads that allocate
  // storage are disallowed in a SYCL kernel. The placement
  // new operator and any user-defined overloads that
  // do not allocate storage are permitted.
    if (FunctionDecl *FD = E->getOperatorNew()) {
      if (FD->isReplaceableGlobalAllocationFunction()) {
          SemaRef.Diag(E->getExprLoc(), diag::err_sycl_restrict) <<
                       KernelAllocateStorage;
      } else if (FunctionDecl *Def = FD->getDefinition()) {
        if (!Def->hasAttr<SYCLDeviceAttr>()) {
          Def->addAttr(SYCLDeviceAttr::CreateImplicit(SemaRef.Context));
          this->TraverseStmt(Def->getBody());
          SemaRef.AddSyclKernel(Def);
        }
      }
    }
    return true;
  }

  bool VisitCXXThrowExpr(CXXThrowExpr *E) {
    SemaRef.Diag(E->getExprLoc(), diag::err_sycl_restrict) <<
                 KernelUseExceptions;
    return true;
  }

  bool VisitCXXCatchStmt(CXXCatchStmt *S) {
    SemaRef.Diag(S->getBeginLoc(), diag::err_sycl_restrict) <<
                 KernelUseExceptions;
    return true;
  }

  bool VisitCXXTryStmt(CXXTryStmt *S) {
    SemaRef.Diag(S->getBeginLoc(), diag::err_sycl_restrict) <<
                 KernelUseExceptions;
    return true;
  }

  bool VisitSEHTryStmt(SEHTryStmt *S) {
    SemaRef.Diag(S->getBeginLoc(), diag::err_sycl_restrict) <<
                 KernelUseExceptions;
    return true;
  }

  bool VisitGCCAsmStmt(GCCAsmStmt *S) {
    SemaRef.Diag(S->getBeginLoc(), diag::err_sycl_restrict)
        << KernelUseAssembly;
    return true;
  }

  bool VisitMSAsmStmt(MSAsmStmt *S) {
    SemaRef.Diag(S->getBeginLoc(), diag::err_sycl_restrict)
        << KernelUseAssembly;
    return true;
  }

  // The call graph for this translation unit.
  CallGraph SYCLCG;
private:
  // Determines whether the function FD is recursive.
  // CalleeNode is a function which is called either directly
  // or indirectly from FD.  If recursion is detected then create
  // diagnostic notes on each function as the callstack is unwound.
  bool IsRecursive(FunctionDecl *CalleeNode, FunctionDecl *FD,
                   llvm::SmallPtrSet<FunctionDecl *, 10> VisitedSet) {
    // We're currently checking CalleeNode on a different
    // trace through the CallGraph, we avoid infinite recursion
    // by using VisitedSet to keep track of this.
    if (!VisitedSet.insert(CalleeNode).second)
      return false;
    if (CallGraphNode *N = SYCLCG.getNode(CalleeNode)) {
      for (const CallGraphNode *CI : *N) {
        if (FunctionDecl *Callee = dyn_cast<FunctionDecl>(CI->getDecl())) {
          Callee = Callee->getCanonicalDecl();
          if (Callee == FD)
            return SemaRef.Diag(FD->getSourceRange().getBegin(),
                         diag::note_sycl_recursive_function_declared_here)
                         << KernelCallRecursiveFunction;
          else if (IsRecursive(Callee, FD, VisitedSet))
            return true;
        }
      }
    }
    return false;
  }

  bool CheckSYCLType(QualType Ty, SourceRange Loc) {
    if (Ty->isVariableArrayType()) {
      SemaRef.Diag(Loc.getBegin(), diag::err_vla_unsupported);
      return false;
    }

    while (Ty->isAnyPointerType() || Ty->isArrayType())
      Ty = QualType{Ty->getPointeeOrArrayElementType(), 0};

    if (const auto *CRD = Ty->getAsCXXRecordDecl()) {
      // FIXME: this seems like a temporary fix for SYCL programs
      // that pre-declare, use, but not define OclCXX classes,
      // which are later translated into SPIRV types.
      if(!CRD->hasDefinition())
        return true;

      if (CRD->isPolymorphic()) {
        SemaRef.Diag(CRD->getLocation(), diag::err_sycl_virtual_types);
        SemaRef.Diag(Loc.getBegin(), diag::note_sycl_used_here);
        return false;
      }

      for (const auto &Field : CRD->fields()) {
        if (!CheckSYCLType(Field->getType(), Field->getSourceRange())) {
          SemaRef.Diag(Loc.getBegin(), diag::note_sycl_used_here);
          return false;
        }
      }
    } else if (const auto *RD = Ty->getAsRecordDecl()) {
      for (const auto &Field : RD->fields()) {
        if (!CheckSYCLType(Field->getType(), Field->getSourceRange())) {
          SemaRef.Diag(Loc.getBegin(), diag::note_sycl_used_here);
          return false;
        }
      }
    } else if (const auto *FPTy = dyn_cast<FunctionProtoType>(Ty)) {
      for (const auto &ParamTy : FPTy->param_types())
        if (!CheckSYCLType(ParamTy, Loc))
          return false;
      return CheckSYCLType(FPTy->getReturnType(), Loc);
    } else if (const auto *FTy = dyn_cast<FunctionType>(Ty)) {
      return CheckSYCLType(FTy->getReturnType(), Loc);
    }
    return true;
  }
  Sema &SemaRef;
};

class KernelBodyTransform : public TreeTransform<KernelBodyTransform> {
public:
  KernelBodyTransform(llvm::DenseMap<DeclaratorDecl *, DeclaratorDecl *> &Map,
                      Sema &S)
      : TreeTransform<KernelBodyTransform>(S), DMap(Map), SemaRef(S) {}
  bool AlwaysRebuild() { return true; }

  ExprResult TransformDeclRefExpr(DeclRefExpr *DRE) {
    auto Ref = dyn_cast<DeclaratorDecl>(DRE->getDecl());
    if (Ref) {
      auto NewDecl = DMap[Ref];
      if (NewDecl) {
        return DeclRefExpr::Create(
            SemaRef.getASTContext(), DRE->getQualifierLoc(),
            DRE->getTemplateKeywordLoc(), NewDecl, false, DRE->getNameInfo(),
            NewDecl->getType(), DRE->getValueKind());
      }
    }
    return DRE;
  }

private:
  DeclMap DMap;
  Sema &SemaRef;
};

static FunctionDecl *CreateSYCLKernelFunction(ASTContext &Context,
                                              StringRef Name,
                                              ArrayRef<ParamDesc> ParamDescs) {

  DeclContext *DC = Context.getTranslationUnitDecl();
  FunctionProtoType::ExtProtoInfo Info(CC_OpenCLKernel);
  QualType RetTy = Context.VoidTy;
  SmallVector<QualType, 8> ArgTys;
  // extract argument types from the descriptor array:
  std::transform(
      ParamDescs.begin(), ParamDescs.end(), std::back_inserter(ArgTys),
      [](const ParamDesc &PD) -> QualType { return std::get<0>(PD); });
  QualType FuncTy = Context.getFunctionType(RetTy, ArgTys, Info);
  DeclarationName DN = DeclarationName(&Context.Idents.get(Name));
  FunctionDecl *SYCLKernel = FunctionDecl::Create(
      Context, DC, SourceLocation(), SourceLocation(), DN, FuncTy,
      Context.getTrivialTypeSourceInfo(RetTy), SC_None);
  llvm::SmallVector<ParmVarDecl *, 16> Params;
  int i = 0;
  for (const auto &PD : ParamDescs) {
    auto P = ParmVarDecl::Create(Context, SYCLKernel, SourceLocation(),
                                 SourceLocation(), std::get<1>(PD),
                                 std::get<0>(PD), std::get<2>(PD), SC_None, 0);
    P->setScopeInfo(0, i++);
    P->setIsUsed();
    Params.push_back(P);
  }
  SYCLKernel->setParams(Params);

  SYCLKernel->addAttr(SYCLDeviceAttr::CreateImplicit(Context));
  SYCLKernel->addAttr(OpenCLKernelAttr::CreateImplicit(Context));
  SYCLKernel->addAttr(AsmLabelAttr::CreateImplicit(Context, Name));
  SYCLKernel->addAttr(ArtificialAttr::CreateImplicit(Context));

  // To see kernel in AST-dump.
  DC->addDecl(SYCLKernel);
  return SYCLKernel;
}

static CompoundStmt *
CreateSYCLKernelBody(Sema &S, FunctionDecl *KernelCallerFunc, DeclContext *DC) {
  llvm::SmallVector<Stmt *, 16> BodyStmts;
  CXXRecordDecl *LC = getKernelObjectType(KernelCallerFunc);
  assert(LC && "Kernel object must be available");
  TypeSourceInfo *TSInfo = LC->isLambda() ? LC->getLambdaTypeInfo() : nullptr;
  // Create a local kernel object (lambda or functor) assembled from the
  // incoming formal parameters
  auto KernelObjClone = VarDecl::Create(
      S.Context, DC, SourceLocation(), SourceLocation(), LC->getIdentifier(),
      QualType(LC->getTypeForDecl(), 0), TSInfo, SC_None);
  Stmt *DS = new (S.Context) DeclStmt(DeclGroupRef(KernelObjClone),
                                      SourceLocation(), SourceLocation());
  BodyStmts.push_back(DS);
  auto CloneRef =
      DeclRefExpr::Create(S.Context, NestedNameSpecifierLoc(), SourceLocation(),
                          KernelObjClone, false, DeclarationNameInfo(),
                          QualType(LC->getTypeForDecl(), 0), VK_LValue);
  auto TargetFunc = dyn_cast<FunctionDecl>(DC);
  assert(TargetFunc && "Not FunctionDecl");
  auto TargetFuncParam =
      TargetFunc->param_begin(); // Iterator to ParamVarDecl (VarDecl)
  if (TargetFuncParam) {
    for (auto Field : LC->fields()) {
      auto getExprForPointer = [](Sema &S, const QualType &paramTy,
                                  DeclRefExpr *DRE) {
        // C++ address space attribute != OpenCL address space attribute
        Expr *qualifiersCast = ImplicitCastExpr::Create(
            S.Context, paramTy, CK_NoOp, DRE, nullptr, VK_LValue);
        Expr *Res =
            ImplicitCastExpr::Create(S.Context, paramTy, CK_LValueToRValue,
                                     qualifiersCast, nullptr, VK_RValue);
        return Res;
      };
      auto getExprForRangeOrOffset = [](Sema &S, const QualType &paramTy,
                                        DeclRefExpr *DRE) {
        Expr *Res = ImplicitCastExpr::Create(S.Context, paramTy, CK_NoOp, DRE,
                                             nullptr, VK_RValue);
        return Res;
      };

      QualType FieldType = Field->getType();
      CXXRecordDecl *CRD = FieldType->getAsCXXRecordDecl();
      if (CRD && Util::isSyclAccessorType(FieldType)) {
        // Since this is an accessor next 3 TargetFuncParams including current
        // should be set in __init method: _ValueType*, range<int>, id<int>
        const size_t NumParams = 3;
        llvm::SmallVector<DeclRefExpr *, NumParams> ParamDREs(NumParams);
        auto TFP = TargetFuncParam;
        for (size_t I = 0; I < NumParams; ++TFP, ++I) {
          QualType ParamType = (*TFP)->getOriginalType();
          ParamDREs[I] = DeclRefExpr::Create(
              S.Context, NestedNameSpecifierLoc(), SourceLocation(), *TFP,
              false, DeclarationNameInfo(), ParamType, VK_LValue);
        }
        std::advance(TargetFuncParam, NumParams - 1);

        DeclAccessPair FieldDAP = DeclAccessPair::make(Field, AS_none);
        // kernel_obj.accessor
        auto AccessorME = MemberExpr::Create(
            S.Context, CloneRef, false, SourceLocation(),
            NestedNameSpecifierLoc(), SourceLocation(), Field, FieldDAP,
            DeclarationNameInfo(Field->getDeclName(), SourceLocation()),
            nullptr, Field->getType(), VK_LValue, OK_Ordinary);

        CXXMethodDecl *InitMethod = nullptr;
        for (auto Method : CRD->methods()) {
          if (Method->getNameInfo().getName().getAsString() == "__init") {
            InitMethod = Method;
            break;
          }
        }
        assert(InitMethod && "The accessor must have the __init method");

        // kernel_obj.accessor.__init
        DeclAccessPair MethodDAP = DeclAccessPair::make(InitMethod, AS_none);
        auto ME = MemberExpr::Create(
            S.Context, AccessorME, false, SourceLocation(),
            NestedNameSpecifierLoc(), SourceLocation(), InitMethod, MethodDAP,
            InitMethod->getNameInfo(), nullptr, InitMethod->getType(),
            VK_LValue, OK_Ordinary);

        // Not referenced -> not emitted
        S.MarkFunctionReferenced(SourceLocation(), InitMethod, true);

        QualType ResultTy = InitMethod->getReturnType();
        ExprValueKind VK = Expr::getValueKindForType(ResultTy);
        ResultTy = ResultTy.getNonLValueExprType(S.Context);

        // __init needs three parameter
        auto ParamItr = InitMethod->param_begin();
        // kernel_parameters
        llvm::SmallVector<Expr *, NumParams> ParamStmts;
        ParamStmts.push_back(getExprForPointer(
            S, (*(ParamItr++))->getOriginalType(), ParamDREs[0]));
        ParamStmts.push_back(getExprForRangeOrOffset(
            S, ((*ParamItr++))->getOriginalType(), ParamDREs[1]));
        ParamStmts.push_back(getExprForRangeOrOffset(
            S, ((*ParamItr++))->getOriginalType(), ParamDREs[2]));
        // kernel_obj.accessor.__init(_ValueType*, range<int>, id<int>)
        CXXMemberCallExpr *Call = CXXMemberCallExpr::Create(
            S.Context, ME, ParamStmts, ResultTy, VK, SourceLocation());
        BodyStmts.push_back(Call);
      } else if (CRD || FieldType->isBuiltinType()) {
        // If field have built-in or a structure/class type just initialize
        // this field with corresponding kernel argument using '=' binary
        // operator. The structure/class type must be copy assignable - this
        // holds because SYCL kernel lambdas capture arguments by copy.
        QualType ParamType = (*TargetFuncParam)->getOriginalType();
        auto DRE =
            DeclRefExpr::Create(S.Context, NestedNameSpecifierLoc(),
                                SourceLocation(), *TargetFuncParam, false,
                                DeclarationNameInfo(), ParamType, VK_LValue);
        DeclAccessPair FieldDAP = DeclAccessPair::make(Field, AS_none);
        auto Lhs = MemberExpr::Create(
            S.Context, CloneRef, false, SourceLocation(),
            NestedNameSpecifierLoc(), SourceLocation(), Field, FieldDAP,
            DeclarationNameInfo(Field->getDeclName(), SourceLocation()),
            nullptr, Field->getType(), VK_LValue, OK_Ordinary);
        auto Rhs = ImplicitCastExpr::Create(
            S.Context, ParamType, CK_LValueToRValue, DRE, nullptr, VK_RValue);
        // lambda.field = kernel_parameter
        Expr *Res = new (S.Context)
            BinaryOperator(Lhs, Rhs, BO_Assign, FieldType, VK_LValue,
                           OK_Ordinary, SourceLocation(), FPOptions());
        BodyStmts.push_back(Res);
      }
      TargetFuncParam++;
    }
  }
  // In function from headers lambda is function parameter, we need
  // to replace all refs to this lambda with our vardecl.
  // I used TreeTransform here, but I'm not sure that it is good solution
  // Also I used map and I'm not sure about it too.
  // TODO SYCL review the above design concerns
  Stmt *FunctionBody = KernelCallerFunc->getBody();
  DeclMap DMap;
  ParmVarDecl *KernelObjParam = *(KernelCallerFunc->param_begin());
  // DeclRefExpr with valid source location but with decl which is not marked
  // as used is invalid.
  KernelObjClone->setIsUsed();
  DMap[KernelObjParam] = KernelObjClone;
  // Without PushFunctionScope I had segfault. Maybe we also need to do pop.
  S.PushFunctionScope();
  KernelBodyTransform KBT(DMap, S);
  Stmt *NewBody = KBT.TransformStmt(FunctionBody).get();
  BodyStmts.push_back(NewBody);
  return CompoundStmt::Create(S.Context, BodyStmts, SourceLocation(),
                              SourceLocation());
}

/// Creates a kernel parameter descriptor
/// \param Src  field declaration to construct name from
/// \param Ty   the desired parameter type
/// \return     the constructed descriptor
static ParamDesc makeParamDesc(const FieldDecl *Src, QualType Ty) {
  ASTContext &Ctx = Src->getASTContext();
  std::string Name = (Twine("_arg_") + Src->getName()).str();
  return std::make_tuple(Ty, &Ctx.Idents.get(Name),
                         Ctx.getTrivialTypeSourceInfo(Ty));
}

/// \return the target of given SYCL accessor type
static target getAccessTarget(const ClassTemplateSpecializationDecl *AccTy) {
  return static_cast<target>(
      AccTy->getTemplateArgs()[3].getAsIntegral().getExtValue());
}

///
static FieldDecl *getFieldDeclByName(const CXXRecordDecl *RD,
                                     const ArrayRef<StringRef> FldExpr,
                                     uint64_t *Offset = nullptr) {

  FieldDecl *Res = nullptr;

  for (const auto FldName : FldExpr) {
    Res = nullptr;
    assert(RD && "field lookup in non-struct type");

    for (FieldDecl *Fld : RD->fields()) {
      if (Fld->getNameAsString() == FldName) {
        if (Offset) {
          const ASTRecordLayout &LO =
              RD->getASTContext().getASTRecordLayout(RD);
          *Offset += LO.getFieldOffset(Fld->getFieldIndex()) / 8;
        }
        RD = Fld->getType()->getAsCXXRecordDecl();
        Res = Fld;
        break;
      }
    }
    assert(Res && "field declaration must have been found");
  }
  return Res;
}

static void buildArgTys(ASTContext &Context, CXXRecordDecl *KernelObj,
                        SmallVectorImpl<ParamDesc> &ParamDescs) {
  const LambdaCapture *Cpt = KernelObj->captures_begin();
  auto CreateAndAddPrmDsc = [&](const FieldDecl *Fld, const QualType &ArgType) {
    // create a parameter descriptor and append it to the result
    ParamDescs.push_back(makeParamDesc(Fld, ArgType));
  };
  for (const auto *Fld : KernelObj->fields()) {
    QualType ArgTy = Fld->getType();
    if (Util::isSyclAccessorType(ArgTy)) {
      // the parameter is a SYCL accessor object
      const auto *RecordDecl = ArgTy->getAsCXXRecordDecl();
      assert(RecordDecl && "accessor must be of a record type");
      const auto *TemplateDecl =
          cast<ClassTemplateSpecializationDecl>(RecordDecl);
      // First accessor template parameter - data type
      QualType PointeeType = TemplateDecl->getTemplateArgs()[0].getAsType();
      // Fourth parameter - access target
      target AccessTarget = getAccessTarget(TemplateDecl);
      Qualifiers Quals = PointeeType.getQualifiers();
      // TODO: Support all access targets
      switch (AccessTarget) {
      case target::global_buffer:
        Quals.setAddressSpace(LangAS::opencl_global);
        break;
      case target::constant_buffer:
        Quals.setAddressSpace(LangAS::opencl_constant);
        break;
      case target::local:
        Quals.setAddressSpace(LangAS::opencl_local);
        break;
      default:
        llvm_unreachable("Unsupported access target");
      }
      // TODO: get address space from accessor template parameter.
      PointeeType =
          Context.getQualifiedType(PointeeType.getUnqualifiedType(), Quals);
      QualType PointerType = Context.getPointerType(PointeeType);

      CreateAndAddPrmDsc(Fld, PointerType);

      FieldDecl *RangeFld = getFieldDeclByName(RecordDecl, {"__impl", "Range"});
      assert(RangeFld &&
             "The accessor must contain the Range from the __impl field");
      CreateAndAddPrmDsc(RangeFld, RangeFld->getType());

      FieldDecl *OffsetFld =
          getFieldDeclByName(RecordDecl, {"__impl", "Offset"});
      assert(OffsetFld &&
             "The accessor must contain the Offset from the __impl field");
      CreateAndAddPrmDsc(OffsetFld, OffsetFld->getType());
    } else if (Util::isSyclStreamType(ArgTy)) {
      // the parameter is a SYCL stream object
      llvm_unreachable("streams not supported yet");
    } else if (ArgTy->isStructureOrClassType()) {
      if (!ArgTy->isStandardLayoutType()) {
        const DeclaratorDecl *V =
            Cpt ? cast<DeclaratorDecl>(Cpt->getCapturedVar())
                : cast<DeclaratorDecl>(Fld);
        KernelObj->getASTContext().getDiagnostics().Report(
            V->getLocation(), diag::err_sycl_non_std_layout_type);
      }
      // structure or class typed parameter - the same handling as a scalar
      CreateAndAddPrmDsc(Fld, ArgTy);
    } else if (ArgTy->isScalarType()) {
      // scalar typed parameter
      CreateAndAddPrmDsc(Fld, ArgTy);
    } else {
      llvm_unreachable("unsupported kernel parameter type");
    }
  }
}

/// Adds necessary data describing given kernel to the integration header.
/// \param H           the integration header object
/// \param Name        kernel name
/// \param NameType    type representing kernel name (first template argument
/// of
///                      single_task, parallel_for, etc)
/// \param KernelObjTy kernel object type
static void populateIntHeader(SYCLIntegrationHeader &H, const StringRef Name,
                              QualType NameType, CXXRecordDecl *KernelObjTy) {

  ASTContext &Ctx = KernelObjTy->getASTContext();
  const ASTRecordLayout &Layout = Ctx.getASTRecordLayout(KernelObjTy);
  H.startKernel(Name, NameType);

  for (const auto Fld : KernelObjTy->fields()) {
    QualType ActualArgType;
    QualType ArgTy = Fld->getType();

    // Get offset in bytes
    uint64_t Offset = Layout.getFieldOffset(Fld->getFieldIndex()) / 8;

    if (Util::isSyclAccessorType(ArgTy)) {
      // The parameter is a SYCL accessor object - split into three
      // parameters, so need to generate three descriptors.
      // ... first descriptor (translated to pointer kernel parameter):
      const auto *AccTy = ArgTy->getAsCXXRecordDecl();
      assert(AccTy && "accessor must be of a record type");
      const auto *AccTmplTy = cast<ClassTemplateSpecializationDecl>(AccTy);
      H.addParamDesc(SYCLIntegrationHeader::kind_accessor,
                     getAccessTarget(AccTmplTy), Offset);
      // ... second descriptor (translated to range kernel parameter):
      FieldDecl *RngFld =
          getFieldDeclByName(AccTy, {"__impl", "Range"}, &Offset);
      uint64_t Sz = Ctx.getTypeSizeInChars(RngFld->getType()).getQuantity();
      H.addParamDesc(SYCLIntegrationHeader::kind_std_layout,
                     static_cast<unsigned>(Sz), static_cast<unsigned>(Offset));
      // ... third descriptor (translated to id kernel parameter):
      // Get offset in bytes
      Offset = Layout.getFieldOffset(Fld->getFieldIndex()) / 8;
      FieldDecl *OffstFld =
          getFieldDeclByName(AccTy, {"__impl", "Offset"}, &Offset);
      Sz = Ctx.getTypeSizeInChars(OffstFld->getType()).getQuantity();
      H.addParamDesc(SYCLIntegrationHeader::kind_std_layout,
                     static_cast<unsigned>(Sz), static_cast<unsigned>(Offset));
    } else if (Util::isSyclStreamType(ArgTy)) {
      // the parameter is a SYCL stream object
      llvm_unreachable("streams not supported yet");
    } else if (ArgTy->isStructureOrClassType() || ArgTy->isScalarType()) {
      // the parameter is an object of standard layout type or scalar;
      // the check for standard layout is done elsewhere
      uint64_t Sz = Ctx.getTypeSizeInChars(Fld->getType()).getQuantity();
      H.addParamDesc(SYCLIntegrationHeader::kind_std_layout,
                     static_cast<unsigned>(Sz), static_cast<unsigned>(Offset));
    } else {
      llvm_unreachable("unsupported kernel parameter type");
    }
  }
}

// Removes all "(anonymous namespace)::" substrings from given string
static std::string eraseAnonNamespace(std::string S) {
  const char S1[] = "(anonymous namespace)::";

  for (auto Pos = S.find(S1); Pos != StringRef::npos; Pos = S.find(S1, Pos))
    S.erase(Pos, sizeof(S1) - 1);
  return S;
}

// Creates a mangled kernel name for given kernel name type
static std::string constructKernelName(QualType KernelNameType,
                                       ASTContext &AC) {
  SmallString<256> Result;
  llvm::raw_svector_ostream Out(Result);

  if (AC.getLangOpts().SYCLXOCCDevice) {
    // Non-mangled name... perhaps some checks are required to prevent accessing
    // non existing information, relying a lot on checks prior to this.
    return KernelNameType.getBaseTypeIdentifier()->getName();
  } else {
    std::unique_ptr<MangleContext> MC(AC.createMangleContext());

    MC->mangleTypeName(KernelNameType, Out);
    return Out.str();
  }
}

void Sema::ConstructSYCLKernel(FunctionDecl *KernelCallerFunc) {
  // TODO: Case when kernel is functor
  CXXRecordDecl *LE = getKernelObjectType(KernelCallerFunc);
  assert(LE && "invalid kernel caller");
  llvm::SmallVector<ParamDesc, 16> ParamDescs;
  buildArgTys(getASTContext(), LE, ParamDescs);
  // Get Name for our kernel.
  const TemplateArgumentList *TemplateArgs =
      KernelCallerFunc->getTemplateSpecializationArgs();
  assert(TemplateArgs && "No template argument info");
  // The first template argument always describes the kernel name - whether
  // it is lambda or functor.
  QualType KernelNameType = TypeName::getFullyQualifiedType(
      TemplateArgs->get(0).getAsType(), getASTContext(), true);
  std::string Name = constructKernelName(KernelNameType, getASTContext());
  populateIntHeader(getSyclIntegrationHeader(), Name, KernelNameType, LE);
  FunctionDecl *SYCLKernel =
      CreateSYCLKernelFunction(getASTContext(), Name, ParamDescs);
  CompoundStmt *SYCLKernelBody =
      CreateSYCLKernelBody(*this, KernelCallerFunc, SYCLKernel);
  SYCLKernel->setBody(SYCLKernelBody);
  AddSyclKernel(SYCLKernel);
  // Let's mark all called functions with SYCL Device attribute.
  MarkDeviceFunction Marker(*this);
  // Create the call graph so we can detect recursion and check the validity
  // of new operator overrides. Add the kernel function itself in case
  // it is recursive.
  Marker.SYCLCG.addToCallGraph(getASTContext().getTranslationUnitDecl());
  Marker.TraverseStmt(SYCLKernelBody);
}

// -----------------------------------------------------------------------------
// Integration header functionality implementation
// -----------------------------------------------------------------------------

/// Returns a string ID of given parameter kind - used in header
/// emission.
static const char *paramKind2Str(KernelParamKind K) {
#define CASE(x)                                                                \
  case SYCLIntegrationHeader::kind_##x:                                        \
    return "kind_" #x
  switch (K) {
    CASE(accessor);
    CASE(std_layout);
    CASE(sampler);
  default:
    return "<ERROR>";
  }
#undef CASE
}

// Emits a forward declaration
void SYCLIntegrationHeader::emitFwdDecl(raw_ostream &O, const Decl *D) {
  // wrap the declaration into namespaces if needed
  unsigned NamespaceCnt = 0;
  std::string NSStr = "";
  const DeclContext *DC = D->getDeclContext();

  while (DC) {
    auto *NS = dyn_cast_or_null<NamespaceDecl>(DC);

    if (!NS) {
      if (!DC->isTranslationUnit()) {
        const TagDecl *TD = isa<ClassTemplateDecl>(D)
                                ? cast<ClassTemplateDecl>(D)->getTemplatedDecl()
                                : dyn_cast<TagDecl>(D);

        if (TD && TD->isCompleteDefinition()) {
          // defined class constituting the kernel name is not globally
          // accessible - contradicts the spec
          Diag.Report(D->getSourceRange().getBegin(),
                      diag::err_sycl_kernel_name_class_not_top_level);
        }
      }
      break;
    }
    ++NamespaceCnt;
    NSStr.insert(0, Twine("namespace " + Twine(NS->getName()) + " { ").str());
    DC = NS->getDeclContext();
  }
  O << NSStr;
  if (NamespaceCnt > 0)
    O << "\n";
  // print declaration into a string:
  PrintingPolicy P(D->getASTContext().getLangOpts());
  P.adjustForCPlusPlusFwdDecl();
  std::string S;
  llvm::raw_string_ostream SO(S);
  D->print(SO, P);
  O << SO.str() << ";\n";

  // print closing braces for namespaces if needed
  for (unsigned I = 0; I < NamespaceCnt; ++I)
    O << "}";
  if (NamespaceCnt > 0)
    O << "\n";
}

// Emits forward declarations of classes and template classes on which
// declaration of given type depends.
// For example, consider SimpleVadd
// class specialization in parallel_for below:
//
//   template <typename T1, unsigned int N, typename ... T2>
//   class SimpleVadd;
//   ...
//   template <unsigned int N, typename T1, typename ... T2>
//   void simple_vadd(const std::array<T1, N>& VA, const std::array<T1, N>&
//   VB,
//     std::array<T1, N>& VC, int param, T2 ... varargs) {
//     ...
//     deviceQueue.submit([&](cl::sycl::handler& cgh) {
//       ...
//       cgh.parallel_for<class SimpleVadd<T1, N, T2...>>(...)
//       ...
//     }
//     ...
//   }
//   ...
//   class MyClass {...};
//   template <typename T> class MyInnerTmplClass { ... }
//   template <typename T> class MyTmplClass { ... }
//   ...
//   MyClass *c = new MyClass();
//   MyInnerTmplClass<MyClass**> c1(&c);
//   simple_vadd(A, B, C, 5, 'a', 1.f,
//     new MyTmplClass<MyInnerTmplClass<MyClass**>>(c1));
//
// it will generate the following forward declarations:
//   class MyClass;
//   template <typename T> class MyInnerTmplClass;
//   template <typename T> class MyTmplClass;
//   template <typename T1, unsigned int N, typename ...T2> class SimpleVadd;
//
void SYCLIntegrationHeader::emitForwardClassDecls(
    raw_ostream &O, QualType T, llvm::SmallPtrSetImpl<const void *> &Printed) {

  // peel off the pointer types and get the class/struct type:
  for (; T->isPointerType(); T = T->getPointeeType())
    ;
  const CXXRecordDecl *RD = T->getAsCXXRecordDecl();

  if (!RD)
    return;

  // see if this is a template specialization ...
  if (const auto *TSD = dyn_cast<ClassTemplateSpecializationDecl>(RD)) {
    // ... yes, it is template specialization:
    // - first, recurse into template parameters and emit needed forward
    //   declarations
    const TemplateArgumentList &Args = TSD->getTemplateArgs();

    for (unsigned I = 0; I < Args.size(); I++) {
      const TemplateArgument &Arg = Args[I];

      switch (Arg.getKind()) {
      case TemplateArgument::ArgKind::Type:
        emitForwardClassDecls(O, Arg.getAsType(), Printed);
        break;
      case TemplateArgument::ArgKind::Pack: {
        ArrayRef<TemplateArgument> Pack = Arg.getPackAsArray();

        for (const auto &T : Pack) {
          if (T.getKind() == TemplateArgument::ArgKind::Type) {
            emitForwardClassDecls(O, T.getAsType(), Printed);
          }
        }
        break;
      }
      case TemplateArgument::ArgKind::Template:
        llvm_unreachable("template template arguments not supported");
      default:
        break; // nop
      }
    }
    // - second, emit forward declaration for the template class being
    //   specialized
    ClassTemplateDecl *CTD = TSD->getSpecializedTemplate();
    assert(CTD && "template declaration must be available");

    if (Printed.insert(CTD).second) {
      emitFwdDecl(O, CTD);
    }
  } else if (Printed.insert(RD).second) {
    // emit forward declarations for "leaf" classes in the template parameter
    // tree;
    emitFwdDecl(O, RD);
  }
}

void SYCLIntegrationHeader::emit(raw_ostream &O) {
  O << "// This is auto-generated SYCL integration header.\n";
  O << "\n";

  O << "#include <CL/sycl/detail/kernel_desc.hpp>\n";

  O << "\n";
  O << "// Forward declarations of templated kernel function types:\n";

  llvm::SmallPtrSet<const void *, 4> Printed;
  for (const KernelDesc &K : KernelDescs) {
    emitForwardClassDecls(O, K.NameType, Printed);
  }
  O << "\n";

  O << "namespace cl {\n";
  O << "namespace sycl {\n";
  O << "namespace detail {\n";

  O << "\n";

  O << "// names of all kernels defined in the corresponding source\n";
  O << "static constexpr\n";
  O << "const char* const kernel_names[] = {\n";

  for (unsigned I = 0; I < KernelDescs.size(); I++) {
    O << "  \"" << KernelDescs[I].Name << "\"";

    if (I < KernelDescs.size() - 1)
      O << ",";
    O << "\n";
  }
  O << "};\n\n";

  O << "// array representing signatures of all kernels defined in the\n";
  O << "// corresponding source\n";
  O << "static constexpr\n";
  O << "const kernel_param_desc_t kernel_signatures[] = {\n";

  for (unsigned I = 0; I < KernelDescs.size(); I++) {
    auto &K = KernelDescs[I];
    O << "  //--- " << K.Name << "\n";

    for (const auto &P : K.Params) {
      std::string TyStr = paramKind2Str(P.Kind);
      O << "  { kernel_param_kind_t::" << TyStr << ", ";
      O << P.Info << ", " << P.Offset << " },\n";
    }
    O << "\n";
  }
  O << "};\n\n";

  O << "// indices into the kernel_signatures array, each representing a "
       "start"
       " of\n";
  O << "// kernel signature descriptor subarray of the kernel_signatures"
       " array;\n";
  O << "// the index order in this array corresponds to the kernel name order"
       " in the\n";
  O << "// kernel_names array\n";
  O << "static constexpr\n";
  O << "const unsigned kernel_signature_start[] = {\n";
  unsigned CurStart = 0;

  for (unsigned I = 0; I < KernelDescs.size(); I++) {
    auto &K = KernelDescs[I];
    O << "  " << CurStart;
    if (I < KernelDescs.size() - 1)
      O << ",";
    O << " // " << K.Name << "\n";
    CurStart += K.Params.size() + 1;
  }
  O << "};\n\n";

  O << "// Specializations of this template class encompasses information\n";
  O << "// about a kernel. The kernel is identified by the template\n";
  O << "// parameter type.\n";
  O << "template <class KernelNameType> struct KernelInfo;\n";
  O << "\n";

  O << "// Specializations of KernelInfo for kernel function types:\n";
  CurStart = 0;

  for (const KernelDesc &K : KernelDescs) {
    const size_t N = K.Params.size();
    O << "template <> struct KernelInfo<"
      << eraseAnonNamespace(K.NameType.getAsString()) << "> {\n";
    O << "  static constexpr const char* getName() { return \"" << K.Name
      << "\"; }\n";
    O << "  static constexpr unsigned getNumParams() { return " << N << "; }\n";
    O << "  static constexpr const kernel_param_desc_t& ";
    O << "getParamDesc(unsigned i) {\n";
    O << "    return kernel_signatures[i+" << CurStart << "];\n";
    O << "  }\n";
    O << "};\n";
    CurStart += N;
  }
  O << "\n";
  O << "} // namespace detail\n";
  O << "} // namespace sycl\n";
  O << "} // namespace cl\n";
  O << "\n";
}

bool SYCLIntegrationHeader::emit(const StringRef &IntHeaderName) {
  if (IntHeaderName.empty())
    return false;
  int IntHeaderFD = 0;
  std::error_code EC =
      llvm::sys::fs::openFileForWrite(IntHeaderName, IntHeaderFD);
  if (EC) {
    llvm::errs() << "Error: " << EC.message() << "\n";
    // compilation will fail on absent include file - don't need to fail here
    return false;
  }
  llvm::raw_fd_ostream Out(IntHeaderFD, true /*close in destructor*/);
  emit(Out);
  return true;
}

void SYCLIntegrationHeader::startKernel(StringRef KernelName,
                                        QualType KernelNameType) {
  KernelDescs.resize(KernelDescs.size() + 1);
  KernelDescs.back().Name = KernelName;
  KernelDescs.back().NameType = KernelNameType;
}

void SYCLIntegrationHeader::addParamDesc(kernel_param_kind_t Kind, int Info,
                                         unsigned Offset) {
  auto *K = getCurKernelDesc();
  assert(K && "no kernels");
  K->Params.push_back(KernelParamDesc());
  KernelParamDesc &PD = K->Params.back();
  PD.Kind = Kind;
  PD.Info = Info;
  PD.Offset = Offset;
}

void SYCLIntegrationHeader::endKernel() {
  // nop for now
}

SYCLIntegrationHeader::SYCLIntegrationHeader(DiagnosticsEngine &_Diag)
    : Diag(_Diag) {}

bool Util::isSyclAccessorType(const QualType &Ty) {
  static std::array<DeclContextDesc, 3> Scopes = {
      Util::DeclContextDesc{clang::Decl::Kind::Namespace, "cl"},
      Util::DeclContextDesc{clang::Decl::Kind::Namespace, "sycl"},
      Util::DeclContextDesc{clang::Decl::Kind::ClassTemplateSpecialization,
                            "accessor"}};
  return matchQualifiedTypeName(Ty, Scopes);
}

bool Util::isSyclStreamType(const QualType &Ty) {
  static std::array<DeclContextDesc, 3> Scopes = {
      Util::DeclContextDesc{clang::Decl::Kind::Namespace, "cl"},
      Util::DeclContextDesc{clang::Decl::Kind::Namespace, "sycl"},
      Util::DeclContextDesc{clang::Decl::Kind::CXXRecord, "stream"}};
  return matchQualifiedTypeName(Ty, Scopes);
}

bool Util::matchQualifiedTypeName(const QualType &Ty,
                                  ArrayRef<Util::DeclContextDesc> Scopes) {
  // The idea: check the declaration context chain starting from the type
  // itself. At each step check the context is of expected kind
  // (namespace) and name.
  const CXXRecordDecl *RecTy = Ty->getAsCXXRecordDecl();

  if (!RecTy)
    return false; // only classes/structs supported
  const auto *Ctx = dyn_cast<DeclContext>(RecTy);
  StringRef Name = "";

  for (const auto &Scope : llvm::reverse(Scopes)) {
    clang::Decl::Kind DK = Ctx->getDeclKind();

    if (DK != Scope.first)
      return false;

    switch (DK) {
    case clang::Decl::Kind::ClassTemplateSpecialization:
      // ClassTemplateSpecializationDecl inherits from CXXRecordDecl
    case clang::Decl::Kind::CXXRecord:
      Name = cast<CXXRecordDecl>(Ctx)->getName();
      break;
    case clang::Decl::Kind::Namespace:
      Name = cast<NamespaceDecl>(Ctx)->getName();
      break;
    default:
      llvm_unreachable("matchQualifiedTypeName: decl kind not supported");
    }
    if (Name != Scope.second)
      return false;
    Ctx = Ctx->getParent();
  }
  return Ctx->isTranslationUnit();
}
