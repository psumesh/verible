// Copyright 2017-2020 The Verible Authors.
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

#include "verilog/analysis/checkers/always_comb_rule.h"

#include <set>
#include <string>

#include "absl/strings/str_cat.h"
#include "absl/strings/string_view.h"
#include "common/analysis/lint_rule_status.h"
#include "common/analysis/matcher/bound_symbol_manager.h"
#include "common/analysis/matcher/matcher.h"
#include "common/text/symbol.h"
#include "common/text/syntax_tree_context.h"
#include "verilog/CST/verilog_matchers.h"  // IWYU pragma: keep
#include "verilog/analysis/descriptions.h"
#include "verilog/analysis/lint_rule_registry.h"

namespace verilog {
namespace analysis {

using verible::LintRuleStatus;
using verible::LintViolation;
using verible::SyntaxTreeContext;
using verible::matcher::Matcher;

// Register AlwaysCombRule
VERILOG_REGISTER_LINT_RULE(AlwaysCombRule);

static constexpr absl::string_view kMessage =
    "Use 'always_comb' instead of 'always @*'.";

const LintRuleDescriptor& AlwaysCombRule::GetDescriptor() {
  static const LintRuleDescriptor d{
      .name = "always-comb",
      .topic = "combinational-logic",
      .desc =
          "Checks that there are no occurrences of "
          "`always @*`. Use `always_comb` instead.",
  };
  return d;
}

// Matches event control (sensitivity list) for all signals.
// For example:
//   always @* begin
//     f = g + h;
//   end
static const Matcher& AlwaysStarMatcher() {
  static const Matcher matcher(NodekAlwaysStatement(
      AlwaysKeyword(), AlwaysStatementHasEventControlStar()));
  return matcher;
}

static const Matcher& AlwaysStarMatcherWithParentheses() {
  static const Matcher matcher(NodekAlwaysStatement(
      AlwaysKeyword(), AlwaysStatementHasEventControlStarAndParentheses()));
  return matcher;
}

void AlwaysCombRule::HandleSymbol(const verible::Symbol& symbol,
                                  const SyntaxTreeContext& context) {
  // Check for offending use of always @*
  verible::matcher::BoundSymbolManager manager;
  if (AlwaysStarMatcher().Matches(symbol, &manager)) {
    violations_.insert(LintViolation(symbol, kMessage, context));
  }
  if (AlwaysStarMatcherWithParentheses().Matches(symbol, &manager)) {
    violations_.insert(LintViolation(symbol, kMessage, context));
  }
}

LintRuleStatus AlwaysCombRule::Report() const {
  return LintRuleStatus(violations_, GetDescriptor());
}

}  // namespace analysis
}  // namespace verilog
