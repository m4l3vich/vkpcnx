cmake -B build -DPLATFORM_DESKTOP=ON -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_POLICY_VERSION_MINIMUM=3.5 \
  -DCMAKE_OSX_DEPLOYMENT_TARGET=11.0
cmake --build build -j$(sysctl -n hw.ncpu)
