//===--- SILGenConstructor.cpp - SILGen for constructors ------------------===//
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

#include "SILGenFunction.h"
#include "ArgumentSource.h"
#include "Initialization.h"
#include "LValue.h"
#include "RValue.h"
#include "Scope.h"
#include "swift/AST/AST.h"
#include "swift/AST/GenericEnvironment.h"
#include "swift/AST/Mangle.h"
#include "swift/SIL/SILArgument.h"
#include "swift/SIL/SILUndef.h"
#include "swift/SIL/TypeLowering.h"
#include "swift/Basic/Defer.h"

using namespace swift;
using namespace Lowering;
using namespace Mangle;

static SILValue emitConstructorMetatypeArg(SILGenFunction &gen,
                                           ValueDecl *ctor) {
  // In addition to the declared arguments, the constructor implicitly takes
  // the metatype as its first argument, like a static function.
  Type metatype = ctor->getType()->castTo<AnyFunctionType>()->getInput();
  auto &AC = gen.getASTContext();
  auto VD = new (AC) ParamDecl(/*IsLet*/ true, SourceLoc(), SourceLoc(),
                               AC.getIdentifier("$metatype"), SourceLoc(),
                               AC.getIdentifier("$metatype"), metatype,
                               ctor->getDeclContext());
  gen.AllocatorMetatype = new (gen.F.getModule()) SILArgument(gen.F.begin(),
                                              gen.getLoweredType(metatype), VD);
  return gen.AllocatorMetatype;
}

static RValue emitImplicitValueConstructorArg(SILGenFunction &gen,
                                              SILLocation loc,
                                              CanType ty,
                                              DeclContext *DC) {
  // Restructure tuple arguments.
  if (CanTupleType tupleTy = dyn_cast<TupleType>(ty)) {
    RValue tuple(ty);
    for (auto fieldType : tupleTy.getElementTypes())
      tuple.addElement(emitImplicitValueConstructorArg(gen, loc, fieldType, DC));

    return tuple;
  } else {
    auto &AC = gen.getASTContext();
    auto VD = new (AC) ParamDecl(/*IsLet*/ true, SourceLoc(), SourceLoc(),
                                 AC.getIdentifier("$implicit_value"),
                                 SourceLoc(),
                                 AC.getIdentifier("$implicit_value"), ty, DC);
    SILValue arg = new (gen.F.getModule()) SILArgument(gen.F.begin(),
                                                       gen.getLoweredType(ty),
                                                       VD);
    return RValue(gen, loc, ty, gen.emitManagedRValueWithCleanup(arg));
  }
}

static void emitImplicitValueConstructor(SILGenFunction &gen,
                                         ConstructorDecl *ctor) {
  RegularLocation Loc(ctor);
  Loc.markAutoGenerated();
  // FIXME: Handle 'self' along with the other arguments.
  auto *paramList = ctor->getParameterList(1);
  auto selfTyCan = ctor->getImplicitSelfDecl()->getType()->getInOutObjectType();
  SILType selfTy = gen.getLoweredType(selfTyCan);

  // Emit the indirect return argument, if any.
  SILValue resultSlot;
  if (selfTy.isAddressOnly(gen.SGM.M)) {
    auto &AC = gen.getASTContext();
    auto VD = new (AC) ParamDecl(/*IsLet*/ false, SourceLoc(), SourceLoc(),
                                 AC.getIdentifier("$return_value"),
                                 SourceLoc(),
                                 AC.getIdentifier("$return_value"), selfTyCan,
                                 ctor);
    resultSlot = new (gen.F.getModule()) SILArgument(gen.F.begin(), selfTy, VD);
  }

  // Emit the elementwise arguments.
  SmallVector<RValue, 4> elements;
  for (size_t i = 0, size = paramList->size(); i < size; ++i) {
    auto &param = paramList->get(i);

    elements.push_back(
      emitImplicitValueConstructorArg(gen, Loc,
                                      param->getType()->getCanonicalType(),
                                      ctor));
  }

  emitConstructorMetatypeArg(gen, ctor);

  auto *decl = selfTy.getStructOrBoundGenericStruct();
  assert(decl && "not a struct?!");

  // If we have an indirect return slot, initialize it in-place.
  if (resultSlot) {

    auto elti = elements.begin(), eltEnd = elements.end();
    for (VarDecl *field : decl->getStoredProperties()) {
      auto fieldTy = selfTy.getFieldType(field, gen.SGM.M);
      auto &fieldTL = gen.getTypeLowering(fieldTy);
      SILValue slot = gen.B.createStructElementAddr(Loc, resultSlot, field,
                                                    fieldTL.getLoweredType().getAddressType());
      InitializationPtr init(new KnownAddressInitialization(slot));

      // An initialized 'let' property has a single value specified by the
      // initializer - it doesn't come from an argument.
      if (!field->isStatic() && field->isLet() &&
          field->getParentInitializer()) {
        assert(field->getType()->isEqual(field->getParentInitializer()
                                         ->getType()) && "Checked by sema");

        // Cleanup after this initialization.
        FullExpr scope(gen.Cleanups, field->getParentPatternBinding());
        gen.emitExprInto(field->getParentInitializer(), init.get());
        continue;
      }

      assert(elti != eltEnd && "number of args does not match number of fields");
      (void)eltEnd;
      std::move(*elti).forwardInto(gen, Loc, init.get());
      ++elti;
    }
    gen.B.createReturn(ImplicitReturnLocation::getImplicitReturnLoc(Loc),
                       gen.emitEmptyTuple(Loc));
    return;
  }

  // Otherwise, build a struct value directly from the elements.
  SmallVector<SILValue, 4> eltValues;

  auto elti = elements.begin(), eltEnd = elements.end();
  for (VarDecl *field : decl->getStoredProperties()) {
    auto fieldTy = selfTy.getFieldType(field, gen.SGM.M);
    SILValue v;

    // An initialized 'let' property has a single value specified by the
    // initializer - it doesn't come from an argument.
    if (!field->isStatic() && field->isLet() && field->getParentInitializer()) {
      // Cleanup after this initialization.
      FullExpr scope(gen.Cleanups, field->getParentPatternBinding());
      v = gen.emitRValue(field->getParentInitializer())
             .forwardAsSingleStorageValue(gen, fieldTy, Loc);
    } else {
      assert(elti != eltEnd && "number of args does not match number of fields");
      (void)eltEnd;
      v = std::move(*elti).forwardAsSingleStorageValue(gen, fieldTy, Loc);
      ++elti;
    }

    eltValues.push_back(v);
  }

  SILValue selfValue = gen.B.createStruct(Loc, selfTy, eltValues);
  gen.B.createReturn(ImplicitReturnLocation::getImplicitReturnLoc(Loc),
                     selfValue);
  return;
}

