// gift-validator.cpp: implementation of the Moodle GIFT quiz file validator.
//
// Emulates Moodle's qformat_gift PHP parser
// (public/question/format/gift/format.php) exactly, including its known bugs.

#include "gift-validator.hpp"

#include <cctype>
#include <fstream>
#include <sstream>

namespace gift
{
namespace detail
{

// ── String helpers
// ────────────────────────────────────────────────────────────

// PHP trim() strips ASCII whitespace from both ends.
static std::string php_trim(const std::string &s)
{
  static const char ws[] = " \t\n\r\v\f";
  size_t a = s.find_first_not_of(ws);
  if (a == std::string::npos)
    return {};
  return s.substr(a, s.find_last_not_of(ws) - a + 1);
}

// Replace all occurrences of 'from' with 'to'.
static std::string replace_all(std::string s, const std::string &from,
                               const std::string &to)
{
  if (from.empty())
    return s;
  size_t p = 0;
  while ((p = s.find(from, p)) != std::string::npos)
  {
    s.replace(p, from.size(), to);
    p += to.size();
  }
  return s;
}

// Apply a sequence of (from, to) pairs in order; replicates PHP array
// str_replace.
static std::string replace_seq(std::string s,
                               const std::vector<std::string> &froms,
                               const std::vector<std::string> &tos)
{
  for (size_t i = 0; i < froms.size(); ++i)
    s = replace_all(std::move(s), froms[i], tos[i]);
  return s;
}

// PHP explode($delim, $s [, $limit]). limit=-1 means unlimited.
static std::vector<std::string> php_explode(const std::string &delim,
                                            const std::string &s,
                                            int limit = -1)
{
  std::vector<std::string> out;
  if (delim.empty() || limit == 1)
  {
    out.push_back(s);
    return out;
  }
  size_t pos = 0, found;
  int n = 1;
  while ((found = s.find(delim, pos)) != std::string::npos)
  {
    if (limit > 0 && n >= limit)
      break;
    out.push_back(s.substr(pos, found - pos));
    pos = found + delim.size();
    ++n;
  }
  out.push_back(s.substr(pos));
  return out;
}

// Remove a leading empty element produced when the delimiter appears first.
// Replicates the "if empty($answers[0]) array_shift" pattern used in every
// type-specific branch of PHP readquestion().
static void drop_empty_first(std::vector<std::string> &v)
{
  if (!v.empty() && php_trim(v.front()).empty())
    v.erase(v.begin());
}

// ── Escape-character handling (exact replication of PHP) ─────────────────────
//
// PHP escapedchar_pre replaces escape sequences with numeric placeholders
// before any structural parsing, so that e.g. '\{' doesn't confuse the brace
// finder. The two-step '\\' handling is deliberate: '\\' → '&&092;' first, then
// the other sequences, then '&&092;' → '\'.  This ensures '\\{' becomes '\{' (a
// literal backslash followed by an open brace) rather than a placeholder.

static std::string escapedchar_pre(std::string s)
{
  static const std::vector<std::string> esc = {"\\:", "\\#", "\\=", "\\{",
                                               "\\}", "\\~", "\\n"};
  static const std::vector<std::string> ph = {
      "&&058;", "&&035;", "&&061;", "&&123;", "&&125;", "&&126;", "&&010"};
  s = replace_all(std::move(s), "\\\\", "&&092;");
  s = replace_seq(std::move(s), esc, ph);
  s = replace_all(std::move(s), "&&092;", "\\");
  return s;
}

static std::string escapedchar_post(std::string s)
{
  static const std::vector<std::string> ph = {
      "&&058;", "&&035;", "&&061;", "&&123;", "&&125;", "&&126;", "&&010"};
  static const std::vector<std::string> chr = {":", "#", "=", "{",
                                               "}", "~", "\n"};
  return replace_seq(std::move(s), ph, chr);
}

// ── PHP is_numeric
// ────────────────────────────────────────────────────────────

static bool php_is_numeric(const std::string &s)
{
  if (s.empty())
    return false;
  size_t i = 0;
  if (s[i] == '+' || s[i] == '-')
    ++i;
  bool d = false;
  while (i < s.size() && std::isdigit((unsigned char)s[i]))
  {
    ++i;
    d = true;
  }
  if (i < s.size() && s[i] == '.')
  {
    ++i;
    while (i < s.size() && std::isdigit((unsigned char)s[i]))
    {
      ++i;
      d = true;
    }
  }
  if (d && i < s.size() && (s[i] == 'e' || s[i] == 'E'))
  {
    ++i;
    if (i < s.size() && (s[i] == '+' || s[i] == '-'))
      ++i;
    bool e = false;
    while (i < s.size() && std::isdigit((unsigned char)s[i]))
    {
      ++i;
      e = true;
    }
    if (!e)
      return false;
  }
  return d && i == s.size();
}

// ── Answer weight prefix parser ───────────────────────────────────────────────
//
// PHP: /^%\-*([0-9]{1,2})\.?([0-9]*)%/
//
// Despite [0-9]{1,2} appearing to restrict the integer part to 1–2 digits,
// PCRE backtracking means %100% fully matches: [0-9]{1,2} matches "10" and
// [0-9]* matches "0".  answerweightparser() extracts the text between the two
// '%' delimiters (not the capture groups), so all of %100%, %-100%, %50%,
// %-50% etc. are handled correctly by Moodle.

// Parses a '%[-]NNN[.NNN]%' prefix. Returns false if s doesn't start with one.
// On success, end_pct = index of closing '%'.
static bool scan_weight_prefix(const std::string &s, size_t &end_pct)
{
  if (s.empty() || s[0] != '%')
    return false;
  size_t i = 1;
  while (i < s.size() && s[i] == '-')
    ++i; // \-* matches zero or more minus signs
  size_t ds = i;
  while (i < s.size() && std::isdigit((unsigned char)s[i]))
    ++i;
  if (i == ds)
    return false; // must have at least one digit
  if (i < s.size() && s[i] == '.')
  {
    ++i;
    while (i < s.size() && std::isdigit((unsigned char)s[i]))
      ++i;
  }
  if (i >= s.size() || s[i] != '%')
    return false;
  end_pct = i;
  return true;
}

// ── File splitting
// ────────────────────────────────────────────────────────────

struct Block
{
  std::vector<std::string> lines;
  std::string raw;
  int start_line = 0; // 1-based line number of the first line
};

// Splits text into lines, normalising \r\n and \r to bare \n boundaries.
static std::vector<std::string> split_lines(const std::string &text)
{
  std::vector<std::string> out;
  size_t start = 0;
  for (size_t i = 0; i < text.size(); ++i)
  {
    if (text[i] == '\r' || text[i] == '\n')
    {
      out.push_back(text.substr(start, i - start));
      if (text[i] == '\r' && i + 1 < text.size() && text[i + 1] == '\n')
        ++i;
      start = i + 1;
    }
  }
  out.push_back(text.substr(start)); // final line (possibly empty)
  return out;
}

// Splits the file into question blocks, separated by blank lines.
// Mirrors the behaviour of qformat_default::readquestions() in the parent
// class.
static std::vector<Block> split_blocks(const std::string &content)
{
  std::vector<Block> blocks;
  Block cur;
  int line_num = 0;
  for (const auto &line : split_lines(content))
  {
    ++line_num;
    if (php_trim(line).empty())
    {
      if (!cur.lines.empty())
      {
        blocks.push_back(std::move(cur));
        cur = {};
      }
    }
    else
    {
      if (cur.lines.empty())
        cur.start_line = line_num;
      cur.lines.push_back(line);
      cur.raw += line;
      cur.raw += '\n';
    }
  }
  if (!cur.lines.empty())
    blocks.push_back(std::move(cur));
  return blocks;
}

// ── Per-question parser
// ───────────────────────────────────────────────────────
//
// Replicates qformat_gift::readquestion() line by line, including:
//   • two-step escape placeholder substitution
//   • naive first-'{' / first-'}' brace matching
//   • type detection order (~ before = and ->, numerical before all)
//   • the strpos(…) > 0 bugs in true/false and numerical parsing
//   • the '$ans = "*"' assignment-not-comparison bug in numerical validation

static Question parse_question(const Block &block)
{
  Question q;
  q.raw = block.raw;
  q.line = block.start_line;
  if (!q.raw.empty() && q.raw.back() == '\n')
    q.raw.pop_back();

  // Strip comment lines (lines whose trimmed content starts with '//') and
  // implode the remainder with '\n'.  PHP replaces comment lines with a single
  // space (not an empty string) to preserve the implode line count, but after
  // the final trim that distinction disappears.
  std::string text;
  for (size_t i = 0; i < block.lines.size(); ++i)
  {
    if (i)
      text += '\n';
    const std::string &line = block.lines[i];
    std::string t = php_trim(line);
    if (t.size() >= 2 && t[0] == '/' && t[1] == '/')
      text += ' '; // blank out comment line, matching PHP behaviour
    else
      text += line;
  }
  text = php_trim(text);

  if (text.empty())
  {
    q.type = QuestionType::Unknown;
    return q; // caller (parse()) will discard Unknown blocks
  }

  // Apply escape-sequence placeholder substitution.
  text = escapedchar_pre(text);

  // $CATEGORY: directive, via preg_match('~^\$CATEGORY:~'), then substr($text,
  // 10).
  if (text.size() >= 10 && text.compare(0, 10, "$CATEGORY:") == 0)
  {
    q.type = QuestionType::Category;
    q.name = php_trim(text.substr(10));
    return q;
  }

  // Question name parser: '::name::' prefix.
  if (text.size() >= 2 && text[0] == ':' && text[1] == ':')
  {
    text = text.substr(2);
    size_t ne = text.find("::");
    if (ne == std::string::npos)
    {
      // PHP sets $question->name = false here; name is auto-generated later.
      q.warn(DiagnosticKind::UnclosedQuestionName,
             "Question starts with '::' but has no closing '::'; "
             "Moodle will auto-generate the name from the question text.");
    }
    else
    {
      q.name = php_trim(escapedchar_post(text.substr(0, ne)));
      text = php_trim(text.substr(ne + 2));
    }
  }

  // Find answer block using the FIRST '{' and FIRST '}'.
  // BUG: no balanced matching; any unescaped '{' or '}' in the question text
  // before the intended answer block will silently corrupt this.
  size_t as = text.find('{');
  size_t af = text.find('}');
  bool desc = (as == std::string::npos && af == std::string::npos);
  std::string at; // answer text

  if (!desc)
  {
    if (as == std::string::npos || af == std::string::npos)
    {
      q.error(DiagnosticKind::MismatchedBraces,
              "Question has '{' or '}' but not both. "
              "Escape literal braces as \\{ and \\}.");
      return q;
    }
    at = php_trim(text.substr(as + 1, af - as - 1));
  }

  // Strip general feedback (#### … at the END of the answer text).
  // PHP uses strrpos (last occurrence), so only the final #### is the
  // separator.
  if (!desc)
  {
    size_t gf = at.rfind("####");
    if (gf != std::string::npos)
      at = php_trim(at.substr(0, gf));
  }

  // ── Determine question type ───────────────────────────────────────────────

  if (desc)
  {
    q.type = QuestionType::Description;
    return q;
  }
  if (at.empty())
  {
    q.type = QuestionType::Essay;
    return q;
  }

  if (at[0] == '#')
  {
    // Numerical: answer block begins with '#'.
    q.type = QuestionType::Numerical;
  }
  else if (at.find('~') != std::string::npos)
  {
    // MultiChoice: any '~' present (checked BEFORE match).
    // A match question that accidentally contains '~' will be misdetected here.
    q.type = QuestionType::MultiChoice;
  }
  else if (at.find('=') != std::string::npos &&
           at.find("->") != std::string::npos)
  {
    // Match: requires both '=' and '->'.
    q.type = QuestionType::Match;
  }
  else
  {
    // True/false or short answer.
    //
    // BUG REPLICATED: PHP uses strpos($answertext, '#') > 0
    // (not !== false).  If '#' is at position 0 it is NOT stripped.
    // In practice position-0 '#' was caught by the numerical check above,
    // but the > 0 vs !== false asymmetry is preserved faithfully.
    std::string tfc = at;
    size_t h = at.find('#');
    if (h != std::string::npos && h > 0)
      tfc = php_trim(at.substr(0, h));

    if (tfc == "T" || tfc == "TRUE" || tfc == "F" || tfc == "FALSE")
      q.type = QuestionType::TrueFalse;
    else
      q.type = QuestionType::ShortAnswer;
  }

  // ── Type-specific validation ──────────────────────────────────────────────

  switch (q.type)
  {

  case QuestionType::MultiChoice: {
    // PHP: str_replace("=", "~=", $answertext) then explode("~", …)
    // This converts correct answers (=) into the same delimiter-prefixed form
    // as wrong answers (~), so both can be split on '~'.
    std::string expanded = replace_all(at, "=", "~=");
    auto answers = php_explode("~", expanded);
    drop_empty_first(answers);

    if (static_cast<int>(answers.size()) < 2)
    {
      q.error(DiagnosticKind::TooFewAnswers,
              "Multichoice question needs at least 2 answers (found " +
                  std::to_string(answers.size()) + ").");
      break;
    }

    break;
  }

  case QuestionType::Match: {
    auto answers = php_explode("=", at);
    drop_empty_first(answers);

    if (static_cast<int>(answers.size()) < 2)
    {
      q.error(DiagnosticKind::TooFewAnswers,
              "Match question needs at least 2 answer pairs (found " +
                  std::to_string(answers.size()) + ").");
      break;
    }
    for (auto &ans : answers)
    {
      std::string a = php_trim(ans);
      if (a.find("->") == std::string::npos)
        q.error(DiagnosticKind::MatchMissingArrow,
                "Match pair is missing '->': \"" +
                    php_trim(escapedchar_post(a)) + "\".");
    }
    break;
  }

  case QuestionType::ShortAnswer: {
    auto answers = php_explode("=", at);
    drop_empty_first(answers);

    if (answers.empty())
    {
      q.error(DiagnosticKind::TooFewAnswers,
              "Short answer question has no answers.");
      break;
    }
    break;
  }

  case QuestionType::Numerical: {
    std::string num = at.substr(1); // strip leading '#'

    // Wrong-answer feedback block starts at the first '~' after the answers.
    size_t tilde = num.find('~');
    if (tilde != std::string::npos)
      num = num.substr(0, tilde);

    auto answers = php_explode("=", num);
    drop_empty_first(answers);

    if (answers.empty())
    {
      q.error(DiagnosticKind::NoNumericalAnswers,
              "Numerical question has no answers after '#'.");
      break;
    }

    for (auto &raw_a : answers)
    {
      std::string a = php_trim(raw_a);

      // Strip weight prefix (%N%, %-N%, %N.M%, etc.) if present.
      // Skip this answer if it starts with '%' but has no valid closing '%'.
      {
        size_t ep;
        if (scan_weight_prefix(a, ep))
          a = a.substr(ep + 1);
        else if (!a.empty() && a[0] == '%')
          continue; // malformed weight prefix, skip
      }

      // Strip per-answer feedback (commentparser uses first '#').
      size_t hash = a.find('#');
      if (hash != std::string::npos)
        a = a.substr(0, hash);
      a = php_trim(a);
      if (a.empty())
        continue;

      // Parse the numeric value.
      //
      // BUG REPLICATED: PHP uses  strpos($answer, '..') > 0
      //                      and  strpos($answer, ':')  > 0
      // (not !== false).  If '..' or ':' is at position 0, that branch is
      // skipped and the whole string falls through to the bare-value check.
      size_t dd = a.find("..");
      size_t cl = a.find(':');

      if (dd != std::string::npos && dd > 0)
      {
        // min..max range format
        std::string lo = php_trim(a.substr(0, dd));
        std::string hi = php_trim(a.substr(dd + 2));
        if (!php_is_numeric(lo) || !php_is_numeric(hi))
        {
          // BUG REPLICATED: PHP checks !(is_numeric($ans) || $ans = '*')
          // The second clause is an assignment, not a comparison.
          // If is_numeric fails, $ans is assigned '*' (truthy), so the
          // whole condition is false and no error is ever raised.
          q.warn(DiagnosticKind::NonNumericAnswerSilentStar,
                 "Numerical range '" + lo + ".." + hi +
                     "' contains "
                     "non-numeric value(s). Moodle's broken validity check "
                     "(a PHP assignment instead of comparison) will silently "
                     "replace the answer with '*'.");
        }
      }
      else if (cl != std::string::npos && cl > 0)
      {
        // answer:tolerance format
        std::string av = php_trim(a.substr(0, cl));
        std::string tv = php_trim(a.substr(cl + 1));
        if (!php_is_numeric(av) && av != "*")
        {
          q.warn(DiagnosticKind::NonNumericAnswerSilentStar,
                 "Numerical answer '" + av +
                     "' is not numeric. "
                     "Moodle's broken validity check will silently "
                     "replace it with '*'.");
        }
        if (!php_is_numeric(tv))
        {
          q.error(DiagnosticKind::NoNumericalAnswers,
                  "Numerical tolerance '" + tv + "' is not numeric.");
        }
      }
      else
      {
        // Bare value, zero tolerance.
        if (!php_is_numeric(a) && a != "*")
        {
          q.warn(DiagnosticKind::NonNumericAnswerSilentStar,
                 "Numerical answer '" + a +
                     "' is not numeric. "
                     "Moodle's broken validity check will silently "
                     "replace it with '*'.");
        }
      }
    }
    break;
  }

  default:
    break;
  }

  return q;
}

static const char *type_str(QuestionType t)
{
  switch (t)
  {
  case QuestionType::Category:
    return "category";
  case QuestionType::Description:
    return "description";
  case QuestionType::Essay:
    return "essay";
  case QuestionType::TrueFalse:
    return "truefalse";
  case QuestionType::MultiChoice:
    return "multichoice";
  case QuestionType::ShortAnswer:
    return "shortanswer";
  case QuestionType::Numerical:
    return "numerical";
  case QuestionType::Match:
    return "match";
  default:
    return "unknown";
  }
}

static const char *sev_str(Severity s)
{
  return s == Severity::Error ? "ERROR  " : "WARNING";
}

} // namespace detail

// ── Public API
// ────────────────────────────────────────────────────────────────

ParseResult parse(const std::string &content)
{
  ParseResult r;
  for (auto &b : detail::split_blocks(content))
  {
    auto q = detail::parse_question(b);
    if (q.type != QuestionType::Unknown) // discard comment-only blocks
      r.questions.push_back(std::move(q));
  }

  // Post-processing: detect questions split by a blank line inside a code fence.
  //
  // Moodle's parent class (qformat_default) splits the file on blank lines
  // before any parsing happens.  A blank line inside a ``` ... ``` code block
  // therefore silently splits one question into two separate blocks:
  //   Block A parses as a Description (no answer braces, but has a name
  //             and a code fence in its raw text)
  //   Block B parses as a real question type (multichoice etc.) but with
  //             no name, because the ::name:: was in block A
  //
  // Both blocks are individually "valid" by Moodle's rules, so the per-block
  // parser cannot catch this.  We detect the pattern here and mark both
  // blocks as errors.
  for (size_t i = 0; i + 1 < r.questions.size(); ++i)
  {
    auto &curr = r.questions[i];
    auto &next = r.questions[i + 1];

    if (curr.type == QuestionType::Description  // first half has no answer block
     && !curr.name.empty()                      // but it has a name
     && curr.raw.find("```") != std::string::npos // and an open code fence
     && next.name.empty()                       // second half has no name
     && next.type != QuestionType::Description
     && next.type != QuestionType::Category)
    {
      curr.error(DiagnosticKind::BlankLineInCodeBlock,
                 "This block has a name and an open ``` code fence but no "
                 "answer block. A blank line inside the code fence caused "
                 "Moodle to split this question here. The following block "
                 "(the unnamed " + std::string(detail::type_str(next.type)) +
                 " question) is its continuation. "
                 "Fix: remove the blank line inside the ``` fence, or replace "
                 "it with a comment line (// ) as a placeholder.");
      next.error(DiagnosticKind::BlankLineInCodeBlock,
                 "This unnamed " + std::string(detail::type_str(next.type)) +
                 " block appears to be the second half of the preceding "
                 "Description (\"" + curr.name + "\"), split by a blank line "
                 "inside a ``` code fence. Its question text will be garbled "
                 "in Moodle (starting mid-code-block).");
    }
  }

  return r;
}

ParseResult parse_file(const std::string &path)
{
  std::ifstream f(path, std::ios::binary);
  if (!f)
    return {};
  std::string s((std::istreambuf_iterator<char>(f)), {});
  return parse(s);
}

std::string filter_valid(const std::string &content)
{
  std::ostringstream out;
  write_valid(parse(content), out);
  return out.str();
}

std::string filter_invalid(const std::string &content)
{
  std::ostringstream out;
  write_invalid(parse(content), out);
  return out.str();
}

void write_valid(const ParseResult &r, std::ostream &out)
{
  for (const auto &q : r.questions)
  {
    if (!q.valid)
      continue;
    out << q.raw << "\n\n";
  }
}

bool write_valid_file(const ParseResult &r, const std::string &path)
{
  std::ofstream f(path);
  if (!f)
    return false;
  write_valid(r, f);
  return true;
}

void write_invalid(const ParseResult &r, std::ostream &out)
{
  for (const auto &q : r.questions)
  {
    if (q.valid)
      continue;
    out << q.raw << "\n\n";
  }
}

bool write_invalid_file(const ParseResult &r, const std::string &path)
{
  std::ofstream f(path);
  if (!f)
    return false;
  write_invalid(r, f);
  return true;
}

std::string report(const ParseResult &r)
{
  std::ostringstream out;
  for (const auto &q : r.questions)
  {
    if (q.diagnostics.empty())
      continue;
    out << "Line " << q.line << " [" << detail::type_str(q.type) << "]";
    if (!q.name.empty())
      out << " \"" << q.name << "\"";
    out << ":\n";
    for (const auto &d : q.diagnostics)
      out << "  " << detail::sev_str(d.severity) << ": " << d.message << "\n";
  }
  out << "\nSummary: " << r.valid_count() << " valid, " << r.invalid_count()
      << " invalid"
      << " (of " << r.total_count() << " questions)\n";
  return out.str();
}

} // namespace gift
