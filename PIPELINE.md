# The pipeline, and exactly what we add to XGrammar

No handwaving. What the three files are, what runs when, every change made to
XGrammar, and the audit of XGrammar's own shortcuts against what we depend on.

## Part 1 — three files, three jobs

Everything used to live in one place: the type system was encoded in grammar rule
names (`__bind_type`, `__lex_name`, `__check_expr`), read in three different parts
of the code. Every new construct needed another naming convention, and the type
rules could not be tested without generating text and inspecting masks. Now:

| file | job | tested by |
|---|---|---|
| `maskgen/fragment.ebnf` | what text is well formed. **No type is named in it** | plain parsing |
| `maskgen/fragment.types` | what each rule does to the types, one line per rule | `test_type_table.cc`, 44 checks, no parser involved |
| `maskgen/cpp/type_table_bind.h` | the only file that knows about both | refuses to build when they disagree |

Two values travel with the parser at every position. **REQUIRED** is what this
position must end up producing, and comes from above: `let x : string =` requires a
string of everything after it. **PRODUCED** is the type of what has been written
here so far, and comes from below: after `count` it is number, after
`count.toFixed` it is string.

Each line of `fragment.types` says what one rule does to those two:

```
_type_ann       finish=require_text            the annotation sets the requirement
_expr           enter=fresh   finish=check      a new value, checked when it ends
_cmp            finish=produce boolean          a comparison, whatever its operands
_cmp_operand    enter=require_none              the operands are free of it
_name           finish=produce_text  content=lexicon
_trailer        finish=replace                  each step retypes what precedes it
_call_step      finish=replace  gate=environment
```

`enter=` is one of inherit / fresh / require_none / require &lt;tag&gt;. `finish=` is one
of pass / keep_first / replace / produce &lt;tag&gt; / produce_text / require_text /
check / combine &lt;operator&gt;. A rule with no line behaves as `enter=inherit finish=keep_first`, which is
right for an ordinary syntactic rule: pass the requirement down, adopt the first
type produced beneath it.

A tag such as `boolean` is a name the **environment** resolves. The grammar names
constructs and the table names tags; neither contains a type.

### The one convention left in the grammar

A rule name beginning with `_` means **something outside this file names this rule**.
Nothing more. Optimisation folds short rules into their users and deletes them,
which is silent: the rule vanishes, its line in `fragment.types` applies to nothing,
and the position quietly behaves as if it had no types attached. That caught three
rules during development. The fifteen underscored rules and the fifteen table lines
are checked against each other.

## Part 2 — what runs when

### Once, before the model starts

1. **Close the type graph** over the member tables: for each pair `(produced,
   required)`, whether the first can still become the second. 125 types, 750 edges,
   7 KB as bitsets, 18 ms of Python. Offline, and independent of any program.
2. **Compile the grammar.** XGrammar builds automata and, for each grammar position,
   a three-way split of the vocabulary: accepted, rejected, uncertain. 21 ms.
3. **Read `fragment.types`, turn it into one record per rule, install it**
   (`SetRuleTypeTransitions`), along with the resolver, the acceptor, the name-group
   resolver and the gate predicate.

### Whenever a declaration lands

4. `SetLexiconNames(tag, names)` for the one affected type. No recompilation, no
   closure update. Per-type name lists are a median of 4 tokens, at most 19.
   **Not yet wired to the grammar's own declarations — see Part 5.**

### Every decoding step

5. **XGrammar walks the live parser positions and unions their masks**: accepted
   tokens straight from the compiled table, uncertain ones decided by feeding their
   bytes through the parser. This is XGrammar's, unchanged.
6. **Our checks fire during that walk:**
   - entering a rule, its `enter=` action sets the two values
   - inside a `content=lexicon` rule, a byte is refused unless the text so far still
     begins some usable declared name
   - such a rule may only finish where the text is exactly a declared name
   - a `finish=check` rule may only finish when PRODUCED satisfies REQUIRED, and
     then carries PRODUCED outward: checking is a refusal, not a transformation
   - a `gate=environment` rule is refused at entry when the environment says so
   - finishing, a rule's `finish=` action updates the enclosing position