void SILGenFunction::emitValueConstructor(ConstructorDecl *ctor) {
  MagicFunctionName = SILGenModule::getMagicFunctionName(ctor);

  if (ctor->isMemberwiseInitializer())
    return emitImplicitValueConstructor(*this, ctor);

  // True if this constructor delegates to a peer constructor with self.init().
  bool isDelegating = ctor->getDelegatingOrChainedInitKind(nullptr) ==
    ConstructorDecl::BodyInitKind::Delegating;

  // Get the 'self' decl and type.
  VarDecl *selfDecl = ctor->getImplicitSelfDecl();
  auto &lowering = getTypeLowering(selfDecl->getType()->getInOutObjectType());
  SILType selfTy = lowering.getLoweredType();
  (void)selfTy;
  assert(!selfTy.getClassOrBoundGenericClass()
         && "can't emit a class ctor here");

  // Allocate the local variable for 'self'.
  emitLocalVariableWithCleanup(selfDecl, false)->finishInitialization(*this);
  
  // Mark self as being uninitialized so that DI knows where it is and how to
  // check for it.
  SILValue selfLV;
  {
    auto &SelfVarLoc = VarLocs[selfDecl];
    auto MUIKind =  isDelegating ? MarkUninitializedInst::DelegatingSelf
                                 : MarkUninitializedInst::RootSelf;
    selfLV = B.createMarkUninitialized(selfDecl, SelfVarLoc.value, MUIKind);
    SelfVarLoc.value = selfLV;
  }
  
  // Emit the prolog.
  emitProlog(ctor->getParameterList(1), ctor->getResultType(), ctor,
             ctor->hasThrows());
  emitConstructorMetatypeArg(*this, ctor);

  // Create a basic block to jump to for the implicit 'self' return.
  // We won't emit this until after we've emitted the body.
  // The epilog takes a void return because the return of 'self' is implicit.
  prepareEpilog(Type(), ctor->hasThrows(), CleanupLocation(ctor));

  // If the constructor can fail, set up an alternative epilog for constructor
  // failure.
  SILBasicBlock *failureExitBB = nullptr;
  SILArgument *failureExitArg = nullptr;
  auto &resultLowering = getTypeLowering(ctor->getResultType());

  if (ctor->getFailability() != OTK_None) {
    SILBasicBlock *failureBB = createBasicBlock(FunctionSection::Postmatter);

    // On failure, we'll clean up everything (except self, which should have
    // been cleaned up before jumping here) and return nil instead.
    SavedInsertionPoint savedIP(*this, failureBB, FunctionSection::Postmatter);
    failureExitBB = createBasicBlock();
    Cleanups.emitCleanupsForReturn(ctor);
    // Return nil.
    if (lowering.isAddressOnly()) {
      // Inject 'nil' into the indirect return.
      assert(F.getIndirectResults().size() == 1);
      B.createInjectEnumAddr(ctor, F.getIndirectResults()[0],
                             getASTContext().getOptionalNoneDecl());
      B.createBranch(ctor, failureExitBB);

      B.setInsertionPoint(failureExitBB);
      B.createReturn(ctor, emitEmptyTuple(ctor));
    } else {
      // Pass 'nil' as the return value to the exit BB.
      failureExitArg = new (F.getModule())
        SILArgument(failureExitBB, resultLowering.getLoweredType());
      SILValue nilResult =
        B.createEnum(ctor, {}, getASTContext().getOptionalNoneDecl(),
                     resultLowering.getLoweredType());
      B.createBranch(ctor, failureExitBB, nilResult);

      B.setInsertionPoint(failureExitBB);
      B.createReturn(ctor, failureExitArg);
    }

    FailDest = JumpDest(failureBB, Cleanups.getCleanupsDepth(), ctor);
  }

  // If this is not a delegating constructor, emit member initializers.
  if (!isDelegating) {
    auto *dc = ctor->getDeclContext();
    auto *nominal = dc->getAsNominalTypeOrNominalTypeExtensionContext();
    emitMemberInitializers(dc, selfDecl, nominal);
  }

  emitProfilerIncrement(ctor->getBody());
  // Emit the constructor body.
  emitStmt(ctor->getBody());

  
  // Build a custom epilog block, since the AST representation of the
  // constructor decl (which has no self in the return type) doesn't match the
  // SIL representation.
  SILValue selfValue;
  {
    SavedInsertionPoint savedIP(*this, ReturnDest.getBlock());
    assert(B.getInsertionBB()->empty() && "Epilog already set up?");
    
    auto cleanupLoc = CleanupLocation::get(ctor);
    
    if (!lowering.isAddressOnly()) {
      // Otherwise, load and return the final 'self' value.
      selfValue = lowering.emitLoad(B, cleanupLoc, selfLV,
                                    LoadOwnershipQualifier::Copy);

      // Inject the self value into an optional if the constructor is failable.
      if (ctor->getFailability() != OTK_None) {
        selfValue = B.createEnum(ctor, selfValue,
                                 getASTContext().getOptionalSomeDecl(),
                                 getLoweredLoadableType(ctor->getResultType()));
      }
    } else {
      // If 'self' is address-only, copy 'self' into the indirect return slot.
      assert(F.getIndirectResults().size() == 1 &&
             "no indirect return for address-only ctor?!");

      // Get the address to which to store the result.
      SILValue completeReturnAddress = F.getIndirectResults()[0];
      SILValue returnAddress;
      switch (ctor->getFailability()) {
      // For non-failable initializers, store to the return address directly.
      case OTK_None:
        returnAddress = completeReturnAddress;
        break;
      // If this is a failable initializer, project out the payload.
      case OTK_Optional:
      case OTK_ImplicitlyUnwrappedOptional:
        returnAddress = B.createInitEnumDataAddr(ctor, completeReturnAddress,
                                       getASTContext().getOptionalSomeDecl(),
                                                 selfLV->getType());
        break;
      }
      
      // We have to do a non-take copy because someone else may be using the
      // box (e.g. someone could have closed over it).
      B.createCopyAddr(cleanupLoc, selfLV, returnAddress,
                       IsNotTake, IsInitialization);
      
      // Inject the enum tag if the result is optional because of failability.
      if (ctor->getFailability() != OTK_None) {
        // Inject the 'Some' tag.
        B.createInjectEnumAddr(ctor, completeReturnAddress,
                               getASTContext().getOptionalSomeDecl());
      }
    }
  }
  
  // Finally, emit the epilog and post-matter.
  auto returnLoc = emitEpilog(ctor, /*UsesCustomEpilog*/true);

  // Finish off the epilog by returning.  If this is a failable ctor, then we
  // actually jump to the failure epilog to keep the invariant that there is
  // only one SIL return instruction per SIL function.
  if (B.hasValidInsertionPoint()) {
    if (!failureExitBB) {
      // If we're not returning self, then return () since we're returning Void.
      if (!selfValue) {
        SILLocation loc(ctor);
        loc.markAutoGenerated();
        selfValue = emitEmptyTuple(loc);
      }
      
      B.createReturn(returnLoc, selfValue);
    } else {
      if (selfValue)
        B.createBranch(returnLoc, failureExitBB, selfValue);
      else
        B.createBranch(returnLoc, failureExitBB);
    }
  }
}

