//===--- TypeCheckDecl.cpp - Type Checking for Declarations ---------------===//
//
// This source file is part of the Swift.org open source project
//
// Copyright (c) 2014 - 2016 Apple Inc. and the Swift project authors
// Licensed under Apache License v2.0 with Runtime Library Exception
//
// See http://swift.org/LICENSE.txt for license information
// See http://swift.org/CONTRIBUTORS.txt for the list of Swift project authors
//
//===----------------------------------------------------------------------===//
//
// This file implements semantic analysis for declarations.
//
//===----------------------------------------------------------------------===//

#include "CodeSynthesis.h"
#include "ConstraintSystem.h"
#include "DerivedConformances.h"
#include "TypeChecker.h"
#include "GenericTypeResolver.h"
#include "MiscDiagnostics.h"
#include "swift/AST/ArchetypeBuilder.h"
#include "swift/AST/ASTPrinter.h"
#include "swift/AST/ASTVisitor.h"
#include "swift/AST/ASTWalker.h"
#include "swift/AST/Attr.h"
#include "swift/AST/Expr.h"
#include "swift/AST/ForeignErrorConvention.h"
#include "swift/AST/NameLookup.h"
#include "swift/AST/PrettyStackTrace.h"
#include "swift/AST/ReferencedNameTracker.h"
#include "swift/AST/TypeWalker.h"
#include "swift/Parse/Lexer.h"
#include "swift/Sema/IterativeTypeChecker.h"
#include "swift/Serialization/SerializedModuleLoader.h"
#include "swift/Strings.h"
#include "swift/Basic/Defer.h"
#include "llvm/ADT/APFloat.h"
#include "llvm/ADT/APInt.h"
#include "llvm/ADT/APSInt.h"
#include "llvm/ADT/SmallString.h"
#include "llvm/ADT/StringExtras.h"
#include "llvm/ADT/Twine.h"
using namespace swift;

namespace {

/// Used during enum raw value checking to identify duplicate raw values.
/// Character, string, float, and integer literals are all keyed by value.
/// Float and integer literals are additionally keyed by numeric equivalence.
struct RawValueKey {
  enum class Kind : uint8_t {
    String, Float, Int, Tombstone, Empty
  } kind;
  
  struct IntValueTy {
    uint64_t v0;
    uint64_t v1;

    IntValueTy(const APInt &bits) {
      APInt bits128 = bits.sextOrTrunc(128);
      assert(bits128.getBitWidth() <= 128);
      const uint64_t *data = bits128.getRawData();
      v0 = data[0];
      v1 = data[1];
    }
  };

  struct FloatValueTy {
    uint64_t v0;
    uint64_t v1;
  };

  // FIXME: doesn't accommodate >64-bit or signed raw integer or float values.
  union {
    StringRef stringValue;
    uint32_t charValue;
    IntValueTy intValue;
    FloatValueTy floatValue;
  };
  
  explicit RawValueKey(LiteralExpr *expr) {
    switch (expr->getKind()) {
    case ExprKind::IntegerLiteral:
      kind = Kind::Int;
      intValue = IntValueTy(cast<IntegerLiteralExpr>(expr)->getValue());
      return;
    case ExprKind::FloatLiteral: {
      APFloat value = cast<FloatLiteralExpr>(expr)->getValue();
      llvm::APSInt asInt(127, /*isUnsigned=*/false);
      bool isExact = false;
      APFloat::opStatus status =
          value.convertToInteger(asInt, APFloat::rmTowardZero, &isExact);
      if (asInt.getBitWidth() <= 128 && status == APFloat::opOK && isExact) {
        kind = Kind::Int;
        intValue = IntValueTy(asInt);
        return;
      }
      APInt bits = value.bitcastToAPInt();
      const uint64_t *data = bits.getRawData();
      if (bits.getBitWidth() == 80) {
        kind = Kind::Float;
        floatValue = FloatValueTy{ data[0], data[1] };
      } else {
        assert(bits.getBitWidth() == 64);
        kind = Kind::Float;
        floatValue = FloatValueTy{ data[0], 0 };
      }
      return;
    }
    case ExprKind::StringLiteral:
      kind = Kind::String;
      stringValue = cast<StringLiteralExpr>(expr)->getValue();
      return;
    default:
      llvm_unreachable("not a valid literal expr for raw value");
    }
  }
  
  explicit RawValueKey(Kind k) : kind(k) {
    assert((k == Kind::Tombstone || k == Kind::Empty)
           && "this ctor is only for creating DenseMap special values");
  }
};
  
/// Used during enum raw value checking to identify the source of a raw value,
/// which may have been derived by auto-incrementing, for diagnostic purposes.
struct RawValueSource {
  /// The decl that has the raw value.
  EnumElementDecl *sourceElt;
  /// If the sourceDecl didn't explicitly name a raw value, this is the most
  /// recent preceding decl with an explicit raw value. This is used to
  /// diagnose 'autoincrementing from' messages.
  EnumElementDecl *lastExplicitValueElt;
};

} // end anonymous namespace

namespace llvm {

template<>
class DenseMapInfo<RawValueKey> {
public:
  static RawValueKey getEmptyKey() {
    return RawValueKey(RawValueKey::Kind::Empty);
  }
  static RawValueKey getTombstoneKey() {
    return RawValueKey(RawValueKey::Kind::Tombstone);
  }
  static unsigned getHashValue(RawValueKey k) {
    switch (k.kind) {
    case RawValueKey::Kind::Float:
      // Hash as bits. We want to treat distinct but IEEE-equal values as not
      // equal.
      return DenseMapInfo<uint64_t>::getHashValue(k.floatValue.v0) ^
             DenseMapInfo<uint64_t>::getHashValue(k.floatValue.v1);
    case RawValueKey::Kind::Int:
      return DenseMapInfo<uint64_t>::getHashValue(k.intValue.v0) &
             DenseMapInfo<uint64_t>::getHashValue(k.intValue.v1);
    case RawValueKey::Kind::String:
      return llvm::HashString(k.stringValue);
    case RawValueKey::Kind::Empty:
    case RawValueKey::Kind::Tombstone:
      return 0;
    }
  }
  static bool isEqual(RawValueKey a, RawValueKey b) {
    if (a.kind != b.kind)
      return false;
    switch (a.kind) {
    case RawValueKey::Kind::Float:
      // Hash as bits. We want to treat distinct but IEEE-equal values as not
      // equal.
      return a.floatValue.v0 == b.floatValue.v0 &&
             a.floatValue.v1 == b.floatValue.v1;
    case RawValueKey::Kind::Int:
      return a.intValue.v0 == b.intValue.v0 &&
             a.intValue.v1 == b.intValue.v1;
    case RawValueKey::Kind::String:
      return a.stringValue.equals(b.stringValue);
    case RawValueKey::Kind::Empty:
    case RawValueKey::Kind::Tombstone:
      return true;
    }
  }
};
  
} // end llvm namespace

/// Determine whether the given declaration can inherit a class.
static bool canInheritClass(Decl *decl) {
  // Classes can inherit from a class.
  if (isa<ClassDecl>(decl))
    return true;

  // Generic type parameters can inherit a class.
  if (isa<GenericTypeParamDecl>(decl))
    return true;

  // Associated types can inherit a class.
  if (isa<AssociatedTypeDecl>(decl))
    return true;

  return false;
}

/// Retrieve the declared type of a type declaration or extension.
static Type getDeclaredType(Decl *decl) {
  if (auto typeDecl = dyn_cast<TypeDecl>(decl))
    return typeDecl->getDeclaredType();
  return cast<ExtensionDecl>(decl)->getExtendedType();
}

// Add implicit conformances to the given declaration.
static void addImplicitConformances(
              TypeChecker &tc, Decl *decl,
              llvm::SmallSetVector<ProtocolDecl *, 4> &allProtocols) {
  if (auto nominal = dyn_cast<NominalTypeDecl>(decl)) {
    SmallVector<ProtocolDecl *, 2> protocols;
    nominal->getImplicitProtocols(protocols);
    allProtocols.insert(protocols.begin(), protocols.end());
  }
}

/// Check that the declaration attributes are ok.
static void validateAttributes(TypeChecker &TC, Decl *D);

void TypeChecker::resolveSuperclass(ClassDecl *classDecl) {
  IterativeTypeChecker ITC(*this);
  ITC.satisfy(requestTypeCheckSuperclass(classDecl));
}

void TypeChecker::resolveRawType(EnumDecl *enumDecl) {
  IterativeTypeChecker ITC(*this);
  ITC.satisfy(requestTypeCheckRawType(enumDecl));
}

void TypeChecker::resolveInheritedProtocols(ProtocolDecl *protocol) {
  IterativeTypeChecker ITC(*this);
  ITC.satisfy(requestInheritedProtocols(protocol));
}

void TypeChecker::resolveInheritanceClause(
       llvm::PointerUnion<TypeDecl *, ExtensionDecl *> decl) {
  IterativeTypeChecker ITC(*this);
  unsigned numInherited;
  if (auto ext = decl.dyn_cast<ExtensionDecl *>()) {
    numInherited = ext->getInherited().size();
  } else {
    numInherited = decl.get<TypeDecl *>()->getInherited().size();
  }

  for (unsigned i = 0; i != numInherited; ++i) {
    ITC.satisfy(requestResolveInheritedClauseEntry({ decl, i }));
  }
}

/// check the inheritance clause of a type declaration or extension thereof.
///
/// This routine validates all of the types in the parsed inheritance clause,
/// recording the superclass (if any and if allowed) as well as the protocols
/// to which this type declaration conforms.
void TypeChecker::checkInheritanceClause(Decl *decl,
                                         GenericTypeResolver *resolver) {
  TypeResolutionOptions options;
  DeclContext *DC;
  if (auto nominal = dyn_cast<NominalTypeDecl>(decl)) {
    DC = nominal;
    options |= TR_GenericSignature | TR_InheritanceClause;
  } else if (auto ext = dyn_cast<ExtensionDecl>(decl)) {
    DC = ext;
    options |= TR_GenericSignature | TR_InheritanceClause;
  } else if (isa<GenericTypeParamDecl>(decl)) {
    // For generic parameters, we want name lookup to look at just the
    // signature of the enclosing entity.
    DC = decl->getDeclContext();
    if (auto nominal = dyn_cast<NominalTypeDecl>(DC)) {
      DC = nominal;
      options |= TR_GenericSignature;
    } else if (auto ext = dyn_cast<ExtensionDecl>(DC)) {
      DC = ext;
      options |= TR_GenericSignature;
    } else if (auto func = dyn_cast<AbstractFunctionDecl>(DC)) {
      DC = func;
      options |= TR_GenericSignature;
    } else if (!DC->isModuleScopeContext()) {
      // Skip the generic parameter's context entirely.
      DC = DC->getParent();
    }
  } else {
    DC = decl->getDeclContext();
  }

  // Establish a default generic type resolver.
  PartialGenericTypeToArchetypeResolver defaultResolver(*this);
  if (!resolver)
    resolver = &defaultResolver;

  MutableArrayRef<TypeLoc> inheritedClause;

  // If we already checked the inheritance clause, don't do so again.
  if (auto type = dyn_cast<TypeDecl>(decl)) {
    if (type->checkedInheritanceClause())
      return;

    // This breaks infinite recursion, which will be diagnosed separately.
    type->setCheckedInheritanceClause();
    inheritedClause = type->getInherited();
  } else {
    auto ext = cast<ExtensionDecl>(decl);

    validateExtension(ext);

    if (ext->checkedInheritanceClause())
      return;

    // This breaks infinite recursion, which will be diagnosed separately.
    ext->setCheckedInheritanceClause();
    inheritedClause = ext->getInherited();

    // Protocol extensions cannot have inheritance clauses.
    if (ext->getExtendedType()->is<ProtocolType>()) {
      if (!inheritedClause.empty()) {
        diagnose(ext->getLoc(), diag::extension_protocol_inheritance,
                 ext->getExtendedType())
          .highlight(SourceRange(inheritedClause.front().getSourceRange().Start,
                                 inheritedClause.back().getSourceRange().End));
        ext->setInherited({ });
        return;
      }
    }

    // Constrained extensions cannot have inheritance clauses.
    if (!inheritedClause.empty() &&
        ext->getGenericParams() &&
        ext->getGenericParams()->hasTrailingWhereClause()) {
      diagnose(ext->getLoc(), diag::extension_constrained_inheritance,
               ext->getExtendedType())
      .highlight(SourceRange(inheritedClause.front().getSourceRange().Start,
                             inheritedClause.back().getSourceRange().End));
      ext->setInherited({ });
    }
  }

  // Check all of the types listed in the inheritance clause.
  Type superclassTy;
  SourceRange superclassRange;
  llvm::SmallSetVector<ProtocolDecl *, 4> allProtocols;
  llvm::SmallDenseMap<CanType, SourceRange> inheritedTypes;
  addImplicitConformances(*this, decl, allProtocols);
  for (unsigned i = 0, n = inheritedClause.size(); i != n; ++i) {
    auto &inherited = inheritedClause[i];

    {
      bool iBTC = decl->isBeingTypeChecked();
      decl->setIsBeingTypeChecked();
      defer {decl->setIsBeingTypeChecked(iBTC); };

      // Validate the type.
      if (validateType(inherited, DC, options, resolver)) {
        inherited.setInvalidType(Context);
        continue;
      }
    }

    auto inheritedTy = inherited.getType();

    // If this is an error type, ignore it.
    if (inheritedTy->is<ErrorType>())
      continue;

    // Retrieve the interface type for this inherited type.
    if (DC->isGenericContext() && DC->isTypeContext()) {
      inheritedTy = ArchetypeBuilder::mapTypeOutOfContext(DC, inheritedTy);
    }

    // Check whether we inherited from the same type twice.
    CanType inheritedCanTy = inheritedTy->getCanonicalType();
    auto knownType = inheritedTypes.find(inheritedCanTy);
    if (knownType != inheritedTypes.end()) {
      SourceLoc afterPriorLoc
        = Lexer::getLocForEndOfToken(Context.SourceMgr,
                                     inheritedClause[i-1].getSourceRange().End);
      SourceLoc afterMyEndLoc
        = Lexer::getLocForEndOfToken(Context.SourceMgr,
                                     inherited.getSourceRange().End);

      diagnose(inherited.getSourceRange().Start,
               diag::duplicate_inheritance, inheritedTy)
        .fixItRemoveChars(afterPriorLoc, afterMyEndLoc)
        .highlight(knownType->second);
      inherited.setInvalidType(Context);
      continue;
    }
    inheritedTypes[inheritedCanTy] = inherited.getSourceRange();

    // If this is a protocol or protocol composition type, record the
    // protocols.
    if (inheritedTy->isExistentialType()) {
      SmallVector<ProtocolDecl *, 4> protocols;
      inheritedTy->isExistentialType(protocols);

      allProtocols.insert(protocols.begin(), protocols.end());
      continue;
    }
    
    // If this is an enum inheritance clause, check for a raw type.
    if (isa<EnumDecl>(decl)) {
      // Check if we already had a raw type.
      if (superclassTy) {
        diagnose(inherited.getSourceRange().Start,
                 diag::multiple_enum_raw_types, superclassTy, inheritedTy)
          .highlight(superclassRange);
        inherited.setInvalidType(Context);
        continue;
      }
      
      // If this is not the first entry in the inheritance clause, complain.
      if (i > 0) {
        SourceLoc afterPriorLoc
          = Lexer::getLocForEndOfToken(
              Context.SourceMgr,
              inheritedClause[i-1].getSourceRange().End);
        SourceLoc afterMyEndLoc
          = Lexer::getLocForEndOfToken(Context.SourceMgr,
                                       inherited.getSourceRange().End);

        diagnose(inherited.getSourceRange().Start,
                 diag::raw_type_not_first, inheritedTy)
          .fixItRemoveChars(afterPriorLoc, afterMyEndLoc)
          .fixItInsert(inheritedClause[0].getSourceRange().Start,
                       inheritedTy.getString() + ", ");

        // Fall through to record the raw type.
      }

      // Record the raw type.
      superclassTy = inheritedTy;
      superclassRange = inherited.getSourceRange();
      
      // Add the RawRepresentable conformance implied by the raw type.
      allProtocols.insert(getProtocol(decl->getLoc(),
                                      KnownProtocolKind::RawRepresentable));
      continue;
    }

    // If this is a class type, it may be the superclass.
    if (inheritedTy->getClassOrBoundGenericClass()) {
      // First, check if we already had a superclass.
      if (superclassTy) {
        // FIXME: Check for shadowed protocol names, i.e., NSObject?

        // Complain about multiple inheritance.
        // Don't emit a Fix-It here. The user has to think harder about this.
        diagnose(inherited.getSourceRange().Start,
                 diag::multiple_inheritance, superclassTy, inheritedTy)
          .highlight(superclassRange);
        inherited.setInvalidType(Context);
        continue;
      }

      // If the declaration we're looking at doesn't allow a superclass,
      // complain.
      if (!canInheritClass(decl)) {
        diagnose(decl->getLoc(),
                 isa<ExtensionDecl>(decl)
                   ? diag::extension_class_inheritance
                   : diag::non_class_inheritance,
                 getDeclaredType(decl), inheritedTy)
          .highlight(inherited.getSourceRange());
        inherited.setInvalidType(Context);
        continue;
      }

      // If this is not the first entry in the inheritance clause, complain.
      if (i > 0) {
        SourceLoc afterPriorLoc
          = Lexer::getLocForEndOfToken(
              Context.SourceMgr,
              inheritedClause[i-1].getSourceRange().End);
        SourceLoc afterMyEndLoc
          = Lexer::getLocForEndOfToken(Context.SourceMgr,
                                       inherited.getSourceRange().End);

        diagnose(inherited.getSourceRange().Start,
                 diag::superclass_not_first, inheritedTy)
          .fixItRemoveChars(afterPriorLoc, afterMyEndLoc)
          .fixItInsert(inheritedClause[0].getSourceRange().Start,
                       inheritedTy.getString() + ", ");

        // Fall through to record the superclass.
      }

      // Record the superclass.
      superclassTy = inheritedTy;
      superclassRange = inherited.getSourceRange();
      continue;
    }

    // We can't inherit from a non-class, non-protocol type.
    diagnose(decl->getLoc(),
             canInheritClass(decl)
               ? diag::inheritance_from_non_protocol_or_class
               : diag::inheritance_from_non_protocol,
             inheritedTy);
    // FIXME: Note pointing to the declaration 'inheritedTy' references?
    inherited.setInvalidType(Context);
  }

  if (auto proto = dyn_cast<ProtocolDecl>(decl)) {
    // FIXME: If we already set the inherited protocols, bail out. We'd rather
    // not have to check this.
    if (proto->isInheritedProtocolsValid())
      return;

    // Check for circular inheritance.
    // FIXME: The diagnostics here should be improved.
    bool diagnosedCircularity = false;
    for (unsigned i = 0, n = allProtocols.size(); i != n; /*in loop*/) {
      if (allProtocols[i] == proto || allProtocols[i]->inheritsFrom(proto)) {
        if (!diagnosedCircularity) {
          diagnose(proto, diag::circular_protocol_def, proto->getName().str());
          diagnosedCircularity = true;
        }

        allProtocols.remove(allProtocols[i]);
        --n;
        continue;
      }

      ++i;
    }

    proto->setInheritedProtocols(Context.AllocateCopy(allProtocols));
    return;
  }

  // Set the superclass.
  if (auto classDecl = dyn_cast<ClassDecl>(decl)) {
    classDecl->setSuperclass(superclassTy);
    if (superclassTy)
      resolveImplicitConstructors(superclassTy->getClassOrBoundGenericClass());
  } else if (auto enumDecl = dyn_cast<EnumDecl>(decl)) {
    enumDecl->setRawType(superclassTy);
  } else {
    assert(!superclassTy || isa<AbstractTypeParamDecl>(decl));
  }
}

/// Retrieve the set of protocols the given protocol inherits.
static ArrayRef<ProtocolDecl *>
getInheritedForCycleCheck(TypeChecker &tc,
                          ProtocolDecl *proto,
                          ProtocolDecl **scratch) {
  return tc.getDirectConformsTo(proto);
}

/// Retrieve the superclass of the given class.
static ArrayRef<ClassDecl *> getInheritedForCycleCheck(TypeChecker &tc,
                                                       ClassDecl *classDecl,
                                                       ClassDecl **scratch) {
  tc.checkInheritanceClause(classDecl);

  if (classDecl->hasSuperclass()) {
    *scratch = classDecl->getSuperclass()->getClassOrBoundGenericClass();
    return *scratch;
  }
  return { };
}

/// Retrieve the raw type of the given enum.
static ArrayRef<EnumDecl *> getInheritedForCycleCheck(TypeChecker &tc,
                                                      EnumDecl *enumDecl,
                                                      EnumDecl **scratch) {
  tc.checkInheritanceClause(enumDecl);
  
  if (enumDecl->hasRawType()) {
    *scratch = enumDecl->getRawType()->getEnumOrBoundGenericEnum();
    return *scratch ? ArrayRef<EnumDecl*>(*scratch) : ArrayRef<EnumDecl*>{};
  }
  return { };
}

// Break the inheritance cycle for a protocol by removing all inherited
// protocols.
//
// FIXME: Just remove the problematic inheritance?
static void breakInheritanceCycle(ProtocolDecl *proto) {
  proto->clearInheritedProtocols();
}

/// Break the inheritance cycle for a class by removing its superclass.
static void breakInheritanceCycle(ClassDecl *classDecl) {
  classDecl->setSuperclass(Type());
}

/// Break the inheritance cycle for an enum by removing its raw type.
static void breakInheritanceCycle(EnumDecl *enumDecl) {
  enumDecl->setRawType(Type());
}

/// Check for circular inheritance.
template<typename T>
static void checkCircularity(TypeChecker &tc, T *decl,
                             Diag<StringRef> circularDiag,
                             Diag<Identifier> declHereDiag,
                             SmallVectorImpl<T *> &path) {
  switch (decl->getCircularityCheck()) {
  case CircularityCheck::Checked:
    return;

  case CircularityCheck::Checking: {
    // We're already checking this protocol, which means we have a cycle.

    // The beginning of the path might not be part of the cycle, so find
    // where the cycle starts.
    auto cycleStart = path.end() - 1;
    while (*cycleStart != decl) {
      assert(cycleStart != path.begin() && "Missing cycle start?");
      --cycleStart;
    }

    // If the path length is 1 the type directly references itself.
    if (path.end() - cycleStart == 1) {
      tc.diagnose(path.back()->getLoc(),
                  circularDiag,
                  path.back()->getName().str());

      decl->setInvalid();
      decl->overwriteType(ErrorType::get(tc.Context));
      breakInheritanceCycle(decl);
      break;
    }

    // Form the textual path illustrating the cycle.
    llvm::SmallString<128> pathStr;
    for (auto i = cycleStart, iEnd = path.end(); i != iEnd; ++i) {
      if (!pathStr.empty())
        pathStr += " -> ";
      pathStr += ("'" + (*i)->getName().str() + "'").str();
    }
    pathStr += (" -> '" + decl->getName().str() + "'").str();

    // Diagnose the cycle.
    tc.diagnose(decl->getLoc(), circularDiag, pathStr);
    for (auto i = cycleStart + 1, iEnd = path.end(); i != iEnd; ++i) {
      tc.diagnose(*i, declHereDiag, (*i)->getName());
    }

    // Set this declaration as invalid, then break the cycle somehow.
    decl->setInvalid();
    decl->overwriteType(ErrorType::get(tc.Context));
    breakInheritanceCycle(decl);
    break;
  }

  case CircularityCheck::Unchecked: {
    // Walk to the inherited class or protocols.
    path.push_back(decl);
    decl->setCircularityCheck(CircularityCheck::Checking);
    T *scratch = nullptr;
    for (auto inherited : getInheritedForCycleCheck(tc, decl, &scratch)) {
      checkCircularity(tc, inherited, circularDiag, declHereDiag, path);
    }
    decl->setCircularityCheck(CircularityCheck::Checked);
    path.pop_back();
    break;
  }
  }
}

/// Set each bound variable in the pattern to have an error type.
static void setBoundVarsTypeError(Pattern *pattern, ASTContext &ctx) {
  pattern->forEachVariable([&](VarDecl *var) {
    // Don't change the type of a variable that we've been able to
    // compute a type for.
    if (var->hasType() && !var->getType()->is<ErrorType>())
      return;

    var->overwriteType(ErrorType::get(ctx));
    var->setInvalid();
  });
}

/// Create a fresh archetype builder.
ArchetypeBuilder TypeChecker::createArchetypeBuilder(Module *mod) {
  return ArchetypeBuilder(*mod, Diags);
}

static void revertDependentTypeLoc(TypeLoc &tl) {
  // If there's no type representation, there's nothing to revert.
  if (!tl.getTypeRepr())
    return;

  // Don't revert an error type; we've already complained.
  if (tl.wasValidated() && tl.isError())
    return;

  // Make sure we validate the type again.
  tl.setType(Type(), /*validated=*/false);
}

/// Revert the dependent types within the given generic parameter list.
void TypeChecker::revertGenericParamList(GenericParamList *genericParams) {
  // Revert the inherited clause of the generic parameter list.
  for (auto param : *genericParams) {
    param->setCheckedInheritanceClause(false);
    for (auto &inherited : param->getInherited())
      revertDependentTypeLoc(inherited);
  }

  // Revert the requirements of the generic parameter list.
  for (auto &req : genericParams->getRequirements()) {
    if (req.isInvalid())
      continue;

    switch (req.getKind()) {
    case RequirementReprKind::TypeConstraint: {
      revertDependentTypeLoc(req.getSubjectLoc());
      revertDependentTypeLoc(req.getConstraintLoc());
      break;
    }

    case RequirementReprKind::SameType:
      revertDependentTypeLoc(req.getFirstTypeLoc());
      revertDependentTypeLoc(req.getSecondTypeLoc());
      break;
    }
  }
}

static void markInvalidGenericSignature(ValueDecl *VD,
                                        TypeChecker &TC) {
  GenericParamList *genericParams;
  if (auto *AFD = dyn_cast<AbstractFunctionDecl>(VD))
    genericParams = AFD->getGenericParams();
  else
    genericParams = cast<NominalTypeDecl>(VD)->getGenericParams();
  
  // If there aren't any generic parameters at this level, we're done.
  if (genericParams == nullptr)
    return;

  DeclContext *DC = VD->getDeclContext();
  ArchetypeBuilder builder = TC.createArchetypeBuilder(DC->getParentModule());

  if (auto sig = DC->getGenericSignatureOfContext())
    builder.addGenericSignature(sig, true);
  
  // Visit each of the generic parameters.
  for (auto param : *genericParams)
    builder.addGenericParameter(param);
  
  // Wire up the archetypes.
  for (auto GP : *genericParams)
    GP->setArchetype(builder.getArchetype(GP));

  genericParams->setAllArchetypes(
      TC.Context.AllocateCopy(builder.getAllArchetypes()));
}


/// Finalize the given generic parameter list, assigning archetypes to
/// the generic parameters.
static void finalizeGenericParamList(ArchetypeBuilder &builder,
                                     GenericParamList *genericParams,
                                     DeclContext *dc,
                                     TypeChecker &TC) {
  Accessibility access;
  if (auto *fd = dyn_cast<FuncDecl>(dc))
    access = fd->getFormalAccess();
  else if (auto *nominal = dyn_cast<NominalTypeDecl>(dc))
    access = nominal->getFormalAccess();
  else
    access = Accessibility::Internal;

  // Wire up the archetypes.
  for (auto GP : *genericParams) {
    GP->setArchetype(builder.getArchetype(GP));
    TC.checkInheritanceClause(GP);
    if (!GP->hasAccessibility())
      GP->setAccessibility(access);
  }
  genericParams->setAllArchetypes(
    TC.Context.AllocateCopy(builder.getAllArchetypes()));

#ifndef NDEBUG
  // Record archetype contexts.
  for (auto archetype : genericParams->getAllArchetypes()) {
    if (TC.Context.ArchetypeContexts.count(archetype) == 0)
      TC.Context.ArchetypeContexts[archetype] = dc;
  }
#endif

  // Replace the generic parameters with their archetypes throughout the
  // types in the requirements.
  // FIXME: This should not be necessary at this level; it is a transitional
  // step.
  for (auto &Req : genericParams->getRequirements()) {
    if (Req.isInvalid())
      continue;

    switch (Req.getKind()) {
    case RequirementReprKind::TypeConstraint: {
      revertDependentTypeLoc(Req.getSubjectLoc());
      if (TC.validateType(Req.getSubjectLoc(), dc)) {
        Req.setInvalid();
        continue;
      }

      revertDependentTypeLoc(Req.getConstraintLoc());
      if (TC.validateType(Req.getConstraintLoc(), dc)) {
        Req.setInvalid();
        continue;
      }
      break;
    }

    case RequirementReprKind::SameType:
      revertDependentTypeLoc(Req.getFirstTypeLoc());
      if (TC.validateType(Req.getFirstTypeLoc(), dc)) {
        Req.setInvalid();
        continue;
      }

      revertDependentTypeLoc(Req.getSecondTypeLoc());
      if (TC.validateType(Req.getSecondTypeLoc(), dc)) {
        Req.setInvalid();
        continue;
      }
      break;
    }
  }
}

/// Expose TypeChecker's handling of GenericParamList to SIL parsing.
GenericSignature *TypeChecker::handleSILGenericParams(
                    GenericParamList *genericParams,
                    DeclContext *DC) {
  SmallVector<GenericParamList *, 2> nestedList;
  for (; genericParams; genericParams = genericParams->getOuterParameters()) {
    nestedList.push_back(genericParams);
  }

  // We call checkGenericParamList() on all lists, then call
  // finalizeGenericParamList() on all lists. After finalizeGenericParamList(),
  // the generic parameters will be assigned to archetypes. That will cause
  // SameType requirement to have Archetypes inside.

  // Since the innermost GenericParamList is in the beginning of the vector,
  // we process in reverse order to handle the outermost list first.
  GenericSignature *parentSig = nullptr;
  for (unsigned i = 0, e = nestedList.size(); i < e; i++) {
    auto genericParams = nestedList.rbegin()[i];
    bool invalid = false;
    auto *genericSig = validateGenericSignature(genericParams, DC, parentSig,
                                                nullptr, invalid);
    if (invalid)
      return nullptr;

    revertGenericParamList(genericParams);

    ArchetypeBuilder builder(*DC->getParentModule(), Diags);
    checkGenericParamList(&builder, genericParams, parentSig);
    finalizeGenericParamList(builder, genericParams, DC, *this);

    parentSig = genericSig;
  }
  return parentSig;
}

void TypeChecker::revertGenericFuncSignature(AbstractFunctionDecl *func) {
  // Revert the result type.
  if (auto fn = dyn_cast<FuncDecl>(func))
    if (!fn->getBodyResultTypeLoc().isNull())
      revertDependentTypeLoc(fn->getBodyResultTypeLoc());

  // Revert the body parameter types.
  for (auto paramList : func->getParameterLists()) {
    for (auto &param : *paramList) {
      // Clear out the type of the decl.
      if (param->hasType() && !param->isInvalid())
        param->overwriteType(Type());
      revertDependentTypeLoc(param->getTypeLoc());
    }
  }

  // Revert the generic parameter list.
  if (func->getGenericParams())
    revertGenericParamList(func->getGenericParams());

  // Clear out the types.
  if (auto fn = dyn_cast<FuncDecl>(func))
    fn->revertType();
  else
    func->overwriteType(Type());
}

