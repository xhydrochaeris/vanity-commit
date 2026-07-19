# vanity-commit

A tool to create **vanity commit IDs**, by appending a **vanity string** to the commit message.

The current implementation only supports Linux with gcc + OpenMP + libcrypto.

GPU support is planned for future versions by adding a CUDA implementation.

Additionally, a Python fallback implementation is planned (not that you'd need one anyway, the CPU implementation will probably work for everyone unless you are on some kind of crazy platform)

Extending support for Windows and macOS is planned. macOS **will not** support the GPU implementation!

## Build:

`gcc -O3 -fopenmp vanity_cpu.c -o vanity_cpu -lcrypto`

## Run:

Test the implementation:

`./vanity_cpu "<commit-content-before-trailer>" "69420"`

Make the commit:

`git add .`

`./vanity-commit.py -m "Initial Commit ( ͡° ͜ʖ ͡°)" "69420"`

This matches the beginning of your commit hash ID with the provided string. And yes, it works. Just look at this commit's ID!!

## Note:

The default branch for this project is called **vain**, not main. This is a deliberate pun.
