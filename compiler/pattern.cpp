/* -*-Mode: c++;-*-
 Copyright 2004 John Plevyak, All Rights Reserved, see COPYRIGHT file
*/

#include "geysa.h"
#include "pattern.h"
#include "pdb.h"
#include "if1.h"

// TODO
// handle context sensitive lookups, like localized pattern matching
//   - handle them with a unique symbol in a hidden first position to uniquify them
//   - the pnode will have multiple return values
// handle generalized partial type pattern matching
//   - build nested structures like with sym->pattern
//   - match an initial sym for the partial pattern (to differentiate from tuple and each other)
// figure out what myclass does in Sather and Clu
// optimize case of filters with no restrictions (sym_any) to prevent
//   recomputation of dispatch functions
// dispatch on return type and number!
// used MPosition to handle destructuring of return values
// convert MPosition into an integer index into the in-order traversal of f->args
// handle conflicts 
// handle generalized structural types

typedef Map<Fun *, Match *> MatchMap;
typedef Vec<Vec<Fun *> *> PartialMatches;

static OpenHash<MPosition *, MPositionHashFuns> cannonical_mposition;

MPosition *
cannonicalize_mposition(MPosition &p) {
  MPosition *cp = cannonical_mposition.get(&p);
  if (cp)
    return cp;
  cp = new MPosition;
  cp->pos.copy(p.pos);
  cannonical_mposition.put(cp);
  return cp;
}

static void
insert_fun(FA *fa, Fun *f, Sym *a, MPosition &p) {
  MPosition *cp = cannonicalize_mposition(p);
  if (fa->patterns->types_set.set_add(a))
    fa->patterns->types.add(a);
  if (!a->match_type) {
    a->match_type = new MType;
    fa->patterns->mtypes.add(a->match_type);
  }
  Vec<Fun *> *funs = a->match_type->funs.get(cp);
  if (!funs) {
    funs = new Vec<Fun *>;
    a->match_type->funs.put(cp, funs);
  }
  funs->set_add(f);
}

static void
build_arg(FA *fa, Fun *f, Sym *a, MPosition &p) {
  if (a->pattern) {
    p.push(1);
    forv_Sym(aa, a->has)
      build_arg(fa, f, aa, p);
    p.pop();
  } else {
    if ((a->symbol || (a->type && a->type->symbol))) {
      Sym *sel = a->symbol ? a : a->type;
      insert_fun(fa, f, sel, p);
    } else
      insert_fun(fa, f, a->type ? a->type : sym_any, p);
  }
  p.inc();
}

static void
build(FA *fa) {
  forv_Fun(f, fa->pdb->funs) {
    MPosition p;
    p.push(1);
    insert_fun(fa, f, f->sym, p);
    forv_Sym(a, f->sym->args)
      build_arg(fa, f, a, p);
  }
}

static void
pattern_match_sym(FA *fa, Sym *s, CreationSet *cs, Vec<Fun *> *funs, Vec<Fun *> *partial_matches, 
		  MatchMap &match_map, MPosition *cp, Vec<Sym *> &done) 
{
  if (s->match_type) {
    Vec<Fun *> *fs = s->match_type->funs.get(cp);
    if (fs) {
      Vec<Fun *> ffs;
      if (partial_matches) {
	partial_matches->set_intersection(*fs, ffs);
	fs = &ffs;
      }
      forv_Fun(f, *fs) if (f) {
	Match *m = match_map.get(f);
	if (!m) {
	  m = new Match(f);
	  match_map.put(f, m); 
	}
	AType *t = m->filters.get(cp);
	if (!t) {
	  t = new AType; 
	  m->filters.put(cp, t);
	}
	t->set_add(cs);
      }
      funs->set_union(*fs);
    }
  }
  forv_Sym(ss, s->dispatch_order)
    if (done.set_add(ss))
      pattern_match_sym(fa, ss, cs, funs, partial_matches, match_map, cp, done);
}

static void
pattern_match_arg(FA *fa, AVar *a, PartialMatches &partial_matches, 
		  MatchMap &match_map, MPosition &p, AVar *send, Vec<MPosition *> *allpositions) 
{
  MPosition *cp = cannonical_mposition.get(&p);
  if (!cp)
    return;
  if (allpositions && !allpositions->set_in(cp))
    return;
  Vec<Fun *> *funs = new Vec<Fun *>;
  a->arg_of_send.set_add(send);
  forv_CreationSet(cs, *a->out) if (cs) {
    if (cs->sym == sym_tuple) {
      p.push(1);
      Vec<Fun *> *push_funs = NULL;
      if (partial_matches.v[partial_matches.n-1])
	push_funs = new Vec<Fun *>(*partial_matches.v[partial_matches.n-1]);
      partial_matches.add(push_funs);
      forv_AVar(av, cs->vars) {
	pattern_match_arg(fa, av, partial_matches, match_map, p, send, allpositions);
	if (!partial_matches.v[partial_matches.n-1] ||
	    !partial_matches.v[partial_matches.n-1]->n)
	  break;
	p.inc();
      }
      p.pop();
      if (partial_matches.v[partial_matches.n-1]) {
	forv_Fun(f, *partial_matches.v[partial_matches.n-1]) if (f) {
	  Match *m = match_map.get(f);
	  if (!m) {
	    m = new Match(f);
	    match_map.put(f, m); 
	  }
	  AType *t = m->filters.get(cp);
	  if (!t) {
	    t = new AType; 
	    m->filters.put(cp, t);
	  }
	  t->set_add(cs);
	}
	funs->set_union(*partial_matches.v[partial_matches.n-1]);
      }
      partial_matches.n--;
    }
    Vec<Sym *> done;
    pattern_match_sym(fa, cs->sym, cs, funs, partial_matches.v[partial_matches.n-1], 
		      match_map, cp, done);
  }
  partial_matches.v[partial_matches.n-1] = funs;
}

