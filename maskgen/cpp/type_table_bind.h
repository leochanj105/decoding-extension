// Turning the type table into the per-rule records XGrammar applies.
//
// This is the only file that knows about both. type_table.h reads fragment.types
// and answers questions about it with no parser in sight; XGrammar applies one
// record per rule and knows nothing about where they came from. This joins them,
// and fails loudly if the two files have drifted apart.
#ifndef MASKGEN_TYPE_TABLE_BIND_H_
#define MASKGEN_TYPE_TABLE_BIND_H_

#include <xgrammar/xgrammar.h>

#include <string>
#include <vector>

#include "type_table.h"

namespace maskgen {

/*!
 * \brief Build the records for a compiled grammar from a table.
 *
 * Every rule the table names must exist in the grammar. A row for a rule that was
 * renamed or folded away is dead weight that would otherwise go unnoticed -- the
 * position would quietly behave as if it had no types attached -- so it is an error.
 *
 * `matcher` is only used to look rule names up; any matcher for the grammar will do.
 */
inline bool BuildTransitions(
    const TypeTable& table, const xgrammar::GrammarMatcher& matcher, const TypeOracle& oracle,
    std::vector<xgrammar::RuleTypeTransition>* out, std::string* error
) {
  out->clear();
  std::string missing;
  std::vector<std::pair<int32_t, Row>> found;
  int32_t highest = -1;
  for (const std::string& rule : table.ListedRules()) {
    const int32_t id = matcher.GetRuleId(rule);
    if (id < 0) {
      missing += " " + rule;
      continue;
    }
    const Row& row = table.RowFor(rule);
    found.emplace_back(id, row);
    highest = std::max(highest, id);

    // Writing a repetition splits a rule in two: `_postfix ::= _name _trailer*`
    // becomes _postfix and a generated _postfix_1 holding the repeated part. The
    // generated rule is a continuation of the one it came from and has to behave the
    // same way, or the repeated part's result is dropped on the way out -- which is
    // exactly how `msg.length` came back typed as a string.
    //
    // Only what the rule does on FINISHING is carried over. Entering is deliberately
    // left at the defaults, because a generated tail is a continuation of its parent
    // and must inherit: `_postfix` starts a new value and so takes enter_have=none,
    // but the tail holding its `.member` steps needs the receiver the atom produced.
    // Copying that across cost member access entirely.
    //
    // Its content does not come from the symbol table either, since a rule that
    // splits has rule references in it, and gating it again would ask the
    // environment twice about one position.
    std::vector<std::string> bases{rule};
    for (size_t i = 0; i < bases.size(); ++i) {
      for (int32_t n = 1;; ++n) {
        const std::string generated = bases[i] + "_" + std::to_string(n);
        const int32_t generated_id = matcher.GetRuleId(generated);
        if (generated_id < 0) {
          break;
        }
        Row carried;
        carried.finish = row.finish;
        carried.produce_tag = row.produce_tag;
        carried.combine_tag = row.combine_tag;
        found.emplace_back(generated_id, carried);
        highest = std::max(highest, generated_id);
        bases.push_back(generated);
      }
    }
  }
  if (!missing.empty()) {
    if (error) *error = "the grammar has no rule named:" + missing;
    return false;
  }
  out->assign(static_cast<size_t>(highest) + 1, xgrammar::RuleTypeTransition{});
  for (const auto& [id, row] : found) {
    xgrammar::RuleTypeTransition t;
    switch (row.need_on_enter) {
      case NeedOnEnter::kInherit: t.need_on_enter = xgrammar::TypeNeedOnEnter::kInherit; break;
      case NeedOnEnter::kNone: t.need_on_enter = xgrammar::TypeNeedOnEnter::kNone; break;
      case NeedOnEnter::kDerived:
        t.need_on_enter = xgrammar::TypeNeedOnEnter::kDerived;
        t.derive_op = oracle.resolve_op ? oracle.resolve_op(row.derive_tag) : -1;
        if (t.derive_op < 0) {
          if (error) *error = "the environment does not know the question \"" + row.derive_tag + "\"";
          return false;
        }
        break;
      case NeedOnEnter::kFixed:
        t.need_on_enter = xgrammar::TypeNeedOnEnter::kFixed;
        t.required_type = oracle.resolve ? oracle.resolve(row.require_tag) : -1;
        if (t.required_type < 0) {
          if (error) *error = "the environment does not know the type \"" + row.require_tag + "\"";
          return false;
        }
        break;
    }
    t.have_on_enter = row.have_on_enter == HaveOnEnter::kInherit
                          ? xgrammar::TypeHaveOnEnter::kInherit
                          : xgrammar::TypeHaveOnEnter::kNone;
        switch (row.finish) {
      case Finish::kPass: t.finish = xgrammar::TypeFinishAction::kPass; break;
      case Finish::kKeepFirst: t.finish = xgrammar::TypeFinishAction::kKeepFirst; break;
      case Finish::kReplace: t.finish = xgrammar::TypeFinishAction::kReplace; break;
      case Finish::kProduceText: t.finish = xgrammar::TypeFinishAction::kProduceText; break;
      case Finish::kRequireText: t.finish = xgrammar::TypeFinishAction::kRequireText; break;
      case Finish::kCheck: t.finish = xgrammar::TypeFinishAction::kCheck; break;
      case Finish::kProduceRequired:
        t.finish = xgrammar::TypeFinishAction::kProduceRequired;
        break;
      case Finish::kCombine:
        t.finish = xgrammar::TypeFinishAction::kCombine;
        t.combine_op = oracle.resolve_op ? oracle.resolve_op(row.combine_tag) : -1;
        if (t.combine_op < 0) {
          if (error) *error = "the environment does not know the operator \"" + row.combine_tag + "\"";
          return false;
        }
        break;
      case Finish::kProduce:
        t.finish = xgrammar::TypeFinishAction::kProduce;
        // The tag is resolved once, here. The grammar and the table name a
        // construct; only the environment turns that into a type.
        t.fixed_type = oracle.resolve ? oracle.resolve(row.produce_tag) : -1;
        if (t.fixed_type < 0) {
          if (error) *error = "the environment does not know the type \"" + row.produce_tag + "\"";
          return false;
        }
        break;
    }
    t.from_lexicon = row.from_lexicon;
    t.fresh_name = row.fresh_name;
    t.gated = row.gated;
    (*out)[id] = t;
  }
  return true;
}

}  // namespace maskgen

#endif  // MASKGEN_TYPE_TABLE_BIND_H_
