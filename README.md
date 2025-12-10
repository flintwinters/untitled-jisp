# JISP
**Jisp** is a programming system which uses the JSON data model as its underlying atomic fabric. The program, including the code, variables, and execution state, are all represented directly in a single JSON object. This approach makes it easier to debug, integrate with other tools, and eliminates LLM syntax errors.

The strict, simple, universally understandable grammar is perfect for LLM outputs and toolcalls, eliminating syntax errors.

### Why JSON?

#### 1. Advanced Debugging

Jisp offers powerful debugging features thanks to its use of JSON for the entire program state—code, heap, stack, and environment. Since everything is encapsulated as a single JSON object, you can:

* **Step Forward and Backward**: Log the program state at different points and jump between them, replaying specific steps in the execution flow.

* **Time Travel with Diffs**: Track program changes using simple diffs, allowing you to easily revert to previous states or inspect a specific moment by applying inverse diffs.

* **Full Inspection**: Inspect and manipulate any part of the program (variables, functions, memory) at any time, all through the transparent structure of JSON.

* **Automated Debugging**: Scripts and LLMs can programmatically inspect program states, enabling automated debugging.

#### 2. Easy Integration:
Jisp’s use of JSON makes it easy to work with other systems and tools, such as APIs or language models. JSON is a common format for data exchange, so connecting Jisp with external tools is seamless.

#### 3. Simple, Readable Code:
Jisp programs are written in JSON, a format most developers are already familiar with. This makes the code easy to read and understand without dealing with new syntax.

### Conclusion

Jisp takes advantage of JSON's simplicity and universality to create a programming system that’s easy to understand, easy to debug, and easy to integrate.