/// Check whether the given type representation will be
/// default-initializable.
static bool isDefaultInitializable(TypeRepr *typeRepr) {
  // Look through most attributes.
  if (auto attributed = dyn_cast<AttributedTypeRepr>(typeRepr)) {
    // Weak ownership implies optionality.
    if (attributed->getAttrs().getOwnership() == Ownership::Weak)
      return true;
    
    return isDefaultInitializable(attributed->getTypeRepr());
  }

  // Look through named types.
  if (auto named = dyn_cast<NamedTypeRepr>(typeRepr))
    return isDefaultInitializable(named->getTypeRepr());
  
  // Optional types are default-initializable.
  if (isa<OptionalTypeRepr>(typeRepr) ||
      isa<ImplicitlyUnwrappedOptionalTypeRepr>(typeRepr))
    return true;

  // Tuple types are default-initializable if all of their element
  // types are.
  if (auto tuple = dyn_cast<TupleTypeRepr>(typeRepr)) {
    // ... but not variadic ones.
    if (tuple->hasEllipsis())
      return false;

    for (auto elt : tuple->getElements()) {
      if (!isDefaultInitializable(elt))
        return false;
    }

    return true;
  }

  // Not default initializable.
  return false;
}

// @NSManaged properties never get default initialized, nor do debugger
// variables and immutable properties.
static bool isNeverDefaultInitializable(Pattern *p) {
  bool result = false;

  p->forEachVariable([&](VarDecl *var) {
    assert(!var->getAttrs().hasAttribute<NSManagedAttr>());
    if (var->isDebuggerVar() ||
        var->isLet())
      result = true;
  });

  return result;
}

/// Determine whether the given pattern binding declaration either has
/// an initializer expression, or is default initialized, without performing
/// any type checking on it.
static bool isDefaultInitializable(PatternBindingDecl *pbd) {
  assert(pbd->hasStorage());

  for (auto entry : pbd->getPatternList()) {
    // If it has an initializer expression, this is trivially true.
    if (entry.getInit())
      continue;

    if (isNeverDefaultInitializable(entry.getPattern()))
      return false;

    // If the pattern is typed as optional (or tuples thereof), it is
    // default initializable.
    if (auto typedPattern = dyn_cast<TypedPattern>(entry.getPattern())) {
      if (auto typeRepr = typedPattern->getTypeLoc().getTypeRepr())
        if (isDefaultInitializable(typeRepr))
          continue;
    }

    // Otherwise, we can't default initialize this binding.
    return false;
  }
  
  return true;
}

/// Build a default initializer for the given type.
static Expr *buildDefaultInitializer(TypeChecker &tc, Type type) {
  // Default-initialize optional types and weak values to 'nil'.
  if (type->getReferenceStorageReferent()->getAnyOptionalObjectType())
    return new (tc.Context) NilLiteralExpr(SourceLoc(), /*implicit=*/true);

  // Build tuple literals for tuple types.
  if (auto tupleType = type->getAs<TupleType>()) {
    SmallVector<Expr *, 2> inits;
    for (const auto &elt : tupleType->getElements()) {
      if (elt.isVararg())
        return nullptr;

      auto eltInit = buildDefaultInitializer(tc, elt.getType());
      if (!eltInit)
        return nullptr;

      inits.push_back(eltInit);
    }

    return TupleExpr::createImplicit(tc.Context, inits, { });
  }

  // We don't default-initialize anything else.
  return nullptr;
}

/// Check whether \c current is a declaration.
static void checkRedeclaration(TypeChecker &tc, ValueDecl *current) {
  // If we've already checked this declaration, don't do it again.
  if (current->alreadyCheckedRedeclaration())
    return;

  // If there's no type yet, come back to it later.
  if (!current->hasType())
    return;

  // Make sure we don't do this checking again.
  current->setCheckedRedeclaration(true);

  // Ignore invalid and anonymous declarations.
  if (current->isInvalid() || !current->hasName())
    return;

  // If this declaration isn't from a source file, don't check it.
  // FIXME: Should restrict this to the source file we care about.
  DeclContext *currentDC = current->getDeclContext();
  SourceFile *currentFile = currentDC->getParentSourceFile();
  if (!currentFile || currentDC->isLocalContext())
    return;

  ReferencedNameTracker *tracker = currentFile->getReferencedNameTracker();
  bool isCascading = true;
  if (current->hasAccessibility())
    isCascading = (current->getFormalAccess() > Accessibility::Private);

  // Find other potential definitions.
  SmallVector<ValueDecl *, 4> otherDefinitionsVec;
  ArrayRef<ValueDecl *> otherDefinitions;
  if (currentDC->isTypeContext()) {
    // Look within a type context.
    if (auto nominal = currentDC->isNominalTypeOrNominalTypeExtensionContext()) {
      otherDefinitions = nominal->lookupDirect(current->getBaseName());
      if (tracker)
        tracker->addUsedMember({nominal, current->getName()}, isCascading);
    }
  } else {
    // Look within a module context.
    currentFile->getParentModule()->lookupValue({ }, current->getBaseName(),
                                                NLKind::QualifiedLookup,
                                                otherDefinitionsVec);
    otherDefinitions = otherDefinitionsVec;
    if (tracker)
      tracker->addTopLevelName(current->getName(), isCascading);
  }

  // Compare this signature against the signature of other
  // declarations with the same name.
  OverloadSignature currentSig = current->getOverloadSignature();
  Module *currentModule = current->getModuleContext();
  for (auto other : otherDefinitions) {
    // Skip invalid declarations and ourselves.
    if (current == other || other->isInvalid())
      continue;

    // Skip declarations in other modules.
    if (currentModule != other->getModuleContext())
      continue;

    // Don't compare methods vs. non-methods (which only happens with
    // operators).
    if (currentDC->isTypeContext() != other->getDeclContext()->isTypeContext())
      continue;

    // Validate the declaration.
    tc.validateDecl(other);
    if (other->isInvalid() || !other->hasType())
      continue;

    // Skip declarations in other files.
    // In practice, this means we will warn on a private declaration that
    // shadows a non-private one, but only in the file where the shadowing
    // happens. We will warn on conflicting non-private declarations in both
    // files.
    if (!other->isAccessibleFrom(currentDC))
      continue;

    // If there is a conflict, complain.
    if (conflicting(currentSig, other->getOverloadSignature())) {
      // If the two declarations occur in the same source file, make sure
      // we get the diagnostic ordering to be sensible.
      if (auto otherFile = other->getDeclContext()->getParentSourceFile()) {
        if (currentFile == otherFile &&
            current->getLoc().isValid() &&
            other->getLoc().isValid() &&
            tc.Context.SourceMgr.isBeforeInBuffer(current->getLoc(),
                                                  other->getLoc())) {
          std::swap(current, other);
        }
      }

      // If we're currently looking at a .sil and the conflicting declaration
      // comes from a .sib, don't error since we won't be considering the sil
      // from the .sib. So it's fine for the .sil to shadow it, since that's the
      // one we want.
      if (currentFile->Kind == SourceFileKind::SIL) {
        auto *otherFile = dyn_cast<SerializedASTFile>(
            other->getDeclContext()->getModuleScopeContext());
        if (otherFile && otherFile->isSIB())
          continue;
      }

      tc.diagnose(current, diag::invalid_redecl, current->getFullName());
      tc.diagnose(other, diag::invalid_redecl_prev, other->getFullName());

      current->setInvalid();
      if (current->hasType())
        current->overwriteType(ErrorType::get(tc.Context));
      break;
    }
  }
}

/// Does the context allow pattern bindings that don't bind any variables?
static bool contextAllowsPatternBindingWithoutVariables(DeclContext *dc) {
  
  // Property decls in type context must bind variables.
  if (dc->isTypeContext())
    return false;
  
  // Global variable decls must bind variables, except in scripts.
  if (dc->isModuleScopeContext()) {
    if (dc->getParentSourceFile()
        && dc->getParentSourceFile()->isScriptMode())
      return true;
    
    return false;
  }
  
  return true;
}

/// Validate the given pattern binding declaration.
static void validatePatternBindingDecl(TypeChecker &tc,
                                       PatternBindingDecl *binding,
                                       unsigned entryNumber) {
  // If the pattern already has a type, we're done.
  if (binding->getPattern(entryNumber)->hasType() ||
      binding->isBeingTypeChecked())
    return;
  
  binding->setIsBeingTypeChecked();

  // On any path out of this function, make sure to mark the binding as done
  // being type checked.
  defer {
    binding->setIsBeingTypeChecked(false);
  };

  // Resolve the pattern.
  auto *pattern = tc.resolvePattern(binding->getPattern(entryNumber),
                                    binding->getDeclContext(),
                                    /*isStmtCondition*/true);
  if (!pattern) {
    binding->setInvalid();
    binding->getPattern(entryNumber)->setType(ErrorType::get(tc.Context));
    return;
  }

  binding->setPattern(entryNumber, pattern);

  // Validate 'static'/'class' on properties in nominal type decls.
  auto StaticSpelling = binding->getStaticSpelling();
  if (StaticSpelling != StaticSpellingKind::None &&
      binding->getDeclContext()->isExtensionContext()) {
    if (Type T = binding->getDeclContext()->getDeclaredTypeInContext()) {
      if (auto NTD = T->getAnyNominal()) {
        if (!isa<ClassDecl>(NTD)) {
          if (StaticSpelling == StaticSpellingKind::KeywordClass) {
            tc.diagnose(binding, diag::class_var_not_in_class)
              .fixItReplace(binding->getStaticLoc(), "static");
            tc.diagnose(NTD, diag::extended_type_declared_here);
          }
        }
      }
    }
  }

  // Check the pattern. We treat type-checking a PatternBindingDecl like
  // type-checking an expression because that's how the initial binding is
  // checked, and they have the same effect on the file's dependencies.
  //
  // In particular, it's /not/ correct to check the PBD's DeclContext because
  // top-level variables in a script file are accessible from other files,
  // even though the PBD is inside a TopLevelCodeDecl.
  TypeResolutionOptions options = TR_InExpression;
  if (binding->getInit(entryNumber)) {
    // If we have an initializer, we can also have unknown types.
    options |= TR_AllowUnspecifiedTypes;
    options |= TR_AllowUnboundGenerics;
  }
  if (tc.typeCheckPattern(pattern, binding->getDeclContext(), options)) {
    setBoundVarsTypeError(pattern, tc.Context);
    binding->setInvalid();
    pattern->setType(ErrorType::get(tc.Context));
    return;
  }

  // If the pattern didn't get a type or if it contains an unbound generic type,
  // we'll need to check the initializer.
  if (!pattern->hasType() || pattern->getType()->hasUnboundGenericType())
    if (tc.typeCheckPatternBinding(binding, entryNumber))
      return;

  // If the pattern binding appears in a type or library file context, then
  // it must bind at least one variable.
  if (!contextAllowsPatternBindingWithoutVariables(binding->getDeclContext())) {
    llvm::SmallVector<VarDecl*, 2> vars;
    binding->getPattern(entryNumber)->collectVariables(vars);
    if (vars.empty()) {
      // Selector for error message.
      enum : unsigned {
        Property,
        GlobalVariable,
      };
      tc.diagnose(binding->getPattern(entryNumber)->getLoc(),
                  diag::pattern_binds_no_variables,
                  binding->getDeclContext()->isTypeContext()
                                                   ? Property : GlobalVariable);
    }
  }

  // If we have any type-adjusting attributes, apply them here.
  if (binding->getPattern(entryNumber)->hasType())
    if (auto var = binding->getSingleVar())
      tc.checkTypeModifyingDeclAttributes(var);

  // If we're in a generic type context, provide interface types for all of
  // the variables.
  {
    auto dc = binding->getDeclContext();
    if (dc->isGenericContext() && dc->isTypeContext()) {
      binding->getPattern(entryNumber)->forEachVariable([&](VarDecl *var) {
        var->setInterfaceType(
          ArchetypeBuilder::mapTypeOutOfContext(dc, var->getType()));
      });
    }

    // For now, we only support static/class variables in specific contexts.
    if (binding->isStatic()) {
      // Selector for unimplemented_type_var message.
      enum : unsigned {
        Misc,
        GenericTypes,
        Classes,
      };
      auto unimplementedStatic = [&](unsigned diagSel) {
        auto staticLoc = binding->getStaticLoc();
        tc.diagnose(staticLoc, diag::unimplemented_type_var,
                    diagSel, binding->getStaticSpelling(),
                    diagSel == Classes)
          .highlight(staticLoc);
      };

      assert(dc->isTypeContext());
      // The parser only accepts 'type' variables in type contexts, so
      // we're either in a nominal type context or an extension.
      NominalTypeDecl *nominal;
      if (auto extension = dyn_cast<ExtensionDecl>(dc)) {
        nominal = extension->getExtendedType()->getAnyNominal();
        assert(nominal);
      } else {
        nominal = cast<NominalTypeDecl>(dc);
      }

      // Non-stored properties are fine.
      if (!binding->hasStorage()) {
        // do nothing

      // Stored type variables in a generic context need to logically
      // occur once per instantiation, which we don't yet handle.
      } else if (dc->isGenericContext()) {
        unimplementedStatic(GenericTypes);
      } else if (dc->isClassOrClassExtensionContext()) {
        auto staticSpelling = binding->getStaticSpelling();
        if (staticSpelling != StaticSpellingKind::KeywordStatic)
          unimplementedStatic(Classes);
      }
    }
  }
}

void swift::makeFinal(ASTContext &ctx, ValueDecl *D) {
  if (D && !D->isFinal()) {
    D->getAttrs().add(new (ctx) FinalAttr(/*IsImplicit=*/true));
  }
}

void swift::makeDynamic(ASTContext &ctx, ValueDecl *D) {
  if (D && !D->isDynamic()) {
    D->getAttrs().add(new (ctx) DynamicAttr(/*IsImplicit=*/true));
  }
}

/// Configure the implicit 'self' parameter of a function, setting its type,
/// pattern, etc.
///
/// \param func The function whose 'self' is being configured.
///
/// \returns the type of 'self'.
Type swift::configureImplicitSelf(TypeChecker &tc,
                                  AbstractFunctionDecl *func) {
  auto selfDecl = func->getImplicitSelfDecl();

  // Validate the context.
  if (auto nominal = dyn_cast<NominalTypeDecl>(func->getDeclContext())) {
    tc.validateDecl(nominal);
  } else {
    tc.validateExtension(cast<ExtensionDecl>(func->getDeclContext()));
  }

  // Compute the type of self.
  Type selfTy = func->computeSelfType();
  assert(selfDecl && selfTy && "Not a method");

  // 'self' is 'let' for reference types (i.e., classes) or when 'self' is
  // neither inout.
  selfDecl->setLet(!selfTy->is<InOutType>());
  selfDecl->overwriteType(selfTy);
  
  // Install the self type on the Parameter that contains it.  This ensures that
  // we don't lose it when generic types get reverted.
  selfDecl->getTypeLoc() = TypeLoc::withoutLoc(selfTy);
  return selfTy;
}

/// Compute the allocating and initializing constructor types for
/// the given constructor.
void swift::configureConstructorType(ConstructorDecl *ctor,
                                     Type selfType,
                                     Type argType,
                                     bool throws) {
  Type fnType;
  Type allocFnType;
  Type initFnType;
  Type resultType = selfType->getInOutObjectType();
  if (ctor->getFailability() != OTK_None) {
    resultType = OptionalType::get(ctor->getFailability(), resultType);
  }

  auto extInfo = AnyFunctionType::ExtInfo().withThrows(throws);

  GenericParamList *outerGenericParams =
      ctor->getDeclContext()->getGenericParamsOfContext();

  if (GenericParamList *innerGenericParams = ctor->getGenericParams()) {
    innerGenericParams->setOuterParameters(outerGenericParams);
    fnType = PolymorphicFunctionType::get(argType, resultType,
                                          innerGenericParams,
                                          extInfo);
  } else {
    fnType = FunctionType::get(argType, resultType, extInfo);
  }
  Type selfMetaType = MetatypeType::get(selfType->getInOutObjectType());
  if (ctor->getDeclContext()->isGenericTypeContext()) {
    allocFnType = PolymorphicFunctionType::get(selfMetaType, fnType,
                                               outerGenericParams);
    initFnType = PolymorphicFunctionType::get(selfType, fnType,
                                              outerGenericParams);
  } else {
    allocFnType = FunctionType::get(selfMetaType, fnType);
    initFnType = FunctionType::get(selfType, fnType);
  }
  ctor->setType(allocFnType);
  ctor->setInitializerType(initFnType);
}

namespace {

class TypeAccessibilityChecker : private TypeWalker {
  using TypeAccessibilityCacheMap =
    decltype(TypeChecker::TypeAccessibilityCache);
  TypeAccessibilityCacheMap &Cache;
  SmallVector<Accessibility, 8> AccessStack;

  explicit TypeAccessibilityChecker(TypeAccessibilityCacheMap &cache)
      : Cache(cache) {
    // Always have something on the stack.
    AccessStack.push_back(Accessibility::Private);
  }

  bool shouldVisitOriginalSubstitutedType() override { return true; }

  Action walkToTypePre(Type ty) override {
    // Assume failure until we post-visit this node.
    // This will be correct as long as we don't ever have self-referential
    // Types.
    auto cached = Cache.find(ty);
    if (cached != Cache.end()) {
      AccessStack.back() = std::min(AccessStack.back(), cached->second);
      return Action::SkipChildren;
    }

    Accessibility current;
    if (auto alias = dyn_cast<NameAliasType>(ty.getPointer()))
      current = alias->getDecl()->getFormalAccess();
    else if (auto nominal = ty->getAnyNominal())
      current = nominal->getFormalAccess();
    else
      current = Accessibility::Public;
    AccessStack.push_back(current);

    return Action::Continue;
  }

  Action walkToTypePost(Type ty) override {
    Accessibility last = AccessStack.pop_back_val();
    Cache[ty] = last;
    AccessStack.back() = std::min(AccessStack.back(), last);
    return Action::Continue;
  }

public:
  static Accessibility getAccessibility(Type ty,
                                        TypeAccessibilityCacheMap &cache) {
    ty.walk(TypeAccessibilityChecker(cache));
    return cache[ty];
  }
};

} // end anonymous namespace


void TypeChecker::computeDefaultAccessibility(ExtensionDecl *ED) {
  if (ED->hasDefaultAccessibility())
    return;

  validateExtension(ED);

  Accessibility maxAccess = Accessibility::Public;

  if (!ED->getExtendedType().isNull() &&
      !ED->getExtendedType()->is<ErrorType>()) {
    if (NominalTypeDecl *nominal = ED->getExtendedType()->getAnyNominal()) {
      validateDecl(nominal);
      maxAccess = nominal->getFormalAccess();
    }
  }

  if (const GenericParamList *genericParams = ED->getGenericParams()) {
    auto getTypeAccess = [this](const TypeLoc &TL) {
      if (!TL.getType())
        return Accessibility::Public;
      return TypeAccessibilityChecker::getAccessibility(TL.getType(),
                                                        TypeAccessibilityCache);
    };

    // Only check the trailing 'where' requirements. Other requirements come
    // from the extended type and have already been checked.
    for (const RequirementRepr &req : genericParams->getTrailingRequirements()){
      switch (req.getKind()) {
      case RequirementReprKind::TypeConstraint:
        maxAccess = std::min(getTypeAccess(req.getSubjectLoc()), maxAccess);
        maxAccess = std::min(getTypeAccess(req.getConstraintLoc()), maxAccess);
        break;
      case RequirementReprKind::SameType:
        maxAccess = std::min(getTypeAccess(req.getFirstTypeLoc()), maxAccess);
        maxAccess = std::min(getTypeAccess(req.getSecondTypeLoc()), maxAccess);
        break;
      }
    }
  }

  Accessibility defaultAccess;
  if (auto *AA = ED->getAttrs().getAttribute<AccessibilityAttr>())
    defaultAccess = AA->getAccess();
  else
    defaultAccess = std::min(maxAccess, Accessibility::Internal);

  // Normally putting a public member in an internal extension is harmless,
  // because that member can never be used elsewhere. But if some of the types
  // in the signature are public, it could actually end up getting picked in
  // overload resolution. Therefore, we only enforce the maximum access if the
  // extension has a 'where' clause.
  if (ED->getTrailingWhereClause())
    defaultAccess = std::min(defaultAccess, maxAccess);
  else
    maxAccess = Accessibility::Public;

  ED->setDefaultAndMaxAccessibility(defaultAccess, maxAccess);
}

void TypeChecker::computeAccessibility(ValueDecl *D) {
  if (D->hasAccessibility())
    return;

  // Check if the decl has an explicit accessibility attribute.
  if (auto *AA = D->getAttrs().getAttribute<AccessibilityAttr>()) {
    D->setAccessibility(AA->getAccess());

  } else if (auto fn = dyn_cast<FuncDecl>(D)) {
    // Special case for accessors, which inherit the access of their storage.
    // decl. A setter attribute can also override this.
    if (AbstractStorageDecl *storage = fn->getAccessorStorageDecl()) {
      if (storage->hasAccessibility()) {
        if (fn->getAccessorKind() == AccessorKind::IsSetter ||
            fn->getAccessorKind() == AccessorKind::IsMaterializeForSet)
          fn->setAccessibility(storage->getSetterAccessibility());
        else
          fn->setAccessibility(storage->getFormalAccess());
      } else {
        computeAccessibility(storage);
      }
    }
  }

  if (!D->hasAccessibility()) {
    DeclContext *DC = D->getDeclContext();
    switch (DC->getContextKind()) {
    case DeclContextKind::SerializedLocal:
    case DeclContextKind::AbstractClosureExpr:
    case DeclContextKind::Initializer:
    case DeclContextKind::TopLevelCodeDecl:
    case DeclContextKind::AbstractFunctionDecl:
    case DeclContextKind::SubscriptDecl:
      D->setAccessibility(Accessibility::Private);
      break;
    case DeclContextKind::Module:
    case DeclContextKind::FileUnit:
      D->setAccessibility(Accessibility::Internal);
      break;
    case DeclContextKind::NominalTypeDecl: {
      auto nominal = cast<NominalTypeDecl>(DC);
      validateAccessibility(nominal);
      Accessibility access = nominal->getFormalAccess();
      if (!isa<ProtocolDecl>(nominal))
        access = std::min(access, Accessibility::Internal);
      D->setAccessibility(access);
      break;
    }
    case DeclContextKind::ExtensionDecl: {
      auto extension = cast<ExtensionDecl>(DC);
      computeDefaultAccessibility(extension);
      D->setAccessibility(extension->getDefaultAccessibility());
    }
    }
  }

  if (auto ASD = dyn_cast<AbstractStorageDecl>(D)) {
    if (auto *AA = D->getAttrs().getAttribute<SetterAccessibilityAttr>())
      ASD->setSetterAccessibility(AA->getAccess());
    else
      ASD->setSetterAccessibility(ASD->getFormalAccess());

    if (auto getter = ASD->getGetter())
      computeAccessibility(getter);
    if (auto setter = ASD->getSetter())
      computeAccessibility(setter);
  }
}

namespace {

class TypeAccessibilityDiagnoser : private ASTWalker {
  const ComponentIdentTypeRepr *minAccessibilityType = nullptr;

  bool walkToTypeReprPre(TypeRepr *TR) override {
    auto CITR = dyn_cast<ComponentIdentTypeRepr>(TR);
    if (!CITR)
      return true;

    const ValueDecl *VD = CITR->getBoundDecl();
    if (!VD)
      return true;

    if (minAccessibilityType) {
      const ValueDecl *minDecl = minAccessibilityType->getBoundDecl();
      if (minDecl->getFormalAccess() <= VD->getFormalAccess())
        return true;
    }

    minAccessibilityType = CITR;
    return true;
  }

public:
  static const TypeRepr *findMinAccessibleType(TypeRepr *TR) {
    TypeAccessibilityDiagnoser diagnoser;
    TR->walk(diagnoser);
    return diagnoser.minAccessibilityType;
  }
};
} // end anonymous namespace

/// Checks if the accessibility of the type described by \p TL is at least
/// \p contextAccess. If it isn't, calls \p diagnose with a TypeRepr
/// representing the offending part of \p TL.
///
/// The TypeRepr passed to \p diagnose may be null, in which case a particular
/// part of the type that caused the problem could not be found.
static void checkTypeAccessibility(
    TypeChecker &TC, TypeLoc TL, Accessibility contextAccess,
    llvm::function_ref<void(Accessibility, const TypeRepr *)> diagnose) {
  // Don't spend time checking private access; this is always valid.
  // This includes local declarations.
  if (contextAccess == Accessibility::Private || !TL.getType())
    return;

  Accessibility typeAccess =
    TypeAccessibilityChecker::getAccessibility(TL.getType(),
                                               TC.TypeAccessibilityCache);
  if (typeAccess >= contextAccess)
    return;

  const TypeRepr *complainRepr = nullptr;
  if (TypeRepr *TR = TL.getTypeRepr())
    complainRepr = TypeAccessibilityDiagnoser::findMinAccessibleType(TR);
  diagnose(typeAccess, complainRepr);
}

static void checkTypeAccessibility(
    TypeChecker &TC, TypeLoc TL, const ValueDecl *context,
    llvm::function_ref<void(Accessibility, const TypeRepr *)> diagnose) {
  checkTypeAccessibility(TC, TL, context->getFormalAccess(), diagnose);
}

/// Highlights the given TypeRepr, and adds a note pointing to the type's
/// declaration if possible.
///
/// Just flushes \p diag as is if \p complainRepr is null.
static void highlightOffendingType(TypeChecker &TC, InFlightDiagnostic &diag,
                                   const TypeRepr *complainRepr) {
  if (!complainRepr) {
    diag.flush();
    return;
  }

  diag.highlight(complainRepr->getSourceRange());
  diag.flush();

  if (auto CITR = dyn_cast<ComponentIdentTypeRepr>(complainRepr)) {
    const ValueDecl *VD = CITR->getBoundDecl();
    TC.diagnose(VD, diag::type_declared_here);
  }
}

static void checkGenericParamAccessibility(TypeChecker &TC,
                                           const GenericParamList *params,
                                           const Decl *owner,
                                           Accessibility contextAccess) {
  if (!params)
    return;

  // This must stay in sync with diag::generic_param_access.
  enum {
    AEK_Parameter = 0,
    AEK_Requirement
  } accessibilityErrorKind;
  Optional<Accessibility> minAccess;
  const TypeRepr *complainRepr = nullptr;

  for (auto param : *params) {
    if (param->getInherited().empty())
      continue;
    assert(param->getInherited().size() == 1);
    checkTypeAccessibility(TC, param->getInherited().front(), contextAccess,
                           [&](Accessibility typeAccess,
                               const TypeRepr *thisComplainRepr) {
      if (!minAccess || *minAccess > typeAccess) {
        minAccess = typeAccess;
        complainRepr = thisComplainRepr;
        accessibilityErrorKind = AEK_Parameter;
      }
    });
  }

  for (auto &requirement : params->getRequirements()) {
    auto callback = [&](Accessibility typeAccess,
                        const TypeRepr *thisComplainRepr) {
      if (!minAccess || *minAccess > typeAccess) {
        minAccess = typeAccess;
        complainRepr = thisComplainRepr;
        accessibilityErrorKind = AEK_Requirement;
      }
    };
    switch (requirement.getKind()) {
    case RequirementReprKind::TypeConstraint:
      checkTypeAccessibility(TC, requirement.getSubjectLoc(), contextAccess,
                             callback);
      checkTypeAccessibility(TC, requirement.getConstraintLoc(), contextAccess,
                             callback);
      break;
    case RequirementReprKind::SameType:
      checkTypeAccessibility(TC, requirement.getFirstTypeLoc(), contextAccess,
                             callback);
      checkTypeAccessibility(TC, requirement.getSecondTypeLoc(), contextAccess,
                             callback);
      break;
    }
  }

  if (minAccess.hasValue()) {
    bool isExplicit =
      owner->getAttrs().hasAttribute<AccessibilityAttr>() ||
      owner->getDeclContext()->isProtocolOrProtocolExtensionContext();
    auto diag = TC.diagnose(owner, diag::generic_param_access,
                            owner->getDescriptiveKind(), isExplicit,
                            contextAccess, minAccess.getValue(),
                            accessibilityErrorKind);
    highlightOffendingType(TC, diag, complainRepr);
  }
}

static void checkGenericParamAccessibility(TypeChecker &TC,
                                           const GenericParamList *params,
                                           const ValueDecl *owner) {
  checkGenericParamAccessibility(TC, params, owner, owner->getFormalAccess());
}

