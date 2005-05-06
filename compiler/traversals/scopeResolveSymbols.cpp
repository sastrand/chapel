#include <typeinfo>
#include "scopeResolveSymbols.h"
#include "expr.h"
#include "stmt.h"
#include "stringutil.h"
#include "symtab.h"
#include "../traversals/fixup.h"


static void resolve_type_helper(FnSymbol* currentFunction, Type* &type) {
  if (dynamic_cast<UnresolvedType*>(type)) {

    //
    // Resolve types in class methods
    //
    if (currentFunction && currentFunction->classBinding) {
      StructuralType* structuralType =
        dynamic_cast<StructuralType*>(currentFunction->classBinding->type);
      if (!structuralType) {
        INT_FATAL(type, "Error finding method's class's type");
      }
      Symbol* new_type = Symboltable::lookupFromScope(type->symbol->name,
                                                      structuralType->structScope);
      if (new_type && new_type->parentScope->type == SCOPE_CLASS) {
        type = new_type->type;
        return;
      }
    }
    
    Symbol* new_type = Symboltable::lookup(type->symbol->name);
    if (new_type) {
      if (!dynamic_cast<UnresolvedType*>(new_type->type)) {
        type = new_type->type;
      }
      else {
        resolve_type_helper(currentFunction, new_type->type);
        type = new_type->type;
      }
    }
    else {
      INT_FATAL(type, "Error resolving type");
    }
  } else if (UserType* user_type = dynamic_cast<UserType*>(type)) {
    resolve_type_helper(currentFunction, user_type->definition);
    if (!user_type->defaultVal) {
      user_type->defaultVal = user_type->definition->defaultVal->copy();
      fixup_expr(user_type->symbol->defPoint);
    }
  } else if (IndexType* index_type = dynamic_cast<IndexType*>(type)) {
    resolve_type_helper(currentFunction, index_type->idxType);
  } else if (ArrayType* array_type = dynamic_cast<ArrayType*>(type)) {
    resolve_type_helper(currentFunction, array_type->elementType);
  } else if (SeqType* seq_type = dynamic_cast<SeqType*>(type)) {
    resolve_type_helper(currentFunction, seq_type->elementType);
  } else if (TupleType* tuple_type = dynamic_cast<TupleType*>(type)) {
    for (int i = 0; i < tuple_type->components.n; i++) {
      resolve_type_helper(currentFunction, tuple_type->components.v[i]);
    }
    tuple_type->rebuildDefaultVal();
  }
}


ScopeResolveSymbols::ScopeResolveSymbols() {
  currentFunction = NULL;
  defList = new Map<SymScope*,Vec<VarSymbol*>*>();
}


void ScopeResolveSymbols::preProcessExpr(Expr* expr) {
  if (DefExpr* def_expr = dynamic_cast<DefExpr*>(expr)) {
    if (FnSymbol* fn = dynamic_cast<FnSymbol*>(def_expr->sym)) {
      currentFunction = fn;
    }
  }

  if (CastExpr* cast_expr = dynamic_cast<CastExpr*>(expr)) {
    resolve_type_helper(currentFunction, cast_expr->newType);
  }

  if (Variable* sym_use = dynamic_cast<Variable*>(expr)) {
    if (dynamic_cast<UnresolvedSymbol*>(sym_use->var)) {
      SymScope* currentScope = Symboltable::getCurrentScope();
      char* name = sym_use->var->name;
      
      if (!strcmp(name, "__primitive")) {
        return;
      }

      //
      // Resolve fields in class methods
      //
      Symbol* field = NULL;
      if (currentFunction && currentFunction->classBinding) {
        StructuralType* structuralType =
          dynamic_cast<StructuralType*>(currentFunction->classBinding->type);
        if (!structuralType) {
          INT_FATAL(expr, "Error finding method's class's type");
        }
        field = Symboltable::lookupFromScope(name, structuralType->structScope);
        if (field && field->parentScope->type != SCOPE_CLASS) {
          field = NULL;
        }
      }

      VarSymbol* sym_in_scope =
        dynamic_cast<VarSymbol*>(Symboltable::lookupInCurrentScope(name));
      
      if (sym_in_scope) {
        Vec<VarSymbol*>* sym_defs = defList->get(currentScope);
        if (!sym_defs || !sym_defs->set_in(sym_in_scope)) {
          USR_FATAL(expr, "Variable '%s' used before it is defined", name);
        }
      }
      
      Symbol* sym_resolve = Symboltable::lookup(name);
      
      if (VarSymbol* var_resolve = dynamic_cast<VarSymbol*>(sym_resolve)) {
        ModuleSymbol* mod =
          dynamic_cast<ModuleSymbol*>(var_resolve->parentScope->symContext);
        if (mod && mod->initFn->paramScope == currentScope) {
          Vec<VarSymbol*>* sym_defs = defList->get(currentScope);
          if (!sym_defs || !sym_defs->set_in(var_resolve)) {
            USR_FATAL(expr, "Variable '%s' used before it is defined", name);
          }
        }
      }

      if (sym_resolve && field) {
        if (sym_resolve->parentScope->parentLength() <
            currentFunction->parentScope->parentLength()) {
          if (!dynamic_cast<FnSymbol*>(field)) {
            sym_use->var = field;
          }
        } else {
          if (!dynamic_cast<FnSymbol*>(sym_resolve)) {
            sym_use->var = sym_resolve;
          }
        }
      } else if (sym_resolve) {
        if (!dynamic_cast<FnSymbol*>(sym_resolve)) {
          sym_use->var = sym_resolve;
        }
      } else if (field) {
        if (!dynamic_cast<FnSymbol*>(field)) {
          sym_use->var = field;
        }
      } else {
        USR_FATAL(expr, "Symbol '%s' is not defined", name);
      }
    }
  }
}


void ScopeResolveSymbols::postProcessExpr(Expr* expr) {
  if (DefExpr* def_expr = dynamic_cast<DefExpr*>(expr)) {
    if (dynamic_cast<FnSymbol*>(def_expr->sym)) {
      currentFunction = NULL;
    } else {
      SymScope* currentScope = Symboltable::getCurrentScope();
      Vec<VarSymbol*>* new_vars = def_expr->varDefSet();
      if (new_vars) {
        Vec<VarSymbol*>* old_vars = defList->get(currentScope);
        if (old_vars) {
          old_vars->set_union(*new_vars);
        } else {
        defList->put(currentScope, new_vars);
        }
      }
    }
  }
}


void ScopeResolveSymbols::preProcessSymbol(Symbol* sym) {
  resolve_type_helper(currentFunction, sym->type);
  FnSymbol* fnSym = dynamic_cast<FnSymbol*>(sym);
  if (fnSym) {
    resolve_type_helper(currentFunction, fnSym->retType);
  }
}
