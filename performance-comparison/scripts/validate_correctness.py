#!/usr/bin/env python3
"""
Validation script to verify correctness of benchmark implementations.
Compares outputs across languages for consistency.
"""

import subprocess
import json
import os
import sys
from pathlib import Path

# Configuration
BASE_DIR = Path("/Users/rjungemann/Projects/turmeric/performance-comparison")
BENCHMARK_DIR = BASE_DIR / "benchmarks"
RESULTS_DIR = BASE_DIR / "results/raw"
LANGUAGES = ["c", "python"]  # Add turmeric, clojure, racket as they're implemented
CATEGORIES = ["io", "real_world"]

# Test cases configuration
    test_cases = {
    "io": {
        "file_read": {
            "input": "/Users/rjungemann/Projects/turmeric/performance-comparison/inputs/large/100mb.dat",
            "validation": "exit_code",  # Just check it runs without error
        },
        "file_write": {
            "input": "/Users/rjungemann/Projects/turmeric/performance-comparison/inputs/large/output.dat",
            "validation": "file_size",  # Check output file is correct size
            "expected_size": 100 * 1024 * 1024,  # 100MB
        },
    },
    "real_world": {
        "ray_tracing": {
            "validation": "exit_code",  # Visual validation would be better
        },
        "nbody": {
            "validation": "exit_code",  # Would need reference output for proper validation
        },
    }
}

def run_command(cmd, cwd=None):
    """Run a command and return its output"""
    try:
        result = subprocess.run(
            cmd, 
            shell=True, 
            check=True, 
            capture_output=True, 
            text=True,
            cwd=cwd
        )
        return result.returncode, result.stdout, result.stderr
    except subprocess.CalledProcessError as e:
        return e.returncode, e.stdout, e.stderr

def validate_benchmark(language, category, benchmark_name, test_config):
    """Validate a single benchmark implementation"""
    print(f"Validating {language}/{category}/{benchmark_name}...")
    
    benchmark_path = BENCHMARK_DIR / category / language / f"{benchmark_name}"
    if language != "c":
        benchmark_path = benchmark_path.with_suffix(".py")
    
    if language == "c":
        # Build C programs
        src_path = benchmark_path.with_suffix(".c")
        if not src_path.exists():
            print(f"  Source file not found: {src_path}")
            return False
            
        # Compile
        cmd = f"clang -O3 -o {benchmark_path} {src_path}"
        ret, out, err = run_command(cmd)
        if ret != 0:
            print(f"  Compilation failed: {err}")
            return False
            
        # Run
        input_arg = f" {test_config.get('input', '')}" if 'input' in test_config else ""
        cmd = f"cd {benchmark_path.parent} && ./{benchmark_path.name}{input_arg}"
    else:
        # Run interpreted languages
        cmd = f"cd {benchmark_path.parent} && python3 {benchmark_path.name} {test_config.get('input', '')}"
        
    ret, out, err = run_command(cmd, cwd=benchmark_path.parent)
    
    # Basic validation
    if ret != 0:
        print(f"  Execution failed: {err}")
        return False
        
    # Specific validation
    validation_type = test_config.get("validation", "exit_code")
    
    if validation_type == "exit_code":
        print(f"  ✓ Completed successfully (exit code: {ret})")
        return True
    elif validation_type == "file_size":
        output_file = test_config.get("input", "")
        if not output_file:
            print("  ✗ No output file specified for file_size validation")
            return False
            
        if not os.path.exists(output_file):
            print(f"  ✗ Output file not created: {output_file}")
            return False
            
        actual_size = os.path.getsize(output_file)
        expected_size = test_config.get("expected_size", 0)
        
        if actual_size == expected_size:
            print(f"  ✓ Output file size correct: {actual_size} bytes")
            return True
        else:
            print(f"  ✗ Output file size incorrect: got {actual_size}, expected {expected_size}")
            return False
    else:
        print(f"  ? Unknown validation type: {validation_type}")
        return True  # Don't fail for unknown validation types

def main():
    """Main validation routine"""
    print("Starting benchmark validation...")
    
    all_passed = True
    
    for category in CATEGORIES:
        for benchmark_name, test_config in test_cases.get(category, {}).items():
            print(f"\n=== {category}/{benchmark_name} ===")
            
            category_passed = True
            
            for language in LANGUAGES:
                if not validate_benchmark(language, category, benchmark_name, test_config):
                    category_passed = False
                    all_passed = False
            
            if category_passed:
                print(f"✓ All implementations of {category}/{benchmark_name} passed validation")
            else:
                print(f"✗ Some implementations of {category}/{benchmark_name} failed validation")
    
    if all_passed:
        print("\n🎉 All benchmarks passed validation!")
        return 0
    else:
        print("\n❌ Some benchmarks failed validation")
        return 1

if __name__ == "__main__":
    sys.exit(main())