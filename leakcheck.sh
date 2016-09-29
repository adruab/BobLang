#!/bin/bash
# Run demo with valgrind

valgrind --leak-check=yes --leak-check=full ./build/jtoy --run-unit-tests test.jtoy