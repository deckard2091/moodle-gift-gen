#ifndef _GIFT_VALIDATOR_
#define _GIFT_VALIDATOR_

// gift-validator.hpp — public interface for the Moodle GIFT quiz file
// validator.
//
// Emulates Moodle's qformat_gift PHP parser
// (public/question/format/gift/format.php) exactly, including its known bugs,
// so that questions passing validation here will import cleanly into Moodle,
// and those failing here would fail or silently misbehave there.
//

#include <ostream>
#include <string>
#include <vector>

namespace gift
{

// ── Severity
// ──────────────────────────────────────────────────────────────────

enum class Severity
{
  Warning,
  Error
};

// ── DiagnosticKind
// ────────────────────────────────────────────────────────────
//
// Errors   — Moodle rejects the question outright.
// Warnings — Moodle accepts the question but the result is wrong or surprising.

enum class DiagnosticKind
{
  // Errors
  MismatchedBraces,   // '{' or '}' present but not both
  TooFewAnswers,      // below minimum answer count for the type
  NoNumericalAnswers, // numerical block with no parseable answers
  MatchMissingArrow,  // a match pair is missing '->'
  // Warnings
  UnclosedQuestionName,       // '::' prefix with no closing '::' → auto-name
  NonNumericAnswerSilentStar, // numerical: non-numeric value silently becomes
                              // '*'
                              //   due to '$ans = "*"' assignment bug in PHP
  BlankLineInCodeBlock,       // blank line inside a ``` fence splits the
                              // question into two separate Moodle questions
};

// ── Diagnostic
// ────────────────────────────────────────────────────────────────

struct Diagnostic
{
  Severity severity;
  DiagnosticKind kind;
  std::string message;
};

// ── QuestionType
// ──────────────────────────────────────────────────────────────

enum class QuestionType
{
  Category,    // $CATEGORY: directive (not a real question)
  Description, // no answer block
  Essay,       // empty answer block {}
  TrueFalse,
  MultiChoice,
  ShortAnswer,
  Numerical,
  Match,
  Unknown, // block was empty after stripping comments
};

// ── Question
// ──────────────────────────────────────────────────────────────────

struct Question
{
  std::string raw;  // original source lines (no trailing blank line)
  std::string name; // parsed name, or empty
  int line = 0;     // 1-based line number of the first line of the block
  QuestionType type = QuestionType::Unknown;
  bool valid = true;
  std::vector<Diagnostic> diagnostics;

  void error(DiagnosticKind k, std::string msg)
  {
    valid = false;
    diagnostics.push_back({Severity::Error, k, std::move(msg)});
  }
  void warn(DiagnosticKind k, std::string msg)
  {
    diagnostics.push_back({Severity::Warning, k, std::move(msg)});
  }
};

// ── ParseResult
// ───────────────────────────────────────────────────────────────

struct ParseResult
{
  std::vector<Question> questions;

  // Count of questions (excluding $CATEGORY directives and unknowns) that are
  // valid.
  int valid_count() const
  {
    int n = 0;
    for (auto &q : questions)
      if (counts_as_question(q) && q.valid)
        ++n;
    return n;
  }
  // Count of questions Moodle would reject or silently mishandle.
  int invalid_count() const
  {
    int n = 0;
    for (auto &q : questions)
      if (!q.valid)
        ++n;
    return n;
  }
  // Total question count (excluding $CATEGORY directives and unknowns).
  int total_count() const
  {
    int n = 0;
    for (auto &q : questions)
      if (counts_as_question(q))
        ++n;
    return n;
  }

private:
  static bool counts_as_question(const Question &q)
  {
    return q.type != QuestionType::Unknown && q.type != QuestionType::Category;
  }
};

// ── Public API
// ────────────────────────────────────────────────────────────────

/// Parse a GIFT-format string held in memory.
ParseResult parse(const std::string &content);

/// Parse a GIFT file by path.
ParseResult parse_file(const std::string &path);

/// Parse GIFT content and return only the valid questions as a GIFT string.
std::string filter_valid(const std::string &content);

/// Parse GIFT content and return only the invalid questions as a GIFT string.
std::string filter_invalid(const std::string &content);

/// Write only valid questions (and $CATEGORY directives) to a stream.
/// The output is itself a valid GIFT file.
void write_valid(const ParseResult &r, std::ostream &out);

/// Write only valid questions to a file.
/// Returns false if the file cannot be opened.
bool write_valid_file(const ParseResult &r, const std::string &path);

/// Write only invalid questions to a stream.
void write_invalid(const ParseResult &r, std::ostream &out);

/// Write only invalid questions to a file.
/// Returns false if the file cannot be opened.
bool write_invalid_file(const ParseResult &r, const std::string &path);

/// Return a human-readable diagnostic report.
std::string report(const ParseResult &r);

} // namespace gift

#endif // _GIFT_VALIDATOR_
