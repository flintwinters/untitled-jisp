# Execution Stack (JPM pointer) Architecture - Summary of Changes

- Introduce root["execution"] as a LIFO array of frames, each frame stored as {"$ptr": "<RFC6901 path>"} pointing into the current document.
- Replace elem-based iteration with jpm_return(doc, path, &p) resolving the top frame; process exactly one target per recursive step.
- Re-implement process_entrypoint and process_ep_array to seed frames for "/entrypoint" and drive recursion via the execution stack; no while loops.
- Add recursive scheduler that expands object "." arrays by pushing child element pointers using schedule_array_recursive (forward order via unwind).
- Preserve previous semantics: literals (string/number/array/object without ".") are deep-copied and pushed to root["stack"]; unknown "." op names become literals.
- Maintain residual JSON Patch logging behavior for pushes/removes; execution stack management is best-effort and non-fatal on OOM.
- New helpers: ensure_execution_array, push_exec_ptr, pop_exec_ptr, join_paths, schedule_array_recursive, process_execution_recursive.