/// Checks the given declaration's accessibility to make sure it is valid given
/// the way it is defined.
///
/// \p D must be a ValueDecl or a Decl that can appear in a type context.
static void checkAccessibility(TypeChecker &TC, const Decl *D) {
  if (D->isInvalid() || D->isImplicit())
    return;

  switch (D->getKind()) {
  case DeclKind::Import:
  case DeclKind::Extension:
  case DeclKind::TopLevelCode:
  case DeclKind::InfixOperator:
  case DeclKind::PrefixOperator:
  case DeclKind::PostfixOperator:
  case DeclKind::Module:
    llvm_unreachable("cannot appear in a type context");

  case DeclKind::Param:
  case DeclKind::GenericTypeParam:
    llvm_unreachable("does not have accessibility");

  case DeclKind::IfConfig:
    // Does not have accessibility.
  case DeclKind::EnumCase:
    // Handled at the EnumElement level.
  case DeclKind::Var:
    // Handled at the PatternBindingDecl level.
  case DeclKind::Destructor:
    // Always correct.
    return;

  case DeclKind::PatternBinding: {
    auto PBD = cast<PatternBindingDecl>(D);
    bool isTypeContext = PBD->getDeclContext()->isTypeContext();

    llvm::DenseSet<const VarDecl *> seenVars;
    for (auto entry : PBD->getPatternList())
    entry.getPattern()->forEachNode([&](const Pattern *P) {
      if (auto *NP = dyn_cast<NamedPattern>(P)) {
        // Only check individual variables if we didn't check an enclosing
        // TypedPattern.
        const VarDecl *theVar = NP->getDecl();
        if (seenVars.count(theVar) || theVar->isInvalid())
          return;

        checkTypeAccessibility(TC, TypeLoc::withoutLoc(theVar->getType()),
                               theVar,
                               [&](Accessibility typeAccess,
                                   const TypeRepr *complainRepr) {
          bool isExplicit =
            theVar->getAttrs().hasAttribute<AccessibilityAttr>();
          auto diag = TC.diagnose(P->getLoc(),
                                  diag::pattern_type_access_inferred,
                                  theVar->isLet(),
                                  isTypeContext,
                                  isExplicit,
                                  theVar->getFormalAccess(),
                                  typeAccess,
                                  theVar->getType());
        });
        return;
      }

      auto *TP = dyn_cast<TypedPattern>(P);
      if (!TP)
        return;

      // FIXME: We need an accessibility value to check against, so we pull
      // one out of some random VarDecl in the pattern. They're all going to
      // be the same, but still, ick.
      const VarDecl *anyVar = nullptr;
      TP->forEachVariable([&](VarDecl *V) {
        seenVars.insert(V);
        anyVar = V;
      });
      if (!anyVar)
        return;

      checkTypeAccessibility(TC, TP->getTypeLoc(), anyVar,
                             [&](Accessibility typeAccess,
                                 const TypeRepr *complainRepr) {
        bool isExplicit =
          anyVar->getAttrs().hasAttribute<AccessibilityAttr>() ||
          anyVar->getDeclContext()->isProtocolOrProtocolExtensionContext();
        auto diag = TC.diagnose(P->getLoc(), diag::pattern_type_access,
                                anyVar->isLet(),
                                isTypeContext,
                                isExplicit,
                                anyVar->getFormalAccess(),
                                typeAccess);
        highlightOffendingType(TC, diag, complainRepr);
      });
    });
    return;
  }

  case DeclKind::TypeAlias: {
    auto TAD = cast<TypeAliasDecl>(D);

    checkTypeAccessibility(TC, TAD->getUnderlyingTypeLoc(), TAD,
                           [&](Accessibility typeAccess,
                               const TypeRepr *complainRepr) {
      bool isExplicit = TAD->getAttrs().hasAttribute<AccessibilityAttr>();
      auto diag = TC.diagnose(TAD, diag::type_alias_underlying_type_access,
                              isExplicit, TAD->getFormalAccess(),
                              typeAccess);
      highlightOffendingType(TC, diag, complainRepr);
    });

    return;
  }

  case DeclKind::AssociatedType: {
    auto assocType = cast<AssociatedTypeDecl>(D);

    // This must stay in sync with diag::associated_type_access.
    enum {
      AEK_DefaultDefinition = 0,
      AEK_Requirement
    } accessibilityErrorKind;
    Optional<Accessibility> minAccess;
    const TypeRepr *complainRepr = nullptr;

    std::for_each(assocType->getInherited().begin(),
                  assocType->getInherited().end(),
                  [&](TypeLoc requirement) {
      checkTypeAccessibility(TC, requirement, assocType,
                             [&](Accessibility typeAccess,
                                 const TypeRepr *thisComplainRepr) {
        if (!minAccess || *minAccess > typeAccess) {
          minAccess = typeAccess;
          complainRepr = thisComplainRepr;
          accessibilityErrorKind = AEK_Requirement;
        }
      });
    });
    checkTypeAccessibility(TC, assocType->getDefaultDefinitionLoc(), assocType,
                           [&](Accessibility typeAccess,
                               const TypeRepr *thisComplainRepr) {
      if (!minAccess || *minAccess > typeAccess) {
        minAccess = typeAccess;
        complainRepr = thisComplainRepr;
        accessibilityErrorKind = AEK_DefaultDefinition;
      }
    });

    if (minAccess) {
      auto diag = TC.diagnose(assocType, diag::associated_type_access,
                              assocType->getFormalAccess(),
                              *minAccess, accessibilityErrorKind);
      highlightOffendingType(TC, diag, complainRepr);
    }
    return;
  }

  case DeclKind::Enum: {
    auto ED = cast<EnumDecl>(D);

    checkGenericParamAccessibility(TC, ED->getGenericParams(), ED);

    if (ED->hasRawType()) {
      Type rawType = ED->getRawType();
      auto rawTypeLocIter = std::find_if(ED->getInherited().begin(),
                                         ED->getInherited().end(),
                                         [&](TypeLoc inherited) {
        if (!inherited.wasValidated())
          return false;
        return inherited.getType().getPointer() == rawType.getPointer();
      });
      if (rawTypeLocIter == ED->getInherited().end())
        return;
      checkTypeAccessibility(TC, *rawTypeLocIter, ED,
                             [&](Accessibility typeAccess,
                                 const TypeRepr *complainRepr) {
        bool isExplicit = ED->getAttrs().hasAttribute<AccessibilityAttr>();
        auto diag = TC.diagnose(ED, diag::enum_raw_type_access,
                                isExplicit, ED->getFormalAccess(),
                                typeAccess);
        highlightOffendingType(TC, diag, complainRepr);
      });
    }

    return;
  }

  case DeclKind::Struct: {
    auto SD = cast<StructDecl>(D);
    checkGenericParamAccessibility(TC, SD->getGenericParams(), SD);
    return;
  }

  case DeclKind::Class: {
    auto CD = cast<ClassDecl>(D);

    checkGenericParamAccessibility(TC, CD->getGenericParams(), CD);

    if (CD->hasSuperclass()) {
      Type superclass = CD->getSuperclass();
      auto superclassLocIter = std::find_if(CD->getInherited().begin(),
                                            CD->getInherited().end(),
                                            [&](TypeLoc inherited) {
        if (!inherited.wasValidated())
          return false;
        return inherited.getType().getPointer() == superclass.getPointer();
      });
      if (superclassLocIter == CD->getInherited().end())
        return;
      checkTypeAccessibility(TC, *superclassLocIter, CD,
                             [&](Accessibility typeAccess,
                                 const TypeRepr *complainRepr) {
        bool isExplicit = CD->getAttrs().hasAttribute<AccessibilityAttr>();
        auto diag = TC.diagnose(CD, diag::class_super_access,
                                isExplicit, CD->getFormalAccess(),
                                typeAccess);
        highlightOffendingType(TC, diag, complainRepr);
      });
    }

    return;
  }

  case DeclKind::Protocol: {
    auto proto = cast<ProtocolDecl>(D);

    Optional<Accessibility> minAccess;
    const TypeRepr *complainRepr = nullptr;

    std::for_each(proto->getInherited().begin(),
                  proto->getInherited().end(),
                  [&](TypeLoc requirement) {
      checkTypeAccessibility(TC, requirement, proto,
                             [&](Accessibility typeAccess,
                                 const TypeRepr *thisComplainRepr) {
        if (!minAccess || *minAccess > typeAccess) {
          minAccess = typeAccess;
          complainRepr = thisComplainRepr;
        }
      });
    });

    if (minAccess) {
      bool isExplicit = proto->getAttrs().hasAttribute<AccessibilityAttr>();
      auto diag = TC.diagnose(proto, diag::protocol_refine_access,
                              isExplicit,
                              proto->getFormalAccess(),
                              *minAccess);
      highlightOffendingType(TC, diag, complainRepr);
    }
    return;
  }

  case DeclKind::Subscript: {
    auto SD = cast<SubscriptDecl>(D);

    Optional<Accessibility> minAccess;
    const TypeRepr *complainRepr = nullptr;
    bool problemIsElement = false;
    for (auto &P : *SD->getIndices()) {
      checkTypeAccessibility(TC, P->getTypeLoc(), SD,
                             [&](Accessibility typeAccess,
                                 const TypeRepr *thisComplainRepr) {
        if (!minAccess || *minAccess > typeAccess) {
          minAccess = typeAccess;
          complainRepr = thisComplainRepr;
        }
      });
    }

    checkTypeAccessibility(TC, SD->getElementTypeLoc(), SD,
                           [&](Accessibility typeAccess,
                               const TypeRepr *thisComplainRepr) {
      if (!minAccess || *minAccess > typeAccess) {
        minAccess = typeAccess;
        complainRepr = thisComplainRepr;
        problemIsElement = true;
      }
    });

    if (minAccess) {
      bool isExplicit =
        SD->getAttrs().hasAttribute<AccessibilityAttr>() ||
        SD->getDeclContext()->isProtocolOrProtocolExtensionContext();
      auto diag = TC.diagnose(SD, diag::subscript_type_access,
                              isExplicit,
                              SD->getFormalAccess(),
                              *minAccess,
                              problemIsElement);
      highlightOffendingType(TC, diag, complainRepr);
    }
    return;
  }

  case DeclKind::Func:
    if (cast<FuncDecl>(D)->isAccessor())
      return;
    SWIFT_FALLTHROUGH;
  case DeclKind::Constructor: {
    auto fn = cast<AbstractFunctionDecl>(D);
    bool isTypeContext = fn->getDeclContext()->isTypeContext();

    checkGenericParamAccessibility(TC, fn->getGenericParams(), fn);

    // This must stay in sync with diag::associated_type_access.
    enum {
      FK_Function = 0,
      FK_Method,
      FK_Initializer
    };

    Optional<Accessibility> minAccess;
    const TypeRepr *complainRepr = nullptr;
    for (auto *PL : fn->getParameterLists().slice(isTypeContext)) {
      for (auto &P : *PL) {
        checkTypeAccessibility(TC, P->getTypeLoc(), fn,
                               [&](Accessibility typeAccess,
                                   const TypeRepr *thisComplainRepr) {
          if (!minAccess || *minAccess > typeAccess) {
            minAccess = typeAccess;
            complainRepr = thisComplainRepr;
          }
        });
      }
    }

    bool problemIsResult = false;
    if (auto FD = dyn_cast<FuncDecl>(fn)) {
      checkTypeAccessibility(TC, FD->getBodyResultTypeLoc(), FD,
                             [&](Accessibility typeAccess,
                                 const TypeRepr *thisComplainRepr) {
        if (!minAccess || *minAccess > typeAccess) {
          minAccess = typeAccess;
          complainRepr = thisComplainRepr;
          problemIsResult = true;
        }
      });
    }

    if (minAccess) {
      bool isExplicit =
        fn->getAttrs().hasAttribute<AccessibilityAttr>() ||
        D->getDeclContext()->isProtocolOrProtocolExtensionContext();
      auto diag = TC.diagnose(fn, diag::function_type_access,
                              isExplicit,
                              fn->getFormalAccess(),
                              *minAccess,
                              isa<ConstructorDecl>(fn) ? FK_Initializer :
                                isTypeContext ? FK_Method : FK_Function,
                              problemIsResult);
      highlightOffendingType(TC, diag, complainRepr);
    }
    return;
  }

  case DeclKind::EnumElement: {
    auto EED = cast<EnumElementDecl>(D);

    if (!EED->hasArgumentType())
      return;
    checkTypeAccessibility(TC, EED->getArgumentTypeLoc(), EED,
                           [&](Accessibility typeAccess,
                               const TypeRepr *complainRepr) {
      auto diag = TC.diagnose(EED, diag::enum_case_access,
                              EED->getFormalAccess(), typeAccess);
      highlightOffendingType(TC, diag, complainRepr);
    });

    return;
  }
  }
}

/// Figure out if a declaration should be exported to Objective C.
static Optional<ObjCReason> shouldMarkAsObjC(TypeChecker &TC,
                                             const ValueDecl *VD,
                                             bool allowImplicit = false){
  assert(!isa<ClassDecl>(VD));

  ProtocolDecl *protocolContext =
      dyn_cast<ProtocolDecl>(VD->getDeclContext());
  bool isMemberOfObjCProtocol =
      protocolContext && protocolContext->isObjC();

  // explicitly declared @objc.
  if (VD->getAttrs().hasAttribute<ObjCAttr>())
    return ObjCReason::ExplicitlyObjC;
  // dynamic, @IBOutlet and @NSManaged imply @objc.
  else if (VD->getAttrs().hasAttribute<DynamicAttr>())
    return ObjCReason::ExplicitlyDynamic;
  else if (VD->getAttrs().hasAttribute<IBOutletAttr>())
    return ObjCReason::ExplicitlyIBOutlet;
  else if (VD->getAttrs().hasAttribute<NSManagedAttr>())
    return ObjCReason::ExplicitlyNSManaged;
  // A member of an @objc protocol is implicitly @objc.
  else if (isMemberOfObjCProtocol)
    return ObjCReason::MemberOfObjCProtocol;
  // A @nonobjc is not @objc, even if it is an override of an @objc, so check
  // for @nonobjc first.
  else if (VD->getAttrs().hasAttribute<NonObjCAttr>())
    return None;
  // An override of an @objc declaration is implicitly @objc.
  else if (VD->getOverriddenDecl() && VD->getOverriddenDecl()->isObjC())
    return ObjCReason::OverridesObjC;
  else if (VD->isInvalid())
    return None;
  // Implicitly generated declarations are not @objc, except for constructors.
  else if (!allowImplicit && VD->isImplicit())
    return None;
  else if (VD->getFormalAccess() == Accessibility::Private)
    return None;

  // If this declaration is part of a class with implicitly @objc members,
  // make it implicitly @objc. However, if the declaration cannot be represented
  // as @objc, don't diagnose.
  Type contextTy = VD->getDeclContext()->getDeclaredTypeInContext();
  if (auto classDecl = contextTy->getClassOrBoundGenericClass())
    if (classDecl->checkObjCAncestry() != ObjCClassKind::NonObjC)
      return ObjCReason::DoNotDiagnose;

  return None;
}

/// If we need to infer 'dynamic', do so now.
///
/// This occurs when
/// - it is implied by an attribute like @NSManaged
/// - we need to dynamically dispatch to a method in an extension.
///
/// FIXME: The latter reason is a hack. We should figure out how to safely
/// put extension methods into the class vtable.
static void inferDynamic(ASTContext &ctx, ValueDecl *D) {
  // If we can't infer dynamic here, don't.
  if (!DeclAttribute::canAttributeAppearOnDecl(DAK_Dynamic, D))
    return;

  // Only 'objc' declarations use 'dynamic'.
  if (!D->isObjC() || D->hasClangNode())
    return;

  // Only introduce 'dynamic' on declarations...
  if (isa<ExtensionDecl>(D->getDeclContext())) {
    // ...in extensions that don't override other declarations.
    if (D->getOverriddenDecl())
      return;
  } else {
    // ...and in classes on decls marked @NSManaged.
    if (!D->getAttrs().hasAttribute<NSManagedAttr>())
      return;
  }

  // The presence of 'dynamic' or 'final' blocks the inference of 'dynamic'.
  if (D->isDynamic() || D->isFinal())
    return;

  // Add the 'dynamic' attribute.
  D->getAttrs().add(new (ctx) DynamicAttr(/*isImplicit=*/true));
}

/// Check runtime functions responsible for implicit bridging of Objective-C
/// types.
static void checkObjCBridgingFunctions(TypeChecker &TC,
                                       Module *mod,
                                       StringRef bridgedTypeName,
                                       StringRef forwardConversion,
                                       StringRef reverseConversion) {
  assert(mod);
  Module::AccessPathTy unscopedAccess = {};
  SmallVector<ValueDecl *, 4> results;
  
  auto &Ctx = TC.Context;
  mod->lookupValue(unscopedAccess, Ctx.getIdentifier(bridgedTypeName),
                   NLKind::QualifiedLookup, results);
  mod->lookupValue(unscopedAccess, Ctx.getIdentifier(forwardConversion),
                   NLKind::QualifiedLookup, results);
  mod->lookupValue(unscopedAccess, Ctx.getIdentifier(reverseConversion),
                   NLKind::QualifiedLookup, results);
  
  for (auto D : results)
    TC.validateDecl(D);
}

