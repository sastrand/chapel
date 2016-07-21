#include "astutil.h"
#include "baseAST.h"
#include "stlUtil.h"
#include "expr.h"
#include "stmt.h"
#include "symbol.h"
#include "type.h"
#include "view.h"
#include "passes.h"
#include "resolution.h"

//helpers
#define ActualUseDefCastMap std::map<SymExpr*, std::pair<Expr*,Type*> >
bool isExprSafeForReorder(Expr * e);
bool canPrimMoveCreateCommunication(CallExpr* ce);
inline bool possibleDepInBetween(Expr* e1, Expr* e2);
inline bool requiresCast(Type* t);
inline bool isIntegerPromotionPrimitive(PrimitiveTag tag);
inline bool isNonEssentialPrimitive(CallExpr* ce);

bool isDenormalizable(Symbol* sym,
    Map<Symbol*,Vec<SymExpr*>*> defMap,
    Map<Symbol*,Vec<SymExpr*>*> useMap, SymExpr** useOut, Expr** defOut,
    Type** castTo);

void denormalizeActuals(CallExpr* ce,
    Map<Symbol*,Vec<SymExpr*>*> defMap,
    Map<Symbol*,Vec<SymExpr*>*> useMap,
    ActualUseDefCastMap actualUseDefMap);

void denormalize(FnSymbol *fn);
void denormalize(Expr* def, SymExpr* use, Type* castTo);

std::map<Expr*,bool> safeExprCache;

void denormalize(void) {
  if(fDenormalize) {
    forv_Vec(FnSymbol, fn, gFnSymbols) {
      denormalize(fn);
    }
  }
}

void denormalize(FnSymbol *fn) {

  Vec<Symbol*> symSet;
  Vec<SymExpr*> symExprs;
  Map<Symbol*,Vec<SymExpr*>*> defMap;
  Map<Symbol*,Vec<SymExpr*>*> useMap;

  collectSymbolSetSymExprVec(fn, symSet, symExprs);
  buildDefUseMaps(symSet, symExprs, defMap, useMap);

  std::set<CallExpr*> deferredFns;
  ActualUseDefCastMap actualUseDefMap;

  forv_Vec(Symbol, sym, symSet) {

    SymExpr *use = NULL;
    Expr *usePar = NULL;
    Expr *def = NULL;
    Expr *defPar;
    Type* castTo = NULL;

    if(isDenormalizable(sym, defMap, useMap, &use, &def, &castTo)) {
      usePar = use->parentExpr;
      defPar = def->parentExpr;

      //defer if the symbol used as actual
      if(CallExpr* useCe = toCallExpr(usePar)){
        if(!useCe->isPrimitive()) {
          deferredFns.insert(useCe);
          std::pair<Expr*, Type*> defCastBundle(def, castTo);
          actualUseDefMap.insert(std::pair<SymExpr*, std::pair<Expr*, Type*> >(use, defCastBundle));
          continue;
        }
      }
      if(isExprSafeForReorder(def)) {
        if(!possibleDepInBetween(defPar, usePar)) {
          denormalize(def, use, castTo);
        }
      }
    }
  } // end loop for symbol

  //handle deferred actuals
  for(std::set<CallExpr*>::iterator ceIt = deferredFns.begin() ;
      ceIt != deferredFns.end() ; ceIt++) {
    denormalizeActuals(*ceIt, defMap, useMap, actualUseDefMap);
  }

  freeDefUseMaps(defMap, useMap);
}

void denormalizeActuals(CallExpr* ce,
    Map<Symbol*,Vec<SymExpr*>*> defMap,
    Map<Symbol*,Vec<SymExpr*>*> useMap,
    ActualUseDefCastMap actualUseDefMap) {

  INT_ASSERT(!ce->isPrimitive());
  for_alist_backward(actual, ce->argList) {
    if(SymExpr* argSym = toSymExpr(actual)) { //else it's already denormd
      if(! (argSym->var->isConstant() || argSym->var->isParameter())) {
        if(actualUseDefMap.count(argSym) > 0) {
          std::pair<Expr*,Type*> tmpTuple = actualUseDefMap[argSym];
          SymExpr* use = argSym;
          Expr* def = tmpTuple.first;
          Type* castTo = tmpTuple.second;
          Expr* usePar = use->parentExpr;
          Expr* defPar = def->parentExpr;
          if(CallExpr* ceTmp = toCallExpr(defPar)) {
            if(!isRecord(ceTmp->get(1)->typeInfo())) { //to preserve pass-by-value
              if(isExprSafeForReorder(def)) {
                if(!possibleDepInBetween(defPar, usePar)) {
                  // In C actual evaluation order is not standard, therefore any
                  // defPar that us unsafe for reordering cannot be moved to
                  // the args
                  denormalize(def, use, castTo);
                }
              }
            }
          }
        }
      }
    }
  }
}

