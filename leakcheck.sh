#!/bin/bash
# Run demo with valgrind

valgrind --leak-check=yes --leak-check=full ./build/bob --run-unit-tests test.bob