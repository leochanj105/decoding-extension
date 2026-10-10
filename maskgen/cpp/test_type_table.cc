// The type-transition table on its own: no grammar, no parser, no tokens.
//
// Every question the table answers is a function of a rule name and two integers,
// so each row can be checked directly. This is where a type-semantics mistake
// should show up; test_end_to_end.cc only says whether the whole pipeline agrees.
#include "type_table.h"

#include <algorithm>
#include <cstdio>
#include <vector>
#include <fstream>
#include <sstream>
#include <string>

using namespace maskgen;

static int failures = 0;
static void check(bool ok, const std::string& what) {
  printf("%s  %s\n", ok ? "pass" : "FAIL", what.c_str());
  if (!ok) ++failures;
}

// A throwaway environment: six types by name, three declared names, two members.
enum { NUMBER = 0, STRING = 1, BOOLEAN = 2, NUMBER_ARRAY = 3, STRING_ARRAY = 4 };

static TypeOracle Oracle() {
  TypeOracle o;
  o.resolve = [](std::string_view text) -> int32_t {
    if (text == "number") return NUMBER;
    if (text == "string") return STRING;
    if (text == "boolean") return BOOLEAN;
    if (text == "number[]") return NUMBER_ARRAY;
    if (text == "string[]") return STRING_ARRAY;
    if (text == "count") return NUMBER;      // a declared name
    if (text == "msg") return STRING;        // a declared name
    if (text == "length") return NUMBER;     // a member of string
    if (text == "split") return STRING_ARRAY;
    return -1;
  };
  o.accepts = [](int32_t required, int32_t produced) { return required == produced; };
  o.resolve_op = [](std::string_view name) -> int32_t { return name == "plus" ? 0 : -1; };
  // TypeScript's `+`: a number only when both are numbers, a string as soon as
  // either is one, and nothing otherwise.
  o.combine = [](int32_t op, int32_t a, int32_t b) -> int32_t {
    if (op != 0) return -1;
    if (a < 0) return b;
    if (b < 0) return a;
    if (a == STRING || b == STRING) return STRING;
    if (a == NUMBER && b == NUMBER) return NUMBER;
    return -1;
  };
  return o;
}

