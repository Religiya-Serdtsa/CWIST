#!/bin/bash

# Render all .puml files in the parent directory to .png
# Requires plantuml to be installed and in PATH.

SCRIPT_DIR=$(dirname "$0")
BLUEPRINTS_DIR="$SCRIPT_DIR/.."

echo "Rendering Blueprints in $BLUEPRINTS_DIR..."

if ! command -v plantuml &> /dev/null; then
    echo "Error: plantuml not found in PATH."
    echo "Please install it (e.g., sudo apt install plantuml or brew install plantuml)."
    exit 1
fi

find "$BLUEPRINTS_DIR" -maxdepth 1 -name "*.puml" -print0 | xargs -0 -n 1 plantuml -tpng -verbose

echo "Rendering Complete."
