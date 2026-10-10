// A whole program, one token at a time, through the real grammar file.
//
// Every other test here uses a small grammar written inline to isolate one
// mechanism. This one uses maskgen/fragment.ebnf as it ships, and drives the
// matcher the way a decoder would: the text is cut into vocabulary tokens by
// longest match, and at every step the token the program needs must be in the
// mask before it is accepted. Tokens that straddle a grammar boundary -- ` msg`
// carries the space, the name, and nothing else -- are therefore exercised
// throughout rather than being set up deliberately.
//
// The environment starts with three names and GROWS: a finished `let` statement puts
// its name in scope, so a later statement can use it. The programs below rely on
// that in places.
//
//   count, total : number        msg : string
//
// A member access hands back the member, and a method is a FUNCTION: `msg.split` is
// "takes a string, gives a string[]", so only `msg.split("x")` is a string[]. A
// property is not a function: `msg.length` is a number on its own. That is why some
// of these have parentheses and some do not.
#include <xgrammar/xgrammar.h>
#include <dlpack/dlpack.h>

#include "type_table.h"
#include "type_table_bind.h"
#include <cstdio>
#include <fstream>
#include <map>
#include <sstream>
#include <string>
#include <unordered_map>
#include <vector>

using namespace xgrammar;
using namespace maskgen;


#include "fragment_env.h"

static int failures = 0;
static void check(bool ok, const std::string& what) {
  printf("%s  %s\n", ok ? "pass" : "FAIL", what.c_str());
  if (!ok) ++failures;
}