static int
best_match_sym(FA *fa, Sym *s, CreationSet *cs, Vec<Fun *> *funs, Vec<Fun *> *partial_matches, 
	       MatchMap &match_map, MPosition *cp, Vec<Sym *> &done) 
{
  if (s->match_type) {
    Vec<Fun *> *fs = s->match_type->funs.get(cp);
    if (fs) {
      Vec<Fun *> ffs;
      if (partial_matches) {
	partial_matches->set_intersection(*fs, ffs);
	fs = &ffs;
	if (fs->n) {
	  funs->set_union(*fs);
	  return 1;
	}
      }
    }
  }
  forv_Sym(ss, s->dispatch_order) 
    if (done.set_add(ss))
      if (best_match_sym(fa, ss, cs, funs, partial_matches, match_map, cp, done))
	return 1;
  return 0;
}


static void
best_match_arg(FA *fa, AVar *a, PartialMatches &partial_matches, 
	       MatchMap &match_map, MPosition &p, int check_ambiguities = 0) 
{
  MPosition *cp = cannonical_mposition.get(&p);
  if (!cp)
    return;
  Vec<Fun *> *funs = new Vec<Fun *>;
  forv_CreationSet(cs, *a->out) if (cs) {
    if (cs->sym == sym_tuple) {
      partial_matches.add(new Vec<Fun *>(*partial_matches.v[partial_matches.n-1]));
      if (!check_ambiguities) {
	p.push(1);
	forv_AVar(av, cs->vars) {
	  best_match_arg(fa, av, partial_matches, match_map, p);
	  p.inc();
	}
      } else {
	p.push(cs->vars.n);
	for (int i = cs->vars.n - 1; i >= 0; i--) {
	  AVar *av = cs->vars.v[i];
	  best_match_arg(fa, av, partial_matches, match_map, p);
	  p.dec();
	}
      }
      p.pop();
      funs->set_union(*partial_matches.v[partial_matches.n-1]);
      partial_matches.n--;
    }
    Vec<Sym *> done;
    best_match_sym(fa, cs->sym, cs, funs, partial_matches.v[partial_matches.n-1], 
		   match_map, cp, done);
  }
  partial_matches.v[partial_matches.n-1] = funs;
}

int
pattern_match(FA *fa, Vec<AVar *> &args, Vec<Match *> &matches, AVar *send) {
  matches.clear();
  MatchMap match_map;
  PartialMatches partial_matches;
  // find all matches
  {
    MPosition p;
    Vec<MPosition *> *allpositions = NULL;
    if (send->var->def->callees) {
      partial_matches.add(new Vec<Fun *>(send->var->def->callees->funs));
      allpositions = &send->var->def->callees->positions;
    } else
      partial_matches.add(NULL);
    p.push(1);
    forv_AVar(av, args) {
      pattern_match_arg(fa, av, partial_matches, match_map, p, send, allpositions);
      p.inc();
    }
  }
  if (!partial_matches.v[0])
    return 0;
  Vec<Fun *> allfuns(*partial_matches.v[0]);
  // find best matches
  {
    MPosition p;
    p.push(1);
    forv_AVar(av, args) {
      best_match_arg(fa, av, partial_matches, match_map, p);
      p.inc();
    }
  }
  Vec<Fun *> bestfuns(*partial_matches.v[0]);
  // check for ambiguities
  partial_matches.v[0]->copy(allfuns);
  {
    MPosition p;
    p.push(args.n);
    for (int i = args.n - 1; i >= 0; i--) {
      AVar *av = args.v[i];
      best_match_arg(fa, av, partial_matches, match_map, p, true);
      p.dec();
    }
  }
  Vec<Fun *> finalfuns, reversebestfuns(*partial_matches.v[0]);
  partial_matches.v[0]->set_intersection(bestfuns, finalfuns);
  if (bestfuns.some_disjunction(reversebestfuns)) {
    Vec<Fun *> *ambiguities = new Vec<Fun *>;
    bestfuns.set_disjunction(reversebestfuns, *ambiguities);
    type_violation(ATypeViolation_DISPATCH_AMBIGUITY, args.v[0], args.v[0]->out, 
		   send, ambiguities);
  }
  // cannonicalize filter types
  forv_Fun(f, finalfuns) if (f) {
    Match *m = match_map.get(f);
    for (int i = 0; i < m->filters.n; i++)
      if (m->filters.v[i].key)
	m->filters.v[i].value = make_AType(*m->filters.v[i].value);
    matches.add(m);
  }
  return matches.n;
}

void
build_patterns(FA *fa) {
  fa->patterns = new Patterns;
  build(fa);
}

static void
build_arg_position(Fun *f, Sym *a, MPosition &p, MPosition *parent = 0) {
  MPosition *cp = cannonicalize_mposition(p);
  cp->parent = parent;
  f->positions.add(cp);
  if (!a->var)
    a->var = new Var(a);
  f->arg_syms.put(cp, a);
  if (a->pattern) {
    p.push(1);
    forv_Sym(aa, a->has) {
      build_arg_position(f, aa, p, cp);
      p.inc();
    }
    p.pop();
  }
}

void
build_positions(FA *fa) {
  forv_Fun(f, fa->pdb->funs) {
    MPosition p;
    p.push(1);
    forv_Sym(a, f->sym->args) {
      build_arg_position(f, a, p);
      p.inc();
    }
  }
}
