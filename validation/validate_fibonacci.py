#!/usr/bin/env python3
"""
Cross-language validation framework for Fibonacci benchmarks.

This script runs all Fibonacci implementations with the same inputs and
verifies they produce identical results.
"""

import subprocess
import json
import sys
import os
from pathlib import Path

# Configuration
LANGUAGES = {
    "c": {
        "path": "../benchmarks/numerical/c/fibonacci.c",
        "build": "clang -O3 -o ../benchmarks/numerical/c/fibonacci ../benchmarks/numerical/c/fibonacci.c",
        "run": "./../benchmarks/numerical/c/fibonacci {n}",
        "binary": "../benchmarks/numerical/c/fibonacci"
    },
    "turmeric": {
        "path": "../benchmarks/numerical/turmeric/fibonacci.tur",
        "run": "../build-rel/tur run {path} -- {n}"
    },
    "python": {
        "path": "../benchmarks/numerical/python/fibonacci.py",
        "run": "python3 {path} {n}"
    },
    "racket": {
        "path": "../benchmarks/numerical/racket/fibonacci.rkt",
        "run": "racket {path} {n}"
    },
    "clojure": {
        "path": "../benchmarks/numerical/clojure/fibonacci.clj",
        "run": "clojure {path} {n}"
    }
}

TEST_CASES = [0, 1, 5, 10, 20]
# Note: Avoid very large n values for recursive implementations

class ValidationError(Exception):
    """Raised when validation fails"""
    pass


def build_c_program():
    """Build the C program if needed"""
    c_config = LANGUAGES["c"]
    if not Path(c_config["binary"]).exists():
        print("Building C program...")
        result = subprocess.run(c_config["build"], shell=True, capture_output=True, text=True)
        if result.returncode != 0:
            print(f"Failed to build C program: {result.stderr}")
            sys.exit(1)


def run_implementation(lang, n, implementation):
    """Run a specific implementation and return parsed results"""
    config = LANGUAGES[lang]
    cmd = config["run"].format(path=config["path"], n=n)
    
    try:
        result = subprocess.run(cmd, shell=True, capture_output=True, text=True, check=True)
        # Parse JSON output (one line per implementation)
        outputs = result.stdout.strip().split('\n')
        for line in outputs:
            try:
                data = json.loads(
                    line.replace("'", '"')  # Handle Turmeric's single quotes
                        .replace("=", ":")  # Handle Racket's format
                )
                if data.get("implementation") == implementation:
                    return data
            except json.JSONDecodeError:
                continue
        
        raise ValidationError(f"Could not parse output from {lang} {implementation}")
        
    except subprocess.CalledProcessError as e:
        raise ValidationError(f"Failed to run {lang} {implementation}: {e.stderr}")


def validate_implementations():
    """Validate that all implementations produce identical results"""
    print("Starting cross-language validation...")
    
    # Build C program first
    build_c_program()
    
    results = {}
    
    for n in TEST_CASES:
        print(f"\nValidating with n={n}")
        
        # Get results from all languages
        for lang in LANGUAGES:
            try:
                recursive_result = run_implementation(lang, n, "recursive")
                iterative_result = run_implementation(lang, n, "iterative")
                
                if lang not in results:
                    results[lang] = {}
                
                results[lang][n] = {
                    "recursive": recursive_result["result"],
                    "iterative": iterative_result["result"]
                }
                
                print(f"  {lang}: recursive={recursive_result['result']}, iterative={iterative_result['result']}")
                
            except ValidationError as e:
                print(f"  ERROR with {lang}: {str(e)}")
                return False
        
        # Validate all recursive implementations agree
        recursive_values = {results[lang][n]["recursive"] for lang in results}
        if len(recursive_values) > 1:
            raise ValidationError(f"Recursive implementations disagree for n={n}: {recursive_values}")
        
        # Validate all iterative implementations agree
        iterative_values = {results[lang][n]["iterative"] for lang in results}
        if len(iterative_values) > 1:
            raise ValidationError(f"Iterative implementations disagree for n={n}: {iterative_values}")
        
        # Validate recursive and iterative agree for each language
        for lang in results:
            if results[lang][n]["recursive"] != results[lang][n]["iterative"]:
                raise ValidationError(
                    f"Recursive and iterative implementations disagree in {lang} for n={n}: "
                    f"recursive={results[lang][n]['recursive']}, "
                    f"iterative={results[lang][n]['iterative']}"
                )
    
    print("\n✅ All implementations produced identical results!")
    return True


def create_golden_files():
    """Create golden test files with expected results"""
    print("\nCreating golden test files...")
    
    # Ensure directory exists
    os.makedirs("golden", exist_ok=True)
    
    # Create golden file for each test case
    for n in TEST_CASES:
        golden_data = {
            "n": n,
            "expected": {
                "recursive": results[next(iter(results))][n]["recursive"],  # Any language
                "iterative": results[next(iter(results))][n]["iterative"]   # Any language
            }
        }
        
        with open(f"golden/fibonacci_{n}.json", "w") as f:
            json.dump(golden_data, f, indent=2)
        
        print(f"Created golden/fibonacci_{n}.json")


if __name__ == "__main__":
    # First validate all implementations
    if validate_implementations():
        # Then create golden files
        create_golden_files()
        print("\n✅ Validation successful and golden files created!")
    else:
        print("\n❌ Validation failed!")
        sys.exit(1)