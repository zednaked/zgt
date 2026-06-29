#!/bin/bash
set -e

if [ ! -d "godot-cpp" ]; then
    echo "Cloning godot-cpp..."
    git clone https://github.com/godotengine/godot-cpp.git
fi

cd godot-cpp
git checkout 4.3
cd ..

echo "Setup complete. Now run: scons platform=linux"