void SILGenFunction::emitEnumConstructor(EnumElementDecl *element) {
  CanType enumTy = element->getParentEnum()
                      ->getDeclaredTypeInContext()
                      ->getCanonicalType();
  auto &enumTI = getTypeLowering(enumTy);

  RegularLocation Loc(element);
  CleanupLocation CleanupLoc(element);
  Loc.markAutoGenerated();

  // Emit the indirect return slot.
  std::unique_ptr<Initialization> dest;
  if (enumTI.isAddressOnly()) {
    auto &AC = getASTContext();
    auto VD = new (AC) ParamDecl(/*IsLet*/ false, SourceLoc(), SourceLoc(),
                                 AC.getIdentifier("$return_value"),
                                 SourceLoc(),
                                 AC.getIdentifier("$return_value"),
                                 CanInOutType::get(enumTy),
                                 element->getDeclContext());
    auto resultSlot = new (SGM.M) SILArgument(F.begin(),
                                              enumTI.getLoweredType(),
                                              VD);
    dest = std::unique_ptr<Initialization>(
        new KnownAddressInitialization(resultSlot));
  }

  Scope scope(Cleanups, CleanupLoc);

  // Emit the exploded constructor argument.
  ArgumentSource payload;
  if (element->hasArgumentType()) {
    RValue arg = emitImplicitValueConstructorArg
      (*this, Loc, element->getArgumentType()->getCanonicalType(),
       element->getDeclContext());
   payload = ArgumentSource(Loc, std::move(arg));
  }

  // Emit the metatype argument.
  emitConstructorMetatypeArg(*this, element);

  // If possible, emit the enum directly into the indirect return.
  SGFContext C = (dest ? SGFContext(dest.get()) : SGFContext());
  ManagedValue mv = emitInjectEnum(Loc, std::move(payload),
                                   enumTI.getLoweredType(),
                                   element, C);

  // Return the enum.
  auto ReturnLoc = ImplicitReturnLocation::getImplicitReturnLoc(Loc);

  if (mv.isInContext()) {
    assert(enumTI.isAddressOnly());
    scope.pop();
    B.createReturn(ReturnLoc, emitEmptyTuple(Loc));
  } else {
    assert(enumTI.isLoadable());
    SILValue result = mv.forward(*this);
    scope.pop();
    B.createReturn(ReturnLoc, result);
  }
}