int main(int, char**) {
  const TypeOracle oracle = Oracle();

  // --- the table parses, and rejects nonsense ---
  {
    TypeTable t; std::string err;
    check(TypeTable::Parse("name enter=inherit finish=produce_text\n", &t, &err),
          "a well-formed line parses");
    check(t.Lists("name") && !t.Lists("absent"), "only listed rules are listed");

    check(!TypeTable::Parse("name finish=nonsense\n", &t, &err), "an unknown action is refused");
    check(!TypeTable::Parse("name finish=produce\n", &t, &err), "finish=produce needs a tag");
    check(!TypeTable::Parse("name finish\n", &t, &err), "a bare word is refused");
    check(!TypeTable::Parse("a finish=pass\na finish=pass\n", &t, &err),
          "a rule listed twice is refused");
    check(TypeTable::Parse("# only a comment\n\n   \n", &t, &err), "comments and blanks are fine");
  }

  // --- the real table, as it ships ---
  TypeTable table; std::string err;
  std::ifstream f(std::string(MASKGEN_DIR) + "/fragment.types");
  std::stringstream ss; ss << f.rdbuf();
  check(!ss.str().empty(), "fragment.types loads");
  check(TypeTable::Parse(ss.str(), &table, &err), "fragment.types parses: " + err);

  // The grammar and the table have to name the same rules: a row for a rule that was
  // renamed away is dead, and an underscored rule with no row is silently untyped.
  {
    std::ifstream gf(std::string(MASKGEN_DIR) + "/fragment.ebnf");
    std::stringstream gs; gs << gf.rdbuf();
    const std::string grammar = gs.str();
    std::vector<std::string> defined;
    for (size_t i = 0; i < grammar.size();) {
      size_t eol = std::min(grammar.find('\n', i), grammar.size());
      std::string line = grammar.substr(i, eol - i);
      i = eol + 1;
      if (line.empty() || line[0] != '_') continue;
      size_t end = line.find_first_not_of(
          "_abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789");
      if (end == std::string::npos) continue;
      if (line.find("::=", end) == std::string::npos) continue;
      defined.push_back(line.substr(0, end));
    }
    check(defined.size() == 16, "the grammar defines 16 underscored rules, found " +
                                    std::to_string(defined.size()));
    std::string missing;
    for (const auto& rule : defined) if (!table.Lists(rule)) missing += " " + rule;
    check(missing.empty(), "every underscored grammar rule has a row:" + missing);
    std::string extra;
    for (const auto& rule : table.ListedRules()) {
      if (std::find(defined.begin(), defined.end(), rule) == defined.end()) extra += " " + rule;
    }
    check(extra.empty(), "every row names a rule the grammar defines:" + extra);
  }

  // Rules whose text comes from the symbol table, and rules the environment gates.
  {
    check(table.RowFor("_name").from_lexicon && table.RowFor("_member_name").from_lexicon,
          "names are drawn from the symbol table");
    check(!table.RowFor("_num_lit").from_lexicon, "a literal is not");
    check(table.RowFor("_call_step").gated && table.RowFor("_member_step").gated,
          "a continuation step is gated by the environment");
    check(!table.RowFor("_postfix").gated, "an ordinary rule is not gated");
  }

  // An unlisted rule passes the requirement down and adopts the first type below it.
  {
    const Types inside = table.OnEnter("eq", Types{STRING, -1}, oracle);
    check(inside == Types{STRING, -1}, "an unlisted rule inherits the requirement");
    const Types after = table.OnFinish("eq", Types{STRING, NUMBER}, Types{STRING, -1}, "", oracle);
    check(after.produced == NUMBER, "an unlisted rule carries a produced type up");
    const Types kept = table.OnFinish("eq", Types{STRING, NUMBER}, Types{STRING, BOOLEAN}, "", oracle);
    check(kept.produced == BOOLEAN, "...but does not overwrite one already there");
  }

  // A declaration's annotation sets the requirement from its own text.
  {
    const Types after = table.OnFinish("_type_ann", Types{}, Types{}, "string", oracle);
    check(after.required == STRING, "type_ann requires what its text names");
    const Types unknown = table.OnFinish("_type_ann", Types{}, Types{NUMBER, -1}, "wat", oracle);
    check(unknown.required == NUMBER, "an unrecognised annotation leaves the requirement alone");
  }

  // An expression starts fresh and may only finish when it satisfies its position.
  {
    const Types inside = table.OnEnter("_expr", Types{STRING, NUMBER}, oracle);
    check(inside == Types{STRING, -1}, "expr keeps the requirement but produces nothing yet");
    check(table.MayFinish("_expr", Types{STRING, STRING}, oracle), "expr may finish on a match");
    check(!table.MayFinish("_expr", Types{STRING, NUMBER}, oracle), "expr may not finish otherwise");
    check(table.MayFinish("_expr", Types{-1, NUMBER}, oracle), "with no requirement, expr may finish");
    check(!table.MayFinish("_expr", Types{STRING, -1}, oracle),
          "having produced nothing cannot satisfy a requirement either");
  }

  // Having passed its check, an expression still reports what it produced -- which
  // is how a parenthesised expression satisfies the position it sits in.
  {
    const Types after =
        table.OnFinish("_expr", Types{NUMBER, NUMBER}, Types{NUMBER, -1}, "", oracle);
    check(after.produced == NUMBER, "a checked expression carries its type outward");
  }

  // A name produces the type it was declared with.
  {
    const Types after = table.OnFinish("_name", Types{}, Types{NUMBER, -1}, "msg", oracle);
    check(after.produced == STRING, "name produces the declared type of its text");
    const Types undeclared = table.OnFinish("_name", Types{}, Types{NUMBER, -1}, "nope", oracle);
    check(undeclared.produced == -1, "an undeclared name produces nothing");
  }

  // A continuation retypes the whole expression. This is the row whose absence made
  // `let a : number = msg.length` fail: the receiver's string survived the member.
  {
    const Types after =
        table.OnFinish("_trailer", Types{NUMBER, NUMBER}, Types{NUMBER, STRING}, "", oracle);
    check(after.produced == NUMBER, "a trailer replaces the receiver's type");
    const Types member =
        table.OnFinish("_member_step", Types{NUMBER, STRING_ARRAY}, Types{-1, STRING}, "", oracle);
    check(member.produced == STRING_ARRAY, "a member step replaces it too");
    const Types empty = table.OnFinish("_trailer", Types{NUMBER, -1}, Types{NUMBER, STRING}, "", oracle);
    check(empty.produced == STRING, "a trailer that produced nothing changes nothing");
  }

  // A comparison is a boolean whatever its operands, and its operands are free.
  {
    const Types after = table.OnFinish("_cmp", Types{STRING, STRING}, Types{STRING, -1}, "", oracle);
    check(after.produced == BOOLEAN, "a comparison produces boolean, not its operand's type");
    const Types operand = table.OnEnter("_cmp_operand", Types{BOOLEAN, -1}, oracle);
    check(operand.required == -1, "a comparison's operand carries no requirement");
    check(table.FixedProduced("_cmp", oracle) == BOOLEAN, "a comparison's type is known in advance");
    check(table.FixedProduced("_name", oracle) == -1, "a name's type is not");
  }

  // A rule may also impose a requirement by fiat, with no text to read: the
  // condition of an `if` is a boolean whatever surrounds it. The fragment has no
  // such rule yet, so this is checked on an inline table.
  {
    TypeTable t; std::string err;
    check(TypeTable::Parse("_cond enter=require boolean finish=check\n", &t, &err),
          "enter=require parses");
    TypeTable bad;
    check(!TypeTable::Parse("_cond enter=require\n", &bad, &err), "enter=require needs a tag");
    const Types inside = t.OnEnter("_cond", Types{STRING, STRING}, oracle);
    check(inside == Types{BOOLEAN, -1}, "a required position ignores what encloses it");
    check(t.FixedRequired("_cond", oracle) == BOOLEAN, "its requirement is known in advance");
    const Types unknown_tag = t.OnEnter("_absent", Types{STRING, NUMBER}, oracle);
    check(unknown_tag == Types{STRING, NUMBER}, "an unlisted rule still inherits");
  }

  // Literals.
  {
    check(table.OnFinish("_num_lit", Types{}, Types{}, "3", oracle).produced == NUMBER,
          "a numeric literal produces number");
    check(table.OnFinish("_str_lit", Types{}, Types{}, "\"hi\"", oracle).produced == STRING,
          "a string literal produces string");
    check(table.FixedProduced("_bool_lit", oracle) == BOOLEAN,
          "a boolean literal's type is known in advance");
  }

  // `+` is typed from both operands. This is the only action that looks at two
  // types at once, and the reason it exists: no single operand decides the result.
  {
    const auto plus = [&](int32_t accumulated, int32_t operand) {
      return table.OnFinish("_primary", Types{-1, operand}, Types{-1, accumulated}, "", oracle)
          .produced;
    };
    check(plus(NUMBER, NUMBER) == NUMBER, "number + number is a number");
    check(plus(STRING, STRING) == STRING, "string + string is a string");
    check(plus(NUMBER, STRING) == STRING, "number + string is a string");
    check(plus(STRING, NUMBER) == STRING, "string + number is a string too");
    check(plus(-1, NUMBER) == NUMBER, "the first operand stands alone");
    check(plus(BOOLEAN, NUMBER) == BOOLEAN,
          "a combination the environment rejects leaves the position as it was");
    TypeTable bad_combine;
    check(!TypeTable::Parse("_a finish=combine\n", &bad_combine, &err),
          "finish=combine needs an operator");
  }

  printf("\n%d failure(s)\n", failures);
  return failures == 0 ? 0 : 1;
}