7. **A rule whose type is fixed is refused at entry** where that type can never
   reach the requirement. This is what keeps the mask honest: without it the model
   is offered a first token it could never finish legally.

   Two different questions live here, and conflating them is a bug either way.
   *Assignability* — may this expression **stop** here — is exact: a number is not a
   string. *Reachability* — may it **start** here — is looser: a number begins
   `3 + msg`, which is a string. The gate asks reachability, the check at the end
   asks assignability. Asking the exact question at entry refused legal programs
   until `finish=combine` made the difference visible.

## Part 3 — the complete list of changes to XGrammar

Branch `typed-lexicon`.

### New: `include/xgrammar/type_transition.h`

The record. Per rule: an enter action, a finish action, a fixed type, a required
type, whether its content comes from the caller, whether the caller gates it.
XGrammar never interprets the two values — they are integers it carries and compares
only through the caller's acceptor, and it does not care whether they mean types.

### New: `cpp/dynamic_lexicon.{h,cc}`

The runtime symbol table. Names grouped by tag; which groups a tag draws from;
"does this prefix still match a name", "is this text exactly a name", "which tokens
start a name here". Start-of-name token lists are sorted id lists, not bitsets — the
dense version cost 30.6 µs to deliver 11 tokens, the sparse one 0.22 µs.

### `cpp/earley_parser.{h,cc}` — the parser

| change | what it is |
|---|---|
| two fields on `ParserState` | `need_type`, `have_type`. In the parsing comparison, so two readings needing different types do not merge. Not in the mask-cache key — see Part 4. |
| `AtElement`, `KeepingRepeatCount` | a refactor: the struct was brace-initialised positionally at 14 sites, and adding a field by hand at each is where a silent corruption would live |
| five virtual hooks | `OnRuleEntered`, `OnRuleCompleted`, `MayScanByte`, `MayCompleteRule`, `MayPredictRule`, each with a `Has*` guard so a grammar not using them pays nothing |
| `RuleNeedsCompletionEvent` | beside the existing `RuleNeedsCaptureEvent`, and for the same reason |
| `RuleContentIsExternal` | static, because compilation needs it before any caller records exist |
| removed | the `__req_<n>` name parsing, the set of rules that reset a value, `NeedTypeForRule`. All of it is now the caller's record. |

### `cpp/grammar_matcher.cc` — the matcher

Holds the records and the symbol table, and implements every hook from them: one
switch for entering, one for finishing, and the three refusals. Byte tracking is on
whenever a rule reads its own text, and each byte enters the speculative buffer
**before** the step that examines it — that ordering was wrong three times, once per
code path. Also: the resolver is told the carried values, not just the text, so a
member name resolves against its receiver.

### `cpp/grammar_compiler.cc` — the compiler

| change | why |
|---|---|
| an external-content rule's mask becomes all-uncertain | its compiled answer is wrong by construction: the grammar accepts every identifier-shaped token while the legal names appear only at generation time |
| a token whose walk enters such a rule becomes uncertain | accepted tokens are unioned in wholesale and never walked, so they escape every check. A real bug: 41,556 tokens offered where 10 were legal |

### `cpp/grammar_functor.cc` — the optimiser

A rule named from outside the grammar is exempt from inlining, alongside the
budgeted, capture, lazy and temperature rules already exempt for the same reason.

### `include/xgrammar/matcher.h` — the public surface

```cpp
void SetRuleTypeTransitions(std::vector<RuleTypeTransition> transitions);
void SetLexiconNames(int32_t tag, std::vector<std::string> names);
void SetLexiconReachableTags(int32_t tag, std::vector<int32_t> source_tags);
void SetTypeResolver(...);        // text + carried values -> a type
void SetTypeAcceptor(...);        // does produced satisfy required
void SetLexiconTagResolver(...);  // which name group this position draws from
void SetStepPredicate(...);       // may this gated rule be entered
int32_t GetRuleId(const std::string& name) const;
```

## Part 4 — XGrammar's shortcuts, audited against what we need