bool Lowering::usesObjCAllocator(ClassDecl *theClass) {
  while (true) {
    // If the root class was implemented in Objective-C, use Objective-C's
    // allocation methods because they may have been overridden.
    if (!theClass->hasSuperclass())
      return theClass->hasClangNode();

    theClass = theClass->getSuperclass()->getClassOrBoundGenericClass();
  }
}

void SILGenFunction::emitClassConstructorAllocator(ConstructorDecl *ctor) {
  assert(!ctor->isFactoryInit() && "factories should not be emitted here");

  // Emit the prolog. Since we're just going to forward our args directly
  // to the initializer, don't allocate local variables for them.
  RegularLocation Loc(ctor);
  Loc.markAutoGenerated();

  // Forward the constructor arguments.
  // FIXME: Handle 'self' along with the other body patterns.
  SmallVector<SILValue, 8> args;
  bindParametersForForwarding(ctor->getParameterList(1), args);

  SILValue selfMetaValue = emitConstructorMetatypeArg(*this, ctor);

  // Allocate the "self" value.
  VarDecl *selfDecl = ctor->getImplicitSelfDecl();
  SILType selfTy = getLoweredType(selfDecl->getType());
  assert(selfTy.hasReferenceSemantics() &&
         "can't emit a value type ctor here");

  // Use alloc_ref to allocate the object.
  // TODO: allow custom allocation?
  // FIXME: should have a cleanup in case of exception
  auto selfClassDecl = ctor->getDeclContext()->getAsClassOrClassExtensionContext();

  SILValue selfValue;

  // Allocate the 'self' value.
  bool useObjCAllocation = usesObjCAllocator(selfClassDecl);

  if (ctor->isConvenienceInit() || ctor->hasClangNode()) {
    // For a convenience initializer or an initializer synthesized
    // for an Objective-C class, allocate using the metatype.
    SILValue allocArg = selfMetaValue;

    // When using Objective-C allocation, convert the metatype
    // argument to an Objective-C metatype.
    if (useObjCAllocation) {
      auto metaTy = allocArg->getType().castTo<MetatypeType>();
      metaTy = CanMetatypeType::get(metaTy.getInstanceType(),
                                    MetatypeRepresentation::ObjC);
      allocArg = B.createThickToObjCMetatype(Loc, allocArg,
                                             getLoweredType(metaTy));
    }

    selfValue = B.createAllocRefDynamic(Loc, allocArg, selfTy,
                                        useObjCAllocation, {}, {});
  } else {
    // For a designated initializer, we know that the static type being
    // allocated is the type of the class that defines the designated
    // initializer.
    selfValue = B.createAllocRef(Loc, selfTy, useObjCAllocation, false, {}, {});
  }
  args.push_back(selfValue);

  // Call the initializer. Always use the Swift entry point, which will be a
  // bridging thunk if we're calling ObjC.
  SILDeclRef initConstant =
    SILDeclRef(ctor,
               SILDeclRef::Kind::Initializer,
               SILDeclRef::ConstructAtBestResilienceExpansion,
               SILDeclRef::ConstructAtNaturalUncurryLevel,
               /*isObjC=*/false);

  ManagedValue initVal;
  SILType initTy;

  ArrayRef<Substitution> subs;
  // Call the initializer.
  ArrayRef<Substitution> forwardingSubs;
  if (auto *genericEnv = ctor->getGenericEnvironmentOfContext()) {
    forwardingSubs = genericEnv->getForwardingSubstitutions(
        SGM.SwiftModule);
  }
  std::tie(initVal, initTy, subs)
    = emitSiblingMethodRef(Loc, selfValue, initConstant, forwardingSubs);

  SILValue initedSelfValue = emitApplyWithRethrow(Loc, initVal.forward(*this),
                                                  initTy, subs, args);

  // Return the initialized 'self'.
  B.createReturn(ImplicitReturnLocation::getImplicitReturnLoc(Loc),
                 initedSelfValue);
}

