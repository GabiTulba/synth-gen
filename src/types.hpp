#pragma once
#include <map>
#include <memory>
#include <string>
#include <vector>

namespace synth {

// Semantic types. User code is fully annotated and monomorphic; type
// variables (Var) appear only in built-in primitive signatures and are
// instantiated at call sites.
struct Type;
using TypePtr = std::shared_ptr<const Type>;

struct Type {
  enum class Kind {
    Scalar,
    Vector,
    Timestamp,
    String,
    Unit,
    Signal,   // elem
    Sample,   // elem
    List,     // elem
    Tuple,    // items
    Fun,      // items = params, ret
    Var,      // var id (primitive signatures only)
  };
  Kind kind;
  TypePtr elem;                 // Signal / Sample / List
  std::vector<TypePtr> items;   // Tuple members or Fun params
  TypePtr ret;                  // Fun
  int var = 0;                  // Var

  explicit Type(Kind k) : kind(k) {}
};

TypePtr tScalar();
TypePtr tVector();
TypePtr tTimestamp();
TypePtr tString();
TypePtr tUnit();
TypePtr tSignal(TypePtr elem);
TypePtr tSample(TypePtr elem);
TypePtr tList(TypePtr elem);
TypePtr tTuple(std::vector<TypePtr> items);
TypePtr tFun(std::vector<TypePtr> params, TypePtr ret);
TypePtr tVar(int id);

bool typeEquals(const TypePtr& a, const TypePtr& b);
std::string typeName(const TypePtr& t);

// Unification of a primitive signature type (may contain Vars) against a
// concrete user-side type. `subst` maps var id -> concrete type.
using Subst = std::map<int, TypePtr>;
bool unify(const TypePtr& sig, const TypePtr& concrete, Subst& subst);
TypePtr applySubst(const TypePtr& t, const Subst& subst);

}  // namespace synth
