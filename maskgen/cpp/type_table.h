// The type-transition table: how each grammar rule transforms the types carried
// through a parse. See maskgen/fragment.types for the table itself and what each
// action means.
//
// Nothing here knows about a parser, a grammar or a token. It takes two integers
// and a rule name and returns two integers, so it can be tested directly.
#ifndef MASKGEN_TYPE_TABLE_H_
#define MASKGEN_TYPE_TABLE_H_

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <functional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace maskgen {

/*! \brief The two types travelling with the parser at one position. */
struct Types {
  /*! \brief What this position must end up producing; -1 if unconstrained. */
  int32_t required = -1;
  /*! \brief What has been produced here so far; -1 if nothing yet. */
  int32_t produced = -1;

  bool operator==(const Types& o) const {
    return required == o.required && produced == o.produced;
  }
};

/*! \brief Where an occurrence's requirement comes from. */
enum class NeedOnEnter {
  kInherit,  // the enclosing position's requirement (default)
  kNone,     // none: anything may be produced here
  kFixed,    // a fixed type, named by a tag
};

/*! \brief What an occurrence starts out having produced. */
enum class HaveOnEnter {
  kInherit,  // carry on from the enclosing position (default)
  kNone,     // nothing: this is a new value
};

/*! \brief What happens when a rule finishes and the enclosing position moves on. */
enum class Finish {
  kPass,         // leave the enclosing position alone
  kKeepFirst,    // adopt what this produced, unless something was produced already
  kReplace,      // adopt what this produced, overwriting
  kProduce,      // produce a fixed type, named by a tag the environment resolves
  kProduceText,  // produce the type the environment gives for the matched text
  kRequireText,  // require the type the environment gives for the matched text
  kCheck,        // refuse to finish unless what was produced satisfies the
                 // requirement; having passed, behaves as kKeepFirst
  kCombine,      // ask the environment to combine what the enclosing position has
                 // produced with what this rule produced, under a named operator
};

/*! \brief One row of the table. */
struct Row {
  NeedOnEnter need_on_enter = NeedOnEnter::kInherit;
  HaveOnEnter have_on_enter = HaveOnEnter::kInherit;
  Finish finish = Finish::kKeepFirst;
  /*! \brief For kProduce: the tag the environment resolves, never a type. */
  std::string produce_tag;
  /*! \brief For NeedOnEnter::kFixed: likewise. */
  std::string require_tag;
  /*! \brief For kCombine: the operator's name, which the environment numbers. */
  std::string combine_tag;
  /*! \brief The legal text here comes from the symbol table, not the grammar. */
  bool from_lexicon = false;
  /*! \brief The text here may be anything the environment does NOT already know. */
  bool fresh_name = false;
  /*! \brief Ask the environment before entering this rule at all. */
  bool gated = false;
};

/*!
 * \brief What the table needs from the environment, and nothing more.
 *
 * `resolve` turns a tag or a piece of matched text into a type, or -1 if it names
 * nothing. `accepts` says whether a produced type satisfies a required one.
 */
struct TypeOracle {
  std::function<int32_t(std::string_view)> resolve;
  std::function<bool(int32_t required, int32_t produced)> accepts;
  /*! \brief Whether this text may be where a fresh name ends: false for a name the
   *         environment already knows, or a word the language reserves. */
  std::function<bool(std::string_view)> is_fresh;
  /*! \brief An operator's number, for a kCombine tag. Operators are not types, so
   *         they have their own namespace. */
  std::function<int32_t(std::string_view)> resolve_op;
  /*! \brief What an operator does to two values. Either may be -1, meaning nothing
   *         produced yet, and the usual answer is then the other one. */
  std::function<int32_t(int32_t op, int32_t accumulated, int32_t operand)> combine;
};

class TypeTable {
 public:
  /*!
   * \brief Read a table. On a malformed line, returns false and sets `error`.
   *
   * `out` is cleared first and is left empty on failure, rather than holding
   * whatever was read before the bad line.
   */
  static bool Parse(std::string_view text, TypeTable* out, std::string* error);

  /*! \brief The row for a rule; the default row if it is not listed. */
  const Row& RowFor(const std::string& rule) const {
    auto it = rows_.find(rule);
    return it == rows_.end() ? default_row_ : it->second;
  }

  bool Lists(const std::string& rule) const { return rows_.count(rule) != 0; }

  /*!
   * \brief The rules the table mentions.
   *
   * These must survive grammar optimisation: the table finds them by name, so a
   * rule that is merged away takes its row with it.
   */
  std::vector<std::string> ListedRules() const {
    std::vector<std::string> names;
    names.reserve(rows_.size());
    for (const auto& [name, row] : rows_) names.push_back(name);
    return names;
  }

  /*! \brief The types an occurrence of `rule` starts with, inside `enclosing`. */
  Types OnEnter(const std::string& rule, const Types& enclosing, const TypeOracle& oracle) const {
    const Row& row = RowFor(rule);
    Types out;
    switch (row.need_on_enter) {
      case NeedOnEnter::kInherit:
        out.required = enclosing.required;
        break;
      case NeedOnEnter::kNone:
        out.required = -1;
        break;
      case NeedOnEnter::kFixed:
        // A tag the environment does not know leaves the position unconstrained
        // rather than impossible, which is the safe direction for a mask.
        out.required = oracle.resolve ? oracle.resolve(row.require_tag) : -1;
        break;
    }
    out.produced = row.have_on_enter == HaveOnEnter::kInherit ? enclosing.produced : -1;
    return out;
  }

  /*!
   * \brief Whether an occurrence of `rule` holding `self` may finish.
   *
   * Only a checked rule can refuse. A rule with no requirement is left alone, since
   * nothing constrains it. A rule that has produced nothing is refused: there is no
   * evidence it satisfies the requirement, and admitting it would let through text
   * we cannot justify. In practice a complete expression always produces something.
   */
  bool MayFinish(
      const std::string& rule, const Types& self, const TypeOracle& oracle,
      std::string_view matched = {}
  ) const {
    const Row& row = RowFor(rule);
    if (row.fresh_name && oracle.is_fresh && !oracle.is_fresh(matched)) return false;
    if (row.finish != Finish::kCheck) return true;
    if (self.required < 0 || !oracle.accepts) return true;
    return oracle.accepts(self.required, self.produced);
  }

  /*!
   * \brief The enclosing position's types after an occurrence of `rule` finishes.
   *
   * `matched` is the text the occurrence consumed, which only the two text-driven
   * actions look at. A text or tag the environment does not recognise leaves the
   * enclosing position unchanged rather than erasing what it had.
   */
  Types OnFinish(
      const std::string& rule, const Types& self, const Types& enclosing,
      std::string_view matched, const TypeOracle& oracle
  ) const {
    const Row& row = RowFor(rule);
    Types out = enclosing;
    switch (row.finish) {
      case Finish::kPass:
        break;
      // A check is a refusal, not a transformation: having confirmed it produced the
      // right thing, the rule still produced it, and the position around it may need
      // to know. Discarding it meant `( count )` reported producing nothing.
      case Finish::kCheck:
      case Finish::kKeepFirst:
        if (self.produced >= 0 && out.produced < 0) out.produced = self.produced;
        break;
      case Finish::kReplace:
        if (self.produced >= 0) out.produced = self.produced;
        break;
      case Finish::kProduce: {
        const int32_t t = oracle.resolve ? oracle.resolve(row.produce_tag) : -1;
        if (t >= 0) out.produced = t;
        break;
      }
      case Finish::kProduceText: {
        const int32_t t = oracle.resolve ? oracle.resolve(matched) : -1;
        if (t >= 0) out.produced = t;
        break;
      }
      case Finish::kCombine: {
        if (!oracle.combine || !oracle.resolve_op) {
          break;
        }
        // A rejected combination produces nothing, rather than leaving the old value
        // in place: `true + 3` is ill typed, and keeping the boolean would let it
        // satisfy a boolean position.
        out.produced =
            oracle.combine(oracle.resolve_op(row.combine_tag), enclosing.produced, self.produced);
        break;
      }
      case Finish::kRequireText: {
        const int32_t t = oracle.resolve ? oracle.resolve(matched) : -1;
        if (t >= 0) out.required = t;
        break;
      }
    }
    return out;
  }

  /*! \brief The type a rule requires of what is inside it, or -1 if it imposes none. */
  int32_t FixedRequired(const std::string& rule, const TypeOracle& oracle) const {
    const Row& row = RowFor(rule);
    if (row.need_on_enter != NeedOnEnter::kFixed || !oracle.resolve) return -1;
    return oracle.resolve(row.require_tag);
  }

  /*!
   * \brief The type a rule produces whatever its text, or -1 if that depends on it.
   *
   * A position may refuse to enter such a rule at all when the type cannot satisfy
   * the requirement: a comparison is a boolean, so it does not begin a string.
   */
  int32_t FixedProduced(const std::string& rule, const TypeOracle& oracle) const {
    const Row& row = RowFor(rule);
    if (row.finish != Finish::kProduce || !oracle.resolve) return -1;
    return oracle.resolve(row.produce_tag);
  }

 private:
  std::unordered_map<std::string, Row> rows_;
  Row default_row_;
};

namespace detail {

inline std::vector<std::string> SplitWords(std::string_view line) {
  std::vector<std::string> words;
  size_t i = 0;
  while (i < line.size()) {
    while (i < line.size() && std::isspace(static_cast<unsigned char>(line[i]))) ++i;
    size_t start = i;
    while (i < line.size() && !std::isspace(static_cast<unsigned char>(line[i]))) ++i;
    if (i > start) words.emplace_back(line.substr(start, i - start));
  }
  return words;
}

}  // namespace detail

inline bool TypeTable::Parse(std::string_view text, TypeTable* out, std::string* error) {
  *out = TypeTable{};
  size_t line_begin = 0;
  int line_no = 0;
  const auto fail = [&](const std::string& why) {
    if (error) *error = "line " + std::to_string(line_no) + ": " + why;
    return false;
  };
  while (line_begin <= text.size()) {
    const size_t line_end = std::min(text.find('\n', line_begin), text.size());
    std::string_view line = text.substr(line_begin, line_end - line_begin);
    line_begin = line_end + 1;
    ++line_no;
    const size_t comment = line.find('#');
    if (comment != std::string_view::npos) line = line.substr(0, comment);
    auto words = detail::SplitWords(line);
    if (words.empty()) {
      if (line_end == text.size()) break;
      continue;
    }
    const std::string rule = words[0];
    Row row;
    for (size_t i = 1; i < words.size(); ++i) {
      const std::string& word = words[i];
      const size_t eq = word.find('=');
      if (eq == std::string::npos) return fail("expected key=value, got \"" + word + "\"");
      const std::string key = word.substr(0, eq);
      const std::string value = word.substr(eq + 1);
      if (key == "enter_need") {
        if (value == "inherit") row.need_on_enter = NeedOnEnter::kInherit;
        else if (value == "none") row.need_on_enter = NeedOnEnter::kNone;
        else {
          row.need_on_enter = NeedOnEnter::kFixed;
          row.require_tag = value;        // anything else names a type
        }
      } else if (key == "enter_have") {
        if (value == "inherit") row.have_on_enter = HaveOnEnter::kInherit;
        else if (value == "none") row.have_on_enter = HaveOnEnter::kNone;
        else return fail("enter_have must be inherit or none, got \"" + value + "\"");
            } else if (key == "finish") {
        if (value == "pass") row.finish = Finish::kPass;
        else if (value == "keep_first") row.finish = Finish::kKeepFirst;
        else if (value == "replace") row.finish = Finish::kReplace;
        else if (value == "produce_text") row.finish = Finish::kProduceText;
        else if (value == "require_text") row.finish = Finish::kRequireText;
        else if (value == "check") row.finish = Finish::kCheck;
        else if (value == "combine") {
          row.finish = Finish::kCombine;
          if (i + 1 >= words.size()) return fail("finish=combine needs an operator");
          row.combine_tag = words[++i];
        }
        else if (value == "produce") {
          row.finish = Finish::kProduce;
          if (i + 1 >= words.size()) return fail("finish=produce needs a tag");
          row.produce_tag = words[++i];
        } else {
          return fail("unknown finish action \"" + value + "\"");
        }
      } else if (key == "content") {
        if (value == "lexicon") row.from_lexicon = true;
        else if (value == "fresh") row.fresh_name = true;
        else return fail("unknown content \"" + value + "\"");
      } else if (key == "gate") {
        if (value != "environment") return fail("unknown gate \"" + value + "\"");
        row.gated = true;
      } else {
        return fail("unknown key \"" + key + "\"");
      }
    }
    if (!out->rows_.emplace(rule, row).second) return fail("rule \"" + rule + "\" listed twice");
    if (line_end == text.size()) break;
  }
  return true;
}

}  // namespace maskgen

#endif  // MASKGEN_TYPE_TABLE_H_