void SILGenFunction::emitClassConstructorInitializer(ConstructorDecl *ctor) {
  MagicFunctionName = SILGenModule::getMagicFunctionName(ctor);

  assert(ctor->getBody() && "Class constructor without a body?");

  // True if this constructor delegates to a peer constructor with self.init().
  bool isDelegating = false;
  if (!ctor->hasStubImplementation()) {
    isDelegating = ctor->getDelegatingOrChainedInitKind(nullptr) ==
      ConstructorDecl::BodyInitKind::Delegating;
  }

  // Set up the 'self' argument.  If this class has a superclass, we set up
  // self as a box.  This allows "self reassignment" to happen in super init
  // method chains, which is important for interoperating with Objective-C
  // classes.  We also use a box for delegating constructors, since the
  // delegated-to initializer may also replace self.
  //
  // TODO: If we could require Objective-C classes to have an attribute to get
  // this behavior, we could avoid runtime overhead here.
  VarDecl *selfDecl = ctor->getImplicitSelfDecl();
  auto *dc = ctor->getDeclContext();
  auto selfClassDecl = dc->getAsClassOrClassExtensionContext();
  bool NeedsBoxForSelf = isDelegating ||
    (selfClassDecl->hasSuperclass() && !ctor->hasStubImplementation());
  bool usesObjCAllocator = Lowering::usesObjCAllocator(selfClassDecl);

  // If needed, mark 'self' as uninitialized so that DI knows to
  // enforce its DI properties on stored properties.
  MarkUninitializedInst::Kind MUKind;

  if (isDelegating)
    MUKind = MarkUninitializedInst::DelegatingSelf;
  else if (selfClassDecl->requiresStoredPropertyInits() &&
           usesObjCAllocator) {
    // Stored properties will be initialized in a separate
    // .cxx_construct method called by the Objective-C runtime.
    assert(selfClassDecl->hasSuperclass() &&
           "Cannot use ObjC allocation without a superclass");
    MUKind = MarkUninitializedInst::DerivedSelfOnly;
  } else if (selfClassDecl->hasSuperclass())
    MUKind = MarkUninitializedInst::DerivedSelf;
  else
    MUKind = MarkUninitializedInst::RootSelf;

  if (NeedsBoxForSelf) {
    // Allocate the local variable for 'self'.
    emitLocalVariableWithCleanup(selfDecl, false)->finishInitialization(*this);

    auto &SelfVarLoc = VarLocs[selfDecl];
    SelfVarLoc.value = B.createMarkUninitialized(selfDecl,
                                                 SelfVarLoc.value, MUKind);
  }

  // Emit the prolog for the non-self arguments.
  // FIXME: Handle self along with the other body patterns.
  emitProlog(ctor->getParameterList(1),
             TupleType::getEmpty(F.getASTContext()), ctor, ctor->hasThrows());

  SILType selfTy = getLoweredLoadableType(selfDecl->getType());
  SILValue selfArg = new (SGM.M) SILArgument(F.begin(), selfTy, selfDecl);

  if (!NeedsBoxForSelf) {
    SILLocation PrologueLoc(selfDecl);
    PrologueLoc.markAsPrologue();
    B.createDebugValue(PrologueLoc, selfArg);
  }

  if (!ctor->hasStubImplementation()) {
    assert(selfTy.hasReferenceSemantics() &&
           "can't emit a value type ctor here");
    if (NeedsBoxForSelf) {
      SILLocation prologueLoc = RegularLocation(ctor);
      prologueLoc.markAsPrologue();
      // SEMANTIC ARC TODO: When the verifier is complete, review this.
      B.emitStoreValueOperation(prologueLoc, selfArg, VarLocs[selfDecl].value,
                                StoreOwnershipQualifier::Init);
    } else {
      selfArg = B.createMarkUninitialized(selfDecl, selfArg, MUKind);
      VarLocs[selfDecl] = VarLoc::get(selfArg);
      enterDestroyCleanup(VarLocs[selfDecl].value);
    }
  }

  // Prepare the end of initializer location.
  SILLocation endOfInitLoc = RegularLocation(ctor);
  endOfInitLoc.pointToEnd();

  // Create a basic block to jump to for the implicit 'self' return.
  // We won't emit the block until after we've emitted the body.
  prepareEpilog(Type(), ctor->hasThrows(),
                CleanupLocation::get(endOfInitLoc));

  // If the constructor can fail, set up an alternative epilog for constructor
  // failure.
  SILBasicBlock *failureExitBB = nullptr;
  SILArgument *failureExitArg = nullptr;
  auto &resultLowering = getTypeLowering(ctor->getResultType());

  if (ctor->getFailability() != OTK_None) {
    SILBasicBlock *failureBB = createBasicBlock(FunctionSection::Postmatter);

    RegularLocation loc(ctor);
    loc.markAutoGenerated();

    // On failure, we'll clean up everything and return nil instead.
    SavedInsertionPoint savedIP(*this, failureBB, FunctionSection::Postmatter);

    failureExitBB = createBasicBlock();
    failureExitArg = new (F.getModule())
      SILArgument(failureExitBB, resultLowering.getLoweredType());

    Cleanups.emitCleanupsForReturn(ctor);
    SILValue nilResult = B.createEnum(loc, {},
                                      getASTContext().getOptionalNoneDecl(),
                                      resultLowering.getLoweredType());
    B.createBranch(loc, failureExitBB, nilResult);

    B.setInsertionPoint(failureExitBB);
    B.createReturn(loc, failureExitArg);

    FailDest = JumpDest(failureBB, Cleanups.getCleanupsDepth(), ctor);
  }

  // Handle member initializers.
  if (isDelegating) {
    // A delegating initializer does not initialize instance
    // variables.
  } else if (ctor->hasStubImplementation()) {
    // Nor does a stub implementation.
  } else if (selfClassDecl->requiresStoredPropertyInits() &&
             usesObjCAllocator) {
    // When the class requires all stored properties to have initial
    // values and we're using Objective-C's allocation, stored
    // properties are initialized via the .cxx_construct method, which
    // will be called by the runtime.

    // Note that 'self' has been fully initialized at this point.
  } else {
    // Emit the member initializers.
    emitMemberInitializers(dc, selfDecl, selfClassDecl);
  }

  emitProfilerIncrement(ctor->getBody());
  // Emit the constructor body.
  emitStmt(ctor->getBody());

  
  // Build a custom epilog block, since the AST representation of the
  // constructor decl (which has no self in the return type) doesn't match the
  // SIL representation.
  {
    SavedInsertionPoint savedIP(*this, ReturnDest.getBlock());
    assert(B.getInsertionBB()->empty() && "Epilog already set up?");
    auto cleanupLoc = CleanupLocation(ctor);

    // If we're using a box for self, reload the value at the end of the init
    // method.
    if (NeedsBoxForSelf) {
      // Emit the call to super.init() right before exiting from the initializer.
      if (Expr *SI = ctor->getSuperInitCall())
        emitRValue(SI);

      selfArg = B.emitLoadValueOperation(cleanupLoc, VarLocs[selfDecl].value,
                                         LoadOwnershipQualifier::Copy);
    } else {
      // We have to do a retain because we are returning the pointer +1.
      //
      // SEMANTIC ARC TODO: When the verifier is complete, we will need to
      // change this to selfArg = B.emitCopyValueOperation(...). Currently due
      // to the way that SILGen performs folding of copy_value, destroy_value,
      // the returned selfArg may be deleted causing us to have a
      // dead-pointer. Instead just use the old self value since we have a
      // class.
      selfArg = B.emitCopyValueOperation(cleanupLoc, selfArg);
    }

    // Inject the self value into an optional if the constructor is failable.
    if (ctor->getFailability() != OTK_None) {
      RegularLocation loc(ctor);
      loc.markAutoGenerated();
      selfArg = B.createEnum(loc, selfArg,
                             getASTContext().getOptionalSomeDecl(),
                             getLoweredLoadableType(ctor->getResultType()));
    }
  }
  
  // Emit the epilog and post-matter.
  auto returnLoc = emitEpilog(ctor, /*UsesCustomEpilog*/true);

  // Finish off the epilog by returning.  If this is a failable ctor, then we
  // actually jump to the failure epilog to keep the invariant that there is
  // only one SIL return instruction per SIL function.
  if (B.hasValidInsertionPoint()) {
    if (failureExitBB)
      B.createBranch(returnLoc, failureExitBB, selfArg);
    else
      B.createReturn(returnLoc, selfArg);
  }
}

