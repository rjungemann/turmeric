#!/bin/bash

# Cross-language validation runner
set -e

# Change to validation directory
cd "$(dirname "$0")"

# Create virtual environment if it doesn't exist
if [ ! -d "venv" ]; then
    echo "Creating Python virtual environment..."
    python3 -m venv venv
fi

# Activate virtual environment
source venv/bin/activate

# Install requirements
pip install -q -r requirements.txt 2>/dev/null || echo "No requirements.txt found, continuing..."

# Run validation
echo "Running cross-language validation..."
python validate_fibonacci.py

# Deactivate virtual environment
deactivate