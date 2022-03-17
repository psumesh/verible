// Copyright 2021 The Verible Authors.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//      http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.
//

#include "verilog/tools/ls/verible-lsp-adapter.h"

#include "common/lsp/lsp-protocol-operators.h"
#include "common/lsp/lsp-protocol.h"
#include "common/text/text_structure.h"
#include "nlohmann/json.hpp"
#include "verilog/analysis/verilog_analyzer.h"
#include "verilog/analysis/verilog_linter.h"
#include "verilog/formatting/format_style_init.h"
#include "verilog/formatting/formatter.h"
#include "verilog/parser/verilog_token_enum.h"
#include "verilog/tools/ls/document-symbol-filler.h"
#include "verilog/tools/ls/lsp-parse-buffer.h"

namespace verilog {
// Convert our representation of a linter violation to a LSP-Diagnostic
static verible::lsp::Diagnostic ViolationToDiagnostic(
    const verible::LintViolationWithStatus &v,
    const verible::TextStructureView &text) {
  const verible::LintViolation &violation = *v.violation;
  const verible::LineColumnRange range = text.GetRangeForToken(violation.token);
  const char *fix_msg = violation.autofixes.empty() ? "" : " (fix available)";
  return verible::lsp::Diagnostic{
      .range =
          {
              .start = {.line = range.start.line,
                        .character = range.start.column},
              .end = {.line = range.end.line, .character = range.end.column},
          },
      .message = absl::StrCat(violation.reason, " ", v.status->url, "[",
                              v.status->lint_rule_name, "]", fix_msg),
  };
}

std::vector<verible::lsp::Diagnostic> CreateDiagnostics(
    const BufferTracker &tracker) {
  // Diagnostics should come from the latest state, including all the
  // syntax errors.
  const ParsedBuffer *const current = tracker.current();
  if (!current) return {};
  // TODO: files that generate a lot of messages will create a huge
  // output. So we limit the messages here.
  // However, we should work towards emitting them around the last known
  // edit point in the document as this is what the user sees.
  static constexpr int kMaxMessages = 100;
  const auto &rejected_tokens = current->parser().GetRejectedTokens();
  auto const &lint_violations =
      verilog::GetSortedViolations(current->lint_result());
  std::vector<verible::lsp::Diagnostic> result;
  int remaining = rejected_tokens.size() + lint_violations.size();
  if (remaining > kMaxMessages) remaining = kMaxMessages;
  result.reserve(remaining);
  for (const auto &rejected_token : rejected_tokens) {
    current->parser().ExtractLinterTokenErrorDetail(
        rejected_token,
        [&result, &rejected_token](
            const std::string &filename, verible::LineColumnRange range,
            verible::ErrorSeverity severity, verible::AnalysisPhase phase,
            absl::string_view token_text, absl::string_view context_line,
            const std::string &msg) {
          std::string message(AnalysisPhaseName(phase));
          absl::StrAppend(&message, " ", ErrorSeverityDescription(severity));
          if (rejected_token.token_info.isEOF()) {
            absl::StrAppend(&message, " (unexpected EOF)");
          } else {
            absl::StrAppend(&message, " at \"", token_text, "\"");
          }
          if (!msg.empty()) {  // Note: msg is often empty and not useful.
            absl::StrAppend(&message, " ", msg);
          }
          // TODO(hzeller): Add severity into lsp::Diagnostic json.
          result.emplace_back(verible::lsp::Diagnostic{
              .range{.start{.line = range.start.line,
                            .character = range.start.column},
                     .end{.line = range.end.line,  //
                          .character = range.end.column}},
              .message = message,
          });
        });
    if (--remaining <= 0) break;
  }

  for (const auto &v : lint_violations) {
    result.emplace_back(ViolationToDiagnostic(v, current->parser().Data()));
    if (--remaining <= 0) break;
  }
  return result;
}

verible::lsp::FullDocumentDiagnosticReport GenerateDiagnosticReport(
    const BufferTracker *tracker,
    const verible::lsp::DocumentDiagnosticParams &p) {
  verible::lsp::FullDocumentDiagnosticReport result;
  if (!tracker) return result;
  result.items = CreateDiagnostics(*tracker);
  return result;
}

static std::vector<verible::lsp::TextEdit> AutofixToTextEdits(
    const verible::AutoFix &fix, const verible::TextStructureView &text) {
  std::vector<verible::lsp::TextEdit> result;
  // TODO(hzeller): figure out if edits are stacking or are all based
  // on the same start status.
  const absl::string_view base = text.Contents();
  for (const verible::ReplacementEdit &edit : fix.Edits()) {
    verible::LineColumn start =
        text.GetLineColAtOffset(edit.fragment.begin() - base.begin());
    verible::LineColumn end =
        text.GetLineColAtOffset(edit.fragment.end() - base.begin());
    result.emplace_back(verible::lsp::TextEdit{
        .range =
            {
                .start = {.line = start.line, .character = start.column},
                .end = {.line = end.line, .character = end.column},
            },
        .newText = edit.replacement,
    });
  }
  return result;
}

std::vector<verible::lsp::CodeAction> GenerateLinterCodeActions(
    const BufferTracker *tracker, const verible::lsp::CodeActionParams &p) {
  std::vector<verible::lsp::CodeAction> result;
  if (!tracker) return result;
  const ParsedBuffer *const current = tracker->current();
  if (!current) return result;

  auto const &lint_violations =
      verilog::GetSortedViolations(current->lint_result());
  if (lint_violations.empty()) return result;

  const verible::TextStructureView &text = current->parser().Data();

  for (const auto &v : lint_violations) {
    const verible::LintViolation &violation = *v.violation;
    if (violation.autofixes.empty()) continue;
    auto diagnostic = ViolationToDiagnostic(v, text);

    // The editor usually has the cursor on a line or word, so we
    // only want to output edits that are relevant.
    if (!rangeOverlap(diagnostic.range, p.range)) continue;

    bool preferred_fix = true;
    for (const auto &fix : violation.autofixes) {
      result.emplace_back(verible::lsp::CodeAction{
          .title = fix.Description(),
          .kind = "quickfix",
          .diagnostics = {diagnostic},
          .isPreferred = preferred_fix,
          // The following is translated from json, map uri -> edits.
          // We're only sending changes for one document, the current one.
          .edit = {.changes = {{p.textDocument.uri,
                                AutofixToTextEdits(fix,
                                                   current->parser().Data())}}},
      });
      preferred_fix = false;  // only the first is preferred.
    }
  }
  return result;
}

nlohmann::json CreateDocumentSymbolOutline(
    const BufferTracker *tracker, const verible::lsp::DocumentSymbolParams &p,
    bool kate_compatible_tags) {
  if (!tracker) return nlohmann::json::array();
  // Only if the tree has been fully parsed, it makes sense to create an outline
  const ParsedBuffer *const last_good = tracker->last_good();
  if (!last_good) return nlohmann::json::array();

  verible::lsp::DocumentSymbol toplevel;
  const auto &text_structure = last_good->parser().Data();
  verilog::DocumentSymbolFiller filler(kate_compatible_tags, text_structure,
                                       &toplevel);
  const auto &syntax_tree = text_structure.SyntaxTree();
  syntax_tree->Accept(&filler);
  // We cut down one level, not interested in toplevel file:
  return toplevel.children;
}

std::vector<verible::lsp::DocumentHighlight> CreateHighlightRanges(
    const BufferTracker *tracker,
    const verible::lsp::DocumentHighlightParams &p) {
  std::vector<verible::lsp::DocumentHighlight> result;
  if (!tracker) return result;
  const ParsedBuffer *const current = tracker->current();
  if (!current) return result;
  const verible::LineColumn cursor{p.position.line, p.position.character};
  const verible::TextStructureView &text = current->parser().Data();

  const verible::TokenInfo cursor_token = text.FindTokenAt(cursor);
  if (cursor_token.token_enum() != SymbolIdentifier) return result;

  // Find all the symbols with the same name in the buffer.
  // Note, this is very simplistic as it does _not_ take scopes into account.
  // For that, we'd need the symbol table, but that implementation is not
  // complete yet.
  for (const verible::TokenInfo &tok : text.TokenStream()) {
    if (tok.token_enum() != cursor_token.token_enum()) continue;
    if (tok.text() != cursor_token.text()) continue;
    const verible::LineColumnRange range = text.GetRangeForToken(tok);
    result.push_back(verible::lsp::DocumentHighlight{
        .range = {
            .start = {.line = range.start.line,
                      .character = range.start.column},
            .end = {.line = range.end.line, .character = range.end.column},
        }});
  }

  return result;
}

std::vector<verible::lsp::TextEdit> FormatRange(
    const BufferTracker *tracker,
    const verible::lsp::DocumentFormattingParams &p) {
  std::vector<verible::lsp::TextEdit> result;
  if (!tracker) return result;
  const ParsedBuffer *const current = tracker->current();
  if (!current) return result;  // Can only format if we have latest version.
  const verible::TextStructureView &text = current->parser().Data();

  verilog::formatter::FormatStyle format_style;
  verilog::formatter::InitializeFromFlags(&format_style);

  if (p.has_range) {
    // If the cursor is at the very beginning of last line, we don't include
    // it in the formatting.
    const int last_line_include = p.range.end.character > 0 ? 1 : 0;
    const verible::Interval<int> format_lines{
        p.range.start.line + 1,  // 1 index based
        p.range.end.line + 1 + last_line_include};
    std::string formatted_range;
    if (!FormatVerilogRange(text, format_style, &formatted_range, format_lines)
             .ok())
      return result;
    result.push_back(verible::lsp::TextEdit{
        .range =
            {
                .start = {.line = format_lines.min - 1, .character = 0},
                .end = {.line = format_lines.max - 1, .character = 0},
            },
        .newText = formatted_range});
  } else {
    std::string newText;
    if (!FormatVerilog(text, current->uri(), format_style, &newText).ok())
      return result;
    // Emit a single edit that replaces the full file. One could consider
    // patches maybe; also be safe and don't emit anything if text is the same.
    result.push_back(verible::lsp::TextEdit{
        .range =
            {
                .start = {.line = 0, .character = 0},
                .end = {.line = static_cast<int>(text.Lines().size() - 1),
                        .character = 0},
            },
        .newText = newText});
  }
  return result;
}

}  // namespace verilog