static ManagedValue emitSelfForMemberInit(SILGenFunction &SGF, SILLocation loc,
                                          VarDecl *selfDecl) {
  CanType selfFormalType = selfDecl->getType()
      ->getInOutObjectType()->getCanonicalType();
  if (selfFormalType->hasReferenceSemantics())
    return SGF.emitRValueForDecl(loc, selfDecl, selfDecl->getType(),
                                 AccessSemantics::DirectToStorage,
                                 SGFContext::AllowImmediatePlusZero)
      .getAsSingleValue(SGF, loc);
  else
    return SGF.emitLValueForDecl(loc, selfDecl,
                                 selfDecl->getType()->getCanonicalType(),
                                 AccessKind::Write,
                                 AccessSemantics::DirectToStorage);
}

static LValue emitLValueForMemberInit(SILGenFunction &SGF, SILLocation loc,
                                      VarDecl *selfDecl,
                                      VarDecl *property) {
  CanType selfFormalType = selfDecl->getType()
    ->getInOutObjectType()->getCanonicalType();
  auto self = emitSelfForMemberInit(SGF, loc, selfDecl);
  return SGF.emitPropertyLValue(loc, self, selfFormalType, property,
                                AccessKind::Write,
                                AccessSemantics::DirectToStorage);
}

/// Emit a member initialization for the members described in the
/// given pattern from the given source value.
static void emitMemberInit(SILGenFunction &SGF, VarDecl *selfDecl,
                           Pattern *pattern, RValue &&src) {
  switch (pattern->getKind()) {
  case PatternKind::Paren:
    return emitMemberInit(SGF, selfDecl,
                          cast<ParenPattern>(pattern)->getSubPattern(),
                          std::move(src));

  case PatternKind::Tuple: {
    auto tuple = cast<TuplePattern>(pattern);
    auto fields = tuple->getElements();

    SmallVector<RValue, 4> elements;
    std::move(src).extractElements(elements);
    for (unsigned i = 0, n = fields.size(); i != n; ++i) {
      emitMemberInit(SGF, selfDecl, fields[i].getPattern(),
                     std::move(elements[i]));
    }
    break;
  }

  case PatternKind::Named: {
    auto named = cast<NamedPattern>(pattern);
    // Form the lvalue referencing this member.
    WritebackScope scope(SGF);
    LValue memberRef = emitLValueForMemberInit(SGF, pattern, selfDecl,
                                               named->getDecl());

    // Assign to it.
    SGF.emitAssignToLValue(pattern, std::move(src), std::move(memberRef));
    return;
  }

  case PatternKind::Any:
    return;

  case PatternKind::Typed:
    return emitMemberInit(SGF, selfDecl,
                          cast<TypedPattern>(pattern)->getSubPattern(),
                          std::move(src));

  case PatternKind::Var:
    return emitMemberInit(SGF, selfDecl,
                          cast<VarPattern>(pattern)->getSubPattern(),
                          std::move(src));

#define PATTERN(Name, Parent)
#define REFUTABLE_PATTERN(Name, Parent) case PatternKind::Name:
#include "swift/AST/PatternNodes.def"
    llvm_unreachable("Refutable pattern in pattern binding");
  }
}

