#include "types.hpp"

namespace synth {

namespace {
TypePtr make(Type::Kind k) { return std::make_shared<Type>(k); }
}  // namespace

TypePtr tScalar() {
  static TypePtr t = make(Type::Kind::Scalar);
  return t;
}
TypePtr tVector() {
  static TypePtr t = make(Type::Kind::Vector);
  return t;
}
TypePtr tTimestamp() {
  static TypePtr t = make(Type::Kind::Timestamp);
  return t;
}
TypePtr tString() {
  static TypePtr t = make(Type::Kind::String);
  return t;
}
TypePtr tUnit() {
  static TypePtr t = make(Type::Kind::Unit);
  return t;
}
TypePtr tSignal(TypePtr elem) {
  auto t = std::make_shared<Type>(Type::Kind::Signal);
  t->elem = std::move(elem);
  return t;
}
TypePtr tSample(TypePtr elem) {
  auto t = std::make_shared<Type>(Type::Kind::Sample);
  t->elem = std::move(elem);
  return t;
}
TypePtr tList(TypePtr elem) {
  auto t = std::make_shared<Type>(Type::Kind::List);
  t->elem = std::move(elem);
  return t;
}
TypePtr tTuple(std::vector<TypePtr> items) {
  auto t = std::make_shared<Type>(Type::Kind::Tuple);
  t->items = std::move(items);
  return t;
}
TypePtr tFun(std::vector<TypePtr> params, TypePtr ret) {
  auto t = std::make_shared<Type>(Type::Kind::Fun);
  t->items = std::move(params);
  t->ret = std::move(ret);
  return t;
}
TypePtr tFun(std::vector<TypePtr> params, std::vector<std::string> labels,
             TypePtr ret) {
  auto t = std::make_shared<Type>(Type::Kind::Fun);
  t->items = std::move(params);
  t->labels = std::move(labels);
  t->ret = std::move(ret);
  return t;
}
TypePtr tVar(int id) {
  auto t = std::make_shared<Type>(Type::Kind::Var);
  t->var = id;
  return t;
}

bool typeEquals(const TypePtr& a, const TypePtr& b) {
  if (a.get() == b.get()) return true;
  if (a->kind != b->kind) return false;
  switch (a->kind) {
    case Type::Kind::Scalar:
    case Type::Kind::Vector:
    case Type::Kind::Timestamp:
    case Type::Kind::String:
    case Type::Kind::Unit:
      return true;
    case Type::Kind::Signal:
    case Type::Kind::Sample:
    case Type::Kind::List:
      return typeEquals(a->elem, b->elem);
    case Type::Kind::Tuple:
      if (a->items.size() != b->items.size()) return false;
      for (size_t i = 0; i < a->items.size(); i++)
        if (!typeEquals(a->items[i], b->items[i])) return false;
      return true;
    case Type::Kind::Fun:
      if (a->items.size() != b->items.size()) return false;
      for (size_t i = 0; i < a->items.size(); i++)
        if (!typeEquals(a->items[i], b->items[i])) return false;
      return typeEquals(a->ret, b->ret);
    case Type::Kind::Var:
      return a->var == b->var;
  }
  return false;
}

std::string typeName(const TypePtr& t) {
  switch (t->kind) {
    case Type::Kind::Scalar: return "Scalar";
    case Type::Kind::Vector: return "Vector";
    case Type::Kind::Timestamp: return "Timestamp";
    case Type::Kind::String: return "String";
    case Type::Kind::Unit: return "unit";
    case Type::Kind::Signal: return typeName(t->elem) + " Signal";
    case Type::Kind::Sample: return typeName(t->elem) + " Sample";
    case Type::Kind::List: return typeName(t->elem) + " list";
    case Type::Kind::Tuple: {
      std::string s = "(";
      for (size_t i = 0; i < t->items.size(); i++) {
        if (i) s += ", ";
        s += typeName(t->items[i]);
      }
      return s + ")";
    }
    case Type::Kind::Fun: {
      std::string s = "(";
      for (size_t i = 0; i < t->items.size(); i++) {
        std::string label = t->labelAt(i);
        if (!label.empty()) s += label + ":";
        s += typeName(t->items[i]) + " -> ";
      }
      return s + typeName(t->ret) + ")";
    }
    case Type::Kind::Var:
      return std::string("'") + char('a' + (t->var % 26));
  }
  return "?";
}

namespace {
// Does variable `v` appear in `t`, chasing bindings through `subst`? Guards
// unification against creating cyclic substitutions.
bool occurs(int v, const TypePtr& t, const Subst& subst) {
  switch (t->kind) {
    case Type::Kind::Var: {
      if (t->var == v) return true;
      auto it = subst.find(t->var);
      return it != subst.end() && occurs(v, it->second, subst);
    }
    case Type::Kind::Signal:
    case Type::Kind::Sample:
    case Type::Kind::List:
      return occurs(v, t->elem, subst);
    case Type::Kind::Tuple:
    case Type::Kind::Fun: {
      for (auto& x : t->items)
        if (occurs(v, x, subst)) return true;
      return t->ret && occurs(v, t->ret, subst);
    }
    default:
      return false;
  }
}
}  // namespace

bool unify(const TypePtr& sig, const TypePtr& concrete, Subst& subst) {
  // Chase existing bindings on both sides so each variable is unified
  // against its current representative, not just checked for equality.
  if (sig->kind == Type::Kind::Var) {
    auto it = subst.find(sig->var);
    if (it != subst.end()) return unify(it->second, concrete, subst);
  }
  if (concrete->kind == Type::Kind::Var) {
    auto it = subst.find(concrete->var);
    if (it != subst.end()) return unify(sig, it->second, subst);
  }
  if (sig->kind == Type::Kind::Var && concrete->kind == Type::Kind::Var &&
      sig->var == concrete->var)
    return true;
  if (sig->kind == Type::Kind::Var) {
    if (occurs(sig->var, concrete, subst)) return false;
    subst[sig->var] = concrete;
    return true;
  }
  if (concrete->kind == Type::Kind::Var) {
    if (occurs(concrete->var, sig, subst)) return false;
    subst[concrete->var] = sig;
    return true;
  }
  if (sig->kind != concrete->kind) return false;
  switch (sig->kind) {
    case Type::Kind::Scalar:
    case Type::Kind::Vector:
    case Type::Kind::Timestamp:
    case Type::Kind::String:
    case Type::Kind::Unit:
      return true;
    case Type::Kind::Signal:
    case Type::Kind::Sample:
    case Type::Kind::List:
      return unify(sig->elem, concrete->elem, subst);
    case Type::Kind::Tuple:
      if (sig->items.size() != concrete->items.size()) return false;
      for (size_t i = 0; i < sig->items.size(); i++)
        if (!unify(sig->items[i], concrete->items[i], subst)) return false;
      return true;
    case Type::Kind::Fun:
      if (sig->items.size() != concrete->items.size()) return false;
      for (size_t i = 0; i < sig->items.size(); i++)
        if (!unify(sig->items[i], concrete->items[i], subst)) return false;
      return unify(sig->ret, concrete->ret, subst);
    case Type::Kind::Var:
      return false;  // unreachable
  }
  return false;
}

TypePtr applySubst(const TypePtr& t, const Subst& subst) {
  switch (t->kind) {
    case Type::Kind::Var: {
      // Chase bindings transitively; the occurs check in unify guarantees
      // the substitution is acyclic, so this terminates.
      auto it = subst.find(t->var);
      return it != subst.end() ? applySubst(it->second, subst) : t;
    }
    case Type::Kind::Signal: return tSignal(applySubst(t->elem, subst));
    case Type::Kind::Sample: return tSample(applySubst(t->elem, subst));
    case Type::Kind::List: return tList(applySubst(t->elem, subst));
    case Type::Kind::Tuple: {
      std::vector<TypePtr> items;
      for (auto& x : t->items) items.push_back(applySubst(x, subst));
      return tTuple(std::move(items));
    }
    case Type::Kind::Fun: {
      std::vector<TypePtr> params;
      for (auto& x : t->items) params.push_back(applySubst(x, subst));
      return tFun(std::move(params), t->labels, applySubst(t->ret, subst));
    }
    default:
      return t;
  }
}

}  // namespace synth