XGrammar is free to discard anything that does not affect *"is this byte legal
here"*. We need two further things: **where a rule started** (to recover the text it
has matched) and **that its completion happens** (to apply its finish action). Every
shortcut was checked against both. Three were already broken and fixed; two were
latent and are now guarded.

| shortcut | verdict |
|---|---|
| folding a short rule into its user and deleting it | **broke us.** The rule's table line applied to nothing, silently. Fixed: a rule named from outside the grammar is not folded. Caught three rules. |
| skipping a parent's completion when the child is in tail position | **broke us.** `_cmp`'s "produce boolean" never ran, so `a == b` was typed as `a`. Fixed: `RuleNeedsCompletionEvent`, beside the identical guard captures already had. |
| swallowing a whole token in one step | **broke us.** One parser position for the whole token, so a rule starting mid-token had nothing to measure from: the name in `␠count` read as `␠count` and matched nothing. Fixed: grammars that read their text go byte by byte. Costs nothing — that pass already ran on every token. |
| jump-forward: emitting bytes the grammar forces, without asking the model | **latent.** It decides from the grammar's character edges alone and never consults our veto, so a grammar whose identifier rule narrowed to one letter would emit a byte nobody allowed. Cannot fire on this grammar — an identifier rule always permits many letters, so nothing is forced. Guarded: no jumping inside a rule whose content comes from the caller. |
| stepping over a rule that can match the empty string | **latent.** The rule is never entered or completed, so its action does not run. Correct for five of the seven actions — a rule that matched nothing produced nothing, which is exactly why `_trailer*` leaves the receiver's type alone. Wrong for two: `produce` would have produced regardless of text, and `check` is a refusal that simply would not happen. Guarded: such a line on such a rule now fails loudly at install time. |
| the mask cache ignoring the two values | **safe, for a reason.** The cache maps a syntactic position to its continuations. The types decide *which positions exist* rather than what follows one: a `;` is only reachable if the expression was permitted to finish, and that permission is the check. |
| state de-duplication | **safe.** Both values are in the parsing comparison, so two readings differing only by type are not merged. |
| rollback | **safe.** It truncates the byte history by exactly the number of positions the byte path created, and we always take that path. |
| subtree pruning during mask generation | **safe.** One rejected prefix eliminates a whole alphabetical block, and our refusal is itself a prefix test: if no name begins `couz`, none begins `couza`. |

## Part 5 — measurements, and what is not done

Measured on the fragment, with three names and six types in scope:

```
mask where a name is being written     125 us
mask at the start of an expression     189 us    (402 tokens offered)
mask at a member position               85 us    (10 or 11 tokens offered)
mask on a plain grammar, for scale       0.6 us
```

A model step is about 33,000 µs, so even the worst of these is well under 1% of
generation. The overhead figure against plain XGrammar on an identical grammar
(previously 61.1 µs against 56.0 µs, about 9%) was measured on the superseded
design and **needs re-measuring**.

Tests: 44 checks on the table with no parser involved, 25 end-to-end checks driving
the real grammar with real vocabulary tokens — a six-statement program accepted 51
tokens out of 51, ten ill-typed programs each blocked at a named position, eight
well-typed ones accepted. Seven test files, all passing.

Not done:

1. **Declarations do not extend the symbol table.** The model can write
   `let x : number = 3;` and then cannot use `x`, because nothing records that it
   now exists. The usable names are only those seeded in advance. This is the
   feature the whole mechanism exists for.
2. **No test that the two input paths agree.** XGrammar can be fed a program
   character by character or token by token, and a model only ever does the second.
   Every test used the first and passed while the second was entirely broken. The
   fix for the category is a test that does both and demands identical masks at
   every step — then the next shortcut that behaves differently for tokens fails the
   day it lands, rather than weeks later.
3. **Calls have never run in the positive direction.** `f(x)` is always refused,
   because nothing in the environment is callable, so the half of the type graph
   that comes from calling a function is untested. It needs function types in the
   environment, and one further action: the required type of an argument is computed
   from whatever the function turned out to be, where `enter=require` only names a
   fixed type. `finish=combine` is the first action that derives a value at runtime,
   so the shape is now established.
