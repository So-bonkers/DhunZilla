// engine/feature_coverage.hpp — WP-0211 (docs/spec/golden-coverage.md): the read-only golden feature-
// coverage report behind `opd golden-coverage` — which model features each scanned golden project
// exercises and which table features NO golden covers, so drafters know where a golden is missing.
// It is a report, not a gate: it adds no model field, no operation and changes no render. Example:
//   op::NamedProject np; np.name = "demo"; np.project = p;  // p loaded by op::load_project
//   const std::string text = op::feature_coverage_json({np});  // "{\n  \"features\": [\n ... }\n"
#pragma once
#include <string>
#include <vector>

#include "engine/model.hpp"

namespace op {

// One scanned golden: the label the report prints plus its already-loaded project.
struct NamedProject {
    std::string name;      // label printed by the report (the CLI passes the .opj file stem)
    Project project;       // already loaded by op::load_project
};

// The coverage report over a set of golden projects: a nlohmann object dumped with dump(2) (two-space
// indent, object keys alphabetically sorted) plus exactly ONE trailing '\n' — so the text starts
// "{\n  \"features\": [\n" and ends with "}\n". Top-level keys, always present, exactly four:
// "features" (one entry per feature-table row, in table order; each entry has exactly "covered_by" —
// the golden names that exercise it, ascending, empty when none does — "key" and "label"), "goldens"
// (one entry per input golden, sorted by "name" ascending regardless of input order; each entry has
// exactly "features" — the table keys that golden exercises, in table order — and "name"),
// "uncovered" (the table keys whose covered_by is empty, in table order) and "version" (the JSON
// integer 1). Empty input is valid: "goldens" is [] and "uncovered" lists every table key.
// Pure function: no I/O, no clock, no randomness; it never modifies a project, and two calls with
// equal input return byte-identical text. Every predicate reads the LOADED project, so loader
// normalizations are what gets reported.
std::string feature_coverage_json(const std::vector<NamedProject>& goldens);

}  // namespace op
