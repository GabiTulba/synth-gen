#pragma once
#include <map>
#include <memory>
#include <string>
#include <vector>

namespace synth {

// Semantic types. User code is fully annotated; type variables (Var) come
// from built-in primitive signatures and from user annotations ('a), and
// are instantiated at every use site.
//
// A Var carries an integer id whose *sign* says how it may be solved:
//   id >= 0  free      - a placeholder created by instantiating a
//                        signature at a use site; unification binds it.
//   id <  0  rigid     - a type variable written in a signature, checked
//                        from inside the definition that declares it. It
//                        stands for one unknown but arbitrary type, so it
//                        unifies only with itself (and with free vars);
//                        binding it to a concrete type would let a body
//                        assume something the caller never promised.
struct Type;
using TypePtr = std::shared_ptr<const Type>;

// A named type declaration (`type Env = { ... }`): the nominal identity a
// Named type points at. Declarations are owned by the CheckedModule that
// declares them; Named types hold a plain pointer, which stays valid for
// as long as the Program the types belong to.
struct TypeDecl {
  enum class Flavor {
    Record,    // fields
    Variant,   // ctors
    Abstract,  // no visible structure (an engine-backed handle)
  };
  Flavor flavor = Flavor::Abstract;
  std::string moduleId;  // canonical id of the declaring module
  std::string name;      // dotted name inside it ("Voice", "A.Voice")
  // Type parameters in order; the field/constructor types below are
  // written over paramVars, and every use site substitutes its own
  // arguments for them.
  std::vector<std::string> params;
  std::vector<TypePtr> paramVars;
  struct Field {
    std::string name;
    TypePtr type;
  };
  std::vector<Field> fields;  // Record
  struct Ctor {
    std::string name;
    TypePtr payload;  // null = no payload
  };
  std::vector<Ctor> ctors;  // Variant

  const Field* findField(const std::string& n) const {
    for (auto& f : fields)
      if (f.name == n) return &f;
    return nullptr;
  }
  const Ctor* findCtor(const std::string& n) const {
    for (auto& c : ctors)
      if (c.name == n) return &c;
    return nullptr;
  }
};

struct Type {
  enum class Kind {
    Scalar,
    Int,
    Vector,
    Timestamp,
    String,
    Bool,
    Unit,
    Signal,   // elem
    Sample,   // elem
    Tuple,    // items
    Fun,      // items = params, ret
    Var,      // var id (primitive signatures only)
    Named,    // decl + items = type arguments; equality is nominal
  };
  Kind kind;
  TypePtr elem;                 // Signal / Sample
  std::vector<TypePtr> items;   // Tuple members, Fun params or Named args
  const TypeDecl* decl = nullptr;  // Named
  // Fun: per-param labels, "" = positional. May be empty (all positional).
  // Labels drive call-site matching and printing; equality ignores them.
  std::vector<std::string> labels;
  TypePtr ret;                  // Fun
  int var = 0;                  // Var: id; negative = rigid (see above)
  std::string varName;          // Var: surface name ("a" for 'a), for
                                // diagnostics only; equality ignores it

  explicit Type(Kind k) : kind(k) {}

  std::string labelAt(size_t i) const {
    return i < labels.size() ? labels[i] : std::string{};
  }
};

TypePtr tScalar();
TypePtr tInt();
TypePtr tVector();
TypePtr tTimestamp();
TypePtr tString();
TypePtr tBool();
TypePtr tUnit();
TypePtr tSignal(TypePtr elem);
TypePtr tSample(TypePtr elem);
TypePtr tTuple(std::vector<TypePtr> items);
TypePtr tNamed(const TypeDecl* decl, std::vector<TypePtr> args);
TypePtr tFun(std::vector<TypePtr> params, TypePtr ret);
TypePtr tFun(std::vector<TypePtr> params, std::vector<std::string> labels,
             TypePtr ret);
TypePtr tVar(int id);
// A named variable; `id` must be negative for a rigid one (tRigidVar) and
// non-negative for a free one that keeps its surface name in diagnostics.
TypePtr tVar(int id, std::string name);
// A user-written 'name. `seq` counts variables allocated so far (0, 1, ...);
// the id it maps to is negative and unique per seq.
TypePtr tRigidVar(int seq, std::string name);
bool isRigidVar(const TypePtr& t);
// Does `t` mention a variable of the given flavour? A rigid one is a
// legitimate part of a polymorphic definition's type; a free one left over
// after a call means the call site failed to determine something.
bool containsRigidVar(const TypePtr& t);
bool containsFreeVar(const TypePtr& t);
bool containsAnyVar(const TypePtr& t);

bool typeEquals(const TypePtr& a, const TypePtr& b);
std::string typeName(const TypePtr& t);

// Unification of two types, either of which may contain Vars (signature
// variables, instantiated fresh per use site). `subst` maps var id -> type;
// bindings are chased on both sides and an occurs check keeps the
// substitution acyclic. Only free variables are ever bound: a rigid one
// matches itself, absorbs a free one, and fails against anything else, so
// it never appears as a key in `subst`.
using Subst = std::map<int, TypePtr>;
bool unify(const TypePtr& sig, const TypePtr& concrete, Subst& subst);
TypePtr applySubst(const TypePtr& t, const Subst& subst);

}  // namespace synth