bool isDenormalizable(Symbol* sym,
    Map<Symbol*,Vec<SymExpr*>*> defMap,
    Map<Symbol*,Vec<SymExpr*>*> useMap, SymExpr** useOut, Expr** defOut,
    Type** castTo) {

  if(sym && !(toFnSymbol(sym) || toArgSymbol(sym) || toTypeSymbol(sym))) {

    SymExpr *use = NULL;
    Expr *usePar = NULL;
    Expr *def = NULL;
    Expr *defPar = NULL;

    Vec<SymExpr*>* defs = defMap.get(sym);
    Vec<SymExpr*>* uses = useMap.get(sym);

    if(defs && defs->n == 1 && uses && uses->n == 1) { // check def-use counts
      SymExpr* se = defs->first();
      defPar = se->parentExpr;

      //defPar has to be a move without any coercion
      CallExpr* ce = toCallExpr(defPar);
      if(ce) {
        if(ce->isPrimitive(PRIM_MOVE)) {
          Type* lhsType = ce->get(1)->typeInfo();
          Type* rhsType = ce->get(2)->typeInfo();
          if(lhsType == rhsType) {
            if(! canPrimMoveCreateCommunication(ce)) {
              if(! (lhsType->symbol->hasFlag(FLAG_EXTERN))){
                if(!lhsType->symbol->hasFlag(FLAG_ATOMIC_TYPE)){
                  def = ce->get(2);
                  if(CallExpr* defCe = toCallExpr(def)) {
                    if(defCe->isPrimitive() &&
                        isIntegerPromotionPrimitive(defCe->primitive->tag)) {
                      if(requiresCast(lhsType)) {
                        *castTo = lhsType;
                      }
                    }
                  }
                }
              }
            }
          }
        }
      }

      if(def) {
        *defOut = def;
        // we have def now find where the value is used
        SymExpr* se = uses->first();
        usePar = se->parentExpr;
        if(CallExpr* ce = toCallExpr(usePar)) {
          if( !(ce->isPrimitive(PRIM_ADDR_OF) ||
                ce->isPrimitive(PRIM_ARRAY_GET) ||
                ce->isPrimitive(PRIM_GET_MEMBER) ||
                ce->isPrimitive(PRIM_DEREF) ||
                ce->isPrimitive(PRIM_GET_MEMBER_VALUE) ||
                (ce->isPrimitive(PRIM_MOVE) && 
                 ce->get(1)->typeInfo() !=
                 ce->get(2)->typeInfo()))) {
            use = se;
          }
        }
      }
      else {
        return false;
      }
      if(use) {
        *useOut = use;
      }
      else {
        return false;
      }
      //here I have my valid use and def
      //so far we checked specific cases for them individually, now check if
      //there is anything wrong with them as a pair

      //only issue I hit was this and it feels too specific, maybe there is
      //safer/better/more general way of doing this check
      //for reference test that caused this was:
      //test/modules/standard/FileSystem/bharshbarg/filer
      //The issue seemed to be yielding string from an iterator
      if(CallExpr* useParentCe = toCallExpr(usePar)) {
        if(useParentCe->isPrimitive(PRIM_FTABLE_CALL)) {
          if(argMustUseCPtr(def->typeInfo())){
            return false;
          }
        }
      }
      return true;
    }
  }
  return false;
}

