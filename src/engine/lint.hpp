// engine/lint.hpp — read-only project sanity checks ("why is my beat silent?") as JSON.
// Spec: docs/spec/lint.md (WP-0170). lint_project never modifies the project; it is a pure function of
// `p`, `base_dir` and (for rule 8 only) the filesystem. Issues come out rule by rule, in ascending index
// order within each rule. Example:
//   op::Project p; std::string err;
//   if (op::load_project("song.opj", p, err)) {
//       const auto issues = op::lint::lint_project(p, "dir/of/song");
//       std::fputs(op::lint::to_json(issues).c_str(), stdout);  // {"version":1,...}\n
//   }
#pragma once
#include <string>
#include <vector>
#include "engine/model.hpp"
namespace op::lint {
struct Issue { std::string severity; std::string code; std::string path; std::string message; };
std::vector<Issue> lint_project(const Project& p, const std::string& base_dir);
std::string to_json(const std::vector<Issue>& issues);
}  // namespace op::lint