static SILValue getBehaviorInitStorageFn(SILGenFunction &gen,
                                         VarDecl *behaviorVar) {
  std::string behaviorInitName;
  {
    Mangler m;
    m.mangleBehaviorInitThunk(behaviorVar);
    behaviorInitName = m.finalize();
  }
  
  SILFunction *thunkFn;
  // Skip out early if we already emitted this thunk.
  if (auto existing = gen.SGM.M.lookUpFunction(behaviorInitName)) {
    thunkFn = existing;
  } else {
    auto init = behaviorVar->getBehavior()->InitStorageDecl.getDecl();
    auto initFn = gen.SGM.getFunction(SILDeclRef(init), NotForDefinition);
    
    // Emit a thunk to inject the `self` metatype and implode tuples.
    auto storageVar = behaviorVar->getBehavior()->StorageDecl;
    auto selfTy = behaviorVar->getDeclContext()->getDeclaredInterfaceType();
    auto initTy = gen.getLoweredType(selfTy).getFieldType(behaviorVar,
                                                          gen.SGM.M);
    auto storageTy = gen.getLoweredType(selfTy).getFieldType(storageVar,
                                                             gen.SGM.M);
    
    auto initConstantTy = initFn->getLoweredType().castTo<SILFunctionType>();
    
    auto param = SILParameterInfo(initTy.getSwiftRValueType(),
                        initTy.isAddress() ? ParameterConvention::Indirect_In
                                           : ParameterConvention::Direct_Owned);
    auto result = SILResultInfo(storageTy.getSwiftRValueType(),
                              storageTy.isAddress() ? ResultConvention::Indirect
                                                    : ResultConvention::Owned);
    
    initConstantTy = SILFunctionType::get(initConstantTy->getGenericSignature(),
                                          initConstantTy->getExtInfo(),
                                          ParameterConvention::Direct_Unowned,
                                          param,
                                          result,
                                          // TODO: throwing initializer?
                                          None,
                                          gen.getASTContext());
    
    // TODO: Generate the body of the thunk.
    thunkFn = gen.SGM.M.getOrCreateFunction(SILLocation(behaviorVar),
                                            behaviorInitName,
                                            SILLinkage::PrivateExternal,
                                            initConstantTy,
                                            IsBare, IsTransparent, IsFragile);
    
    
  }
  return gen.B.createFunctionRef(behaviorVar, thunkFn);
}

