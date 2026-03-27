cd build
cmake .. -DCMAKE_BUILD_TYPE=Debug
make -j$(sysctl -n hw.logicalcpu)
echo "Build complete run ./build/hologram to start the visualizer"
cd ..

./build/hologram
