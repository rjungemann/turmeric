# Turmeric Argument Parser

## Objective
Design and implement a standard argument parser for Turmeric scripts to handle named arguments, flags, and type conversion.

## Background
The [CLI Arguments plan](cli-arguments-plan.md) introduces raw argument passing via `*args*`. This plan builds on that foundation to provide a structured way to parse and validate arguments.

## Proposed Solution

### 1. API Design
- Provide a `parse-args` function that takes a specification and the raw `*args*` list:
  ```turmeric
  (defn parse-args [spec args] :map
    "Parses CLI arguments according to the given spec.")
  ```

- **Spec Format**:
  ```turmeric
  {:flags   ["--verbose" "--help"]  ; boolean flags
   :options {"--input"  :string      ; named args with types
            "--count"  :int}
   :args    [:string :string]}        ; positional args
  ```

### 2. Features
- **Flags**: Boolean flags (e.g., `--verbose`).
- **Options**: Named arguments with types (e.g., `--input file.txt`).
- **Positional Args**: Ordered arguments with types (e.g., `input.txt output.txt`).
- **Type Conversion**: Automatic conversion to `:string`, `:int`, `:float`, or `:bool`.
- **Help Generation**: Auto-generate help text from the spec.
- **Error Handling**: Clear error messages for invalid arguments.

### 3. Example Usage
```turmeric
(defn main []
  (let [args-spec {:flags   ["--verbose"]
                   :options {"--input" :string}
                   :args    [:string]}
        parsed    (parse-args args-spec *args*)]
    (if (contains? parsed :help)
      (print-help args-spec)
      (process (get parsed :input) (get parsed :verbose false)))))
```

### 4. Error Handling
- Validate required arguments.
- Check argument types and provide descriptive errors.
- Support custom validation functions.

## Implementation Steps
1. Design the `parse-args` function and spec format.
2. Implement argument parsing logic in Turmeric.
3. Add type conversion and validation.
4. Implement help text generation.
5. Write tests for all features.
6. Update documentation.

## Open Questions
- Should we support subcommands (e.g., `git commit`)?
- How should we handle default values for optional arguments?

## Alternatives Considered
- **Manual Parsing**: Rejected for being error-prone and inconsistent.
- **External Libraries**: Rejected to keep the standard library self-contained.