static SILValue getBehaviorSetterFn(SILGenFunction &gen, VarDecl *behaviorVar) {
  auto set = behaviorVar->getSetter();
  auto setFn = gen.SGM.getFunction(SILDeclRef(set), NotForDefinition);

  // TODO: The setter may need to be a thunk, to implode tuples or perform
  // reabstractions.
  return gen.B.createFunctionRef(behaviorVar, setFn);
}

static Type getInitializationTypeInContext(
    DeclContext *fromDC, DeclContext *toDC,
    Expr *init) {
  auto interfaceType =
      ArchetypeBuilder::mapTypeOutOfContext(fromDC, init->getType());
  auto resultType =
      ArchetypeBuilder::mapTypeIntoContext(toDC, interfaceType);

  return resultType;
}

void SILGenFunction::emitMemberInitializers(DeclContext *dc,
                                            VarDecl *selfDecl,
                                            NominalTypeDecl *nominal) {
  for (auto member : nominal->getMembers()) {
    // Find instance pattern binding declarations that have initializers.
    if (auto pbd = dyn_cast<PatternBindingDecl>(member)) {
      if (pbd->isStatic()) continue;

      for (auto entry : pbd->getPatternList()) {
        auto init = entry.getInit();
        if (!init) continue;

        // Cleanup after this initialization.
        FullExpr scope(Cleanups, entry.getPattern());

        // Get the substitutions for the constructor context.
        ArrayRef<Substitution> subs;
        auto *genericEnv = dc->getGenericEnvironmentOfContext();
        if (genericEnv) {
          subs = genericEnv->getForwardingSubstitutions(
              SGM.SwiftModule);
        }

        // Get the type of the initialization result, in terms
        // of the constructor context's archetypes.
        CanType resultType = getInitializationTypeInContext(
            pbd->getDeclContext(), dc, init)->getCanonicalType();
        AbstractionPattern origResultType(resultType);

        // FIXME: Can emitMemberInit() share code with
        // InitializationForPattern in SILGenDecl.cpp?
        RValue result = emitApplyOfStoredPropertyInitializer(
                                  init, entry, subs,
                                  resultType, origResultType,
                                  SGFContext());

        emitMemberInit(*this, selfDecl, entry.getPattern(), std::move(result));
      }
    }
    
    // Introduce behavior initialization markers for properties that need them.
    if (auto var = dyn_cast<VarDecl>(member)) {
      if (var->isStatic()) continue;
      if (!var->hasBehaviorNeedingInitialization()) continue;
      
      // Get the initializer method for behavior.
      auto init = var->getBehavior()->InitStorageDecl;
      SILValue initFn = getBehaviorInitStorageFn(*this, var);
      
      // Get the behavior's storage we need to initialize.
      auto storage = var->getBehavior()->StorageDecl;
      LValue storageRef = emitLValueForMemberInit(*this, var, selfDecl,storage);
      // Shed any reabstraction over the member.
      while (storageRef.isLastComponentTranslation())
        storageRef.dropLastTranslationComponent();
      
      auto storageAddr = emitAddressOfLValue(var, std::move(storageRef),
                                             AccessKind::ReadWrite);
      
      // Get the setter.
      auto setterFn = getBehaviorSetterFn(*this, var);
      auto self = emitSelfForMemberInit(*this, var, selfDecl);
      
      auto mark = B.createMarkUninitializedBehavior(var,
               initFn, init.getSubstitutions(), storageAddr.getValue(),
               setterFn, getForwardingSubstitutions(), self.getValue(),
               getLoweredType(var->getType()).getAddressType());
      
      // The mark instruction stands in for the behavior property.
      VarLocs[var].value = mark;
    }
  }
}

void SILGenFunction::emitIVarInitializer(SILDeclRef ivarInitializer) {
  auto cd = cast<ClassDecl>(ivarInitializer.getDecl());
  RegularLocation loc(cd);
  loc.markAutoGenerated();

  // Emit 'self', then mark it uninitialized.
  auto selfDecl = cd->getDestructor()->getImplicitSelfDecl();
  SILType selfTy = getLoweredLoadableType(selfDecl->getType());
  SILValue selfArg = new (SGM.M) SILArgument(F.begin(), selfTy, selfDecl);
  SILLocation PrologueLoc(selfDecl);
  PrologueLoc.markAsPrologue();
  B.createDebugValue(PrologueLoc, selfArg);
  selfArg = B.createMarkUninitialized(selfDecl, selfArg,
                                      MarkUninitializedInst::RootSelf);
  assert(selfTy.hasReferenceSemantics() && "can't emit a value type ctor here");
  VarLocs[selfDecl] = VarLoc::get(selfArg);

  auto cleanupLoc = CleanupLocation::get(loc);
  prepareEpilog(TupleType::getEmpty(getASTContext()), false, cleanupLoc);

  // Emit the initializers.
  emitMemberInitializers(cd, selfDecl, cd);

  // Return 'self'.
  B.createReturn(loc, selfArg);

  emitEpilog(loc);
}