static void checkBridgedFunctions(TypeChecker &TC) {
  if (TC.HasCheckedBridgeFunctions)
    return;
  
  TC.HasCheckedBridgeFunctions = true;
  
  #define BRIDGE_TYPE(BRIDGED_MOD, BRIDGED_TYPE, _, NATIVE_TYPE, OPT) \
  Identifier ID_##BRIDGED_MOD = TC.Context.getIdentifier(#BRIDGED_MOD);\
  if (Module *module = TC.Context.getLoadedModule(ID_##BRIDGED_MOD)) {\
    checkObjCBridgingFunctions(TC, module, #BRIDGED_TYPE, \
    "_convert" #BRIDGED_TYPE "To" #NATIVE_TYPE, \
    "_convert" #NATIVE_TYPE "To" #BRIDGED_TYPE); \
  }
  #include "swift/SIL/BridgedTypes.def"
  
  if (Module *module = TC.Context.getLoadedModule(ID_Foundation)) {
    checkObjCBridgingFunctions(TC, module,
                               TC.Context.getSwiftName(
                                 KnownFoundationEntity::NSArray),
                               "_convertNSArrayToArray",
                               "_convertArrayToNSArray");
    checkObjCBridgingFunctions(TC, module,
                               TC.Context.getSwiftName(
                                 KnownFoundationEntity::NSDictionary),
                               "_convertNSDictionaryToDictionary",
                               "_convertDictionaryToNSDictionary");
    checkObjCBridgingFunctions(TC, module,
                               TC.Context.getSwiftName(
                                 KnownFoundationEntity::NSSet),
                               "_convertNSSetToSet",
                               "_convertSetToNSSet");
    checkObjCBridgingFunctions(TC, module,
                               TC.Context.getSwiftName(
                                 KnownFoundationEntity::NSError),
                               "_convertNSErrorToErrorType",
                               "_convertErrorTypeToNSError");
  }
}

/// Mark the given declaration as being Objective-C compatible (or
/// not) as appropriate.
///
/// If the declaration has a @nonobjc attribute, diagnose an error
/// using the given Reason, if present.
void swift::markAsObjC(TypeChecker &TC, ValueDecl *D,
                       Optional<ObjCReason> isObjC,
                       Optional<ForeignErrorConvention> errorConvention) {
  D->setIsObjC(isObjC.hasValue());

  if (!isObjC) {
    // FIXME: For now, only @objc declarations can be dynamic.
    if (auto attr = D->getAttrs().getAttribute<DynamicAttr>(D))
      attr->setInvalid();
    return;
  }

  // By now, the caller will have handled the case where an implicit @objc
  // could be overridden by @nonobjc. If we see a @nonobjc and we are trying
  // to add an @objc for whatever reason, diagnose an error.
  if (auto *attr = D->getAttrs().getAttribute<NonObjCAttr>()) {
    if (*isObjC == ObjCReason::DoNotDiagnose)
      isObjC = ObjCReason::ImplicitlyObjC;

    TC.diagnose(D->getStartLoc(), diag::nonobjc_not_allowed,
                getObjCDiagnosticAttrKind(*isObjC));

    attr->setInvalid();
  }

  // Make sure we have the appropriate bridging operations.
  checkBridgedFunctions(TC);

  // Record the name of this Objective-C method in its class.
  if (auto classDecl
        = D->getDeclContext()->isClassOrClassExtensionContext()) {
    if (auto method = dyn_cast<AbstractFunctionDecl>(D)) {
      // If we are overriding another method, make sure the
      // selectors line up.
      if (auto baseMethod = method->getOverriddenDecl()) {
        // If the overridden method has a foreign error convention,
        // adopt it.  Set the foreign error convention for a
        // throwing method.  Note that the foreign error convention
        // affects the selector, so we perform this first.
        if (method->isBodyThrowing()) {
          if (auto baseErrorConvention
                = baseMethod->getForeignErrorConvention()) {
            errorConvention = baseErrorConvention;
          }

          assert(errorConvention && "Missing error convention");
          method->setForeignErrorConvention(*errorConvention);
        }

        ObjCSelector baseSelector = baseMethod->getObjCSelector(&TC);
        if (baseSelector != method->getObjCSelector(&TC)) {
          // The selectors differ. If the method's selector was
          // explicitly specified, this is an error. Otherwise, we
          // inherit the selector.
          if (auto attr = method->getAttrs().getAttribute<ObjCAttr>()) {
            if (attr->hasName() && !attr->isNameImplicit()) {
              llvm::SmallString<64> baseScratch;
              TC.diagnose(attr->AtLoc,
                          diag::objc_override_method_selector_mismatch,
                          *attr->getName(), baseSelector)
                .fixItReplaceChars(attr->getNameLocs().front(),
                                   attr->getRParenLoc(),
                                   baseSelector.getString(baseScratch));
              TC.diagnose(baseMethod, diag::overridden_here);
            }

            // Override the name on the attribute.
            const_cast<ObjCAttr *>(attr)->setName(baseSelector,
                                                  /*implicit=*/true);
          } else {
            method->getAttrs().add(ObjCAttr::create(TC.Context,
                                                    baseSelector,
                                                    true));
          }
        }
      } else if (method->isBodyThrowing()) {
        // Attach the foreign error convention.
        assert(errorConvention && "Missing error convention");
        method->setForeignErrorConvention(*errorConvention);
      }

      classDecl->recordObjCMethod(method);

      // Swift does not permit class methods with Objective-C selectors 'load',
      // 'alloc', or 'allocWithZone:'.
      if (!method->isInstanceMember()) {
        auto isForbiddenSelector = [&TC](ObjCSelector sel) {
          switch (sel.getNumArgs()) {
          case 0:
            return sel.getSelectorPieces().front() == TC.Context.Id_load ||
                   sel.getSelectorPieces().front() == TC.Context.Id_alloc;
          case 1:
            return sel.getSelectorPieces().front()==TC.Context.Id_allocWithZone;
          default:
            return false;
          }
        };
        auto sel = method->getObjCSelector(&TC);
        if (isForbiddenSelector(sel)) {
          auto diagInfo = getObjCMethodDiagInfo(method);
          TC.diagnose(method, diag::objc_class_method_not_permitted,
                      diagInfo.first, diagInfo.second, sel);
        }
      }
    } else if (auto var = dyn_cast<VarDecl>(D)) {
      // If we are overriding a property, make sure that the
      // Objective-C names of the properties match.
      if (auto baseVar = var->getOverriddenDecl()) {
        if (var->getObjCPropertyName() != baseVar->getObjCPropertyName()) {
          Identifier baseName = baseVar->getObjCPropertyName();
          ObjCSelector baseSelector(TC.Context, 0, baseName);

          // If not, see whether we can implicitly adjust.
          if (auto attr = var->getAttrs().getAttribute<ObjCAttr>()) {
            if (attr->hasName() && !attr->isNameImplicit()) {
              TC.diagnose(attr->AtLoc,
                          diag::objc_override_property_name_mismatch,
                          attr->getName()->getSelectorPieces()[0],
                          baseName)
                .fixItReplaceChars(attr->getNameLocs().front(),
                                   attr->getRParenLoc(),
                                   baseName.str());
              TC.diagnose(baseVar, diag::overridden_here);
            }

            // Override the name on the attribute.
            const_cast<ObjCAttr *>(attr)->setName(baseSelector,
                                                  /*implicit=*/true);
          } else {
            var->getAttrs().add(ObjCAttr::create(TC.Context,
                                                 baseSelector,
                                                 true));
          }
        }
      }
    }
  } else if (auto method = dyn_cast<AbstractFunctionDecl>(D)) {
    if (method->isBodyThrowing()) {
      // Attach the foreign error convention.
      assert(errorConvention && "Missing error convention");
      method->setForeignErrorConvention(*errorConvention);
    }
  }

  // Record this method in the source-file-specific Objective-C method
  // table.
  if (auto method = dyn_cast<AbstractFunctionDecl>(D)) {
    if (auto sourceFile = method->getParentSourceFile()) {
      sourceFile->ObjCMethods[method->getObjCSelector()].push_back(method);
    }
  }
}

namespace {
  /// How to generate the raw value for each element of an enum that doesn't
  /// have one explicitly specified.
  enum class AutomaticEnumValueKind {
    /// Raw values cannot be automatically generated.
    None,
    /// The raw value is the enum element's name.
    String,
    /// The raw value is the previous element's raw value, incremented.
    ///
    /// For the first element in the enum, the raw value is 0.
    Integer,
  };
} // end anonymous namespace

/// Given the raw value literal expression for an enum case, produces the
/// auto-incremented raw value for the subsequent case, or returns null if
/// the value is not auto-incrementable.
static LiteralExpr *getAutomaticRawValueExpr(TypeChecker &TC,
                                             AutomaticEnumValueKind valueKind,
                                             EnumElementDecl *forElt,
                                             LiteralExpr *prevValue) {
  switch (valueKind) {
  case AutomaticEnumValueKind::None:
    TC.diagnose(forElt->getLoc(),
                diag::enum_non_integer_convertible_raw_type_no_value);
    return nullptr;

  case AutomaticEnumValueKind::String:
    return new (TC.Context) StringLiteralExpr(forElt->getNameStr(), SourceLoc(),
                                              /*implicit=*/true);

  case AutomaticEnumValueKind::Integer:
    // If there was no previous value, start from zero.
    if (!prevValue) {
      return new (TC.Context) IntegerLiteralExpr("0", SourceLoc(),
                                                 /*Implicit=*/true);
    }
    
    if (auto intLit = dyn_cast<IntegerLiteralExpr>(prevValue)) {
      APInt nextVal = intLit->getValue() + 1;
      bool negative = nextVal.slt(0);
      if (negative)
        nextVal = -nextVal;

      llvm::SmallString<10> nextValStr;
      nextVal.toStringSigned(nextValStr);
      auto expr = new (TC.Context)
        IntegerLiteralExpr(TC.Context.AllocateCopy(StringRef(nextValStr)),
                           forElt->getLoc(), /*Implicit=*/true);
      if (negative)
        expr->setNegative(forElt->getLoc());

      return expr;
    }

    TC.diagnose(forElt->getLoc(),
                diag::enum_non_integer_raw_value_auto_increment);
    return nullptr;
  }
}

static void checkEnumRawValues(TypeChecker &TC, EnumDecl *ED) {
  Type rawTy = ED->getRawType();

  if (!rawTy) {
    // @objc enums must have a raw type.
    if (ED->isObjC())
      TC.diagnose(ED->getNameLoc(), diag::objc_enum_no_raw_type);
    return;
  }

  rawTy = ArchetypeBuilder::mapTypeIntoContext(ED, rawTy);
  if (rawTy->is<ErrorType>())
    return;

  AutomaticEnumValueKind valueKind;

  if (ED->isObjC()) {
    // @objc enums must have a raw type that's an ObjC-representable
    // integer type.
    if (!TC.isCIntegerType(ED, rawTy)) {
      TC.diagnose(ED->getInherited().front().getSourceRange().Start,
                  diag::objc_enum_raw_type_not_integer,
                  rawTy);
      ED->getInherited().front().setInvalidType(TC.Context);
      return;
    }
    valueKind = AutomaticEnumValueKind::Integer;
  } else {
    // Swift enums require that the raw type is convertible from one of the
    // primitive literal protocols.
    auto conformsToProtocol = [&](KnownProtocolKind protoKind) {
        ProtocolDecl *proto = TC.getProtocol(ED->getLoc(), protoKind);
        return TC.conformsToProtocol(rawTy, proto, ED->getDeclContext(), None);
    };

    static auto otherLiteralProtocolKinds = {
      KnownProtocolKind::FloatLiteralConvertible,
      KnownProtocolKind::UnicodeScalarLiteralConvertible,
      KnownProtocolKind::ExtendedGraphemeClusterLiteralConvertible,
    };

    if (conformsToProtocol(KnownProtocolKind::IntegerLiteralConvertible)) {
      valueKind = AutomaticEnumValueKind::Integer;
    } else if (conformsToProtocol(KnownProtocolKind::StringLiteralConvertible)){
      valueKind = AutomaticEnumValueKind::String;
    } else if (std::any_of(otherLiteralProtocolKinds.begin(),
                           otherLiteralProtocolKinds.end(),
                           conformsToProtocol)) {
      valueKind = AutomaticEnumValueKind::None;
    } else {
      TC.diagnose(ED->getInherited().front().getSourceRange().Start,
                  diag::raw_type_not_literal_convertible,
                  rawTy);
      ED->getInherited().front().setInvalidType(TC.Context);
      return;
    }
  }

  // We need at least one case to have a raw value.
  if (ED->getAllElements().empty()) {
    TC.diagnose(ED->getInherited().front().getSourceRange().Start,
                diag::empty_enum_raw_type);
    return;
  }

  // Check the raw values of the cases.
  LiteralExpr *prevValue = nullptr;
  EnumElementDecl *lastExplicitValueElt = nullptr;

  // Keep a map we can use to check for duplicate case values.
  llvm::SmallDenseMap<RawValueKey, RawValueSource, 8> uniqueRawValues;

  for (auto elt : ED->getAllElements()) {
    // Make sure the element is checked out before we poke at it.
    TC.validateDecl(elt);
    
    if (elt->isInvalid())
      continue;

    // We don't yet support raw values on payload cases.
    if (elt->hasArgumentType()) {
      TC.diagnose(elt->getLoc(),
                  diag::enum_with_raw_type_case_with_argument);
      TC.diagnose(ED->getInherited().front().getSourceRange().Start,
                  diag::enum_raw_type_here, rawTy);
      continue;
    }
    
    // Check the raw value expr, if we have one.
    if (auto *rawValue = elt->getRawValueExpr()) {
      Expr *typeCheckedExpr = rawValue;
      if (!TC.typeCheckExpression(typeCheckedExpr, ED, rawTy,
                                  CTP_EnumCaseRawValue)) {
        elt->setTypeCheckedRawValueExpr(typeCheckedExpr);
      }
      lastExplicitValueElt = elt;
    } else {
      // If the enum element has no explicit raw value, try to
      // autoincrement from the previous value, or start from zero if this
      // is the first element.
      auto nextValue = getAutomaticRawValueExpr(TC, valueKind, elt, prevValue);
      if (!nextValue) {
        break;
      }
      elt->setRawValueExpr(nextValue);
      Expr *typeChecked = nextValue;
      if (!TC.typeCheckExpression(typeChecked, ED, rawTy,
                                  CTP_EnumCaseRawValue))
        elt->setTypeCheckedRawValueExpr(typeChecked);
    }
    prevValue = elt->getRawValueExpr();
    assert(prevValue && "continued without setting raw value of enum case");

    // If we didn't find a valid initializer (maybe the initial value was
    // incompatible with the raw value type) mark the entry as being erroneous.
    if (!elt->getTypeCheckedRawValueExpr()) {
      elt->setInvalid();
      continue;
    }

    TC.checkEnumElementErrorHandling(elt);

    // Find the type checked version of the LiteralExpr used for the raw value.
    // this is unfortunate, but is needed because we're digging into the
    // literals to get information about them, instead of accepting general
    // expressions.
    LiteralExpr *rawValue = elt->getRawValueExpr();
    if (!rawValue->getType()) {
      elt->getTypeCheckedRawValueExpr()->forEachChildExpr([&](Expr *E)->Expr* {
        if (E->getKind() == rawValue->getKind())
          rawValue = cast<LiteralExpr>(E);
        return E;
      });
      elt->setRawValueExpr(rawValue);
    }

    prevValue = rawValue;
    assert(prevValue && "continued without setting raw value of enum case");

    // Check that the raw value is unique.
    RawValueKey key(rawValue);
    RawValueSource source{elt, lastExplicitValueElt};

    auto insertIterPair = uniqueRawValues.insert({key, source});
    if (insertIterPair.second)
      continue;

    // Diagnose the duplicate value.
    SourceLoc diagLoc = elt->getRawValueExpr()->isImplicit()
        ? elt->getLoc() : elt->getRawValueExpr()->getLoc();
    TC.diagnose(diagLoc, diag::enum_raw_value_not_unique);
    assert(lastExplicitValueElt &&
           "should not be able to have non-unique raw values when "
           "relying on autoincrement");
    if (lastExplicitValueElt != elt &&
        valueKind == AutomaticEnumValueKind::Integer) {
      TC.diagnose(lastExplicitValueElt->getRawValueExpr()->getLoc(),
                  diag::enum_raw_value_incrementing_from_here);
    }

    RawValueSource prevSource = insertIterPair.first->second;
    auto foundElt = prevSource.sourceElt;
    diagLoc = foundElt->getRawValueExpr()->isImplicit()
        ? foundElt->getLoc() : foundElt->getRawValueExpr()->getLoc();
    TC.diagnose(diagLoc, diag::enum_raw_value_used_here);
    if (foundElt != prevSource.lastExplicitValueElt &&
        valueKind == AutomaticEnumValueKind::Integer) {
      if (prevSource.lastExplicitValueElt)
        TC.diagnose(prevSource.lastExplicitValueElt
                      ->getRawValueExpr()->getLoc(),
                    diag::enum_raw_value_incrementing_from_here);
      else
        TC.diagnose(ED->getAllElements().front()->getLoc(),
                    diag::enum_raw_value_incrementing_from_zero);
    }
  }
}

/// Walks up the override chain for \p CD until it finds an initializer that is
/// required and non-implicit. If no such initializer exists, returns the
/// declaration where \c required was introduced (i.e. closest to the root
/// class).
static const ConstructorDecl *
findNonImplicitRequiredInit(const ConstructorDecl *CD) {
  while (CD->isImplicit()) {
    auto *overridden = CD->getOverriddenDecl();
    if (!overridden || !overridden->isRequired())
      break;
    CD = overridden;
  }
  return CD;
}

namespace {
class DeclChecker : public DeclVisitor<DeclChecker> {
public:
  TypeChecker &TC;

  // For library-style parsing, we need to make two passes over the global
  // scope.  These booleans indicate whether this is currently the first or
  // second pass over the global scope (or neither, if we're in a context where
  // we only visit each decl once).
  unsigned IsFirstPass : 1;
  unsigned IsSecondPass : 1;

  DeclChecker(TypeChecker &TC, bool IsFirstPass, bool IsSecondPass)
      : TC(TC), IsFirstPass(IsFirstPass), IsSecondPass(IsSecondPass) {}

  void visit(Decl *decl) {
    
    DeclVisitor<DeclChecker>::visit(decl);

    if (auto VD = dyn_cast<ValueDecl>(decl)) {
      checkRedeclaration(TC, VD);
      
      // If this is a member of a nominal type, don't allow it to have a name of
      // "Type" or "Protocol" since we reserve the X.Type and X.Protocol
      // expressions to mean something builtin to the language.  We *do* allow
      // these if they are escaped with backticks though.
      auto &Context = TC.Context;
      if (VD->getDeclContext()->isTypeContext() &&
          (VD->getFullName().isSimpleName(Context.Id_Type) ||
           VD->getFullName().isSimpleName(Context.Id_Protocol)) &&
          VD->getNameLoc().isValid() &&
          Context.SourceMgr.extractText({VD->getNameLoc(), 1}) != "`") {
        TC.diagnose(VD->getNameLoc(), diag::reserved_member_name,
                    VD->getFullName(), VD->getNameStr());
        TC.diagnose(VD->getNameLoc(), diag::backticks_to_escape)
          .fixItReplace(VD->getNameLoc(), "`"+VD->getNameStr().str()+"`");
      }
    }

    if ((IsSecondPass && !IsFirstPass) ||
        decl->getDeclContext()->isProtocolOrProtocolExtensionContext()) {
      TC.checkUnsupportedProtocolType(decl);
    }
  }

  //===--------------------------------------------------------------------===//
  // Helper Functions.
  //===--------------------------------------------------------------------===//

  static bool isPrivateConformer(const ExtensionDecl *ED) {
    return ED->getDefaultAccessibility() == Accessibility::Private;
  }

  static bool isPrivateConformer(const NominalTypeDecl *NTD) {
    return NTD->getFormalAccess() == Accessibility::Private;
  }

  template<typename NominalOrExtensionDecl>
  void checkExplicitConformance(NominalOrExtensionDecl *D, Type T) {
    // For anything with a Clang node, lazily check conformances.
    if (D->hasClangNode())
      return;

    ReferencedNameTracker *tracker = nullptr;
    if (SourceFile *SF = D->getParentSourceFile())
      tracker = SF->getReferencedNameTracker();

    // Check each of the conformances associated with this context.
    SmallVector<ConformanceDiagnostic, 4> diagnostics;
    SmallVector<ProtocolDecl *, 4> protocols;
    for (auto conformance : D->getLocalConformances(ConformanceLookupKind::All,
                                                    &diagnostics,
                                                    /*sorted=*/true)) {
      // Check and record normal conformances.
      if (auto normal = dyn_cast<NormalProtocolConformance>(conformance)) {
        TC.checkConformance(normal);

        protocols.push_back(conformance->getProtocol());
      }

      if (tracker)
        tracker->addUsedMember({conformance->getProtocol(), Identifier()},
                               !isPrivateConformer(D));
    }

    // Diagnose any conflicts attributed to this declaration context.
    for (const auto &diag : diagnostics) {
      // Figure out the declaration of the existing conformance.
      Decl *existingDecl = dyn_cast<NominalTypeDecl>(diag.ExistingDC);
      if (!existingDecl)
        existingDecl = cast<ExtensionDecl>(diag.ExistingDC);

      // Complain about redundant conformances.
      TC.diagnose(diag.Loc, diag::redundant_conformance,
                  D->getDeclaredTypeInContext(),
                  diag.Protocol->getName());

      TC.diagnose(existingDecl, diag::declared_protocol_conformance_here,
                  D->getDeclaredTypeInContext(),
                  static_cast<unsigned>(diag.ExistingKind),
                  diag.Protocol->getName(),
                  diag.ExistingExplicitProtocol->getName());
    }
  }

  //===--------------------------------------------------------------------===//
  // Visit Methods.
  //===--------------------------------------------------------------------===//

  void visitGenericTypeParamDecl(GenericTypeParamDecl *D) {
    llvm_unreachable("cannot reach here");
  }
  
  void visitImportDecl(ImportDecl *ID) {
    TC.checkDeclAttributesEarly(ID);
    TC.checkDeclAttributes(ID);
  }

  void visitOperatorDecl(OperatorDecl *OD) {
    TC.checkDeclAttributesEarly(OD);
    TC.checkDeclAttributes(OD);
  }

  void visitBoundVariable(VarDecl *VD) {
    if (!VD->getType()->isMaterializable()) {
      TC.diagnose(VD->getStartLoc(), diag::var_type_not_materializable,
                  VD->getType());
      VD->overwriteType(ErrorType::get(TC.Context));
      VD->setInvalid();
    }

    TC.validateDecl(VD);

    // WARNING: Anything you put in this function will only be run when the
    // VarDecl is fully type-checked within its own file. It will NOT be run
    // when the VarDecl is merely used from another file.

    // Reject cases where this is a variable that has storage but it isn't
    // allowed.
    if (VD->hasStorage()) {
      // In a protocol context, variables written as "var x : Int" are errors
      // and recovered by building a computed property with just a getter.
      // Diagnose this and create the getter decl now.
      if (isa<ProtocolDecl>(VD->getDeclContext())) {
        if (VD->isLet())
          TC.diagnose(VD->getLoc(),
                      diag::protocol_property_must_be_computed_var);
        else
          TC.diagnose(VD->getLoc(), diag::protocol_property_must_be_computed);

        convertStoredVarInProtocolToComputed(VD, TC);
      } else if (isa<EnumDecl>(VD->getDeclContext()) &&
                 !VD->isStatic()) {
        // Enums can only have computed properties.
        TC.diagnose(VD->getLoc(), diag::enum_stored_property);
        VD->setInvalid();
        VD->overwriteType(ErrorType::get(TC.Context));
      } else if (isa<ExtensionDecl>(VD->getDeclContext()) &&
                 !VD->isStatic()) {
        TC.diagnose(VD->getLoc(), diag::extension_stored_property);
        VD->setInvalid();
        VD->overwriteType(ErrorType::get(TC.Context));
      }
    }

    // Synthesize accessors for lazy, all checking already been performed.
    if (VD->getAttrs().hasAttribute<LazyAttr>() && !VD->isStatic() &&
        !VD->getGetter()->hasBody())
      TC.completeLazyVarImplementation(VD);

    // If this is a willSet/didSet property, synthesize the getter and setter
    // decl.
    if (VD->hasObservers() && !VD->getGetter()->getBody())
      synthesizeObservingAccessors(VD, TC);

    // If this is a get+mutableAddress property, synthesize the setter body.
    if (VD->getStorageKind() == VarDecl::ComputedWithMutableAddress &&
        !VD->getSetter()->getBody()) {
      synthesizeSetterForMutableAddressedStorage(VD, TC);
    }

    // Synthesize materializeForSet in non-protocol contexts.
    if (auto materializeForSet = VD->getMaterializeForSetFunc()) {
      if (!VD->getDeclContext()->isProtocolOrProtocolExtensionContext()) {
        synthesizeMaterializeForSet(materializeForSet, VD, TC);
        TC.typeCheckDecl(materializeForSet, true);
        TC.typeCheckDecl(materializeForSet, false);
      }
    }

    TC.checkDeclAttributes(VD);
    TC.checkOmitNeedlessWords(VD);
  }


  void visitBoundVars(Pattern *P) {
    P->forEachVariable([&] (VarDecl *VD) { this->visitBoundVariable(VD); });
  }

  void visitPatternBindingDecl(PatternBindingDecl *PBD) {
    // Check all the pattern/init pairs in the PBD.
    for (unsigned i = 0, e = PBD->getNumPatternEntries(); i != e; ++i)
      validatePatternBindingDecl(TC, PBD, i);

    if (PBD->isBeingTypeChecked())
      return;

    // If the initializers in the PBD aren't checked yet, do so now.
    if (!IsFirstPass) {
      for (unsigned i = 0, e = PBD->getNumPatternEntries(); i != e; ++i) {
        if (!PBD->isInitializerChecked(i) && PBD->getInit(i))
          TC.typeCheckPatternBinding(PBD, i);
      }
    }

    TC.checkDeclAttributesEarly(PBD);

    if (!IsSecondPass) {
      for (unsigned i = 0, e = PBD->getNumPatternEntries(); i != e; ++i) {
        // Type check each VarDecl that this PatternBinding handles.
        visitBoundVars(PBD->getPattern(i));

        // If we have a type but no initializer, check whether the type is
        // default-initializable. If so, do it.
        if (PBD->getPattern(i)->hasType() &&
            !PBD->getInit(i) &&
            PBD->hasStorage() &&
            !PBD->getPattern(i)->getType()->is<ErrorType>()) {

          // If we have a type-adjusting attribute (like ownership), apply it now.
          if (auto var = PBD->getSingleVar())
            TC.checkTypeModifyingDeclAttributes(var);

          // Decide whether we should suppress default initialization.
          if (isNeverDefaultInitializable(PBD->getPattern(i)))
            continue;

          auto type = PBD->getPattern(i)->getType();
          if (auto defaultInit = buildDefaultInitializer(TC, type)) {
            // If we got a default initializer, install it and re-type-check it
            // to make sure it is properly coerced to the pattern type.
            PBD->setInit(i, defaultInit);
            TC.typeCheckPatternBinding(PBD, i);
          }
        }
      }
    }

    bool isInSILMode = false;
    if (auto sourceFile = PBD->getDeclContext()->getParentSourceFile())
      isInSILMode = sourceFile->Kind == SourceFileKind::SIL;
    bool isTypeContext = PBD->getDeclContext()->isTypeContext();

    // If this is a declaration without an initializer, reject code if
    // uninitialized vars are not allowed.
    for (unsigned i = 0, e = PBD->getNumPatternEntries(); i != e; ++i) {
      auto entry = PBD->getPatternList()[i];
    
      if (entry.getInit() || isInSILMode) continue;
      
      entry.getPattern()->forEachVariable([&](VarDecl *var) {
        // If the variable has no storage, it never needs an initializer.
        if (!var->hasStorage())
          return;

        auto *varDC = var->getDeclContext();

        // Non-member observing properties need an initializer.
        if (var->getStorageKind() == VarDecl::StoredWithObservers &&
            !isTypeContext) {
          TC.diagnose(var->getLoc(), diag::observingprop_requires_initializer);
          PBD->setInvalid();
          var->setInvalid();
          if (!var->hasType())
            var->setType(ErrorType::get(TC.Context));
          return;
        }

        // Static/class declarations require an initializer unless in a
        // protocol.
        if (var->isStatic() && !isa<ProtocolDecl>(varDC) &&
            !var->isInvalid() && !PBD->isInvalid()) {
          TC.diagnose(var->getLoc(), diag::static_requires_initializer,
                      var->getCorrectStaticSpelling());
          PBD->setInvalid();
          var->setInvalid();
          if (!var->hasType())
            var->setType(ErrorType::get(TC.Context));
          return;
        }

        // Global variables require an initializer (except in top level code).
        if (varDC->isModuleScopeContext() &&
            !varDC->getParentSourceFile()->isScriptMode() &&
            !var->isInvalid() && !PBD->isInvalid()) {
          TC.diagnose(var->getLoc(),
                      diag::global_requires_initializer, var->isLet());
          PBD->setInvalid();
          var->setInvalid();
          if (!var->hasType())
            var->setType(ErrorType::get(TC.Context));
          return;
        }
      });
    }

    if (!IsFirstPass)
      checkAccessibility(TC, PBD);

    TC.checkDeclAttributes(PBD);
  }

  void visitSubscriptDecl(SubscriptDecl *SD) {
    if (IsSecondPass) {
      checkAccessibility(TC, SD);
      return;
    }

    if (SD->hasType())
      return;

    assert(SD->getDeclContext()->isTypeContext() &&
           "Decl parsing must prevent subscripts outside of types!");

    TC.checkDeclAttributesEarly(SD);
    TC.computeAccessibility(SD);

    auto dc = SD->getDeclContext();
    bool isInvalid = TC.validateType(SD->getElementTypeLoc(), dc);
    isInvalid |= TC.typeCheckParameterList(SD->getIndices(), dc,
                                           TypeResolutionOptions());

    if (isInvalid) {
      SD->overwriteType(ErrorType::get(TC.Context));
      SD->setInvalid();
    } else {
      // Hack to deal with types already getting set during type validation
      // above.
      if (SD->hasType())
        return;

      // Relabel the indices according to the subscript name.
      auto indicesType = SD->getIndices()->getType(TC.Context);
      SD->setType(FunctionType::get(indicesType, SD->getElementType()));

      // If we're in a generic context, set the interface type.
      if (dc->isGenericContext()) {
        auto indicesTy = ArchetypeBuilder::mapTypeOutOfContext(
                           dc, indicesType);
        auto elementTy = ArchetypeBuilder::mapTypeOutOfContext(
                           dc, SD->getElementType());
        SD->setInterfaceType(FunctionType::get(indicesTy, elementTy));
      }
    }

    validateAttributes(TC, SD);

    if (!checkOverrides(TC, SD)) {
      // If a subscript has an override attribute but does not override
      // anything, complain.
      if (auto *OA = SD->getAttrs().getAttribute<OverrideAttr>()) {
        if (!SD->getOverriddenDecl()) {
          TC.diagnose(SD, diag::subscript_does_not_override)
              .highlight(OA->getLocation());
          OA->setInvalid();
        }
      }
    }

    // Member subscripts need some special validation logic.
    if (auto contextType = dc->getDeclaredTypeInContext()) {
      // If this is a class member, mark it final if the class is final.
      if (auto cls = contextType->getClassOrBoundGenericClass()) {
        if (cls->isFinal() && !SD->isFinal()) {
          makeFinal(TC.Context, SD);
        }
      }

      // A subscript is ObjC-compatible if it's explicitly @objc, or a
      // member of an ObjC-compatible class or protocol.
      Optional<ObjCReason> isObjC = shouldMarkAsObjC(TC, SD);

      if (isObjC && !TC.isRepresentableInObjC(SD, *isObjC))
        isObjC = None;
      markAsObjC(TC, SD, isObjC);
    }

    // If this variable is marked final and has a getter or setter, mark the
    // getter and setter as final as well.
    if (SD->isFinal()) {
      makeFinal(TC.Context, SD->getGetter());
      makeFinal(TC.Context, SD->getSetter());
      makeFinal(TC.Context, SD->getMaterializeForSetFunc());
    }

    if (SD->hasAccessorFunctions()) {
      maybeAddMaterializeForSet(SD, TC);
    }

    // Make sure the getter and setter have valid types, since they will be
    // used by SILGen for any accesses to this subscript.
    if (auto getter = SD->getGetter())
      TC.validateDecl(getter);
    if (auto setter = SD->getSetter())
      TC.validateDecl(setter);

    // If this is a get+mutableAddress property, synthesize the setter body.
    if (SD->getStorageKind() == SubscriptDecl::ComputedWithMutableAddress &&
        !SD->getSetter()->getBody()) {
      synthesizeSetterForMutableAddressedStorage(SD, TC);
    }

    inferDynamic(TC.Context, SD);

    // Synthesize materializeForSet in non-protocol contexts.
    if (auto materializeForSet = SD->getMaterializeForSetFunc()) {
      if (!SD->getDeclContext()->isProtocolOrProtocolExtensionContext()) {
        synthesizeMaterializeForSet(materializeForSet, SD, TC);
        TC.typeCheckDecl(materializeForSet, true);
        TC.typeCheckDecl(materializeForSet, false);
      }
    }

    TC.checkDeclAttributes(SD);
  }

  void visitTypeAliasDecl(TypeAliasDecl *TAD) {
    if (TAD->isBeingTypeChecked()) {
      
      if (!TAD->hasUnderlyingType()) {
        TAD->setInvalid();
        TAD->overwriteType(ErrorType::get(TC.Context));
        TAD->getUnderlyingTypeLoc().setInvalidType(TC.Context);
        
        TC.diagnose(TAD->getLoc(), diag::circular_type_alias, TAD->getName());
      }
      return;
    }
    
    TAD->setIsBeingTypeChecked();
    
    TC.checkDeclAttributesEarly(TAD);
    TC.computeAccessibility(TAD);
    if (!IsSecondPass) {
      if (!TAD->hasType())
        TAD->computeType();

      TypeResolutionOptions options;
      if (!TAD->getDeclContext()->isTypeContext())
        options |= TR_GlobalTypeAlias;
      if (TAD->getFormalAccess() == Accessibility::Private)
        options |= TR_KnownNonCascadingDependency;
      
      if (TAD->getDeclContext()->isModuleScopeContext()) {
        IterativeTypeChecker ITC(TC);
        ITC.satisfy(requestResolveTypeDecl(TAD));
      } else if (TC.validateType(TAD->getUnderlyingTypeLoc(),
                                 TAD->getDeclContext(), options)) {
        TAD->setInvalid();
        TAD->overwriteType(ErrorType::get(TC.Context));
        TAD->getUnderlyingTypeLoc().setInvalidType(TC.Context);
      } else if (TAD->getDeclContext()->isGenericContext()) {
        TAD->setInterfaceType(
          ArchetypeBuilder::mapTypeOutOfContext(TAD->getDeclContext(),
                                                TAD->getType()));
      }

      // We create TypeAliasTypes with invalid underlying types, so we
      // need to propagate recursive properties now.
      TAD->getAliasType()->setRecursiveProperties(
                       TAD->getUnderlyingType()->getRecursiveProperties());
    }

    if (IsSecondPass)
      checkAccessibility(TC, TAD);

    TC.checkDeclAttributes(TAD);
    
    TAD->setIsBeingTypeChecked(false);
  }
  
  void visitAssociatedTypeDecl(AssociatedTypeDecl *assocType) {
    if (assocType->isBeingTypeChecked()) {

      if (!assocType->isInvalid()) {
        assocType->setInvalid();
        assocType->overwriteType(ErrorType::get(TC.Context));
        TC.diagnose(assocType->getLoc(), diag::circular_type_alias, assocType->getName());
      }
      return;
    }

    assocType->setIsBeingTypeChecked();

    TC.checkDeclAttributesEarly(assocType);
    if (!assocType->hasAccessibility())
      assocType->setAccessibility(assocType->getProtocol()->getFormalAccess());

    TC.checkInheritanceClause(assocType);

    // Check the default definition, if there is one.
    TypeLoc &defaultDefinition = assocType->getDefaultDefinitionLoc();
    if (!defaultDefinition.isNull() &&
        TC.validateType(defaultDefinition, assocType->getDeclContext())) {
      defaultDefinition.setInvalidType(TC.Context);
    }
    TC.checkDeclAttributes(assocType);

    assocType->setIsBeingTypeChecked(false);
  }

  bool checkUnsupportedNestedGeneric(NominalTypeDecl *NTD) {
    // We don't support nested types in generics yet.
    if (NTD->isGenericContext()) {
      auto DC = NTD->getDeclContext();
      if (DC->isTypeContext()) {
        if (NTD->getGenericParams())
          TC.diagnose(NTD->getLoc(), diag::unsupported_generic_nested_in_type,
                NTD->getName(),
                DC->getDeclaredTypeOfContext());
        else
          TC.diagnose(NTD->getLoc(),
                      diag::unsupported_type_nested_in_generic_type,
                      NTD->getName(),
                      DC->getDeclaredTypeOfContext());
        return true;
      } else if (DC->isLocalContext() && DC->isGenericContext()) {
        // A local generic context is a generic function.
        if (auto AFD = dyn_cast<AbstractFunctionDecl>(DC)) {
          TC.diagnose(NTD->getLoc(),
                      diag::unsupported_type_nested_in_generic_function,
                      NTD->getName(),
                      AFD->getName());
          return true;
        }
      }
    }
    return false;
  }

  void visitEnumDecl(EnumDecl *ED) {
    // This enum declaration is technically a parse error, so do not type
    // check.
    if (isa<ProtocolDecl>(ED->getParent()))
      return;

    // Types cannot be defined in a protocol extension.
    if (ED->getDeclContext()->isProtocolExtensionContext()) {
      if (!ED->isInvalid())
        TC.diagnose(ED->getLoc(), diag::extension_protocol_type_definition,
                    ED->getFullName());
      ED->setInvalid();
      return;
    }

    TC.checkDeclAttributesEarly(ED);
    TC.computeAccessibility(ED);

    if (!IsSecondPass) {
      checkUnsupportedNestedGeneric(ED);

      TC.validateDecl(ED);

      TC.ValidatedTypes.remove(ED);

      {
        // Check for circular inheritance of the raw type.
        SmallVector<EnumDecl *, 8> path;
        checkCircularity(TC, ED, diag::circular_enum_inheritance,
                         diag::enum_here, path);
      }
      {
        // Check for duplicate enum members.
        llvm::DenseMap<Identifier, EnumElementDecl *> Elements;
        for (auto *EED : ED->getAllElements()) {
          auto Res = Elements.insert({ EED->getName(), EED });
          if (!Res.second) {
            EED->overwriteType(ErrorType::get(TC.Context));
            EED->setInvalid();
            if (auto *RawValueExpr = EED->getRawValueExpr())
              RawValueExpr->setType(ErrorType::get(TC.Context));

            auto PreviousEED = Res.first->second;
            TC.diagnose(EED->getLoc(), diag::duplicate_enum_element);
            TC.diagnose(PreviousEED->getLoc(),
                        diag::previous_decldef, true, EED->getName());
          }
        }
      }
    }

    if (!IsFirstPass) {
      checkAccessibility(TC, ED);

      if (ED->hasRawType() && !ED->isObjC()) {
        // ObjC enums have already had their raw values checked, but pure Swift
        // enums haven't.
        checkEnumRawValues(TC, ED);
      }

      checkExplicitConformance(ED, ED->getDeclaredTypeInContext());
    }
    
    for (Decl *member : ED->getMembers())
      visit(member);
    for (Decl *global : ED->getDerivedGlobalDecls())
      visit(global);
    

    TC.checkDeclAttributes(ED);
  }

  void visitStructDecl(StructDecl *SD) {
    // This struct declaration is technically a parse error, so do not type
    // check.
    if (isa<ProtocolDecl>(SD->getParent()))
      return;

    // Types cannot be defined in a protocol extension.
    if (SD->getDeclContext()->isProtocolExtensionContext()) {
      if (!SD->isInvalid())
        TC.diagnose(SD->getLoc(), diag::extension_protocol_type_definition,
                    SD->getFullName());
      SD->setInvalid();
      return;
    }

    TC.checkDeclAttributesEarly(SD);
    TC.computeAccessibility(SD);

    if (!IsSecondPass) {
      checkUnsupportedNestedGeneric(SD);

      TC.validateDecl(SD);
      TC.ValidatedTypes.remove(SD);
      TC.addImplicitConstructors(SD);
    }

    if (!IsFirstPass) {
      checkAccessibility(TC, SD);

      if (!SD->isInvalid())
        checkExplicitConformance(SD, SD->getDeclaredTypeInContext());
    }

    // Visit each of the members.
    for (Decl *Member : SD->getMembers())
      visit(Member);
    for (Decl *Global : SD->getDerivedGlobalDecls())
      visit(Global);

    TC.checkDeclAttributes(SD);
  }

  /// Check whether the given properties can be @NSManaged in this class.
  static bool propertiesCanBeNSManaged(ClassDecl *classDecl,
                                       ArrayRef<VarDecl *> vars) {
    // Check whether we have an Objective-C-defined class in our
    // inheritance chain.
    while (classDecl) {
      // If we found an Objective-C-defined class, continue checking.
      if (classDecl->hasClangNode())
        break;

      // If we ran out of superclasses, we're done.
      if (!classDecl->hasSuperclass())
        return false;

      classDecl = classDecl->getSuperclass()->getClassOrBoundGenericClass();
    }

    // If all of the variables are @objc, we can use @NSManaged.
    for (auto var : vars) {
      if (!var->isObjC())
        return false;
    }

    // Okay, we can use @NSManaged.
    return true;
  }

  /// Check that all stored properties have in-class initializers.
  void checkRequiredInClassInits(ClassDecl *cd) {
    ClassDecl *source = nullptr;
    for (auto member : cd->getMembers()) {
      auto pbd = dyn_cast<PatternBindingDecl>(member);
      if (!pbd)
        continue;

      if (pbd->isStatic() || !pbd->hasStorage() || 
          isDefaultInitializable(pbd) || pbd->isInvalid())
        continue;

      // The variables in this pattern have not been
      // initialized. Diagnose the lack of initial value.
      pbd->setInvalid();
      SmallVector<VarDecl *, 4> vars;
      for (auto entry : pbd->getPatternList())
        entry.getPattern()->collectVariables(vars);
      bool suggestNSManaged = propertiesCanBeNSManaged(cd, vars);
      switch (vars.size()) {
      case 0:
        llvm_unreachable("should have been marked invalid");

      case 1:
        TC.diagnose(pbd->getLoc(), diag::missing_in_class_init_1,
                    vars[0]->getName(), suggestNSManaged);
        break;

      case 2:
        TC.diagnose(pbd->getLoc(), diag::missing_in_class_init_2,
                    vars[0]->getName(), vars[1]->getName(), suggestNSManaged);
        break;

      case 3:
        TC.diagnose(pbd->getLoc(), diag::missing_in_class_init_3plus,
                    vars[0]->getName(), vars[1]->getName(), vars[2]->getName(),
                    false, suggestNSManaged);
        break;

      default:
        TC.diagnose(pbd->getLoc(), diag::missing_in_class_init_3plus,
                    vars[0]->getName(), vars[1]->getName(), vars[2]->getName(),
                    true, suggestNSManaged);
        break;
      }

      // Figure out where this requirement came from.
      if (!source) {
        source = cd;
        while (true) {
          // If this class had the 'requires_stored_property_inits'
          // attribute, diagnose here.
          if (source->getAttrs().
                hasAttribute<RequiresStoredPropertyInitsAttr>())
            break;

          // If the superclass doesn't require in-class initial
          // values, the requirement was introduced at this point, so
          // stop here.
          auto superclass = cast<ClassDecl>(
                              source->getSuperclass()->getAnyNominal());
          if (!superclass->requiresStoredPropertyInits())
            break;

          // Keep looking.
          source = superclass;
        }
      }

      // Add a note describing why we need an initializer.
      TC.diagnose(source, diag::requires_stored_property_inits_here,
                  source->getDeclaredType(), cd == source, suggestNSManaged);
    }
  }


  void visitClassDecl(ClassDecl *CD) {
    // This class declaration is technically a parse error, so do not type
    // check.
    if (isa<ProtocolDecl>(CD->getParent()))
      return;

    // Types cannot be defined in a protocol extension.
    if (CD->getDeclContext()->isProtocolExtensionContext()) {
      if (!CD->isInvalid())
        TC.diagnose(CD->getLoc(), diag::extension_protocol_type_definition,
                    CD->getFullName());
      CD->setInvalid();
      return;
    }

    TC.checkDeclAttributesEarly(CD);
    TC.computeAccessibility(CD);

    if (!IsSecondPass) {
      checkUnsupportedNestedGeneric(CD);

      TC.validateDecl(CD);

      TC.ValidatedTypes.remove(CD);

      {
        // Check for circular inheritance.
        SmallVector<ClassDecl *, 8> path;
        checkCircularity(TC, CD, diag::circular_class_inheritance,
                         diag::class_here, path);
      }
    }

    // If this class needs an implicit constructor, add it.
    if (!IsFirstPass)
      TC.addImplicitConstructors(CD);

    TC.addImplicitDestructor(CD);

    if (!IsFirstPass && !CD->isInvalid())
      checkExplicitConformance(CD, CD->getDeclaredTypeInContext());

    for (Decl *Member : CD->getMembers())
      visit(Member);
    for (Decl *global : CD->getDerivedGlobalDecls())
      visit(global);

    // If this class requires all of its stored properties to have
    // in-class initializers, diagnose this now.
    if (CD->requiresStoredPropertyInits())
      checkRequiredInClassInits(CD);

    if (!IsFirstPass) {
      if (auto superclassTy = CD->getSuperclass()) {
        ClassDecl *Super = superclassTy->getClassOrBoundGenericClass();

        if (auto *SF = CD->getParentSourceFile()) {
          if (auto *tracker = SF->getReferencedNameTracker()) {
            bool isPrivate = CD->getFormalAccess() == Accessibility::Private;
            tracker->addUsedMember({Super, Identifier()}, !isPrivate);
          }
        }

        if (Super->isFinal()) {
          TC.diagnose(CD, diag::inheritance_from_final_class,
                      Super->getName());
          return;
        }
      }

      checkAccessibility(TC, CD);
    }

    TC.checkDeclAttributes(CD);
  }

  void visitProtocolDecl(ProtocolDecl *PD) {
    // This protocol declaration is technically a parse error, so do not type
    // check.
    if (isa<ProtocolDecl>(PD->getParent()))
      return;

    TC.checkDeclAttributesEarly(PD);
    TC.computeAccessibility(PD);

    if (IsSecondPass) {
      checkAccessibility(TC, PD);
      for (auto member : PD->getMembers())
        checkAccessibility(TC, member);
      TC.checkInheritanceClause(PD);
      return;
    }

    PD->setIsBeingTypeChecked();
    
    TC.validateDecl(PD);

    {
      // Check for circular inheritance within the protocol.
      SmallVector<ProtocolDecl *, 8> path;
      checkCircularity(TC, PD, diag::circular_protocol_def,
                       diag::protocol_here, path);

      // Make sure the parent protocols have been fully validated.
      for (auto inherited : PD->getLocalProtocols()) {
        TC.validateDecl(inherited);
        for (auto *member : inherited->getMembers())
          if (auto *requirement = dyn_cast<ValueDecl>(member))
            TC.validateDecl(requirement);
      }

      if (auto *SF = PD->getParentSourceFile()) {
        if (auto *tracker = SF->getReferencedNameTracker()) {
          bool isNonPrivate = (PD->getFormalAccess() != Accessibility::Private);
          for (auto *parentProto : PD->getInheritedProtocols(nullptr))
            tracker->addUsedMember({parentProto, Identifier()}, isNonPrivate);
        }
      }
    }
    PD->setIsBeingTypeChecked(false);

    // Check the members.
    for (auto Member : PD->getMembers())
      visit(Member);

    TC.checkDeclAttributes(PD);
  }

  void visitVarDecl(VarDecl *VD) {
    // Delay type-checking on VarDecls until we see the corresponding
    // PatternBindingDecl.
  }

  bool semaFuncParamPatterns(AbstractFunctionDecl *fd,
                             GenericTypeResolver *resolver = nullptr) {
    bool hadError = false;
    for (auto paramList : fd->getParameterLists()) {
      hadError |= TC.typeCheckParameterList(paramList, fd,
                                            TypeResolutionOptions(),
                                            resolver);
    }

    return hadError;
  }

  void semaFuncDecl(FuncDecl *FD, GenericTypeResolver *resolver) {
    if (FD->hasType())
      return;

    TC.checkForForbiddenPrefix(FD);

    FD->setIsBeingTypeChecked();

    bool badType = false;
    if (!FD->getBodyResultTypeLoc().isNull()) {
      TypeResolutionOptions options;
      if (FD->hasDynamicSelf())
        options |= TR_DynamicSelfResult;
      if (TC.validateType(FD->getBodyResultTypeLoc(), FD, options,
                          resolver)) {
        badType = true;
      }
    }

    if (!badType) {
      badType = semaFuncParamPatterns(FD, resolver);
    }

    FD->setIsBeingTypeChecked(false);

    // Checking the function parameter patterns might (recursively)
    // end up setting the type.
    if (FD->hasType())
      return;

    if (badType) {
      FD->setType(ErrorType::get(TC.Context));
      FD->setInvalid();
      return;
    }

    Type funcTy = FD->getBodyResultTypeLoc().getType();
    if (!funcTy) {
      funcTy = TupleType::getEmpty(TC.Context);
    }
    auto bodyResultType = funcTy;

    // Form the function type by building the curried function type
    // from the back to the front, "prepending" each of the parameter
    // patterns.
    GenericParamList *genericParams = FD->getGenericParams();
    GenericParamList *outerGenericParams = nullptr;
    auto paramLists = FD->getParameterLists();
    bool hasSelf = FD->getDeclContext()->isTypeContext();
    if (FD->getDeclContext()->isGenericTypeContext())
      outerGenericParams = FD->getDeclContext()->getGenericParamsOfContext();

    for (unsigned i = 0, e = paramLists.size(); i != e; ++i) {
      Type argTy = paramLists[e - i - 1]->getType(TC.Context);
      if (!argTy) {
        FD->setType(ErrorType::get(TC.Context));
        FD->setInvalid();
        return;
      }

      // Determine the appropriate generic parameters at this level.
      GenericParamList *params = nullptr;
      if (e - i - 1 == hasSelf && genericParams) {
        params = genericParams;
      } else if (e - i - 1 == 0 && outerGenericParams) {
        params = outerGenericParams;
      }
      
      auto Info = TC.applyFunctionTypeAttributes(FD, i);
      
      if (params) {
        funcTy = PolymorphicFunctionType::get(argTy, funcTy, params, Info);
      } else {
        funcTy = FunctionType::get(argTy, funcTy, Info);
      }
    }
    FD->setType(funcTy);
    FD->setBodyResultType(bodyResultType);

    // For a non-generic method that returns dynamic Self, we need to
    // provide an interface type where the 'self' argument is the
    // nominal type.
    if (FD->hasDynamicSelf() && !genericParams && !outerGenericParams) {
      auto fnType = FD->getType()->castTo<FunctionType>();
      auto inputType = fnType->getInput().transform([&](Type type) -> Type {
        if (type->is<DynamicSelfType>())
          return FD->getExtensionType();
        return type;
      });
      FD->setInterfaceType(FunctionType::get(inputType, fnType->getResult(),
                                             fnType->getExtInfo()));
    }
  }

  /// Bind the given function declaration, which declares an operator, to
  /// the corresponding operator declaration.
  void bindFuncDeclToOperator(FuncDecl *FD) {
    OperatorDecl *op = nullptr;
    auto operatorName = FD->getFullName().getBaseName();
    SourceFile &SF = *FD->getDeclContext()->getParentSourceFile();
    if (FD->isUnaryOperator()) {
      if (FD->getAttrs().hasAttribute<PrefixAttr>()) {
        op = SF.lookupPrefixOperator(operatorName,
                                     FD->isCascadingContextForLookup(false),
                                     FD->getLoc());
      } else if (FD->getAttrs().hasAttribute<PostfixAttr>()) {
        op = SF.lookupPostfixOperator(operatorName,
                                      FD->isCascadingContextForLookup(false),
                                      FD->getLoc());
      } else {
        auto prefixOp =
            SF.lookupPrefixOperator(operatorName,
                                    FD->isCascadingContextForLookup(false),
                                    FD->getLoc());
        auto postfixOp =
            SF.lookupPostfixOperator(operatorName,
                                     FD->isCascadingContextForLookup(false),
                                     FD->getLoc());

        // If we found both prefix and postfix, or neither prefix nor postfix,
        // complain. We can't fix this situation.
        if (static_cast<bool>(prefixOp) == static_cast<bool>(postfixOp)) {
          TC.diagnose(FD, diag::declared_unary_op_without_attribute);

          // If we found both, point at them.
          if (prefixOp) {
            TC.diagnose(prefixOp, diag::unary_operator_declaration_here,false)
              .fixItInsert(FD->getLoc(), "prefix ");
            TC.diagnose(postfixOp, diag::unary_operator_declaration_here, true)
              .fixItInsert(FD->getLoc(), "postfix ");
          } else {
            // FIXME: Introduce a Fix-It that adds the operator declaration?
          }

          // FIXME: Errors could cascade here, because name lookup for this
          // operator won't find this declaration.
          return;
        }

        // We found only one operator declaration, so we know whether this
        // should be a prefix or a postfix operator.

        // Fix the AST and determine the insertion text.
        const char *insertionText;
        auto &C = FD->getASTContext();
        if (postfixOp) {
          insertionText = "postfix ";
          op = postfixOp;
          FD->getAttrs().add(new (C) PostfixAttr(/*implicit*/false));
        } else {
          insertionText = "prefix ";
          op = prefixOp;
          FD->getAttrs().add(new (C) PrefixAttr(/*implicit*/false));
        }

        // Emit diagnostic with the Fix-It.
        TC.diagnose(FD->getFuncLoc(), diag::unary_op_missing_prepos_attribute,
                    static_cast<bool>(postfixOp))
          .fixItInsert(FD->getFuncLoc(), insertionText);
        TC.diagnose(op, diag::unary_operator_declaration_here,
                    static_cast<bool>(postfixOp));
      }
    } else if (FD->isBinaryOperator()) {
      op = SF.lookupInfixOperator(operatorName,
                                  FD->isCascadingContextForLookup(false),
                                  FD->getLoc());
    } else {
      TC.diagnose(FD, diag::invalid_arg_count_for_operator);
      return;
    }

    if (!op) {
      // FIXME: Add Fix-It introducing an operator declaration?
      TC.diagnose(FD, diag::declared_operator_without_operator_decl);
      return;
    }

    FD->setOperatorDecl(op);
  }

  /// Determine whether the given declaration requires a definition.
  ///
  /// Only valid for declarations that can have definitions, i.e.,
  /// functions, initializers, etc.
  static bool requiresDefinition(Decl *decl) {
    // Invalid, implicit, and Clang-imported declarations never
    // require a definition.
    if (decl->isInvalid() || decl->isImplicit() || decl->hasClangNode())
      return false;

    // Functions can have _silgen_name, semantics, and NSManaged attributes.
    if (auto func = dyn_cast<AbstractFunctionDecl>(decl)) {
      if (func->getAttrs().hasAttribute<SILGenNameAttr>() ||
          func->getAttrs().hasAttribute<SemanticsAttr>() ||
          func->getAttrs().hasAttribute<NSManagedAttr>())
        return false;
    }

    // Declarations in SIL don't require definitions.
    if (auto sourceFile = decl->getDeclContext()->getParentSourceFile()) {
      if (sourceFile->Kind == SourceFileKind::SIL)
        return false;
    }

    // Everything else requires a definition.
    return true;
  }

  /// Check for methods that return 'DynamicResult'.
  bool checkDynamicSelfReturn(FuncDecl *func) {
    // Check whether we have a specified result type.
    auto typeRepr = func->getBodyResultTypeLoc().getTypeRepr();
    if (!typeRepr)
      return false;
      
    return checkDynamicSelfReturn(func, typeRepr, 0);
  }

  bool checkDynamicSelfReturn(FuncDecl *func, TypeRepr *typeRepr,
                              unsigned optionalDepth) {
    // Look through parentheses.
    if (auto parenRepr = dyn_cast<TupleTypeRepr>(typeRepr)) {
      if (!parenRepr->isParenType()) return false;
      return checkDynamicSelfReturn(func, parenRepr->getElement(0),
                                    optionalDepth);
    }

    // Look through attributes.
    if (auto attrRepr = dyn_cast<AttributedTypeRepr>(typeRepr)) {
      TypeAttributes attrs = attrRepr->getAttrs();
      if (!attrs.empty())
        return false;
      return checkDynamicSelfReturn(func, attrRepr->getTypeRepr(),
                                    optionalDepth);
    }

    // Look through optional types.
    if (auto attrRepr = dyn_cast<OptionalTypeRepr>(typeRepr)) {
      // But only one level.
      if (optionalDepth != 0) return false;
      return checkDynamicSelfReturn(func, attrRepr->getBase(),
                                    optionalDepth + 1);
    }

    // Check whether we have a simple identifier type.
    auto simpleRepr = dyn_cast<SimpleIdentTypeRepr>(typeRepr);
    if (!simpleRepr)
      return false;

    // Check whether it is 'Self'.
    if (simpleRepr->getIdentifier() != TC.Context.Id_Self)
      return false;

    // Dynamic 'Self' is only permitted on methods.
    auto dc = func->getDeclContext();
    if (!dc->isTypeContext()) {
      TC.diagnose(simpleRepr->getIdLoc(), diag::dynamic_self_non_method,
                  dc->isLocalContext());
      simpleRepr->setInvalid();
      return true;
    }

    // 'Self' in protocol extensions is not dynamic 'Self'.
    if (dc->isProtocolExtensionContext()) {
      return false;
    }

    // 'Self' is only a dynamic self on class methods.
    auto nominal = dc->isNominalTypeOrNominalTypeExtensionContext();
    assert(nominal && "Non-nominal container for method type?");
    if (!isa<ClassDecl>(nominal) && !isa<ProtocolDecl>(nominal)) {
      int which;
      if (isa<StructDecl>(nominal))
        which = 0;
      else if (isa<EnumDecl>(nominal))
        which = 1;
      else
        llvm_unreachable("Unknown nominal type");
      TC.diagnose(simpleRepr->getIdLoc(), diag::dynamic_self_struct_enum,
                  which, nominal->getName())
        .fixItReplace(simpleRepr->getIdLoc(), nominal->getName().str());
      simpleRepr->setInvalid();
      return true;
    }

    // Note that the function has a dynamic Self return type and set
    // the return type component to the dynamic self type.
    func->setDynamicSelf(true);
    return false;
  }

  /// Determine whether this is an unparenthesized closure type.
  static AnyFunctionType *isUnparenthesizedTrailingClosure(Type type) {
    if (isa<ParenType>(type.getPointer()))
      return nullptr;

    // Only consider the rvalue type.
    type = type->getRValueType();

    // Look through one level of optionality.
    if (auto objectType = type->getAnyOptionalObjectType())
      type = objectType;

    // Is it a function type?
    return type->getAs<AnyFunctionType>();
  }

  void visitFuncDecl(FuncDecl *FD) {
    if (!IsFirstPass) {
      if (FD->hasBody()) {
        // Record the body.
        TC.definedFunctions.push_back(FD);
      } else if (requiresDefinition(FD)) {
        // Complain if we should have a body.
        TC.diagnose(FD->getLoc(), diag::func_decl_without_brace);
      }
    }

    if (IsSecondPass) {
      checkAccessibility(TC, FD);
      TC.checkOmitNeedlessWords(FD);
      return;
    }

    TC.checkDeclAttributesEarly(FD);
    TC.computeAccessibility(FD);

    if (FD->hasType())
      return;

    // Bind operator functions to the corresponding operator declaration.
    if (FD->isOperator())
      bindFuncDeclToOperator(FD);

    // Validate 'static'/'class' on functions in extensions.
    auto StaticSpelling = FD->getStaticSpelling();
    if (StaticSpelling != StaticSpellingKind::None &&
        FD->getDeclContext()->isExtensionContext()) {
      if (Type T = FD->getDeclContext()->getDeclaredTypeInContext()) {
        if (auto NTD = T->getAnyNominal()) {
          if (!isa<ClassDecl>(NTD)) {
            if (StaticSpelling == StaticSpellingKind::KeywordClass) {
              TC.diagnose(FD, diag::class_func_not_in_class)
                  .fixItReplace(FD->getStaticLoc(), "static");
              TC.diagnose(NTD, diag::extended_type_declared_here);
            }
          }
        }
      }
    }

    // Validate the mutating attribute if present, and install it into the bit
    // on funcdecl (instead of just being a DeclAttribute).
    if (FD->getAttrs().hasAttribute<MutatingAttr>())
      FD->setMutating(true);
    else if (FD->getAttrs().hasAttribute<NonMutatingAttr>())
      FD->setMutating(false);

    // Check whether the return type is dynamic 'Self'.
    if (checkDynamicSelfReturn(FD))
      FD->setInvalid();

    // Observing accessors (and their generated regular accessors) may have
    // the type of the var inferred.
    if (AbstractStorageDecl *ASD = FD->getAccessorStorageDecl()) {
      if (ASD->hasObservers()) {
        TC.validateDecl(ASD);
        Type valueTy = ASD->getType()->getReferenceStorageReferent();
        if (FD->isObservingAccessor() || (FD->isSetter() && FD->isImplicit())) {
          unsigned firstParamIdx = FD->getParent()->isTypeContext();
          auto *firstParamPattern = FD->getParameterList(firstParamIdx);
          firstParamPattern->get(0)->getTypeLoc().setType(valueTy, true);
        } else if (FD->isGetter() && FD->isImplicit()) {
          FD->getBodyResultTypeLoc().setType(valueTy, true);
        }
      }
    }

    // Before anything else, set up the 'self' argument correctly if present.
    if (FD->getDeclContext()->isTypeContext())
      configureImplicitSelf(TC, FD);

    // If we have generic parameters, check the generic signature now.
    if (auto gp = FD->getGenericParams()) {
      gp->setOuterParameters(FD->getDeclContext()->getGenericParamsOfContext());

      if (TC.validateGenericFuncSignature(FD)) {
        markInvalidGenericSignature(FD, TC);
      } else {
        // Create a fresh archetype builder.
        ArchetypeBuilder builder =
          TC.createArchetypeBuilder(FD->getModuleContext());
        auto *parentSig = FD->getDeclContext()->getGenericSignatureOfContext();
        TC.checkGenericParamList(&builder, gp, parentSig);

        // Infer requirements from parameter patterns.
        for (auto pattern : FD->getParameterLists()) {
          builder.inferRequirements(pattern, gp);
        }

        // Infer requirements from the result type.
        if (!FD->getBodyResultTypeLoc().isNull()) {
          builder.inferRequirements(FD->getBodyResultTypeLoc(), gp);
        }

        // Revert the types within the signature so it can be type-checked with
        // archetypes below.
        TC.revertGenericFuncSignature(FD);

        // Assign archetypes.
        finalizeGenericParamList(builder, gp, FD, TC);
      }
    } else if (FD->getDeclContext()->isGenericTypeContext()) {
      if (TC.validateGenericFuncSignature(FD)) {
        markInvalidGenericSignature(FD, TC);
      } else if (!FD->hasType()) {
        // Revert all of the types within the signature of the function.
        TC.revertGenericFuncSignature(FD);
      } else {
        // Recursively satisfied.
        // FIXME: This is an awful hack.
        return;
      }
    }

    // Type check the parameters and return type again, now with archetypes.
    GenericTypeToArchetypeResolver resolver;
    semaFuncDecl(FD, &resolver);

    if (FD->isInvalid())
      return;

    // This type check should have created a non-dependent type.
    assert(!FD->getType()->hasTypeParameter());

    validateAttributes(TC, FD);

    // Member functions need some special validation logic.
    if (FD->getDeclContext()->isTypeContext()) {
      if (!checkOverrides(TC, FD)) {
        // If a method has an 'override' keyword but does not
        // override anything, complain.
        if (auto *OA = FD->getAttrs().getAttribute<OverrideAttr>()) {
          if (!FD->getOverriddenDecl()) {
            TC.diagnose(FD, diag::method_does_not_override)
              .highlight(OA->getLocation());
            OA->setInvalid();
          }
        }
      }

      Optional<ObjCReason> isObjC = shouldMarkAsObjC(TC, FD);

      ProtocolDecl *protocolContext = dyn_cast<ProtocolDecl>(
          FD->getDeclContext());
      if (protocolContext && FD->isAccessor()) {
        // Don't complain about accessors in protocols.  We will emit a
        // diagnostic about the property itself.
        if (isObjC)
          isObjC = ObjCReason::DoNotDiagnose;
      }

      if (FD->isGetterOrSetter()) {
        // If the property decl is an instance property, its accessors will
        // be instance methods and the above condition will mark them ObjC.
        // The only additional condition we need to check is if the var decl
        // had an @objc or @iboutlet property.

        ValueDecl *prop = cast<ValueDecl>(FD->getAccessorStorageDecl());
        // Validate the subscript or property because it might not be type
        // checked yet.
        if (isa<SubscriptDecl>(prop))
          TC.validateDecl(prop);
        else if (isa<VarDecl>(prop))
          TC.validateDecl(prop);

        if (prop->getAttrs().hasAttribute<NonObjCAttr>())
          isObjC = None;
        else if (!isObjC && prop->isObjC())
          isObjC = ObjCReason::DoNotDiagnose;

        // If the property is dynamic, propagate to this accessor.
        if (isObjC && prop->isDynamic() && !FD->isDynamic())
          FD->getAttrs().add(new (TC.Context) DynamicAttr(/*implicit*/ true));
      }

      Optional<ForeignErrorConvention> errorConvention;
      if (isObjC &&
          (FD->isInvalid() || !TC.isRepresentableInObjC(FD, *isObjC,
                                                        errorConvention)))
        isObjC = None;
      markAsObjC(TC, FD, isObjC, errorConvention);
    }
    
    inferDynamic(TC.Context, FD);

    TC.checkDeclAttributes(FD);

    // If this is a class member, mark it final if the class is final.
    if (auto contextType = FD->getDeclContext()->getDeclaredTypeInContext()) {
      if (auto cls = contextType->getClassOrBoundGenericClass()) {
        if (cls->isFinal() && !FD->isAccessor() &&
            !FD->isFinal() && !FD->isDynamic()) {
          makeFinal(TC.Context, FD);
        }
        // static func declarations in classes are synonyms
        // for `class final func` declarations.
        if (FD->getStaticSpelling() == StaticSpellingKind::KeywordStatic) {
          auto finalAttr = FD->getAttrs().getAttribute<FinalAttr>();
          if (finalAttr) {
            auto finalRange = finalAttr->getRange();
            if (finalRange.isValid())
              TC.diagnose(finalRange.Start, diag::decl_already_final)
              .highlight(finalRange)
              .fixItRemove(finalRange);
          }
          makeFinal(TC.Context, FD);
        }
      }
    }

    // Check whether we have parameters with default arguments that follow a
    // closure parameter; warn about such things, because the closure will not
    // be treated as a trailing closure.
    if (!FD->isImplicit()) {
      auto paramList = FD->getParameterList(FD->getImplicitSelfDecl() ? 1 : 0);
      bool anyDefaultArguments = false;
      for (unsigned i = paramList->size(); i != 0; --i) {
        // Determine whether the parameter is of (possibly lvalue, possibly
        // optional), non-autoclosure function type, which could receive a
        // closure. We look at the type sugar directly, so that one can
        // suppress this warning by adding parentheses.
        auto &param = paramList->get(i-1);
        auto paramType = param->getType();

        if (auto *funcTy = isUnparenthesizedTrailingClosure(paramType)) {
          // If we saw any default arguments before this, complain.
          // This doesn't apply to autoclosures.
          if (anyDefaultArguments && !funcTy->getExtInfo().isAutoClosure()) {
            TC.diagnose(param->getStartLoc(),
                        diag::non_trailing_closure_before_default_args)
              .highlight(param->getSourceRange());
          }

          break;
        }

        // If we have a default argument, keep going.
        if (param->isDefaultArgument()) {
          anyDefaultArguments = true;
          continue;
        }

        // We're done.
        break;
      }
    }
  }

  void visitModuleDecl(ModuleDecl *) { }

  /// Adjust the type of the given declaration to appear as if it were
  /// in the given subclass of its actual declared class.
  static Type adjustSuperclassMemberDeclType(TypeChecker &TC,
                                             const ValueDecl *decl,
                                             Type subclass) {
    ClassDecl *superclassDecl =
      decl->getDeclContext()->getDeclaredTypeInContext()
        ->getClassOrBoundGenericClass();
    auto superclass = subclass;
    while (superclass->getClassOrBoundGenericClass() != superclassDecl)
      superclass = TC.getSuperClassOf(superclass);
    auto type = TC.substMemberTypeWithBase(decl->getModuleContext(), decl,
                                           superclass,
                                           /*isTypeReference=*/false);
    if (auto func = dyn_cast<FuncDecl>(decl)) {
      if (func->hasDynamicSelf()) {
        type = type->replaceCovariantResultType(subclass,
                                                func->getNaturalArgumentCount());
      }
    } else if (isa<ConstructorDecl>(decl)) {
      type = type->replaceCovariantResultType(subclass, /*uncurryLevel=*/2);
    }

    return type;
  }

  /// Perform basic checking to determine whether a declaration can override a
  /// declaration in a superclass.
  static bool areOverrideCompatibleSimple(ValueDecl *decl,
                                          ValueDecl *parentDecl) {
    // If the number of argument labels does not match, these overrides cannot
    // be compatible.
    if (decl->getFullName().getArgumentNames().size() !=
          parentDecl->getFullName().getArgumentNames().size())
      return false;

    if (auto func = dyn_cast<FuncDecl>(decl)) {
      // Specific checking for methods.
      auto parentFunc = cast<FuncDecl>(parentDecl);
      if (func->isStatic() != parentFunc->isStatic())
        return false;
    } else if (auto var = dyn_cast<VarDecl>(decl)) {
      auto parentVar = cast<VarDecl>(parentDecl);
      if (var->isStatic() != parentVar->isStatic())
        return false;
    }

    return true;
  }

  /// Drop the optionality of the result type of the given function type.
  static Type dropResultOptionality(Type type, unsigned uncurryLevel) {
    // We've hit the result type.
    if (uncurryLevel == 0) {
      if (auto objectTy = type->getAnyOptionalObjectType())
        return objectTy;

      return type;
    }

    // Determine the input and result types of this function.
    auto fnType = type->castTo<AnyFunctionType>();
    Type inputType = fnType->getInput();
    Type resultType = dropResultOptionality(fnType->getResult(), 
                                            uncurryLevel - 1);
    
    // Produce the resulting function type.
    if (auto genericFn = dyn_cast<GenericFunctionType>(fnType)) {
      return GenericFunctionType::get(genericFn->getGenericSignature(),
                                      inputType, resultType,
                                      fnType->getExtInfo());
    }
    
    assert(!isa<PolymorphicFunctionType>(fnType));  
    return FunctionType::get(inputType, resultType, fnType->getExtInfo());    
  }

  /// Diagnose overrides of '(T) -> T?' with '(T!) -> T!'.
  static void diagnoseUnnecessaryIUOs(TypeChecker &TC,
                                      const AbstractFunctionDecl *method,
                                      const AbstractFunctionDecl *parentMethod,
                                      Type owningTy) {
    Type plainParentTy = adjustSuperclassMemberDeclType(TC, parentMethod,
                                                        owningTy);
    const auto *parentTy = plainParentTy->castTo<AnyFunctionType>();
    parentTy = parentTy->getResult()->castTo<AnyFunctionType>();

    // Check the parameter types.
    auto checkParam = [&](const ParamDecl *decl, Type parentParamTy) {
      Type paramTy = decl->getType();
      if (!paramTy || !paramTy->getImplicitlyUnwrappedOptionalObjectType())
        return;
      if (!parentParamTy || parentParamTy->getAnyOptionalObjectType())
        return;

      TypeLoc TL = decl->getTypeLoc();
      if (!TL.getTypeRepr())
        return;

      // Allow silencing this warning using parens.
      if (isa<ParenType>(TL.getType().getPointer()))
        return;

      TC.diagnose(decl->getStartLoc(), diag::override_unnecessary_IUO,
                  method->getDescriptiveKind(), parentParamTy, paramTy)
        .highlight(TL.getSourceRange());

      auto sugaredForm =
        dyn_cast<ImplicitlyUnwrappedOptionalTypeRepr>(TL.getTypeRepr());
      if (sugaredForm) {
        TC.diagnose(sugaredForm->getExclamationLoc(),
                    diag::override_unnecessary_IUO_remove)
          .fixItRemove(sugaredForm->getExclamationLoc());
      }

      TC.diagnose(TL.getSourceRange().Start,
                  diag::override_unnecessary_IUO_silence)
        .fixItInsert(TL.getSourceRange().Start, "(")
        .fixItInsertAfter(TL.getSourceRange().End, ")");
    };

    auto paramList = method->getParameterList(1);
    auto parentInput = parentTy->getInput();
    
    if (auto parentTupleInput = parentInput->getAs<TupleType>()) {
      // FIXME: If we ever allow argument reordering, this is incorrect.
      ArrayRef<ParamDecl*> sharedParams = paramList->getArray();
      sharedParams = sharedParams.slice(0, parentTupleInput->getNumElements());
      for_each(sharedParams, parentTupleInput->getElementTypes(), checkParam);
    } else {
      // Otherwise, the parent has a single parameter with no label.
      checkParam(paramList->get(0), parentInput);
    }

    auto methodAsFunc = dyn_cast<FuncDecl>(method);
    if (!methodAsFunc)
      return;

    // FIXME: This is very nearly the same code as checkParam.
    auto checkResult = [&](TypeLoc resultTL, Type parentResultTy) {
      Type resultTy = resultTL.getType();
      if (!resultTy || !resultTy->getImplicitlyUnwrappedOptionalObjectType())
        return;
      if (!parentResultTy || !parentResultTy->getOptionalObjectType())
        return;

      // Allow silencing this warning using parens.
      if (isa<ParenType>(resultTy.getPointer()))
        return;

      TC.diagnose(resultTL.getSourceRange().Start,
                  diag::override_unnecessary_result_IUO,
                  method->getDescriptiveKind(), parentResultTy, resultTy)
        .highlight(resultTL.getSourceRange());

      auto sugaredForm =
        dyn_cast<ImplicitlyUnwrappedOptionalTypeRepr>(resultTL.getTypeRepr());
      if (sugaredForm) {
        TC.diagnose(sugaredForm->getExclamationLoc(),
                    diag::override_unnecessary_IUO_use_strict)
          .fixItReplace(sugaredForm->getExclamationLoc(), "?");
      }

      TC.diagnose(resultTL.getSourceRange().Start,
                  diag::override_unnecessary_IUO_silence)
        .fixItInsert(resultTL.getSourceRange().Start, "(")
        .fixItInsertAfter(resultTL.getSourceRange().End, ")");
    };

    checkResult(methodAsFunc->getBodyResultTypeLoc(), parentTy->getResult());
  }

  /// Make sure that there is an invalid 'override' attribute on the
  /// given declaration.
  static void makeInvalidOverrideAttr(TypeChecker &TC, ValueDecl *decl) {
    if (auto overrideAttr = decl->getAttrs().getAttribute<OverrideAttr>()) {
      overrideAttr->setInvalid();
    } else {
      auto attr = new (TC.Context) OverrideAttr(true);
      decl->getAttrs().add(attr);
      attr->setInvalid();
    }

    if (auto storage = dyn_cast<AbstractStorageDecl>(decl)) {
      if (auto getter = storage->getGetter())
        makeInvalidOverrideAttr(TC, getter);
      if (auto setter = storage->getSetter())
        makeInvalidOverrideAttr(TC, setter);
    }
  }

  static void adjustFunctionTypeForOverride(Type &type) {
    // Drop 'noreturn' and 'throws'.
    auto fnType = type->castTo<AnyFunctionType>();
    auto extInfo = fnType->getExtInfo();
    extInfo = extInfo.withThrows(false).withIsNoReturn(false);
    if (fnType->getExtInfo() != extInfo)
      type = fnType->withExtInfo(extInfo);
  }

  /// Determine which method or subscript this method or subscript overrides
  /// (if any).
  ///
  /// \returns true if an error occurred.
  static bool checkOverrides(TypeChecker &TC, ValueDecl *decl) {
    if (decl->isInvalid() || decl->getOverriddenDecl())
      return false;

    auto owningTy = decl->getDeclContext()->getDeclaredInterfaceType();
    if (!owningTy)
      return false;

    auto classDecl = owningTy->getClassOrBoundGenericClass();
    if (!classDecl)
      return false;

    Type superclass = classDecl->getSuperclass();
    if (!superclass)
      return false;

    // Ignore accessor methods (e.g. getters and setters), they will be handled
    // when their storage decl is processed.
    if (auto *fd = dyn_cast<FuncDecl>(decl))
      if (fd->isAccessor())
        return false;
    
    auto method = dyn_cast<AbstractFunctionDecl>(decl);
    ConstructorDecl *ctor = nullptr;
    if (method)
      ctor = dyn_cast<ConstructorDecl>(method);

    auto abstractStorage = dyn_cast<AbstractStorageDecl>(decl);
    assert((method || abstractStorage) && "Not a method or abstractStorage?");
    SubscriptDecl *subscript = nullptr;
    if (abstractStorage)
      subscript = dyn_cast<SubscriptDecl>(abstractStorage);

    // Figure out the type of the declaration that we're using for comparisons.
    auto declTy = decl->getInterfaceType()->getUnlabeledType(TC.Context);
    if (method) {
      declTy = declTy->castTo<AnyFunctionType>()->getResult();
      adjustFunctionTypeForOverride(declTy);
    } else {
      declTy = declTy->getReferenceStorageReferent();
    }

    // Ignore the optionality of initializers when comparing types;
    // we'll enforce this separately
    if (ctor) {
      declTy = dropResultOptionality(declTy, 1);
    }

    // Look for members with the same name and matching types as this
    // one.
    auto superclassMetaTy = MetatypeType::get(superclass);
    bool retried = false;
    DeclName name = decl->getFullName();

  retry:
    NameLookupOptions lookupOptions
      = defaultMemberLookupOptions - NameLookupFlags::DynamicLookup;
    LookupResult members = TC.lookupMember(decl->getDeclContext(),
                                           superclassMetaTy, name,
                                           lookupOptions);

    typedef std::tuple<ValueDecl *, bool, Type> MatchType;
    SmallVector<MatchType, 2> matches;
    bool hadExactMatch = false;

    for (auto memberResult : members) {
      auto member = memberResult.Decl;

      if (member->isInvalid())
        continue;

      if (member->getKind() != decl->getKind())
        continue;

      if (!member->getDeclContext()->isClassOrClassExtensionContext())
        continue;

      auto parentDecl = cast<ValueDecl>(member);

      // Check whether there are any obvious reasons why the two given
      // declarations do not have an overriding relationship.
      if (!areOverrideCompatibleSimple(decl, parentDecl))
        continue;

      auto parentMethod = dyn_cast<AbstractFunctionDecl>(parentDecl);
      auto parentStorage = dyn_cast<AbstractStorageDecl>(parentDecl);
      assert(parentMethod || parentStorage);

      // If both are Objective-C, then match based on selectors or
      // subscript kind and check the types separately.
      bool objCMatch = false;
      if (parentDecl->isObjC() && decl->isObjC()) {
        if (method) {
          if (method->getObjCSelector(&TC)
                == parentMethod->getObjCSelector(&TC))
            objCMatch = true;
        } else if (auto *parentSubscript =
                     dyn_cast<SubscriptDecl>(parentStorage)) {
          // If the subscript kinds don't match, it's not an override.
          if (subscript->getObjCSubscriptKind(&TC)
                == parentSubscript->getObjCSubscriptKind(&TC))
            objCMatch = true;
        }

        // Properties don't need anything here since they are always
        // checked by name.
      }

      // Check whether the types are identical.
      // FIXME: It's wrong to use the uncurried types here for methods.
      auto parentDeclTy = adjustSuperclassMemberDeclType(TC, parentDecl,
                                                         owningTy);
      parentDeclTy = parentDeclTy->getUnlabeledType(TC.Context);
      if (method) {
        parentDeclTy = parentDeclTy->castTo<AnyFunctionType>()->getResult();
        adjustFunctionTypeForOverride(parentDeclTy);
      } else {
        parentDeclTy = parentDeclTy->getReferenceStorageReferent();
      }

      // Ignore the optionality of initializers when comparing types;
      // we'll enforce this separately
      if (ctor) {
        parentDeclTy = dropResultOptionality(parentDeclTy, 1);

        // Factory methods cannot be overridden.
        auto parentCtor = cast<ConstructorDecl>(parentDecl);
        if (parentCtor->isFactoryInit())
          continue;
      }

      if (declTy->isEqual(parentDeclTy)) {
        matches.push_back(std::make_tuple(parentDecl, true, parentDeclTy));
        hadExactMatch = true;
        continue;
      }
      
      // If this is a property, we accept the match and then reject it below if
      // the types don't line up, since you can't overload properties based on
      // types.
      if (isa<VarDecl>(parentDecl)) {
        matches.push_back(std::make_tuple(parentDecl, false, parentDeclTy));
        continue;
      }

      // Failing that, check for subtyping.
      if (declTy->canOverride(parentDeclTy, parentDecl->isObjC(), &TC)) {
        // If the Objective-C selectors match, always call it exact.
        matches.push_back(
            std::make_tuple(parentDecl, objCMatch, parentDeclTy));
        hadExactMatch |= objCMatch;
        continue;
      }

      // Not a match. If we had an Objective-C match, this is a serious problem.
      if (objCMatch) {
        if (method) {
          TC.diagnose(decl, diag::override_objc_type_mismatch_method,
                      method->getObjCSelector(&TC), declTy);
        } else {
          TC.diagnose(decl, diag::override_objc_type_mismatch_subscript,
                      static_cast<unsigned>(
                        subscript->getObjCSubscriptKind(&TC)),
                      declTy);
        }
        TC.diagnose(parentDecl, diag::overridden_here_with_type,
                    parentDeclTy);
        
        // Put an invalid 'override' attribute here.
        makeInvalidOverrideAttr(TC, decl);

        return true;
      }
    }

    // If we have no matches.
    if (matches.empty()) {
      // If we already re-tried, or if the user didn't indicate that this is
      // an override, or we don't know what else to look for, try again.
      if (retried || name.isSimpleName() ||
          name.getArgumentNames().size() == 0 ||
          !decl->getAttrs().hasAttribute<OverrideAttr>())
        return false;

      // Try looking again, this time using just the base name, so that we'll
      // catch mismatched names.
      retried = true;
      name = name.getBaseName();
      goto retry;
    }

    // If we had an exact match, throw away any non-exact matches.
    if (hadExactMatch)
      matches.erase(std::remove_if(matches.begin(), matches.end(),
                                   [&](MatchType &match) {
                                     return !std::get<1>(match);
                                   }), matches.end());

    // If we have a single match (exact or not), take it.
    if (matches.size() == 1) {
      auto matchDecl = std::get<0>(matches[0]);
      auto matchType = std::get<2>(matches[0]);

      // If the name of our match differs from the name we were looking for,
      // complain.
      if (decl->getFullName() != matchDecl->getFullName()) {
        auto diag = TC.diagnose(decl, diag::override_argument_name_mismatch,
                                isa<ConstructorDecl>(decl),
                                decl->getFullName(),
                                matchDecl->getFullName());
        TC.fixAbstractFunctionNames(diag, cast<AbstractFunctionDecl>(decl),
                                    matchDecl->getFullName());
      }

      // If we have an explicit ownership modifier and our parent doesn't,
      // complain.
      auto parentAttr = matchDecl->getAttrs().getAttribute<OwnershipAttr>();
      if (auto ownershipAttr = decl->getAttrs().getAttribute<OwnershipAttr>()) {
        Ownership parentOwnership;
        if (parentAttr)
          parentOwnership = parentAttr->get();
        else
          parentOwnership = Ownership::Strong;
        if (parentOwnership != ownershipAttr->get()) {
          TC.diagnose(decl, diag::override_ownership_mismatch,
                      (unsigned)parentOwnership,
                      (unsigned)ownershipAttr->get());
          TC.diagnose(matchDecl, diag::overridden_here);
        }
      }

      // Check accessibility.
      // FIXME: Copied from TypeCheckProtocol.cpp.
      Accessibility requiredAccess =
        std::min(classDecl->getFormalAccess(), matchDecl->getFormalAccess());
      bool shouldDiagnose = false;
      bool shouldDiagnoseSetter = false;
      if (requiredAccess > Accessibility::Private &&
          !isa<ConstructorDecl>(decl)) {
        shouldDiagnose = (decl->getFormalAccess() < requiredAccess);

        if (!shouldDiagnose && matchDecl->isSettable(classDecl)) {
          auto matchASD = cast<AbstractStorageDecl>(matchDecl);
          if (matchASD->isSetterAccessibleFrom(classDecl)) {
            auto ASD = cast<AbstractStorageDecl>(decl);
            const DeclContext *accessDC = nullptr;
            if (requiredAccess == Accessibility::Internal)
              accessDC = classDecl->getParentModule();
            shouldDiagnoseSetter = ASD->isSettable(accessDC) &&
                                   !ASD->isSetterAccessibleFrom(accessDC);
          }
        }
      }
      if (shouldDiagnose || shouldDiagnoseSetter) {
        bool overriddenForcesAccess =
          (requiredAccess == matchDecl->getFormalAccess());
        {
          auto diag = TC.diagnose(decl, diag::override_not_accessible,
                                  shouldDiagnoseSetter,
                                  decl->getDescriptiveKind(),
                                  overriddenForcesAccess);
          fixItAccessibility(diag, decl, requiredAccess, shouldDiagnoseSetter);
        }
        TC.diagnose(matchDecl, diag::overridden_here);
      }

      // If this is an exact type match, we're successful!
      if (declTy->isEqual(matchType)) {
        // Nothing to do.
        
      } else if (method) {
        // Private migration help for overrides of Objective-C methods.
        if ((!isa<FuncDecl>(method) || !cast<FuncDecl>(method)->isAccessor()) &&
            superclass->getClassOrBoundGenericClass()->isObjC()) {
          diagnoseUnnecessaryIUOs(TC, method,
                                  cast<AbstractFunctionDecl>(matchDecl),
                                  owningTy);
        }

      } else if (auto subscript =
                   dyn_cast_or_null<SubscriptDecl>(abstractStorage)) {
        // Otherwise, if this is a subscript, validate that covariance is ok.
        // If the parent is non-mutable, it's okay to be covariant.
        auto parentSubscript = cast<SubscriptDecl>(matchDecl);
        if (parentSubscript->getSetter()) {
          TC.diagnose(subscript, diag::override_mutable_covariant_subscript,
                      declTy, matchType);
          TC.diagnose(matchDecl, diag::subscript_override_here);
          return true;
        }
      } else if (auto property = dyn_cast_or_null<VarDecl>(abstractStorage)) {
        auto propertyTy = property->getInterfaceType();
        auto parentPropertyTy = adjustSuperclassMemberDeclType(TC, matchDecl,
                                                               superclass);
        
        if (!propertyTy->canOverride(parentPropertyTy, false, &TC)) {
          TC.diagnose(property, diag::override_property_type_mismatch,
                      property->getName(), propertyTy, parentPropertyTy);
          TC.diagnose(matchDecl, diag::property_override_here);
          return true;
        }
        
        // Differing only in Optional vs. ImplicitlyUnwrappedOptional is fine.
        bool IsSilentDifference = false;
        if (auto propertyTyNoOptional = propertyTy->getAnyOptionalObjectType())
          if (auto parentPropertyTyNoOptional =
              parentPropertyTy->getAnyOptionalObjectType())
            if (propertyTyNoOptional->isEqual(parentPropertyTyNoOptional))
              IsSilentDifference = true;
        
        // The overridden property must not be mutable.
        if (cast<AbstractStorageDecl>(matchDecl)->getSetter() &&
            !IsSilentDifference) {
          TC.diagnose(property, diag::override_mutable_covariant_property,
                      property->getName(), parentPropertyTy, propertyTy);
          TC.diagnose(matchDecl, diag::property_override_here);
          return true;
        }
      }

      return recordOverride(TC, decl, matchDecl);
    }

    // We override more than one declaration. Complain.
    TC.diagnose(decl,
                retried ? diag::override_multiple_decls_arg_mismatch
                        : diag::override_multiple_decls_base,
                decl->getFullName());
    for (auto match : matches) {
      auto matchDecl = std::get<0>(match);
      if (retried) {
        auto diag = TC.diagnose(matchDecl, diag::overridden_near_match_here,
                                isa<ConstructorDecl>(matchDecl),
                                matchDecl->getFullName());
        TC.fixAbstractFunctionNames(diag, cast<AbstractFunctionDecl>(decl),
                                    matchDecl->getFullName());
        continue;
      }

      TC.diagnose(std::get<0>(match), diag::overridden_here);
    }
    return true;
  }

  /// Attribute visitor that checks how the given attribute should be
  /// considered when overriding a declaration.
  ///
  /// Note that the attributes visited are those of the base
  /// declaration, so if you need to check that the overriding
  /// declaration doesn't have an attribute if the base doesn't have
  /// it, this isn't sufficient.
  class AttributeOverrideChecker
          : public AttributeVisitor<AttributeOverrideChecker> {
    TypeChecker &TC;
    ValueDecl *Base;
    ValueDecl *Override;

  public:
    AttributeOverrideChecker(TypeChecker &tc, ValueDecl *base,
                             ValueDecl *override)
      : TC(tc), Base(base), Override(override) { }

    /// Deleting this ensures that all attributes are covered by the visitor
    /// below.
    void visitDeclAttribute(DeclAttribute *A) = delete;

#define UNINTERESTING_ATTR(CLASS)                                              \
    void visit##CLASS##Attr(CLASS##Attr *) {}

    UNINTERESTING_ATTR(Accessibility)
    UNINTERESTING_ATTR(Alignment)
    UNINTERESTING_ATTR(SILGenName)
    UNINTERESTING_ATTR(Exported)
    UNINTERESTING_ATTR(IBAction)
    UNINTERESTING_ATTR(IBDesignable)
    UNINTERESTING_ATTR(IBInspectable)
    UNINTERESTING_ATTR(IBOutlet)
    UNINTERESTING_ATTR(Indirect)
    UNINTERESTING_ATTR(Inline)
    UNINTERESTING_ATTR(Effects)
    UNINTERESTING_ATTR(FixedLayout)
    UNINTERESTING_ATTR(Lazy)
    UNINTERESTING_ATTR(LLDBDebuggerFunction)
    UNINTERESTING_ATTR(Mutating)
    UNINTERESTING_ATTR(NonMutating)
    UNINTERESTING_ATTR(NonObjC)
    UNINTERESTING_ATTR(NSApplicationMain)
    UNINTERESTING_ATTR(NSCopying)
    UNINTERESTING_ATTR(NSManaged)
    UNINTERESTING_ATTR(ObjC)
    UNINTERESTING_ATTR(ObjCBridged)
    UNINTERESTING_ATTR(Optional)
    UNINTERESTING_ATTR(Override)
    UNINTERESTING_ATTR(RawDocComment)
    UNINTERESTING_ATTR(Required)
    UNINTERESTING_ATTR(Convenience)
    UNINTERESTING_ATTR(Semantics)
    UNINTERESTING_ATTR(SetterAccessibility)
    UNINTERESTING_ATTR(UIApplicationMain)
    UNINTERESTING_ATTR(ObjCNonLazyRealization)
    UNINTERESTING_ATTR(UnsafeNoObjCTaggedPointer)
    UNINTERESTING_ATTR(SwiftNativeObjCRuntimeBase)

    // These can't appear on overridable declarations.
    UNINTERESTING_ATTR(AutoClosure)
    UNINTERESTING_ATTR(NoEscape)

    UNINTERESTING_ATTR(Prefix)
    UNINTERESTING_ATTR(Postfix)
    UNINTERESTING_ATTR(Infix)
    UNINTERESTING_ATTR(Ownership)

    UNINTERESTING_ATTR(SynthesizedProtocol)
    UNINTERESTING_ATTR(RequiresStoredPropertyInits)
    UNINTERESTING_ATTR(Transparent)
    UNINTERESTING_ATTR(SILStored)
    UNINTERESTING_ATTR(Testable)

    UNINTERESTING_ATTR(WarnUnusedResult)
    UNINTERESTING_ATTR(WarnUnqualifiedAccess)

#undef UNINTERESTING_ATTR

    void visitAvailableAttr(AvailableAttr *attr) {
      // FIXME: Check that this declaration is at least as available as the
      // one it overrides.
    }

    void visitRethrowsAttr(RethrowsAttr *attr) {
      // 'rethrows' functions are a subtype of ordinary 'throws' functions.
      // Require 'rethrows' on the override if it was there on the base,
      // unless the override is completely non-throwing.
      if (!Override->getAttrs().hasAttribute<RethrowsAttr>() &&
          cast<AbstractFunctionDecl>(Override)->isBodyThrowing()) {
        TC.diagnose(Override, diag::override_rethrows_with_non_rethrows,
                    isa<ConstructorDecl>(Override));
        TC.diagnose(Base, diag::overridden_here);
      }
    }

    void visitFinalAttr(FinalAttr *attr) {
      // If this is an accessor, don't complain if we would have
      // complained about the storage declaration.
      if (auto func = dyn_cast<FuncDecl>(Override)) {
        if (auto storageDecl = func->getAccessorStorageDecl()) {
          if (storageDecl->getOverriddenDecl() &&
              storageDecl->getOverriddenDecl()->isFinal())
            return;
        }
      }

      // FIXME: Customize message to the kind of thing.
      TC.diagnose(Override, diag::override_final, 
                  Override->getDescriptiveKind());
      TC.diagnose(Base, diag::overridden_here);
    }

    void visitNoReturnAttr(NoReturnAttr *attr) {
      // Disallow overriding a @noreturn function with a returning one.
      if (Base->getAttrs().hasAttribute<NoReturnAttr>() &&
          !Override->getAttrs().hasAttribute<NoReturnAttr>()) {
        TC.diagnose(Override, diag::override_noreturn_with_return);
        TC.diagnose(Base, diag::overridden_here);
      }
    }

    void visitDynamicAttr(DynamicAttr *attr) {
      if (!Override->getAttrs().hasAttribute<DynamicAttr>())
        // Dynamic is inherited.
        Override->getAttrs().add(
                                new (TC.Context) DynamicAttr(/*implicit*/true));
    }

    void visitSwift3MigrationAttr(Swift3MigrationAttr *attr) {
      if (!Override->getAttrs().hasAttribute<Swift3MigrationAttr>()) {
        // Inherit swift3_migration attribute.
        Override->getAttrs().add(new (TC.Context) Swift3MigrationAttr(
                                                    SourceLoc(), SourceLoc(),
                                                    SourceLoc(),
                                                    attr->getRenamed(),
                                                    attr->getMessage(),
                                                    SourceLoc(),
                                                    /*implicit=*/true));
      }
    }
  };

  /// Determine whether overriding the given declaration requires a keyword.
  static bool overrideRequiresKeyword(ValueDecl *overridden) {
    if (auto ctor = dyn_cast<ConstructorDecl>(overridden)) {
      return ctor->isDesignatedInit() && !ctor->isRequired();
    }

    return true;
  }

  /// Returns true if a diagnostic about an accessor being less available
  /// than the accessor it overrides would be redundant because we will
  /// already emit another diagnostic.
  static bool
  isRedundantAccessorOverrideAvailabilityDiagnostic(TypeChecker &TC,
                                                    ValueDecl *override,
                                                    ValueDecl *base) {

    auto *overrideFn = dyn_cast<FuncDecl>(override);
    auto *baseFn = dyn_cast<FuncDecl>(base);
    if (!overrideFn || !baseFn)
      return false;

    AbstractStorageDecl *overrideASD = overrideFn->getAccessorStorageDecl();
    AbstractStorageDecl *baseASD = baseFn->getAccessorStorageDecl();
    if (!overrideASD || !baseASD)
      return false;

    if (overrideASD->getOverriddenDecl() != baseASD)
      return false;

    // If we have already emitted a diagnostic about an unsafe override
    // for the property, don't complain about the accessor.
    if (!TC.isAvailabilitySafeForOverride(overrideASD, baseASD)) {
      return true;
    }

    // Returns true if we will already diagnose a bad override
    // on the property's accessor of the given kind.
    auto accessorOverrideAlreadyDiagnosed = [&](AccessorKind kind) {
      FuncDecl *overrideAccessor = overrideASD->getAccessorFunction(kind);
      FuncDecl *baseAccessor = baseASD->getAccessorFunction(kind);
      if (overrideAccessor && baseAccessor &&
          !TC.isAvailabilitySafeForOverride(overrideAccessor, baseAccessor)) {
        return true;
      }
      return false;
    };

    // If we have already emitted a diagnostic about an unsafe override
    // for a getter or a setter, no need to complain about materializeForSet,
    // which is synthesized to be as available as both the getter and
    // the setter.
    if (overrideFn->getAccessorKind() == AccessorKind::IsMaterializeForSet) {
      if (accessorOverrideAlreadyDiagnosed(AccessorKind::IsGetter) ||
          accessorOverrideAlreadyDiagnosed(AccessorKind::IsSetter)) {
        return true;
      }
    }

    return false;
  }

  /// Diagnose an override for potential availability. Returns true if
  /// a diagnostic was emitted and false otherwise.
  static bool diagnoseOverrideForAvailability(TypeChecker &TC,
                                              ValueDecl *override,
                                              ValueDecl *base) {
    if (TC.isAvailabilitySafeForOverride(override, base))
      return false;

    // Suppress diagnostics about availability overrides for accessors
    // if they would be redundant with other diagnostics.
    if (isRedundantAccessorOverrideAvailabilityDiagnostic(TC, override, base))
      return false;

    if (auto *FD = dyn_cast<FuncDecl>(override)) {
      if (FD->isAccessor()) {
        TC.diagnose(override, diag::override_accessor_less_available,
                    FD->getDescriptiveKind(),
                    FD->getAccessorStorageDecl()->getName());
        TC.diagnose(base, diag::overridden_here);
        return true;
      }
    }

    TC.diagnose(override, diag::override_less_available, override->getName());
    TC.diagnose(base, diag::overridden_here);

    return true;
  }

  /// Record that the \c overriding declarations overrides the
  /// \c overridden declaration.
  ///
  /// \returns true if an error occurred.
  static bool recordOverride(TypeChecker &TC, ValueDecl *override,
                             ValueDecl *base, bool isKnownObjC = false) {
    // Check property and subscript overriding.
    if (auto *baseASD = dyn_cast<AbstractStorageDecl>(base)) {
      auto *overrideASD = cast<AbstractStorageDecl>(override);
      
      // Make sure that the overriding property doesn't have storage.
      if (overrideASD->hasStorage() && !overrideASD->hasObservers()) {
        TC.diagnose(overrideASD, diag::override_with_stored_property,
                    overrideASD->getName());
        TC.diagnose(baseASD, diag::property_override_here);
        return true;
      }

      // Make sure that an observing property isn't observing something
      // read-only.  Observing properties look at change, read-only properties
      // have nothing to observe!
      bool baseIsSettable = baseASD->isSettable(baseASD->getDeclContext());
      if (baseIsSettable && TC.Context.LangOpts.EnableAccessControl) {
        baseIsSettable =
           baseASD->isSetterAccessibleFrom(overrideASD->getDeclContext());
      }
      if (overrideASD->hasObservers() && !baseIsSettable) {
        TC.diagnose(overrideASD, diag::observing_readonly_property,
                    overrideASD->getName());
        TC.diagnose(baseASD, diag::property_override_here);
        return true;
      }

      // Make sure we're not overriding a settable property with a non-settable
      // one.  The only reasonable semantics for this would be to inherit the
      // setter but override the getter, and that would be surprising at best.
      if (baseIsSettable && !override->isSettable(override->getDeclContext())) {
        TC.diagnose(overrideASD, diag::override_mutable_with_readonly_property,
                    overrideASD->getName());
        TC.diagnose(baseASD, diag::property_override_here);
        return true;
      }
      
      
      // Make sure a 'let' property is only overridden by 'let' properties.  A
      // let property provides more guarantees than the getter of a 'var'
      // property.
      if (isa<VarDecl>(baseASD) && cast<VarDecl>(baseASD)->isLet()) {
        TC.diagnose(overrideASD, diag::override_let_property,
                    overrideASD->getName());
        TC.diagnose(baseASD, diag::property_override_here);
        return true;
      }
    }
    
    // Non-Objective-C declarations in extensions cannot override or
    // be overridden.
    if ((base->getDeclContext()->isExtensionContext() ||
         override->getDeclContext()->isExtensionContext()) &&
        !base->isObjC() && !isKnownObjC) {
      TC.diagnose(override, diag::override_decl_extension,
                  !override->getDeclContext()->isExtensionContext());
      TC.diagnose(base, diag::overridden_here);
      return true;
    }
    
    // If the overriding declaration does not have the 'override' modifier on
    // it, complain.
    if (!override->getAttrs().hasAttribute<OverrideAttr>() &&
        overrideRequiresKeyword(base)) {
      // FIXME: rdar://16320042 - For properties, we don't have a useful
      // location for the 'var' token.  Instead of emitting a bogus fixit, only
      // emit the fixit for 'func's.
      if (!isa<VarDecl>(override))
        TC.diagnose(override, diag::missing_override)
            .fixItInsert(override->getStartLoc(), "override ");
      else
        TC.diagnose(override, diag::missing_override);
      TC.diagnose(base, diag::overridden_here);
      override->getAttrs().add(
          new (TC.Context) OverrideAttr(SourceLoc()));
    }

    // If the overriding declaration is 'throws' but the base is not,
    // complain.
    if (auto overrideFn = dyn_cast<AbstractFunctionDecl>(override)) {
      if (overrideFn->isBodyThrowing() &&
          !cast<AbstractFunctionDecl>(base)->isBodyThrowing()) {
        TC.diagnose(override, diag::override_throws,
                    isa<ConstructorDecl>(override));
        TC.diagnose(base, diag::overridden_here);
      }

      if (!overrideFn->isBodyThrowing() && base->isObjC() &&
          cast<AbstractFunctionDecl>(base)->isBodyThrowing()) {
        TC.diagnose(override, diag::override_throws_objc,
                    isa<ConstructorDecl>(override));
        TC.diagnose(base, diag::overridden_here);
      }
    }

    // FIXME: Possibly should extend to more availability checking.
    if (base->getAttrs().isUnavailable(TC.Context)) {
      TC.diagnose(override, diag::override_unavailable, override->getName());
    }
    
    if (!TC.getLangOpts().DisableAvailabilityChecking) {
      diagnoseOverrideForAvailability(TC, override, base);
    }

    /// Check attributes associated with the base; some may need to merged with
    /// or checked against attributes in the overriding declaration.
    AttributeOverrideChecker attrChecker(TC, base, override);
    for (auto attr : base->getAttrs()) {
      attrChecker.visit(attr);
    }

    if (auto overridingFunc = dyn_cast<FuncDecl>(override)) {
      overridingFunc->setOverriddenDecl(cast<FuncDecl>(base));
    } else if (auto overridingCtor = dyn_cast<ConstructorDecl>(override)) {
      overridingCtor->setOverriddenDecl(cast<ConstructorDecl>(base));
    } else if (auto overridingASD = dyn_cast<AbstractStorageDecl>(override)) {
      auto *baseASD = cast<AbstractStorageDecl>(base);
      overridingASD->setOverriddenDecl(baseASD);

      // Make sure we get consistent overrides for the accessors as well.
      if (!baseASD->hasAccessorFunctions())
        addTrivialAccessorsToStorage(baseASD, TC);
      maybeAddMaterializeForSet(overridingASD, TC);

      auto recordAccessorOverride = [&](AccessorKind kind) {
        // We need the same accessor on both.
        auto baseAccessor = baseASD->getAccessorFunction(kind);
        if (!baseAccessor) return;
        auto overridingAccessor = overridingASD->getAccessorFunction(kind);
        if (!overridingAccessor) return;

        // For setter accessors, we need the base's setter to be
        // accessible from the overriding context, or it's not an override.
        if ((kind == AccessorKind::IsSetter ||
             kind == AccessorKind::IsMaterializeForSet) &&
            !baseASD->isSetterAccessibleFrom(overridingASD->getDeclContext()))
          return;

        // FIXME: Egregious hack to set an 'override' attribute.
        if (!overridingAccessor->getAttrs().hasAttribute<OverrideAttr>()) {
          auto loc = overridingASD->getOverrideLoc();
          overridingAccessor->getAttrs().add(
              new (TC.Context) OverrideAttr(loc));
        }

        recordOverride(TC, overridingAccessor, baseAccessor,
                       baseASD->isObjC());
      };

      recordAccessorOverride(AccessorKind::IsGetter);
      recordAccessorOverride(AccessorKind::IsSetter);
      recordAccessorOverride(AccessorKind::IsMaterializeForSet);
    } else {
      llvm_unreachable("Unexpected decl");
    }
    
    return false;
  }

  /// Compute the interface type of the given enum element.
  void computeEnumElementInterfaceType(EnumElementDecl *elt) {
    auto enumDecl = cast<EnumDecl>(elt->getDeclContext());
    assert(enumDecl->isGenericContext() && "Not a generic enum");

    // Build the generic function type.
    auto funcTy = elt->getType()->castTo<AnyFunctionType>();
    auto inputTy = ArchetypeBuilder::mapTypeOutOfContext(enumDecl,
                                                         funcTy->getInput());
    auto resultTy = ArchetypeBuilder::mapTypeOutOfContext(enumDecl,
                                                          funcTy->getResult());
    auto interfaceTy
      = GenericFunctionType::get(enumDecl->getGenericSignatureOfContext(),
                                 inputTy, resultTy, funcTy->getExtInfo());

    // Record the interface type.
    elt->setInterfaceType(interfaceTy);
  }

  void visitEnumCaseDecl(EnumCaseDecl *ECD) {
    // The type-checker doesn't care about how these are grouped.
  }

  void visitEnumElementDecl(EnumElementDecl *EED) {
    if (IsSecondPass) {
      checkAccessibility(TC, EED);
      return;
    }
    if (EED->hasType())
      return;

    TC.checkDeclAttributesEarly(EED);

    EnumDecl *ED = EED->getParentEnum();

    if (!EED->hasAccessibility())
      EED->setAccessibility(ED->getFormalAccess());
    
    EED->setIsBeingTypeChecked();

    // Only attempt to validate the argument type or raw value if the element
    // is not currently being validated.
    if (EED->getRecursiveness() == ElementRecursiveness::NotRecursive) {
      EED->setRecursiveness(ElementRecursiveness::PotentiallyRecursive);
      
      validateAttributes(TC, EED);
      
      if (!EED->getArgumentTypeLoc().isNull()) {
        if (TC.validateType(EED->getArgumentTypeLoc(), EED->getDeclContext(),
                            TR_EnumCase)) {
          EED->overwriteType(ErrorType::get(TC.Context));
          EED->setInvalid();
          return;
        }
      }

      // If we have a raw value, make sure there's a raw type as well.
      if (auto *rawValue = EED->getRawValueExpr()) {
        if (!ED->hasRawType()) {
          TC.diagnose(rawValue->getLoc(),diag::enum_raw_value_without_raw_type);
          // Recover by setting the raw type as this element's type.
          Expr *typeCheckedExpr = rawValue;
          if (!TC.typeCheckExpression(typeCheckedExpr, ED)) {
            EED->setTypeCheckedRawValueExpr(typeCheckedExpr);
            TC.checkEnumElementErrorHandling(EED);
          }
        } else {
          // Wait until the second pass, when all the raw value expressions
          // can be checked together.
        }
      }
    } else if (EED->getRecursiveness() ==
                ElementRecursiveness::PotentiallyRecursive) {
      EED->setRecursiveness(ElementRecursiveness::Recursive);
    }
    
    // If the element was not already marked as recursive by a re-entrant call,
    // we can be sure it's not recursive.
    if (EED->getRecursiveness() == ElementRecursiveness::PotentiallyRecursive) {
      EED->setRecursiveness(ElementRecursiveness::NotRecursive);
    }

    // Now that we have an argument type we can set the element's declared
    // type.
    EED->computeType();
    EED->setIsBeingTypeChecked(false);

    // Test for type parameters, as opposed to a generic decl context, in
    // case the enclosing enum type was illegally declared inside of a generic
    // context. (In that case, we'll post a diagnostic while visiting the
    // parent enum.)
    if (EED->getDeclContext()->isGenericTypeContext())
      computeEnumElementInterfaceType(EED);

    // Require the carried type to be materializable.
    if (EED->getArgumentType() &&
        !EED->getArgumentType()->isMaterializable()) {
      TC.diagnose(EED->getLoc(), diag::enum_element_not_materializable);
      EED->overwriteType(ErrorType::get(TC.Context));
      EED->setInvalid();
    }
    TC.checkDeclAttributes(EED);
  }

  void visitExtensionDecl(ExtensionDecl *ED) {
    TC.validateExtension(ED);

    if (ED->isInvalid()) {
      // Mark children as invalid.
      // FIXME: This is awful.
      for (auto member : ED->getMembers()) {
        member->setInvalid();
        if (ValueDecl *VD = dyn_cast<ValueDecl>(member))
          VD->overwriteType(ErrorType::get(TC.Context));
      }
      return;
    }

    TC.checkDeclAttributesEarly(ED);

    if (!IsSecondPass) {
      CanType ExtendedTy = ED->getExtendedType()->getCanonicalType();

      if (!isa<NominalType>(ExtendedTy) &&
          !isa<BoundGenericType>(ExtendedTy) &&
          !isa<ErrorType>(ExtendedTy)) {
        // FIXME: Redundant diagnostic test here?
        TC.diagnose(ED->getStartLoc(), diag::non_nominal_extension,
                    ExtendedTy);
        // FIXME: It would be nice to point out where we found the named type
        // declaration, if any.
        ED->setInvalid();
      }

      TC.checkInheritanceClause(ED);
      if (auto nominal = ExtendedTy->getAnyNominal())
        TC.validateDecl(nominal);

      validateAttributes(TC, ED);
    }

    // Check conformances before visiting members, since we might
    // synthesize bodies for derived conformances
    if (!IsFirstPass) {
      TC.computeDefaultAccessibility(ED);
      if (auto *AA = ED->getAttrs().getAttribute<AccessibilityAttr>()) {
        checkGenericParamAccessibility(TC, ED->getGenericParams(), ED,
                                       AA->getAccess());
      }
      checkExplicitConformance(ED, ED->getExtendedType());
    }

    if (!ED->isInvalid()) {
      for (Decl *Member : ED->getMembers())
        visit(Member);
      for (Decl *Global : ED->getDerivedGlobalDecls())
        visit(Global);
    }

    TC.checkDeclAttributes(ED);
 }

  void visitTopLevelCodeDecl(TopLevelCodeDecl *TLCD) {
    // See swift::performTypeChecking for TopLevelCodeDecl handling.
    llvm_unreachable("TopLevelCodeDecls are handled elsewhere");
  }
  
  void visitIfConfigDecl(IfConfigDecl *ICD) {
    // The active members of the #if block will be type checked along with
    // their enclosing declaration.
    TC.checkDeclAttributesEarly(ICD);
    TC.checkDeclAttributes(ICD);
  }

  void visitConstructorDecl(ConstructorDecl *CD) {
    if (CD->isInvalid()) {
      CD->overwriteType(ErrorType::get(TC.Context));
      return;
    }

    if (!IsFirstPass) {
      if (CD->getBody()) {
        TC.definedFunctions.push_back(CD);
      } else if (requiresDefinition(CD)) {
        // Complain if we should have a body.
        TC.diagnose(CD->getLoc(), diag::missing_initializer_def);
      }
    }

    if (IsSecondPass) {
      checkAccessibility(TC, CD);
      TC.checkOmitNeedlessWords(CD);
      return;
    }
    if (CD->hasType())
      return;

    TC.checkDeclAttributesEarly(CD);
    TC.computeAccessibility(CD);

    assert(CD->getDeclContext()->isTypeContext()
           && "Decl parsing must prevent constructors outside of types!");

    // convenience initializers are only allowed on classes and in
    // extensions thereof.
    if (CD->isConvenienceInit()) {
      if (auto extType = CD->getExtensionType()) {
        if (!extType->getClassOrBoundGenericClass() &&
            !extType->is<ErrorType>()) {
          auto ConvenienceLoc =
            CD->getAttrs().getAttribute<ConvenienceAttr>()->getLocation();

          // Produce a tailored diagnostic for structs and enums.
          bool isStruct = extType->getStructOrBoundGenericStruct() != nullptr;
          if (isStruct || extType->getEnumOrBoundGenericEnum()) {
            TC.diagnose(CD->getLoc(), diag::enumstruct_convenience_init,
                        isStruct ? "structs" : "enums")
              .fixItRemove(ConvenienceLoc);
          } else {
            TC.diagnose(CD->getLoc(), diag::nonclass_convenience_init, extType)
              .fixItRemove(ConvenienceLoc);
          }
          CD->setInitKind(CtorInitializerKind::Designated);
        }
      }
    } else if (auto extType = CD->getExtensionType()) {
      // A designated initializer for a class must be written within the class
      // itself.
      if (extType->getClassOrBoundGenericClass() &&
          isa<ExtensionDecl>(CD->getDeclContext())) {
        TC.diagnose(CD->getLoc(), diag::designated_init_in_extension, extType)
          .fixItInsert(CD->getLoc(), "convenience ");
        CD->setInitKind(CtorInitializerKind::Convenience);
      } else if (CD->getDeclContext()->isProtocolExtensionContext()) {
        CD->setInitKind(CtorInitializerKind::Convenience);
      }
    }

    Type SelfTy = configureImplicitSelf(TC, CD);

    Optional<ArchetypeBuilder> builder;
    if (auto gp = CD->getGenericParams()) {
      // Write up generic parameters and check the generic parameter list.
      gp->setOuterParameters(CD->getDeclContext()->getGenericParamsOfContext());

      if (TC.validateGenericFuncSignature(CD)) {
        markInvalidGenericSignature(CD, TC);
      } else {
        ArchetypeBuilder builder =
          TC.createArchetypeBuilder(CD->getModuleContext());
        auto *parentSig = CD->getDeclContext()->getGenericSignatureOfContext();
        TC.checkGenericParamList(&builder, gp, parentSig);

        // Infer requirements from the parameters of the constructor.
        builder.inferRequirements(CD->getParameterList(1), gp);

        // Revert the types within the signature so it can be type-checked with
        // archetypes below.
        TC.revertGenericFuncSignature(CD);

        // Assign archetypes.
        finalizeGenericParamList(builder, gp, CD, TC);
      }
    } else if (CD->getDeclContext()->isGenericTypeContext()) {
      if (TC.validateGenericFuncSignature(CD)) {
        CD->setInvalid();
      } else {
        // Revert all of the types within the signature of the constructor.
        TC.revertGenericFuncSignature(CD);
      }
    }

    // Type check the constructor parameters.
    if (CD->isInvalid() || semaFuncParamPatterns(CD)) {
      CD->overwriteType(ErrorType::get(TC.Context));
      CD->setInvalid();
    } else {
      configureConstructorType(CD, SelfTy,
                               CD->getParameterList(1)->getType(TC.Context),
                               CD->getThrowsLoc().isValid());
    }

    validateAttributes(TC, CD);

    // Check whether this initializer overrides an initializer in its
    // superclass.
    if (!checkOverrides(TC, CD)) {
      // If an initializer has an override attribute but does not override
      // anything or overrides something that doesn't need an 'override'
      // keyword (e.g., a convenience initializer), complain.
      // anything, or overrides something that complain.
      if (auto *attr = CD->getAttrs().getAttribute<OverrideAttr>()) {
        if (!CD->getOverriddenDecl()) {
          TC.diagnose(CD, diag::initializer_does_not_override)
            .highlight(attr->getLocation());
          attr->setInvalid();
        } else if (!overrideRequiresKeyword(CD->getOverriddenDecl())) {
          // Special case: we are overriding a 'required' initializer, so we
          // need (only) the 'required' keyword.
          if (cast<ConstructorDecl>(CD->getOverriddenDecl())->isRequired()) {
            if (CD->getAttrs().hasAttribute<RequiredAttr>()) {
              TC.diagnose(CD, diag::required_initializer_override_keyword)
                .fixItRemove(attr->getLocation());
            } else {
              TC.diagnose(CD, diag::required_initializer_override_wrong_keyword)
                .fixItReplace(attr->getLocation(), "required");
              CD->getAttrs().add(
                new (TC.Context) RequiredAttr(/*implicit=*/true));
            }

            TC.diagnose(findNonImplicitRequiredInit(CD->getOverriddenDecl()),
                        diag::overridden_required_initializer_here);
          } else {
            // We tried to override a convenience initializer.
            TC.diagnose(CD, diag::initializer_does_not_override)
              .highlight(attr->getLocation());
            TC.diagnose(CD->getOverriddenDecl(),
                        diag::convenience_init_override_here);
          }
        }
      }

      // A failable initializer cannot override a non-failable one.
      // This would normally be diagnosed by the covariance rules;
      // however, those are disabled so that we can provide a more
      // specific diagnostic here.
      if (CD->getFailability() != OTK_None &&
          CD->getOverriddenDecl() &&
          CD->getOverriddenDecl()->getFailability() == OTK_None) {
        TC.diagnose(CD, diag::failable_initializer_override,
                    CD->getFullName());
        TC.diagnose(CD->getOverriddenDecl(), 
                    diag::nonfailable_initializer_override_here,
                    CD->getOverriddenDecl()->getFullName());
      }
    }

    // An initializer is ObjC-compatible if it's explicitly @objc or a member
    // of an ObjC-compatible class.
    Type ContextTy = CD->getDeclContext()->getDeclaredTypeInContext();
    if (ContextTy) {
      Optional<ObjCReason> isObjC = shouldMarkAsObjC(TC, CD,
          /*allowImplicit=*/true);

      Optional<ForeignErrorConvention> errorConvention;
      if (isObjC &&
          (CD->isInvalid() ||
           !TC.isRepresentableInObjC(CD, *isObjC, errorConvention)))
        isObjC = None;
      markAsObjC(TC, CD, isObjC, errorConvention);
    }

    // If this initializer overrides a 'required' initializer, it must itself
    // be marked 'required'.
    if (!CD->getAttrs().hasAttribute<RequiredAttr>()) {
      if (CD->getOverriddenDecl() && CD->getOverriddenDecl()->isRequired()) {
        TC.diagnose(CD, diag::required_initializer_missing_keyword)
          .fixItInsert(CD->getLoc(), "required ");

        TC.diagnose(findNonImplicitRequiredInit(CD->getOverriddenDecl()),
                    diag::overridden_required_initializer_here);

        CD->getAttrs().add(
            new (TC.Context) RequiredAttr(/*IsImplicit=*/true));
      }
    }

    if (CD->isRequired() && ContextTy) {
      if (auto nominal = ContextTy->getAnyNominal()) {
        if (CD->getFormalAccess() < nominal->getFormalAccess()) {
          auto diag = TC.diagnose(CD,
                                  diag::required_initializer_not_accessible);
          fixItAccessibility(diag, CD, nominal->getFormalAccess());
        }
      }
    }

    inferDynamic(TC.Context, CD);

    TC.checkDeclAttributes(CD);
  }

  void visitDestructorDecl(DestructorDecl *DD) {
    if (DD->isInvalid()) {
      DD->overwriteType(ErrorType::get(TC.Context));
      return;
    }

    if (!IsFirstPass) {
      if (DD->getBody())
        TC.definedFunctions.push_back(DD);
    }

    if (IsSecondPass || DD->hasType()) {
      return;
    }

    assert(DD->getDeclContext()->isTypeContext()
           && "Decl parsing must prevent destructors outside of types!");

    TC.checkDeclAttributesEarly(DD);
    if (!DD->hasAccessibility()) {
      auto enclosingClass = cast<ClassDecl>(DD->getParent());
      DD->setAccessibility(enclosingClass->getFormalAccess());
    }

    Type SelfTy = configureImplicitSelf(TC, DD);

    if (DD->getDeclContext()->isGenericTypeContext())
      TC.validateGenericFuncSignature(DD);

    if (semaFuncParamPatterns(DD)) {
      DD->overwriteType(ErrorType::get(TC.Context));
      DD->setInvalid();
    }

    Type FnTy;
    if (DD->getDeclContext()->isGenericTypeContext())
      FnTy = PolymorphicFunctionType::get(SelfTy,
                                          TupleType::getEmpty(TC.Context),
                             DD->getDeclContext()->getGenericParamsOfContext());
    else
      FnTy = FunctionType::get(SelfTy, TupleType::getEmpty(TC.Context));

    DD->setType(FnTy);

    // Do this before markAsObjC() to diagnose @nonobjc better
    validateAttributes(TC, DD);

    // Destructors are always @objc, because their Objective-C entry point is
    // -dealloc.
    markAsObjC(TC, DD, ObjCReason::ImplicitlyObjC);

    TC.checkDeclAttributes(DD);
  }
};
}; // end anonymous namespace.

bool swift::checkOverrides(TypeChecker &TC, ValueDecl *decl) {
  return DeclChecker::checkOverrides(TC, decl);
}

bool TypeChecker::isAvailabilitySafeForOverride(ValueDecl *override,
                                                ValueDecl *base) {
  // API availability ranges are contravariant: make sure the version range
  // of an overridden declaration is fully contained in the range of the
  // overriding declaration.
  AvailabilityContext overrideInfo =
      AvailabilityInference::availableRange(override, Context);
  AvailabilityContext baseInfo =
      AvailabilityInference::availableRange(base, Context);

  return baseInfo.isContainedIn(overrideInfo);
}

bool TypeChecker::isAvailabilitySafeForConformance(
    ValueDecl *witness, ValueDecl *requirement,
    NormalProtocolConformance *conformance,
    AvailabilityContext &requirementInfo) {
  DeclContext *DC = conformance->getDeclContext();

  // We assume conformances in
  // non-SourceFiles have already been checked for availability.
  if (!DC->getParentSourceFile())
    return true;

  NominalTypeDecl *conformingDecl = DC->isNominalTypeOrNominalTypeExtensionContext();
  assert(conformingDecl && "Must have conforming declaration");

  // Make sure that any access of the witness through the protocol
  // can only occur when the witness is available. That is, make sure that
  // on every version where the conforming declaration is available, if the
  // requirement is available then the witness is available as well.
  // We do this by checking that (an over-approximation of) the intersection of
  // the requirement's available range with both the conforming declaration's
  // available range and the protocol's available range is fully contained in
  // (an over-approximation of) the intersection of the witnesses's available
  // range with both the conforming type's available range and the protocol
  // declaration's available range.
  AvailabilityContext witnessInfo =
      AvailabilityInference::availableRange(witness, Context);
  requirementInfo = AvailabilityInference::availableRange(requirement, Context);

  AvailabilityContext infoForConformingDecl =
      overApproximateAvailabilityAtLocation(conformingDecl->getLoc(),
                                            conformingDecl);

  // Constrain over-approximates intersection of version ranges.
  witnessInfo.constrainWith(infoForConformingDecl);
  requirementInfo.constrainWith(infoForConformingDecl);

  ProtocolDecl *protocolDecl = conformance->getProtocol();
  AvailabilityContext infoForProtocolDecl =
      overApproximateAvailabilityAtLocation(protocolDecl->getLoc(),
                                            protocolDecl);

  witnessInfo.constrainWith(infoForProtocolDecl);
  requirementInfo.constrainWith(infoForProtocolDecl);

  return requirementInfo.isContainedIn(witnessInfo);
}

void TypeChecker::typeCheckDecl(Decl *D, bool isFirstPass) {
  PrettyStackTraceDecl StackTrace("type-checking", D);
  checkForForbiddenPrefix(D);
  bool isSecondPass =
    !isFirstPass && D->getDeclContext()->isModuleScopeContext();
  DeclChecker(*this, isFirstPass, isSecondPass).visit(D);
}

// A class is @objc if it does not have generic ancestry, and it either has
// an explicit @objc attribute, or its superclass is @objc.
static Optional<ObjCReason> shouldMarkClassAsObjC(TypeChecker &TC,
                                                  ClassDecl *CD) {
  ObjCClassKind kind = CD->checkObjCAncestry();

  if (auto attr = CD->getAttrs().getAttribute<ObjCAttr>()) {
    if (kind == ObjCClassKind::ObjCMembers) {
      TC.diagnose(attr->getLocation(), diag::objc_for_generic_class)
        .fixItRemove(attr->getRangeWithAt());
    }

    // Only allow ObjC-rooted classes to be @objc.
    // (Leave a hole for test cases.)
    if (kind == ObjCClassKind::ObjCWithSwiftRoot &&
        TC.getLangOpts().EnableObjCAttrRequiresFoundation) {
      TC.diagnose(attr->getLocation(), diag::invalid_objc_swift_rooted_class)
        .fixItRemove(attr->getRangeWithAt());
    }

    return ObjCReason::ExplicitlyObjC;
  }

  if (kind == ObjCClassKind::ObjCWithSwiftRoot ||
      kind == ObjCClassKind::ObjC)
    return ObjCReason::ImplicitlyObjC;

  return None;
}

void TypeChecker::validateDecl(ValueDecl *D, bool resolveTypeParams) {
  if (hasEnabledForbiddenTypecheckPrefix())
    checkForForbiddenPrefix(D);

  validateAccessibility(D);

  // Validate the context. We don't do this for generic parameters,
  // because those are validated as part of their context.
  if (D->getKind() != DeclKind::GenericTypeParam) {
    auto dc = D->getDeclContext();
    if (auto nominal = dyn_cast<NominalTypeDecl>(dc)) {
      if (nominal->isBeingTypeChecked())
        return;

      validateDecl(nominal, false);
    } else if (auto ext = dyn_cast<ExtensionDecl>(dc)) {
      if (ext->isBeingTypeChecked())
        return;

      validateExtension(ext);
    }
  }
  
  switch (D->getKind()) {
  case DeclKind::Import:
  case DeclKind::Extension:
  case DeclKind::PatternBinding:
  case DeclKind::EnumCase:
  case DeclKind::TopLevelCode:
  case DeclKind::InfixOperator:
  case DeclKind::PrefixOperator:
  case DeclKind::PostfixOperator:
  case DeclKind::IfConfig:
    llvm_unreachable("not a value decl");

  case DeclKind::Module:
    return;

  case DeclKind::TypeAlias: {
    // Type aliases may not have an underlying type yet.
    auto typeAlias = cast<TypeAliasDecl>(D);

    if (typeAlias->getDeclContext()->isModuleScopeContext()) {
      IterativeTypeChecker ITC(*this);
      ITC.satisfy(requestResolveTypeDecl(typeAlias));
    } else {
      // Compute the declared type.
      if (!typeAlias->hasType())
        typeAlias->computeType();

      if (typeAlias->getUnderlyingTypeLoc().getTypeRepr() &&
          !typeAlias->getUnderlyingTypeLoc().wasValidated())
        typeCheckDecl(typeAlias, true);
    }

    break;
  }

  case DeclKind::GenericTypeParam:
  case DeclKind::AssociatedType: {
    auto typeParam = cast<AbstractTypeParamDecl>(D);
    auto assocType = dyn_cast<AssociatedTypeDecl>(typeParam);
    if (assocType && assocType->isRecursive()) {
      D->setInvalid();
      break;
    }
      
    if (!resolveTypeParams || typeParam->getArchetype()) {
      if (assocType) {
        DeclChecker(*this, false, false).visitAssociatedTypeDecl(assocType);

        if (!assocType->hasType())
          assocType->computeType();
      }

      break;
    }
    
    // FIXME: Avoid full check in these cases?
    DeclContext *DC = typeParam->getDeclContext();
    switch (DC->getContextKind()) {
    case DeclContextKind::SerializedLocal:
    case DeclContextKind::Module:
    case DeclContextKind::FileUnit:
    case DeclContextKind::TopLevelCodeDecl:
    case DeclContextKind::Initializer:
    case DeclContextKind::SubscriptDecl:
      llvm_unreachable("cannot have type params");

    case DeclContextKind::NominalTypeDecl: {
      auto nominal = cast<NominalTypeDecl>(DC);
      typeCheckDecl(nominal, true);
      if (auto assocType = dyn_cast<AssociatedTypeDecl>(typeParam))
        if (!assocType->hasType())
          assocType->computeType();
      if (!typeParam->hasAccessibility())
        typeParam->setAccessibility(nominal->getFormalAccess());
      break;
    }

    case DeclContextKind::ExtensionDecl:
      llvm_unreachable("not yet implemented");
    
    case DeclContextKind::AbstractClosureExpr:
      llvm_unreachable("cannot have type params");

    case DeclContextKind::AbstractFunctionDecl: {
      if (auto nominal = dyn_cast<NominalTypeDecl>(DC->getParent()))
        typeCheckDecl(nominal, true);
      else if (auto extension = dyn_cast<ExtensionDecl>(DC->getParent()))
        typeCheckDecl(extension, true);
      auto fn = cast<AbstractFunctionDecl>(DC);
      typeCheckDecl(fn, true);
      if (auto assocType = dyn_cast<AssociatedTypeDecl>(typeParam))
        if (!assocType->hasType())
          assocType->computeType();
      if (!typeParam->hasAccessibility())
        typeParam->setAccessibility(fn->getFormalAccess());
      break;
    }
    }
    break;
  }
  
  case DeclKind::Enum:
  case DeclKind::Struct:
  case DeclKind::Class: {
    auto nominal = cast<NominalTypeDecl>(D);
    if (nominal->hasType())
      return;
    nominal->computeType();

    // Check generic parameters, if needed.
    if (auto gp = nominal->getGenericParams()) {
      gp->setOuterParameters(
        nominal->getDeclContext()->getGenericParamsOfContext());

      // Validate the generic type parameters.
      if (validateGenericTypeSignature(nominal)) {
        markInvalidGenericSignature(nominal, *this);
        return;
      }

      // If we're already validating the type declaration's generic signature,
      // avoid a potential infinite loop by not re-validating the generic
      // parameter list.
      if (!nominal->IsValidatingGenericSignature()) {
        revertGenericParamList(gp);

        ArchetypeBuilder builder =
          createArchetypeBuilder(nominal->getModuleContext());
        auto *parentSig = nominal->getDeclContext()->getGenericSignatureOfContext();
        checkGenericParamList(&builder, gp, parentSig);
        finalizeGenericParamList(builder, gp, nominal, *this);
      }
    }

    checkInheritanceClause(D);
    validateAttributes(*this, D);

    // Mark a class as @objc. This must happen before checking its members.
    if (auto CD = dyn_cast<ClassDecl>(nominal)) {
      Optional<ObjCReason> isObjC = shouldMarkClassAsObjC(*this, CD);
      markAsObjC(*this, CD, isObjC);

      // Determine whether we require in-class initializers.
      if (CD->getAttrs().hasAttribute<RequiresStoredPropertyInitsAttr>() ||
          (CD->hasSuperclass() &&
           CD->getSuperclass()->getClassOrBoundGenericClass()
             ->requiresStoredPropertyInits()))
        CD->setRequiresStoredPropertyInits(true);
    }

    if (auto *ED = dyn_cast<EnumDecl>(nominal)) {
      // @objc enums use their raw values as the value representation, so we
      // need to force the values to be checked.
      if (ED->isObjC())
        checkEnumRawValues(*this, ED);
    }

    ValidatedTypes.insert(nominal);
    break;
  }

  case DeclKind::Protocol: {
    auto proto = cast<ProtocolDecl>(D);
    if (proto->hasType())
      return;
    proto->computeType();
    
    
    auto gp = proto->getGenericParams();

    // Resolve the inheritance clauses for each of the associated
    // types.
    for (auto member : proto->getMembers()) {
      if (auto assocType = dyn_cast<AssociatedTypeDecl>(member)) {
        resolveInheritanceClause(assocType);
      }
    }

    // Validate the generic type signature, which is just <Self : P>.
    validateGenericTypeSignature(proto);

    assert(gp->getOuterParameters() ==
           proto->getDeclContext()->getGenericParamsOfContext());

    revertGenericParamList(gp);

    ArchetypeBuilder builder =
      createArchetypeBuilder(proto->getModuleContext());
    auto *parentSig = proto->getDeclContext()->getGenericSignatureOfContext();
    checkGenericParamList(&builder, gp, parentSig);
    finalizeGenericParamList(builder, gp, proto, *this);

    // Record inherited protocols.
    resolveInheritedProtocols(proto);

    validateAttributes(*this, D);

    // Set the underlying type of each of the associated types to the
    // appropriate archetype.
    auto selfDecl = proto->getProtocolSelf();
    ArchetypeType *selfArchetype = builder.getArchetype(selfDecl);
    for (auto member : proto->getMembers()) {
      if (auto assocType = dyn_cast<AssociatedTypeDecl>(member)) {
        TypeLoc underlyingTy;
        ArchetypeType *archetype = selfArchetype;
        archetype = selfArchetype->getNestedType(assocType->getName())
          .getAsArchetype();
        if (!archetype)
          return;

        assocType->setArchetype(archetype);
        if (!assocType->hasType())
          assocType->computeType();
      }
    }

    // If the protocol is @objc, it may only refine other @objc protocols.
    // FIXME: Revisit this restriction.
    if (proto->getAttrs().hasAttribute<ObjCAttr>()) {
      Optional<ObjCReason> isObjC = ObjCReason::ImplicitlyObjC;

      for (auto inherited : proto->getInheritedProtocols(nullptr)) {
        if (!inherited->isObjC()) {
          diagnose(proto->getLoc(),
                   diag::objc_protocol_inherits_non_objc_protocol,
                   proto->getDeclaredType(), inherited->getDeclaredType());
          diagnose(inherited->getLoc(), diag::protocol_here,
                   inherited->getName());
          isObjC = None;
        }
      }

      markAsObjC(*this, proto, isObjC);
    }

    ValidatedTypes.insert(proto);
    break;
  }
      
  case DeclKind::Var:
  case DeclKind::Param: {
    auto VD = cast<VarDecl>(D);
    if (!VD->hasType()) {
      if (PatternBindingDecl *PBD = VD->getParentPatternBinding()) {
        if (PBD->isBeingTypeChecked()) {
          diagnose(VD, diag::pattern_used_in_type, VD->getName());

        } else {
          for (unsigned i = 0, e = PBD->getNumPatternEntries(); i != e; ++i)
            validatePatternBindingDecl(*this, PBD, i);
        }

        auto parentPattern = VD->getParentPattern();
        if (PBD->isInvalid() || !parentPattern->hasType()) {
          parentPattern->setType(ErrorType::get(Context));
          setBoundVarsTypeError(parentPattern, Context);
          
          // If no type has been set for the initializer, we need to diagnose
          // the failure.
          if (VD->getParentInitializer() &&
              !VD->getParentInitializer()->getType()) {
            diagnose(parentPattern->getLoc(), diag::identifier_init_failure,
                     parentPattern->getBoundName());
          }
          
          return;
        }
      } else if (VD->isSelfParameter()) {
        // If the variable declaration is for a 'self' parameter, it may be
        // because the self variable was reverted whilst validating the function
        // signature.  In that case, reset the type.
        if (isa<NominalTypeDecl>(VD->getDeclContext()->getParent())) {
          if (auto funcDeclContext =
                  dyn_cast<AbstractFunctionDecl>(VD->getDeclContext())) {
            configureImplicitSelf(*this, funcDeclContext);
          }
        } else {
          D->setType(ErrorType::get(Context));
        }      
      } else {
        // FIXME: This case is hit when code completion occurs in a function
        // parameter list. Previous parameters are definitely in scope, but
        // we don't really know how to type-check them.
        // We can also hit this when code-completing in a closure body.
        assert(isa<AbstractFunctionDecl>(D->getDeclContext()) ||
               isa<AbstractClosureExpr>(D->getDeclContext()) ||
               isa<TopLevelCodeDecl>(D->getDeclContext()));
        D->setType(ErrorType::get(Context));
      }

      // Make sure the getter and setter have valid types, since they will be
      // used by SILGen for any accesses to this variable.
      if (auto getter = VD->getGetter())
        validateDecl(getter);
      if (auto setter = VD->getSetter())
        validateDecl(setter);
    }

    // Synthesize accessors as necessary.
    maybeAddAccessorsToVariable(VD, *this);

    if (!VD->didEarlyAttrValidation()) {
      checkDeclAttributesEarly(VD);
      validateAttributes(*this, VD);

      // FIXME: Guarding the rest of these things together with early attribute
      // validation is a hack. It's necessary because properties can get types
      // before validateDecl is called.

      if (!DeclChecker::checkOverrides(*this, VD)) {
        // If a property has an override attribute but does not override
        // anything, complain.
        auto overridden = VD->getOverriddenDecl();
        if (auto *OA = VD->getAttrs().getAttribute<OverrideAttr>()) {
          if (!overridden) {
            diagnose(VD, diag::property_does_not_override)
              .highlight(OA->getLocation());
            OA->setInvalid();
          }
        }
      }

      // Properties need some special validation logic.
      if (Type contextType = VD->getDeclContext()->getDeclaredTypeInContext()) {
        // If this is a property, check if it needs to be exposed to
        // Objective-C.
        Optional<ObjCReason> isObjC = shouldMarkAsObjC(*this, VD);

        if (isObjC && !isRepresentableInObjC(VD, *isObjC))
          isObjC = None;

        markAsObjC(*this, VD, isObjC);

        inferDynamic(Context, VD);

        // If this variable is a class member, mark it final if the
        // class is final, or if it was declared with 'let'.
        if (auto cls = contextType->getClassOrBoundGenericClass()) {
          if (cls->isFinal() || VD->isLet()) {
            if (!VD->isFinal() && !VD->isDynamic()) {
              makeFinal(Context, VD);
            }
          }
          if (VD->isStatic()) {
            auto staticSpelling =
              VD->getParentPatternBinding()->getStaticSpelling();
            if (staticSpelling == StaticSpellingKind::KeywordStatic) {
              auto finalAttr = VD->getAttrs().getAttribute<FinalAttr>();
              if (finalAttr) {
                auto finalRange = finalAttr->getRange();
                if (finalRange.isValid())
                  diagnose(finalRange.Start, diag::decl_already_final)
                  .highlight(finalRange)
                  .fixItRemove(finalRange);
              }
              makeFinal(Context, VD);
            }
          }
        }
      }


      // If this variable is marked final and has a getter or setter, mark the
      // getter and setter as final as well.
      if (VD->isFinal()) {
        makeFinal(Context, VD->getGetter());
        makeFinal(Context, VD->getSetter());
        makeFinal(Context, VD->getMaterializeForSetFunc());
      } else if (VD->isDynamic()) {
        makeDynamic(Context, VD->getGetter());
        makeDynamic(Context, VD->getSetter());
        // Skip materializeForSet -- it won't be used with a dynamic property.
      }

      if (VD->hasAccessorFunctions()) {
        maybeAddMaterializeForSet(VD, *this);
      }
    }

    break;
  }
      
  case DeclKind::Func: {
    if (D->hasType())
      return;
    typeCheckDecl(D, true);
    break;
  }

  case DeclKind::Subscript:
  case DeclKind::Constructor:
    if (D->hasType())
      return;
    typeCheckDecl(D, true);
    break;

  case DeclKind::Destructor:
  case DeclKind::EnumElement: {
    if (D->hasType())
      return;
    auto container = cast<NominalTypeDecl>(D->getDeclContext());
    validateDecl(container);
    typeCheckDecl(D, true);
    break;
  }
  }

  assert(D->hasType());
}

void TypeChecker::validateAccessibility(ValueDecl *D) {
  if (D->hasAccessibility())
    return;

  // FIXME: Encapsulate the following in computeAccessibility() ?

  switch (D->getKind()) {
  case DeclKind::Import:
  case DeclKind::Extension:
  case DeclKind::PatternBinding:
  case DeclKind::EnumCase:
  case DeclKind::TopLevelCode:
  case DeclKind::InfixOperator:
  case DeclKind::PrefixOperator:
  case DeclKind::PostfixOperator:
  case DeclKind::IfConfig:
    llvm_unreachable("not a value decl");

  case DeclKind::Module:
    break;

  case DeclKind::TypeAlias:
    computeAccessibility(D);
    break;

  case DeclKind::GenericTypeParam:
    // Ultimately handled in validateDecl() with resolveTypeParams=true.
    return;

  case DeclKind::AssociatedType: {
      auto assocType = cast<AssociatedTypeDecl>(D);
      auto prot = assocType->getProtocol();
      validateAccessibility(prot);
      assocType->setAccessibility(prot->getFormalAccess());
      break;
    }

  case DeclKind::Enum:
  case DeclKind::Struct:
  case DeclKind::Class:
  case DeclKind::Protocol:
  case DeclKind::Var:
  case DeclKind::Param:
  case DeclKind::Func:
  case DeclKind::Subscript:
  case DeclKind::Constructor:
    computeAccessibility(D);
    break;

  case DeclKind::Destructor:
  case DeclKind::EnumElement: {
    if (D->isInvalid()) {
      D->setAccessibility(Accessibility::Private);
    } else {
      auto container = cast<NominalTypeDecl>(D->getDeclContext());
      validateAccessibility(container);
      D->setAccessibility(container->getFormalAccess());
    }
    break;
  }
  }

  assert(D->hasAccessibility());
}

/// Check the generic parameters of an extension, recursively handling all of
/// the parameter lists within the extension.
static Type checkExtensionGenericParams(
              TypeChecker &tc, ExtensionDecl *ext,
              Type type, GenericParamList *genericParams,
              GenericSignature *&sig) {
  // Find the nominal type declaration and its parent type.
  Type parentType;
  NominalTypeDecl *nominal;
  if (auto unbound = type->getAs<UnboundGenericType>()) {
    parentType = unbound->getParent();
    nominal = unbound->getDecl();
  } else if (auto bound = type->getAs<BoundGenericType>()) {
    parentType = bound->getParent();
    nominal = bound->getDecl();
  } else {
    auto nominalType = type->castTo<NominalType>();
    parentType = nominalType->getParent();
    nominal = nominalType->getDecl();
  }

  // Recurse to check the parent type, if there is one.
  Type newParentType = parentType;
  if (parentType) {
    newParentType = checkExtensionGenericParams(
                      tc, ext, parentType,
                      nominal->getGenericParams()
                        ? genericParams->getOuterParameters()
                        : genericParams,
                      sig);
    if (!newParentType)
      return Type();
  }

  // If we don't need generic parameters, just build the result.
  if (!nominal->getGenericParams()) {
    assert(!genericParams);

    // If the parent was unchanged, return the original pointer.
    if (parentType.getPointer() == newParentType.getPointer())
      return type;

    return NominalType::get(nominal, newParentType, tc.Context);
  }

  // Local function used to infer requirements from the extended type.
  TypeLoc extendedTypeInfer;
  auto inferExtendedTypeReqs = [&](ArchetypeBuilder &builder) -> bool {
    if (extendedTypeInfer.isNull()) {
      if (isa<ProtocolDecl>(nominal)) {
        // Simple case: protocols don't form bound generic types.
        extendedTypeInfer.setType(nominal->getDeclaredInterfaceType());
      } else {
        SmallVector<Type, 2> genericArgs;
        for (auto gp : *genericParams) {
          genericArgs.push_back(gp->getDeclaredInterfaceType());
        }

        extendedTypeInfer.setType(BoundGenericType::get(nominal,
                                                        newParentType,
                                                        genericArgs));
      }
    }
    
    return builder.inferRequirements(extendedTypeInfer, genericParams);
  };

  ext->setIsBeingTypeChecked(true);
  defer { ext->setIsBeingTypeChecked(false); };

  // Validate the generic type signature.
  bool invalid = false;
  sig = tc.validateGenericSignature(genericParams, ext->getDeclContext(),
                                    nullptr, inferExtendedTypeReqs, invalid);
  if (invalid) {
    return nullptr;
  }

  // Validate the generic parameters for the last time.
  tc.revertGenericParamList(genericParams);
  ArchetypeBuilder builder = tc.createArchetypeBuilder(ext->getModuleContext());
  auto *parentSig = ext->getDeclContext()->getGenericSignatureOfContext();
  tc.checkGenericParamList(&builder, genericParams, parentSig);
  inferExtendedTypeReqs(builder);
  finalizeGenericParamList(builder, genericParams, ext, tc);

  if (isa<ProtocolDecl>(nominal)) {
    // Retain type sugar if it's there.
    if (nominal->getDeclaredType()->isEqual(type))
      return type;

    return nominal->getDeclaredType();
  }

  // Compute the final extended type.
  SmallVector<Type, 2> genericArgs;
  for (auto gp : *genericParams) {
    genericArgs.push_back(gp->getArchetype());
  }
  Type resultType = BoundGenericType::get(nominal, newParentType, genericArgs);
  return resultType->isEqual(type) ? type : resultType;
}

// FIXME: In TypeChecker.cpp; only needed because LLDB creates
// extensions of typealiases to unbound generic types, which is
// ill-formed but convenient.
namespace swift {
GenericParamList *cloneGenericParams(ASTContext &ctx,
                                     DeclContext *dc,
                                     GenericParamList *fromParams,
                                     GenericParamList *outerParams);
}

void TypeChecker::validateExtension(ExtensionDecl *ext) {
  // If we already validated this extension, there's nothing more to do.
  if (ext->validated())
    return;

  ext->setValidated();

  // If the extension is already known to be invalid, we're done.
  if (ext->isInvalid())
    return;

  // FIXME: We need to check whether anything is specialized, because
  // the innermost extended type might itself be a non-generic type
  // within a generic type.
  auto extendedType = ext->getExtendedType();

  if (extendedType.isNull() || extendedType->is<ErrorType>())
    return;

  if (auto unbound = extendedType->getAs<UnboundGenericType>()) {
    // Validate the nominal type declaration being extended.
    auto nominal = unbound->getDecl();
    validateDecl(nominal);

    auto genericParams = ext->getGenericParams();

    // The debugger synthesizes typealiases of unbound generic types
    // to produce its extensions, which subverts bindExtensionDecl's
    // ability to create the generic parameter lists. Create the list now.
    if (!genericParams && Context.LangOpts.DebuggerSupport) {
      genericParams = cloneGenericParams(Context, ext,
                                         nominal->getGenericParams(),
                                         nullptr);
      ext->setGenericParams(genericParams);
    }
    assert(genericParams && "bindExtensionDecl didn't set generic params?");

    // Check generic parameters.
    GenericSignature *sig = nullptr;
    extendedType = checkExtensionGenericParams(*this, ext,
                                               ext->getExtendedType(),
                                               ext->getGenericParams(),
                                               sig);
    if (!extendedType) {
      ext->setInvalid();
      ext->getExtendedTypeLoc().setInvalidType(Context);
      return;
    }

    ext->setGenericSignature(sig);
    ext->getExtendedTypeLoc().setType(extendedType);
    return;
  }

  // If we're extending a protocol, check the generic parameters.
  if (auto proto = extendedType->getAs<ProtocolType>()) {
    if (!isa<ProtocolType>(extendedType.getPointer()) &&
        proto->getDecl()->getParentModule() == ext->getParentModule()) {
      // Protocols in the same module cannot be extended via a typealias;
      // we could end up being unable to resolve the generic signature.
      diagnose(ext->getLoc(), diag::extension_protocol_via_typealias, proto)
        .fixItReplace(ext->getExtendedTypeLoc().getSourceRange(),
                      proto->getDecl()->getName().str());
      ext->setInvalid();
      ext->getExtendedTypeLoc().setInvalidType(Context);
      return;
    }

    GenericSignature *sig = nullptr;
    extendedType = checkExtensionGenericParams(*this, ext,
                                               ext->getExtendedType(),
                                               ext->getGenericParams(),
                                               sig);
    if (!extendedType) {
      ext->setInvalid();
      ext->getExtendedTypeLoc().setInvalidType(Context);
      return;
    }

    ext->setGenericSignature(sig);
    ext->getExtendedTypeLoc().setType(extendedType);

    // Speculatively ban extension of AnyObject; it won't be a
    // protocol forever, and we don't want to allow code that we know
    // we'll break later.
    if (proto->getDecl()->isSpecificProtocol(
          KnownProtocolKind::AnyObject)) {
      diagnose(ext, diag::extension_anyobject)
        .highlight(ext->getExtendedTypeLoc().getSourceRange());
    }
    return;
  }
}

ArrayRef<ProtocolDecl *>
TypeChecker::getDirectConformsTo(ProtocolDecl *proto) {
  resolveInheritedProtocols(proto);
  return proto->getInheritedProtocols(nullptr);
}

/// Build a default initializer string for the given pattern.
///
/// This string is suitable for display in diagnostics.
static Optional<std::string> buildDefaultInitializerString(TypeChecker &tc,
                                                           DeclContext *dc,
                                                           Pattern *pattern) {
  switch (pattern->getKind()) {
#define REFUTABLE_PATTERN(Id, Parent) case PatternKind::Id:
#define PATTERN(Id, Parent)
#include "swift/AST/PatternNodes.def"
    return None;
  case PatternKind::Any:
    return None;

  case PatternKind::Named: {
    if (!pattern->hasType())
      return None;

    // Special-case the various types we might see here.
    auto type = pattern->getType();

    // For literal-convertible types, form the corresponding literal.
#define CHECK_LITERAL_PROTOCOL(Kind, String) \
    if (auto proto = tc.getProtocol(SourceLoc(), KnownProtocolKind::Kind)) { \
      if (tc.conformsToProtocol(type, proto, dc, \
                                ConformanceCheckFlags::InExpression)) \
        return std::string(String); \
    }
    CHECK_LITERAL_PROTOCOL(ArrayLiteralConvertible, "[]")
    CHECK_LITERAL_PROTOCOL(DictionaryLiteralConvertible, "[:]")
    CHECK_LITERAL_PROTOCOL(UnicodeScalarLiteralConvertible, "\"\"")
    CHECK_LITERAL_PROTOCOL(ExtendedGraphemeClusterLiteralConvertible, "\"\"")
    CHECK_LITERAL_PROTOCOL(FloatLiteralConvertible, "0.0")
    CHECK_LITERAL_PROTOCOL(IntegerLiteralConvertible, "0")
    CHECK_LITERAL_PROTOCOL(StringLiteralConvertible, "\"\"")
#undef CHECK_LITERAL_PROTOCOL

    // For optional types, use 'nil'.
    if (type->getAnyOptionalObjectType())
      return std::string("nil");

    return None;
  }

  case PatternKind::Paren: {
    if (auto sub = buildDefaultInitializerString(
                     tc, dc, cast<ParenPattern>(pattern)->getSubPattern())) {
      return "(" + *sub + ")";
    }

    return None;
  }

  case PatternKind::Tuple: {
    std::string result = "(";
    bool first = true;
    for (auto elt : cast<TuplePattern>(pattern)->getElements()) {
      if (auto sub = buildDefaultInitializerString(tc, dc, elt.getPattern())) {
        if (first) {
          first = false;
        } else {
          result += ", ";
        }

        result += *sub;
      } else {
        return None;
      }
    }
    result += ")";
    return result;
  }

  case PatternKind::Typed:
    return buildDefaultInitializerString(
             tc, dc, cast<TypedPattern>(pattern)->getSubPattern());

  case PatternKind::Var:
    return buildDefaultInitializerString(
             tc, dc, cast<VarPattern>(pattern)->getSubPattern());
  }
}

/// Diagnose a class that does not have any initializers.
static void diagnoseClassWithoutInitializers(TypeChecker &tc,
                                             ClassDecl *classDecl) {
  tc.diagnose(classDecl, diag::class_without_init,
              classDecl->getDeclaredType());

  for (auto member : classDecl->getMembers()) {
    auto pbd = dyn_cast<PatternBindingDecl>(member);
    if (!pbd)
      continue;

    if (pbd->isStatic() || !pbd->hasStorage() || isDefaultInitializable(pbd) ||
        pbd->isInvalid())
      continue;
   
    for (auto entry : pbd->getPatternList()) {
      if (entry.getInit()) continue;
      
      SmallVector<VarDecl *, 4> vars;
      entry.getPattern()->collectVariables(vars);
      if (vars.empty()) continue;

      auto varLoc = vars[0]->getLoc();
      
      Optional<InFlightDiagnostic> diag;
      switch (vars.size()) {
      case 1:
        diag.emplace(tc.diagnose(varLoc, diag::note_no_in_class_init_1,
                                 vars[0]->getName()));
        break;
      case 2:
        diag.emplace(tc.diagnose(varLoc, diag::note_no_in_class_init_2,
                                 vars[0]->getName(), vars[1]->getName()));
        break;
      case 3:
        diag.emplace(tc.diagnose(varLoc, diag::note_no_in_class_init_3plus,
                                 vars[0]->getName(), vars[1]->getName(), 
                                 vars[2]->getName(), false));
        break;
      default:
        diag.emplace(tc.diagnose(varLoc, diag::note_no_in_class_init_3plus,
                                 vars[0]->getName(), vars[1]->getName(), 
                                 vars[2]->getName(), true));
        break;
      }

      if (auto defaultValueSuggestion
             = buildDefaultInitializerString(tc, classDecl, entry.getPattern()))
        diag->fixItInsertAfter(entry.getPattern()->getEndLoc(),
                               " = " + *defaultValueSuggestion);
    }
  }
}

namespace {
  /// AST stream printer that adds extra indentation to each line.
  class ExtraIndentStreamPrinter : public StreamPrinter {
    StringRef ExtraIndent;

  public:
    ExtraIndentStreamPrinter(raw_ostream &out, StringRef extraIndent)
    : StreamPrinter(out), ExtraIndent(extraIndent) { }

    virtual void printIndent() {
      printText(ExtraIndent);
      StreamPrinter::printIndent();
    }
  };
}

/// Diagnose a missing required initializer.
static void diagnoseMissingRequiredInitializer(
              TypeChecker &TC,
              ClassDecl *classDecl,
              ConstructorDecl *superInitializer) {
  // Find the location at which we should insert the new initializer.
  SourceLoc insertionLoc;
  SourceLoc indentationLoc;
  for (auto member : classDecl->getMembers()) {
    // If we don't have an indentation location yet, grab one from this
    // member.
    if (indentationLoc.isInvalid()) {
      indentationLoc = member->getLoc();
    }

    // We only want to look at explicit constructors.
    auto ctor = dyn_cast<ConstructorDecl>(member);
    if (!ctor)
      continue;

    if (ctor->isImplicit())
      continue;

    insertionLoc = ctor->getEndLoc();
    indentationLoc = ctor->getLoc();
  }

  // If no initializers were listed, start at the opening '{' for the class.
  if (insertionLoc.isInvalid()) {
    insertionLoc = classDecl->getBraces().Start;
  }
  if (indentationLoc.isInvalid()) {
    indentationLoc = classDecl->getBraces().End;
  }

  // Adjust the insertion location to point at the end of this line (i.e.,
  // the start of the next line).
  insertionLoc = Lexer::getLocForEndOfLine(TC.Context.SourceMgr,
                                           insertionLoc);

  // Find the indentation used on the indentation line.
  StringRef indentation = Lexer::getIndentationForLine(TC.Context.SourceMgr,
                                                       indentationLoc);

  // Pretty-print the superclass initializer into a string.
  // FIXME: Form a new initializer by performing the appropriate
  // substitutions of subclass types into the superclass types, so that
  // we get the right generic parameters.
  std::string initializerText;
  {
    PrintOptions options;
    options.PrintDefaultParameterPlaceholder = false;
    options.PrintImplicitAttrs = false;

    // Render the text.
    llvm::raw_string_ostream out(initializerText);
    {
      ExtraIndentStreamPrinter printer(out, indentation);
      printer.printNewline();

      // If there is no explicit 'required', print one.
      bool hasExplicitRequiredAttr = false;
      if (auto requiredAttr
            = superInitializer->getAttrs().getAttribute<RequiredAttr>())
          hasExplicitRequiredAttr = !requiredAttr->isImplicit();

      if (!hasExplicitRequiredAttr)
        printer << "required ";

      superInitializer->print(printer, options);
    }

    // FIXME: Infer body indentation from the source rather than hard-coding
    // 4 spaces.

    // Add a dummy body.
    out << " {\n";
    out << indentation << "    fatalError(\"";
    superInitializer->getFullName().printPretty(out);
    out << " has not been implemented\")\n";
    out << indentation << "}\n";
  }

  // Complain.
  TC.diagnose(insertionLoc, diag::required_initializer_missing,
              superInitializer->getFullName(),
              superInitializer->getDeclContext()->getDeclaredTypeOfContext())
    .fixItInsert(insertionLoc, initializerText);

  TC.diagnose(findNonImplicitRequiredInit(superInitializer),
              diag::required_initializer_here);
}

void TypeChecker::addImplicitConstructors(NominalTypeDecl *decl) {
  // We can only synthesize implicit constructors for classes and structs.
 if (!isa<ClassDecl>(decl) && !isa<StructDecl>(decl))
   return;

  // If we already added implicit initializers, we're done.
  if (decl->addedImplicitInitializers())
    return;
  
  // Don't add implicit constructors for an invalid declaration
  if (decl->isInvalid())
    return;

  // Local function that produces the canonical parameter type of the given
  // initializer.
  // FIXME: Doesn't work properly for generics.
  auto getInitializerParamType = [](ConstructorDecl *ctor) -> CanType {
    auto interfaceTy = ctor->getInterfaceType();

    // Skip the 'self' parameter.
    auto uncurriedInitTy = interfaceTy->castTo<AnyFunctionType>()->getResult();

    // Grab the parameter type;
    auto paramTy = uncurriedInitTy->castTo<AnyFunctionType>()->getInput();

    return paramTy->getCanonicalType();
  };

  // Check whether there is a user-declared constructor or an instance
  // variable.
  bool FoundMemberwiseInitializedProperty = false;
  bool SuppressDefaultInitializer = false;
  bool FoundDesignatedInit = false;
  decl->setAddedImplicitInitializers();
  SmallPtrSet<CanType, 4> initializerParamTypes;
  llvm::SmallPtrSet<ConstructorDecl *, 4> overriddenInits;
  for (auto member : decl->getMembers()) {
    if (auto ctor = dyn_cast<ConstructorDecl>(member)) {
      validateDecl(ctor);

      if (ctor->isDesignatedInit())
        FoundDesignatedInit = true;

      if (!ctor->isInvalid())
        initializerParamTypes.insert(getInitializerParamType(ctor));

      if (auto overridden = ctor->getOverriddenDecl())
        overriddenInits.insert(overridden);

      continue;
    }

    if (auto var = dyn_cast<VarDecl>(member)) {
      if (var->hasStorage() && !var->isStatic() && !var->isInvalid()) {
        // Initialized 'let' properties have storage, but don't get an argument
        // to the memberwise initializer since they already have an initial
        // value that cannot be overridden.
        if (var->isLet() && var->getParentInitializer()) {
          
          // We cannot handle properties like:
          //   let (a,b) = (1,2)
          // for now, just disable implicit init synthesization in structs in
          // this case.
          auto SP = var->getParentPattern();
          if (auto *TP = dyn_cast<TypedPattern>(SP))
            SP = TP->getSubPattern();
          if (!isa<NamedPattern>(SP) && isa<StructDecl>(decl))
            return;
          
          continue;
        }
        
        FoundMemberwiseInitializedProperty = true;
      }
      continue;
    }

    // If a stored property lacks an initial value and if there is no way to
    // synthesize an initial value (e.g. for an optional) then we suppress
    // generation of the default initializer.
    if (auto pbd = dyn_cast<PatternBindingDecl>(member)) {
      if (pbd->hasStorage() && !pbd->isStatic() && !pbd->isImplicit())
        for (auto entry : pbd->getPatternList()) {
          if (entry.getInit()) continue;

          // If one of the bound variables is @NSManaged, go ahead no matter
          // what.
          bool CheckDefaultInitializer = true;
          entry.getPattern()->forEachVariable([&](VarDecl *vd) {
            if (vd->getAttrs().hasAttribute<NSManagedAttr>())
              CheckDefaultInitializer = false;
          });
          
          // If we cannot default initialize the property, we cannot
          // synthesize a default initializer for the class.
          if (CheckDefaultInitializer && !isDefaultInitializable(pbd))
            SuppressDefaultInitializer = true;
        }
      continue;
    }
  }

  if (auto structDecl = dyn_cast<StructDecl>(decl)) {
    if (!FoundDesignatedInit && !structDecl->hasUnreferenceableStorage()) {
      // For a struct with memberwise initialized properties, we add a
      // memberwise init.
      if (FoundMemberwiseInitializedProperty) {
        // Create the implicit memberwise constructor.
        auto ctor = createImplicitConstructor(
                      *this, decl, ImplicitConstructorKind::Memberwise);
        decl->addMember(ctor);
      }

      // If we found a stored property, add a default constructor.
      if (!SuppressDefaultInitializer)
        defineDefaultConstructor(decl);
    }
    return;
  }
 
  // For a class with a superclass, automatically define overrides
  // for all of the superclass's designated initializers.
  // FIXME: Currently skipping generic classes.
  auto classDecl = cast<ClassDecl>(decl);
  assert(!classDecl->hasSuperclass() ||
         classDecl->getSuperclass()->getAnyNominal()->isInvalid() ||
         classDecl->getSuperclass()->getAnyNominal()
           ->addedImplicitInitializers());
  if (classDecl->hasSuperclass() && !classDecl->isGenericContext() &&
      !classDecl->getSuperclass()->isSpecialized()) {
    bool canInheritInitializers = !FoundDesignatedInit;

    // We can't define these overrides if we have any uninitialized
    // stored properties.
    if (SuppressDefaultInitializer && !FoundDesignatedInit) {
      diagnoseClassWithoutInitializers(*this, classDecl);
      return;
    }

    auto superclassTy = classDecl->getSuperclass();
    for (auto memberResult : lookupConstructors(classDecl, superclassTy)) {
      auto member = memberResult.Decl;

      // Skip unavailable superclass initializers.
      if (AvailableAttr::isUnavailable(member))
        continue;

      // Skip invalid superclass initializers.
      auto superclassCtor = dyn_cast<ConstructorDecl>(member);
      if (superclassCtor->isInvalid())
        continue;

      // We only care about required or designated initializers.
      if (!superclassCtor->isRequired() &&
          !superclassCtor->isDesignatedInit())
        continue;

      // If we have an override for this constructor, it's okay.
      if (overriddenInits.count(superclassCtor) > 0)
        continue;

      // If the superclass constructor is a convenience initializer
      // that is inherited into the current class, it's okay.
      if (superclassCtor->isInheritable() &&
          classDecl->inheritsSuperclassInitializers(this)) {
        assert(superclassCtor->isRequired());
        continue;
      }

      // Diagnose a missing override of a required initializer.
      if (superclassCtor->isRequired() && FoundDesignatedInit) {
        diagnoseMissingRequiredInitializer(*this, classDecl, superclassCtor);
        continue;
      }

      // A designated or required initializer has not been overridden.

      // Skip this designated initializer if it's in an extension.
      // FIXME: We shouldn't allow this.
      if (isa<ExtensionDecl>(superclassCtor->getDeclContext()))
        continue;

      // If we have already introduced an initializer with this parameter type,
      // don't add one now.
      if (!initializerParamTypes.insert(
             getInitializerParamType(superclassCtor)).second)
        continue;

      // We have a designated initializer. Create an override of it.
      if (auto ctor = createDesignatedInitOverride(
                        *this, classDecl, superclassCtor,
                        canInheritInitializers
                          ? DesignatedInitKind::Chaining
                          : DesignatedInitKind::Stub)) {
        classDecl->addMember(ctor);
      }
    }

    return;
  }

  if (!FoundDesignatedInit) {
    // For a class with no superclass, automatically define a default
    // constructor.

    // ... unless there are uninitialized stored properties.
    if (SuppressDefaultInitializer) {
      diagnoseClassWithoutInitializers(*this, classDecl);
      return;
    }

    defineDefaultConstructor(decl);
  }
}

void TypeChecker::addImplicitStructConformances(StructDecl *SD) {
  // Type-check the protocol conformances of the struct decl to instantiate its
  // derived conformances.
  DeclChecker(*this, false, false)
    .checkExplicitConformance(SD, SD->getDeclaredTypeInContext());
}

void TypeChecker::addImplicitEnumConformances(EnumDecl *ED) {
  // Type-check the raw values of the enum.
  for (auto elt : ED->getAllElements()) {
    assert(elt->hasRawValueExpr());
    if (elt->getTypeCheckedRawValueExpr()) continue;
    Expr *typeChecked = elt->getRawValueExpr();
    Type rawTy = ArchetypeBuilder::mapTypeIntoContext(ED, ED->getRawType());
    bool error = typeCheckExpression(typeChecked, ED, rawTy,
                                     CTP_EnumCaseRawValue);
    assert(!error); (void)error;
    elt->setTypeCheckedRawValueExpr(typeChecked);
    checkEnumElementErrorHandling(elt);
  }
  
  // Type-check the protocol conformances of the enum decl to instantiate its
  // derived conformances.
  DeclChecker(*this, false, false)
    .checkExplicitConformance(ED, ED->getDeclaredTypeInContext());
}

void TypeChecker::defineDefaultConstructor(NominalTypeDecl *decl) {
  PrettyStackTraceDecl stackTrace("defining default constructor for",
                                  decl);

  // Clang-imported types should never get a default constructor, just a
  // memberwise one.
  if (decl->hasClangNode())
    return;

  // For a class, check whether the superclass (if it exists) is
  // default-initializable.
  if (isa<ClassDecl>(decl)) {
    // We need to look for a default constructor.
    if (auto superTy = getSuperClassOf(decl->getDeclaredTypeInContext())) {
      // If there are no default ctors for our supertype, we can't do anything.
      auto ctors = lookupConstructors(decl, superTy);
      if (!ctors)
        return;

      // Check whether we have a constructor that can be called with an empty
      // tuple.
      bool foundDefaultConstructor = false;
      for (auto memberResult : ctors) {
        auto member = memberResult.Decl;

        // Dig out the parameter tuple for this constructor.
        auto ctor = dyn_cast<ConstructorDecl>(member);
        if (!ctor || ctor->isInvalid())
          continue;

        // Check to see if this ctor has zero arguments, or if they all have
        // default values.
        auto params = ctor->getParameters();
        
        bool missingInit = false;
        for (auto param : *params) {
          if (!param->isDefaultArgument()) {
            missingInit = true;
            break;
          }
        }

        // Check to see if this is an impossible candidate.
        if (missingInit) {
          // If we found an impossible designated initializer, then we cannot
          // call super.init(), even if there is a match.
          if (ctor->isDesignatedInit())
            return;

          // Otherwise, keep looking.
          continue;
        }

        // Ok, we found a constructor that can be invoked with an empty tuple.
        // If this is our second, then we bail out, because we don't want to
        // pick one arbitrarily.
        if (foundDefaultConstructor)
          return;

        foundDefaultConstructor = true;
      }

      // If our superclass isn't default constructible, we aren't either.
      if (!foundDefaultConstructor) return;
    }
  }

  // Create the default constructor.
  auto ctor = createImplicitConstructor(*this, decl,
                                        ImplicitConstructorKind::Default);

  // Add the constructor.
  decl->addMember(ctor);

  // Create an empty body for the default constructor. The type-check of the
  // constructor body will introduce default initializations of the members.
  ctor->setBody(BraceStmt::create(Context, SourceLoc(), { }, SourceLoc()));
}

static void validateAttributes(TypeChecker &TC, Decl *D) {
  DeclAttributes &Attrs = D->getAttrs();

  auto checkObjCDeclContext = [](Decl *D) {
    DeclContext *DC = D->getDeclContext();
    if (DC->isClassOrClassExtensionContext())
      return true;
    if (auto *PD = dyn_cast<ProtocolDecl>(DC))
      if (PD->isObjC())
        return true;
    return false;
  };

  if (auto objcAttr = Attrs.getAttribute<ObjCAttr>()) {
    // Only certain decls can be ObjC.
    Optional<Diag<>> error;
    if (isa<ClassDecl>(D) ||
        isa<ProtocolDecl>(D)) {
      /* ok */
    } else if (auto ED = dyn_cast<EnumDecl>(D)) {
      if (ED->isGenericContext())
        error = diag::objc_enum_generic;
    } else if (auto EED = dyn_cast<EnumElementDecl>(D)) {
      auto ED = EED->getParentEnum();
      if (!ED->getAttrs().hasAttribute<ObjCAttr>())
        error = diag::objc_enum_case_req_objc_enum;
      else if (objcAttr->hasName() && EED->getParentCase()->getElements().size() > 1)
        error = diag::objc_enum_case_multi;
    } else if (isa<FuncDecl>(D)) {
      auto func = cast<FuncDecl>(D);
      if (!checkObjCDeclContext(D))
        error = diag::invalid_objc_decl_context;
      else if (func->isOperator())
        error = diag::invalid_objc_decl;
      else if (func->isAccessor() && !func->isGetterOrSetter())
        error = diag::objc_observing_accessor;
    } else if (isa<ConstructorDecl>(D) ||
               isa<DestructorDecl>(D) ||
               isa<SubscriptDecl>(D) ||
               isa<VarDecl>(D)) {
      if (!checkObjCDeclContext(D))
        error = diag::invalid_objc_decl_context;
      /* ok */
    } else {
      error = diag::invalid_objc_decl;
    }

    if (error) {
      TC.diagnose(D->getStartLoc(), *error)
        .fixItRemove(objcAttr->getRangeWithAt());
      objcAttr->setInvalid();
      return;
    }

    // If there is a name, check whether the kind of name is
    // appropriate.
    if (auto objcName = objcAttr->getName()) {
      if (isa<ClassDecl>(D) || isa<ProtocolDecl>(D) || isa<VarDecl>(D)
          || isa<EnumDecl>(D) || isa<EnumElementDecl>(D)) {
        // Types and properties can only have nullary
        // names. Complain and recover by chopping off everything
        // after the first name.
        if (objcName->getNumArgs() > 0) {
          int which = isa<ClassDecl>(D)? 0
                    : isa<ProtocolDecl>(D)? 1
                    : isa<EnumDecl>(D)? 2
                    : isa<EnumElementDecl>(D)? 3
                    : 4;
          SourceLoc firstNameLoc = objcAttr->getNameLocs().front();
          SourceLoc afterFirstNameLoc = 
            Lexer::getLocForEndOfToken(TC.Context.SourceMgr, firstNameLoc);
          TC.diagnose(firstNameLoc, diag::objc_name_req_nullary, which)
            .fixItRemoveChars(afterFirstNameLoc, objcAttr->getRParenLoc());
          const_cast<ObjCAttr *>(objcAttr)->setName(
            ObjCSelector(TC.Context, 0, objcName->getSelectorPieces()[0]),
            /*implicit=*/false);
        }
      } else if (isa<SubscriptDecl>(D)) {
        // Subscripts can never have names.
        TC.diagnose(objcAttr->getLParenLoc(), diag::objc_name_subscript);
        const_cast<ObjCAttr *>(objcAttr)->clearName();
      } else {
        // We have a function. Make sure that the number of parameters
        // matches the "number of colons" in the name.
        auto func = cast<AbstractFunctionDecl>(D);
        auto params = func->getParameterList(1);
        unsigned numParameters = params->size();
        if (auto CD = dyn_cast<ConstructorDecl>(func))
          if (CD->isObjCZeroParameterWithLongSelector())
            numParameters = 0;  // Something like "init(foo: ())"

        // A throwing method has an error parameter.
        if (func->isBodyThrowing())
          ++numParameters;

        unsigned numArgumentNames = objcName->getNumArgs();
        if (numArgumentNames != numParameters) {
          TC.diagnose(objcAttr->getNameLocs().front(), 
                      diag::objc_name_func_mismatch,
                      isa<FuncDecl>(func), 
                      numArgumentNames, 
                      numArgumentNames != 1,
                      numParameters,
                      numParameters != 1,
                      func->isBodyThrowing());
          D->getAttrs().add(
            ObjCAttr::createUnnamed(TC.Context,
                                    objcAttr->AtLoc,
                                    objcAttr->Range.Start));
          D->getAttrs().removeAttribute(objcAttr);
        }
      }
    } else if (isa<EnumElementDecl>(D)) {
      // Enum elements require names.
      TC.diagnose(objcAttr->getLocation(), diag::objc_enum_case_req_name)
        .fixItRemove(objcAttr->getRangeWithAt());
      objcAttr->setInvalid();
    }
  }

  if (auto nonObjcAttr = Attrs.getAttribute<NonObjCAttr>()) {
    // Only methods, properties, subscripts and constructors can be NonObjC.
    // The last three are handled automatically by generic attribute
    // validation -- for the first one, we have to check FuncDecls
    // ourselves.
    Optional<Diag<>> error;

    auto func = dyn_cast<FuncDecl>(D);
    if (func &&
        (isa<DestructorDecl>(func) ||
         !checkObjCDeclContext(func) ||
         (func->isAccessor() && !func->isGetterOrSetter()))) {
      error = diag::invalid_nonobjc_decl;
    }

    if (error) {
      TC.diagnose(D->getStartLoc(), *error)
        .fixItRemove(nonObjcAttr->getRangeWithAt());
      nonObjcAttr->setInvalid();
      return;
    }
  }

  // Only protocol members can be optional.
  if (auto *OA = Attrs.getAttribute<OptionalAttr>()) {
    if (!isa<ProtocolDecl>(D->getDeclContext())) {
      TC.diagnose(OA->getLocation(),
                  diag::optional_attribute_non_protocol);
      D->getAttrs().removeAttribute(OA);
    } else if (!cast<ProtocolDecl>(D->getDeclContext())->isObjC()) {
      TC.diagnose(OA->getLocation(),
                  diag::optional_attribute_non_objc_protocol);
      D->getAttrs().removeAttribute(OA);
    } else if (isa<ConstructorDecl>(D)) {
      TC.diagnose(OA->getLocation(),
                  diag::optional_attribute_initializer);
      D->getAttrs().removeAttribute(OA);
    }
  }

  // Only protocols that are @objc can have "unavailable" methods.
  if (auto AvAttr = Attrs.getUnavailable(TC.Context)) {
    if (auto PD = dyn_cast<ProtocolDecl>(D->getDeclContext())) {
      if (!PD->isObjC()) {
        TC.diagnose(AvAttr->getLocation(),
                    diag::unavailable_method_non_objc_protocol);
        D->getAttrs().removeAttribute(AvAttr);
      }
    }
  }
}

/// Fix the names in the given function to match those in the given target
/// name by adding Fix-Its to the provided in-flight diagnostic.
void TypeChecker::fixAbstractFunctionNames(InFlightDiagnostic &diag,
                                           AbstractFunctionDecl *func,
                                           DeclName targetName) {
  // There is no reasonable way to fix an implicitly-generated function.
  if (func->isImplicit())
    return;

  auto name = func->getFullName();
  
  // Fix the name of the function itself.
  if (name.getBaseName() != targetName.getBaseName()) {
    diag.fixItReplace(func->getLoc(), targetName.getBaseName().str());
  }
  
  // Fix the argument names that need fixing.
  assert(name.getArgumentNames().size()
           == targetName.getArgumentNames().size());
  auto params = func->getParameterList(func->getDeclContext()->isTypeContext());
  for (unsigned i = 0, n = name.getArgumentNames().size(); i != n; ++i) {
    auto origArg = name.getArgumentNames()[i];
    auto targetArg = targetName.getArgumentNames()[i];
    
    if (origArg == targetArg)
      continue;
    
    auto *param = params->get(i);
    
    // The parameter has an explicitly-specified API name, and it's wrong.
    if (param->getArgumentNameLoc() != param->getLoc() &&
        param->getArgumentNameLoc().isValid()) {
      // ... but the internal parameter name was right. Just zap the
      // incorrect explicit specialization.
      if (param->getName() == targetArg) {
        diag.fixItRemoveChars(param->getArgumentNameLoc(),
                              param->getLoc());
        continue;
      }
      
      // Fix the API name.
      StringRef targetArgStr = targetArg.empty()? "_" : targetArg.str();
      diag.fixItReplace(param->getArgumentNameLoc(), targetArgStr);
      continue;
    }
    
    // The parameter did not specify a separate API name. Insert one.
    if (targetArg.empty())
      diag.fixItInsert(param->getLoc(), "_ ");
    else {
      llvm::SmallString<8> targetArgStr;
      targetArgStr += targetArg.str();
      targetArgStr += ' ';
      diag.fixItInsert(param->getLoc(), targetArgStr);
    }
  }
  
  // FIXME: Update the AST accordingly.
}
