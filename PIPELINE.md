# The pipeline, and exactly what we add to XGrammar

No handwaving. Three parts: the grammar, what runs when, and the complete list of
changes to XGrammar with nothing omitted.

## Part 1 — the grammar

`maskgen/fragment.ebnf`. **No type is named anywhere in it.** One rule per
construct, never one per type — that is the whole reason the extension exists.

Five rule-name prefixes are read by the matcher rather than only by the parser:

| prefix | meaning |
|---|---|
| `__bind_<x>` | on completing, the text it matched decides the type **required** of what follows |
| `__have_<x>` | on completing, the text it matched decides the type it **produced** |
| `__lex_<x>` | its legal content comes from the runtime symbol table, so the grammar only says "an identifier goes here" |
| `__check_<x>` | it may only finish when the type produced satisfies the type required |
| `__req_<n>` | a fixed requirement baked into the rule name; superseded by `__bind_`, kept for tests |

A rule carrying one of these **must survive grammar optimisation**, because the
matcher finds it by name. `RuleInliner` exempts these prefixes. Forgetting that is
silent: the rule disappears, the matcher never sees it, and the position falls back
to the compiled mask. It has caught three rules so far.

## Part 2 — what runs when

### Once, before the model starts

1. **Build the type table.** Close the type graph over the member tables, giving for
   each pair `(have, need)` whether `have` can still become `need`, and which
   continuation starts that path. Measured: 125 types, 750 edges, 19 distinct
   first-step answers over 15,625 pairs. Python, offline.
2. **Compile the grammar.** XGrammar turns the EBNF into automata and, for every
   grammar position, a three-way split of the vocabulary: accepted, rejected,
   uncertain. Measured 21 ms, 0.83 MB.
3. **Hand the matcher its tables** — `SetTypeResolver`, `SetTypeAcceptor`,
   `SetLexiconReachableTags`.

### Whenever a declaration lands

4. **`SetLexiconNames(type, names)`** for the one affected type. Nothing else is
   touched: no recompilation, no closure update. Measured: the per-type name list is
   a median of 4 tokens, at most 19.

### Every decoding step

5. **XGrammar walks the live parser states** and unions their masks: accepted tokens
   straight from the compiled table, uncertain ones decided by feeding their bytes
   into the parser.
6. **During that walk, our checks fire per byte:**
   - inside a `__lex_` rule, a byte is vetoed unless the text so far still matches
     the start of some declared name of a usable type
   - a `__lex_` rule may only finish where the text is exactly a declared name
   - a `__check_` rule may only finish when the produced type satisfies the required
     type
7. **On each rule completion, our hook may bind a type** from the text that rule
   matched — the required type for what follows, or the type just produced.

Step 5 is XGrammar's, unchanged. Steps 6 and 7 are ours.

## Part 3 — the complete list of changes to XGrammar

Branch `typed-lexicon`. 1,114 lines added across 8 files.

### New file: `cpp/dynamic_lexicon.{h,cc}` (399 lines)

The runtime symbol table. Names grouped by type; which groups a type draws from;
queries for "does this prefix still match a name", "is this text exactly a name",
"which tokens start a name here". Start-of-name token lists are cached as sorted
id lists, not bitsets — the dense version cost 30.6 µs per call to deliver 11
tokens, the sparse one 0.22 µs.

### `cpp/earley_parser.h/.cc` — the parser

| change | what it is |
|---|---|
| two fields on `ParserState` | `need_type`, `have_type`. In the parsing hash and equality, so two readings requiring different types do not merge. **Not** in the mask-cache key, so the compiled cache is not multiplied. |
| `AtElement`, `KeepingRepeatCount` | a refactor, not a feature: the struct was brace-initialised positionally at 14 sites, and adding a field by hand at each is where a silent corruption would live |
| four virtual hooks | `OnRuleCompleted`, `MayCompleteRule`, `MayScanByte`, each with a `Has*` guard so a grammar not using them pays nothing |
| rule-name scan | which rules are `__lex_` or `__req_`, and which rules can reach a `__lex_` rule |
| `Scan` consults `MayScanByte` | one place; it is the single point at which a byte is offered to a state |
| `Complete` consults `MayCompleteRule` | at the top, so it applies during mask computation as well as a real advance |
| completion sites call `OnRuleCompleted` | including the FSM path, where the parent was already advanced at prediction time, so the hook belongs on the re-enqueue |
| the prediction site sets `need_type` | from the predicted rule, inherited inside it, exactly as the token and character budgets already are |

### `cpp/grammar_matcher.cc` — the matcher

Owns a `DynamicLexicon`, overrides the four hooks, and adds four public methods.
Also: byte tracking is enabled when a lexicon rule is present, and each byte is
pushed into the speculative buffer **before** advancing, so a check running during
the advance can see it. That ordering was wrong twice.

### `cpp/grammar_compiler.cc` — the compiler

| change | why |
|---|---|
| a `__lex_` rule's mask becomes all-uncertain | its compiled answer is wrong by construction: the grammar accepts every identifier-shaped token while the legal names appear only at runtime |
| a token whose walk enters a `__lex_` rule becomes uncertain | accepted tokens are unioned in wholesale and never walked, so they would escape every check. This was a real bug: 41,556 tokens offered where 10 were legal |
| the accept-without-walking shortcut is off for `__lex_` rules | it accepts a token without ever walking it |

### `cpp/grammar_functor.cc` — the optimiser

Marker-prefixed rules are exempt from inlining, alongside the budgeted, capture,
lazy and temperature rules already exempt for the same reason.

### `include/xgrammar/matcher.h` — the public surface

```cpp
void SetLexiconNames(int32_t tag, std::vector<std::string> names);
void SetLexiconReachableTags(int32_t tag, std::vector<int32_t> source_tags);
void SetTypeResolver(std::function<int32_t(int32_t rule_id, std::string_view matched)>);
void SetTypeAcceptor(std::function<bool(int32_t need_type, int32_t have_type)>);
```

Four methods. XGrammar never interprets a type id; the caller holds the type table.

## Part 4 — what this costs

Measured at the worst position of the fragment grammar:

```
with our type machinery      61.1 us
every marker renamed away    56.0 us     <- plain XGrammar, same grammar
overhead                          ~9%
```

A model step is about 33,000 µs, so the whole mechanism is well under 1% of
generation.

## Part 5 — what is NOT done

1. **A produced type does not propagate up through ordinary rules.** It is set on the
   completing rule's immediate parent only. The chain
   `__check_expr -> eq -> sum -> primary -> postfix -> __lex_name` therefore loses it,
   so `__check_expr` is asked to finish with no produced type and refuses. **A
   complete statement does not parse**: the final `;` of `let x : string = msg;` is
   rejected. This is the blocking bug.
2. **Member names are not wired.** `__lex_member` is looked up under the required
   type rather than the receiver's, so `count.toString()` cannot be written.
3. **Whitespace is fixed, and should not be.** Flexible formatting costs about 5% of
   generation time, which is affordable; the fixed-layout restriction was an
   overreaction to one slow position and should be reverted.
