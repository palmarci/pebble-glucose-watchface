# Format C source files with clang-format
format:
    find src -name '*.c' -o -name '*.h' | xargs clang-format-18 -i

# Check formatting without modifying files
format-check:
    find src -name '*.c' -o -name '*.h' | xargs clang-format-18 --dry-run --Werror