bool isExprSafeForReorder(Expr * e) {
  if(safeExprCache.count(e) > 0) {
    return safeExprCache[e];
  }
  if(CallExpr* ce = toCallExpr(e)) {
    if(!ce->isPrimitive()) {
      FnSymbol* fnSym = ce->theFnSymbol();
      for_formals(formal, fnSym) {
        if(isReferenceType(formal->typeInfo())) {
          safeExprCache[e] = false;
          return false;
        }
      }

      std::vector<BaseAST*> asts;
      collect_asts(fnSym->body, asts);
      for_vector(BaseAST, ast, asts) {
        if (SymExpr* se = toSymExpr(ast)) {
          Symbol* var = se->var;

          if(!var->isImmediate() && !var->isConstant() && isGlobal(var)){
            safeExprCache[e] = false;
            return false;
          }
        }
        //this else if lazily marks a def to be immovable if the
        //function body has a call to another user function
        //this can be enhanced by going recursive, but it might be a bit overkill
        else if(CallExpr* ce = toCallExpr(ast)) {
          if(!isNonEssentialPrimitive(ce)) {
            safeExprCache[e] = false;
            return false;
          }
        }
      }
    }
    else {
      //primitive
      if(ce->primitive->isEssential){
        safeExprCache[e] = false;
        return false;
      }
      else {
        //here we have a non essential primitive. But consider following issue:
        //
        // tmp = (atomic_read() == 5);
        //
        // where `==` is non-essential yet one of its children has side effects

        //I had implemented the following recursion to prevent the issue , but
        //then realized that it doesn't cause any tests to fail(and I had thought
        //this would have been a widespread case). Maybe I was wrong?. Further
        //this has performance impact on compilation time

        /*
        Expr* e = ce->argList.first();
        while(e) {
          if(!isExprSafeForReorder(e)) {//is this recursion safe?
            return false;
          }
          e = e->next;
        }
        */
      }
    }
  }
  safeExprCache[e] = true;
  return true;
}


void denormalize(Expr* def, SymExpr* use, Type* castTo) {
  Expr* defPar = def->parentExpr;

  //remove variable declaration
  use->var->defPoint->remove();

  //remove def
  Expr* replExpr = def->remove();

  //replace use with def
  if(castTo != NULL) {
    Expr* castExpr = new CallExpr(PRIM_CAST, castTo->symbol, replExpr);
    use->replace(castExpr);
  }
  else {
    use->replace(replExpr);
  }
  //remove defPar
  defPar->remove();
}

inline bool requiresCast(Type* t) {
  if(is_int_type(t) || is_uint_type(t)) {
    return true;
  }
  return false;
}

inline bool isIntegerPromotionPrimitive(PrimitiveTag tag) {
  switch(tag) {
    case PRIM_ADD:
    case PRIM_SUBTRACT:
    case PRIM_MULT:
    case PRIM_DIV:
    case PRIM_MOD:
    case PRIM_LSH:
    case PRIM_RSH:
      return true;
      break;
    default:
      return false;
      break;
  }
  return false;
}

// If RHS of a move contain access to anything wide, it can generate
// communication. chpl_gen_comm_get "returns" the data in the first argument,
// and the function itself is generated during codegen(see expr.cpp
// codegenAssign). Although the value assigned is actually temporary in the AST, 
// it cannot be denormalized due to that.
//
// Such temporaries that are passed as address are not denormalized in other
// functions due to PRIM_ADDROF, since chpl_gen_comm_get is generated at codegen
// time, such information is not readily available at AST.
bool canPrimMoveCreateCommunication(CallExpr* ce) {
  INT_ASSERT(ce);
  INT_ASSERT(ce->isPrimitive(PRIM_MOVE));

  Expr* lhs = ce->get(1);
  Expr* rhs = ce->get(2);
  Type* lhsType = lhs->typeInfo();
  Type* rhsType = rhs->typeInfo();

  if(lhsType->symbol->hasEitherFlag(FLAG_WIDE_REF, FLAG_WIDE_CLASS))
    return true; // direct put
  if(rhsType->symbol->hasEitherFlag(FLAG_WIDE_REF, FLAG_WIDE_CLASS))
    return true; // direct get

  //now it is still possible that rhs primitive has a nonwide symbol yet the
  //primitive itself generates communication
  if(CallExpr* rhsCe = toCallExpr(rhs)) {
    if(rhsCe->isPrimitive()) {
      switch(rhsCe->primitive->tag) {
        case PRIM_SET_MEMBER:
        case PRIM_GET_MEMBER:
        case PRIM_GET_MEMBER_VALUE:
        case PRIM_SET_SVEC_MEMBER:
        case PRIM_GET_SVEC_MEMBER:
        case PRIM_GET_SVEC_MEMBER_VALUE:
          if(rhsCe->get(1)->typeInfo()->symbol->hasEitherFlag(FLAG_WIDE_REF, FLAG_WIDE_CLASS)) {
            return true;
          }
          break;
        default:
          return false;
      }
    }
  }
  else {
    // var to var move
    // I don't see how this might create communication
  }
  return false;
}

inline bool isNonEssentialPrimitive(CallExpr* ce) {
  return ce->isPrimitive() && !ce->primitive->isEssential;
}

inline bool possibleDepInBetween(Expr* e1, Expr* e2){
  for(Expr* e = e1; e != e2 ; e = getNextExpr(e)) {
    if(! isExprSafeForReorder(e)) {
      return true;
    }
  }
  return false;
}
