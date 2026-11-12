# Building

1. Configure and build:

        cmake -S . -B build
        cmake --build build -j

2. Run the tests:

		ctest --test-dir build

Or with qmake:

```sh
qmake6 ../meld-qt.pro && make
```
