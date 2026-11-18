// engine/model_json.hpp — engine-internal Project ⇄ in-memory JSON bridge. NOT public API
// (the public surface is engine/model.hpp): save_project/load_project in model.cpp and the
// operation layer in ops.cpp share these so validation re-uses the exact same serialize/parse
// code paths (docs/spec/operations.md §Apply semantics). Example:
//   nlohmann::json j = op::project_to_json(proj);          // canonical bytes-as-JSON
//   op::Project back; std::string err;
//   if (!op::project_from_json(j, back, err)) { /* invalid project JSON */ }
#pragma once
#include <string>

#include <nlohmann/json.hpp>

#include "engine/model.hpp"

namespace op {

// Canonical serialization of a Project — the same JSON `save_project` writes (stable key order,
// omit-at-default rules). Never throws; pure function of `p`.
nlohmann::json project_to_json(const Project& p);

// Parse+validate project JSON with exactly the code path `load_project` uses. Returns false +
// one-line machine-parsable err on any violation. May throw nlohmann::json::exception on
// mistyped fields (same contract load_project wraps) — callers must catch. `out` is filled
// incrementally; on failure its contents are unspecified — parse into a scratch Project.
bool project_from_json(const nlohmann::json& j, Project& out, std::string& err);

}  // namespace op