int main(int, char** argv) {
  auto vocab = LoadVocab(argv[1]);
  const int V = static_cast<int>(vocab.size());
  TokenizerInfo ti(vocab, VocabType::BYTE_LEVEL, V, std::vector<int32_t>{151643});
  const auto& decoded = ti.GetDecodedVocab();
  std::unordered_map<std::string, int32_t> by_text;
  for (int32_t i = 0; i < V; ++i) by_text.emplace(decoded[i], i);

  std::ifstream gf(std::string(MASKGEN_DIR) + "/fragment.ebnf");
  std::stringstream gs; gs << gf.rdbuf();
  check(!gs.str().empty(), "the real grammar file loads");
  GrammarCompiler compiler(ti, 8, false);
  auto compiled = compiler.CompileGrammar(gs.str(), "root");

  TypeTable table;
  {
    std::ifstream tf(std::string(MASKGEN_DIR) + "/fragment.types");
    std::stringstream ts; ts << tf.rdbuf();
    std::string error;
    check(TypeTable::Parse(ts.str(), &table, &error), "fragment.types parses: " + error);
  }

  Environment env;
  RuleIds rules;
  {
    GrammarMatcher probe(compiled);
    rules.member_name = probe.GetRuleId("_lex_member");
    rules.call_step = probe.GetRuleId("_call_step");
    rules.member_step = probe.GetRuleId("_member_step");
    rules.arr_lit = probe.GetRuleId("_arr_lit");
    check(rules.member_name >= 0 && rules.call_step >= 0 && rules.member_step >= 0 &&
              rules.arr_lit >= 0,
          "the grammar defines the rules this environment recognises");
    std::vector<RuleTypeTransition> transitions;
    std::string error;
    check(BuildTransitions(table, probe, OracleFor(env), &transitions, &error),
          "every row of the table names a rule of the grammar: " + error);
  }

  // A well-typed program. Each statement needs a different part of the machinery:
  // a name of the required type, a member access that converts one type to
  // another, a comparison, a literal, and a sum.
  const std::string program =
      "let a : string = msg ;\n"
      "let b : number = msg.length ;\n"
      "let c : boolean = count == total ;\n"
      "let d : string = count.toString() ;\n"
      "let e : number = 3 + count ;\n"
      "let f : string[] = msg.split(msg) ;\n"
      "let g : string = msg.split(msg).join(msg) ;\n"
      "let h : number[] = [ 1 , 2 , 3 ] ;\n";

  auto tokens = Tokenize(program, by_text);
  check(!tokens.empty(), "the program cuts into vocabulary tokens");

  {
    GrammarMatcher m(compiled);
    Install(m, env, table, rules);
    Bitmask mask(V);
    size_t accepted = 0;
    std::string refused_at;
    for (int32_t id : tokens) {
      m.FillNextTokenBitmask(&mask.t, 0);
      if (!mask.allows(id)) {
        refused_at = program.substr(0, 0);
        for (size_t k = 0; k < accepted; ++k) refused_at += decoded[tokens[k]];
        refused_at += " <<[" + decoded[id] + "]";
        break;
      }
      if (!m.AcceptToken(id)) { refused_at = "AcceptToken disagreed with the mask"; break; }
      ++accepted;
    }
    check(accepted == tokens.size(),
          "every token of the program is in the mask (" + std::to_string(accepted) + "/" +
              std::to_string(tokens.size()) + ")" +
              (refused_at.empty() ? "" : "  stopped: " + refused_at));
    check(m.IsTerminated() || accepted == tokens.size(), "the program is complete");
  }

  // Each of these is grammatical and every one is a type error. The mask must stop
  // the program somewhere, and the position is named so a change of behaviour shows
  // up as a different position rather than a silent pass.
  const std::vector<std::pair<std::string, std::string>> ill_typed = {
      {"let a : string = count ;\n", "count is a number, so the ';' must be refused"},
      {"let a : number = msg ;\n", "msg is a string, so the ';' must be refused"},
      {"let a : number[] = msg ;\n", "nothing reaches number[] from a string"},
      {"let a : string = 3 ;\n", "a numeric literal is not a string"},
      {"let a : number = true ;\n", "a boolean literal is not a number"},
      {"let a : number = count == total ;\n", "a comparison is a boolean, not a number"},
      {"let a : string = count.valueOf() ;\n", "valueOf gives a number, not a string"},
      {"let a : number = count() ;\n", "count is not a function, so it cannot be called"},
      {"let a : number = count.nosuch ;\n", "number has no member called nosuch"},
      {"let a : string = msg.toUpperCase ;\n",
       "the method itself is a function, not the string it would return"},
      {"let a : string = msg.toUpperCase(msg) ;\n",
       "toUpperCase takes nothing, so an argument must be refused"},
      {"let a : string[] = msg.split() ;\n",
       "split takes a string, so no argument must be refused"},
      {"let a : string[] = msg.split(3) ;\n", "split takes a string, not a number"},
      {"let a : number = msg.length() ;\n", "length is a property, so it cannot be called"},
      {"msg = count ;\n", "msg is a string, so assigning a number must be refused"},
      {"count = msg ;\n", "count is a number, so assigning a string must be refused"},
      {"let a : number = ( msg ) ;\n", "a parenthesised string is still not a number"},
      {"let a : string = ( count ) ;\n", "nor the other way round"},
      {"let a:string=count;", "the same holds with no whitespace at all"},
      {"let a : boolean[] = msg ;\n", "nothing here can produce a boolean[] at all"},
      {"let a : number = 3 + msg ;\n", "a number plus a string is a string, not a number"},
      {"let a : number = [ 1 ] ;\n", "an array literal is not a number"},
      {"let a : number[] = [ msg ] ;\n", "an array of strings is not an array of numbers"},
      {"let a : number[] = [ 1 , msg ] ;\n", "a mixed array has no type at all"},
      {"let a : boolean = true + true ;\n", "booleans do not add"},
      // A declared name must be new. Refused when the name ENDS, not per byte: the
      // prefix of a taken name is a fine start for a free one.
      {"let count : string = msg ;\n", "count is already in scope"},
      {"let msg : boolean = true ;\n", "so is msg, whatever type it is given"},
      {"let let : number = 3 ;\n", "let is a reserved word"},
      {"let number : number = 3 ;\n", "so is a type's name"},
  };
  for (const auto& [text, why] : ill_typed) {
    auto ill = Tokenize(text, by_text);
    GrammarMatcher m(compiled);
    Install(m, env, table, rules);
    Bitmask mask(V);
    bool blocked = false;
    for (int32_t id : ill) {
      m.FillNextTokenBitmask(&mask.t, 0);
      if (!mask.allows(id) || !m.AcceptToken(id)) { blocked = true; break; }
    }
    check(blocked, why);
  }

  // The same programs with the requirement satisfied, to show the refusals above
  // come from the types and not from a syntax the grammar simply cannot express.
  const std::vector<std::string> well_typed = {
      "let a : number = count ;\n",
      "let a : string = msg ;\n",
      "let a : number = msg.length ;\n",
      "let a : number = 3 ;\n",
      "let a : boolean = true ;\n",
      "let a : boolean = count == total ;\n",
      // Calls. A method is a function; calling it is what changes the type.
      "let a : string = msg.toUpperCase() ;\n",
      "let a : number = count.valueOf() ;\n",
      "let a : string = count.toString() ;\n",
      "let a : string = count.toFixed(count) ;\n",
      "let a : string[] = msg.split(msg) ;\n",
      // A chain: string -> (string -> string[]) -> string[] -> number
      "let a : number = msg.split(msg).length ;\n",
      "let a : string = msg.split(msg).join(msg) ;\n",
      // Assignment, where the requirement comes from the target's declared type.
      "msg = msg.toUpperCase() ;\n",
      "msg = count.toString() ;\n",
      "count = msg.length ;\n",
      // Parentheses, the only place enter_have=none applies outside an argument.
      "let a : number = ( count ) ;\n",
      "let a : string = ( msg.toUpperCase() ) ;\n",
      "let a : number = ( count ) + count ;\n",
      "declare function f ( a : number ) : string ;\n",
      "let a:number=count;msg=count.toString() ;",
      // Array literals, typed by folding their elements together.
      "let a : number[] = [ 1 ] ;\n",
      "let a : number[] = [ 1 , 2 , 3 ] ;\n",
      "let a : string[] = [ msg , msg ] ;\n",
      "let a : boolean[] = [ true , false ] ;\n",
      "let a : number[] = [ count , 3 ] ;\n",
      "let a : string = msg + 3 ;\n",
      "let a : string = 3 + msg ;\n",
      "let a : number = count + count ;\n",
      "let a : string = msg + msg ;\n",
      // A new name that merely extends one in scope is fine, which is what makes the
      // check belong at the end of the name rather than on its bytes.
      "let counter : number = 3 ;\n",
      "let co : number = 3 ;\n",
      "let fresh : number = 3 ;\n",
  };
  for (const auto& text : well_typed) {
    auto ok = Tokenize(text, by_text);
    GrammarMatcher m(compiled);
    Install(m, env, table, rules);
    Bitmask mask(V);
    bool all = true;
    std::string got;
    for (int32_t id : ok) {
      m.FillNextTokenBitmask(&mask.t, 0);
      if (!mask.allows(id) || !m.AcceptToken(id)) {
        all = false;
        got += " <<[" + decoded[id] + "]";
        break;
      }
      got += decoded[id];
    }
    check(all, "accepted: " + text.substr(0, text.size() - 1) +
                   (all ? "" : "  stopped: " + got));
  }

  // ---- declarations reaching the symbol table ----
  //
  // The environment is copied per case, so one program's declarations cannot leak
  // into another's. Each case is a program plus the one thing about it that matters.
  {
    GrammarMatcher m(compiled);
    Environment growing = env;
    Install(m, growing, table, rules);
    Declarations declared(&growing, &m);
    Bitmask bm(V);
    const std::string program =
        "let n : number = 3 ;\n"
        "let s : string = n.toString() ;\n"
        "let t : string = s + n ;\n";
    auto program_tokens = Tokenize(program, by_text);
    std::vector<std::string> registered;
    bool all = true;
    std::string got;
    for (int32_t id : program_tokens) {
      m.FillNextTokenBitmask(&bm.t, 0);
      if (!bm.allows(id) || !m.AcceptToken(id)) {
        all = false;
        got += " <<[" + decoded[id] + "]";
        break;
      }
      got += decoded[id];
      for (const auto& [name, type] : declared.Poll()) {
        registered.push_back(name + ":" + kTypeNames[type]);
      }
    }
    check(all, "a program that uses the names it declares parses" +
                   (all ? "" : "  stopped: " + got));
    check(registered.size() == 3, "three names were registered, got " +
                                      std::to_string(registered.size()));
    if (registered.size() == 3) {
      check(registered[0] == "n:number", "n registered as a number, got " + registered[0]);
      check(registered[1] == "s:string", "s registered as a string, got " + registered[1]);
      check(registered[2] == "t:string", "t registered as a string, got " + registered[2]);
    }
    check(growing.symbols.count("n") == 1 && growing.symbols.count("s") == 1,
          "the environment grew");
    check(env.symbols.count("n") == 0, "and the original environment did not");
  }

  // The blacklist grows with the program: a name is free until it is declared, and
  // taken afterwards. Without registration feeding it, this could not be tested at
  // all -- which is why it comes last.
  {
    GrammarMatcher m(compiled);
    Environment growing = env;
    Install(m, growing, table, rules);
    Declarations declared(&growing, &m);
    Bitmask bm(V);
    auto first = Tokenize("let fresh : number = 3 ;\n", by_text);
    bool ok = true;
    for (int32_t id : first) {
      m.FillNextTokenBitmask(&bm.t, 0);
      if (!bm.allows(id) || !m.AcceptToken(id)) { ok = false; break; }
      declared.Poll();
    }
    check(ok, "a free name can be declared");
    // Now the same name is taken, so declaring it again must be refused.
    auto again = Tokenize("let fresh : string = msg ;\n", by_text);
    bool blocked = false;
    for (int32_t id : again) {
      m.FillNextTokenBitmask(&bm.t, 0);
      if (!bm.allows(id) || !m.AcceptToken(id)) { blocked = true; break; }
      declared.Poll();
    }
    check(blocked, "and declaring it a second time is refused");
  }

  // A `declare function` puts a function type in scope, which is the only way the
  // type GRAPH gains an edge the model wrote. Everything else -- members, operators,
  // literals -- was fixed before generation started.
  //
  // The chosen edge is number -> boolean, because nothing in the starting environment
  // reaches boolean from anything: no name has that type and no member returns one.
  // So the second statement here is impossible until the first one is written.
  {
    GrammarMatcher m(compiled);
    Environment growing = env;
    Install(m, growing, table, rules);
    Declarations declared(&growing, &m);
    Bitmask bm(V);
    std::vector<std::string> registered;
    const std::string program =
        "declare function g ( a : number ) : boolean ;\n"
        "let y : boolean = g(count) ;\n";
    bool all = true;
    std::string got;
    for (int32_t id : Tokenize(program, by_text)) {
      m.FillNextTokenBitmask(&bm.t, 0);
      if (!bm.allows(id) || !m.AcceptToken(id)) {
        all = false;
        got += " <<[" + decoded[id] + "]";
        break;
      }
      got += decoded[id];
      for (const auto& [name, type] : declared.Poll()) {
        registered.push_back(name + (IsCallable(type) ? ":function" : ":value"));
      }
    }
    check(all, "a function the program declared can then be called" +
                   (all ? "" : "  stopped: " + got));
    check(registered.size() == 2 && registered[0] == "g:function",
          "g was registered as a function");
  }

  // The same program without the declaration must fail, or the test above proves
  // nothing: it has to be the declaration that made the call possible.
  {
    GrammarMatcher m(compiled);
    Environment growing = env;
    Install(m, growing, table, rules);
    Declarations declared(&growing, &m);
    Bitmask bm(V);
    bool blocked = false;
    for (int32_t id : Tokenize("let y : boolean = g(count) ;\n", by_text)) {
      m.FillNextTokenBitmask(&bm.t, 0);
      if (!bm.allows(id) || !m.AcceptToken(id)) { blocked = true; break; }
      declared.Poll();
    }
    check(blocked, "and without the declaration the same call is refused");
  }

  // A declared function is checked like anything else.
  for (const auto& [text, why] : std::vector<std::pair<std::string, std::string>>{
           {"declare function f ( a : number ) : string ;\nlet s : number = f(3) ;\n",
            "f returns a string, not a number"},
           {"declare function f ( a : number ) : string ;\nlet s : string = f(msg) ;\n",
            "f takes a number, not a string"},
           {"declare function f ( a : number ) : string ;\nlet s : string = f() ;\n",
            "f takes a number, so no argument is refused"},
           {"declare function f ( a : number ) : string ;\nlet s : string = f ;\n",
            "f itself is a function, not a string"},
           {"declare function count ( a : number ) : string ;\n",
            "count is already in scope, so the function's name is refused too"},
       }) {
    GrammarMatcher m(compiled);
    Environment growing = env;
    Install(m, growing, table, rules);
    Declarations declared(&growing, &m);
    Bitmask bm(V);
    bool blocked = false;
    for (int32_t id : Tokenize(text, by_text)) {
      m.FillNextTokenBitmask(&bm.t, 0);
      if (!bm.allows(id) || !m.AcceptToken(id)) { blocked = true; break; }
      declared.Poll();
    }
    check(blocked, why);
  }

  // A declared name is NOT in scope inside its own declaration: until the statement
  // ends there is nothing to register, so `let x : number = x ;` cannot parse.
  {
    GrammarMatcher m(compiled);
    Environment growing = env;
    Install(m, growing, table, rules);
    Declarations declared(&growing, &m);
    Bitmask bm(V);
    auto toks = Tokenize("let x : number = x ;\n", by_text);
    bool blocked = false;
    for (int32_t id : toks) {
      m.FillNextTokenBitmask(&bm.t, 0);
      if (!bm.allows(id) || !m.AcceptToken(id)) { blocked = true; break; }
      declared.Poll();
    }
    check(blocked, "a name is not in scope inside its own declaration");
  }

  // A name declared with one type is not usable where another is required.
  {
    GrammarMatcher m(compiled);
    Environment growing = env;
    Install(m, growing, table, rules);
    Declarations declared(&growing, &m);
    Bitmask bm(V);
    auto toks = Tokenize("let flag : boolean = true ;\nlet n : number = flag ;\n", by_text);
    bool blocked = false;
    for (int32_t id : toks) {
      m.FillNextTokenBitmask(&bm.t, 0);
      if (!bm.allows(id) || !m.AcceptToken(id)) { blocked = true; break; }
      declared.Poll();
    }
    check(blocked, "a boolean name is refused where a number is required");
  }

  printf("\n%d failure(s)\n", failures);
  return failures == 0 ? 0 : 1;
}
